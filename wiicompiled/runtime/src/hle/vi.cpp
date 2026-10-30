#include "frontend/runtime_mode.h"
#include <vector>
#include "frame_input.h"
#include "det_hash.h"
#include "timebase_contract.h"
#include "det_clock.h"
#include "hle_stubs.h"
#include "runtime_config.h"
#include "memory.h"
#include "abi_bridge.h"
#include "guest_interrupt_context.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "settings_overlay.h"
#include "fiber_manager.h"
#include "platform/host_platform.h"
#include "runtime_log.h"
#include "guest_profiler.h"
#include "spm_map_sweep.h"
#include "save_state.h"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <aurora/gfx.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

#include <aurora/aurora.h>
#include <SDL3/SDL_video.h>

// Forward declaration for OSWakeupThread - used to wake threads on VI retrace queue
extern "C" void OSWakeupThread_HLE_801aaaa4(CpuContext* ctx);

// Forward declaration for OSSleepThread - used by VIWaitForRetrace HLE
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* cpu);
extern "C" int g_gxFrameCount;
extern "C" int32_t OS__DisableInterrupts_801a65ac();
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level);

// Aurora frame cycle tracking - needs external linkage for the GX HLE
// (declared in gx_internal.h, consumed by gx_frame.cpp). We need to call
// aurora_begin_frame() before GX commands and aurora_end_frame() after.
std::atomic_bool g_auroraFrameActive{false};
std::atomic_bool g_auroraFrameHadWork{false};

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGuestGxRenderModeCopyBytes = 0x39;

bool ReadGuestRenderModeObj(uint32_t guestPtr, GXRenderModeObj& out) {
    if (guestPtr == 0 || !Memory::Contains(guestPtr, kGuestGxRenderModeCopyBytes)) {
        return false;
    }

    try {
        out.viTVmode = static_cast<VITVMode>(Memory::Read32(guestPtr + 0x00));
        out.fbWidth = Memory::Read16(guestPtr + 0x04);
        out.efbHeight = Memory::Read16(guestPtr + 0x06);
        out.xfbHeight = Memory::Read16(guestPtr + 0x08);
        out.viXOrigin = Memory::Read16(guestPtr + 0x0a);
        out.viYOrigin = Memory::Read16(guestPtr + 0x0c);
        out.viWidth = Memory::Read16(guestPtr + 0x0e);
        out.viHeight = Memory::Read16(guestPtr + 0x10);
        out.xFBmode = static_cast<VIXFBMode>(Memory::Read32(guestPtr + 0x14));
        out.field_rendering = Memory::Read8(guestPtr + 0x18);
        out.aa = Memory::Read8(guestPtr + 0x19);

        for (size_t i = 0; i < 12; ++i) {
            out.sample_pattern[i][0] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2));
            out.sample_pattern[i][1] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2) + 1);
        }
        for (size_t i = 0; i < 7; ++i) {
            out.vfilter[i] = Memory::Read8(guestPtr + 0x32 + static_cast<uint32_t>(i));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }

    return true;
}

struct ViState {
    bool initialized = false;
    uint32_t tvFormat = 0; // VIGetTvFormat returns VI_NTSC before VIInit
    uint32_t nextFrameBuffer = 0;
    uint32_t currentFrameBuffer = 0;
    uint32_t retraceCount = 0;
    bool black = false;
    uint32_t preRetraceCallback = 0;
    uint32_t postRetraceCallback = 0;
    uint32_t renderWidth = 640;
    uint32_t renderHeight = 480;
    uint32_t viXOrigin = 0;
    uint32_t viYOrigin = 0;
    uint32_t xfbWidth = 640;
    uint32_t xfbHeight = 480;
    bool fieldOdd = false;
    Clock::time_point lastRetrace = Clock::now();
    std::chrono::microseconds retraceInterval{16666us}; // ~60 Hz
    // Deterministic mode: the virtual tick of the last retrace (see det_clock.h); lastRetrace above
    // keeps real time for presentation only.
    uint64_t detLastRetraceTicks = 0;
    bool hasValidXfb = false; // True once we've received at least one GXCopyDisp
    uint32_t readyXfb = 0;    // XFB address from the most recent GXCopyDisp

    // VIConfigure/VISetNextFrameBuffer/VISetBlack only write pending values below; VIFlush arms them but
    // the commit happens at the next retrace, matching real VI hardware. A VIFlush called from a
    // pre-retrace callback therefore misses the imminent field and lands one field late.

    // Pending values (written by VISetNextFrameBuffer, VISetBlack, VIConfigure)
    uint32_t pendingNextFrameBuffer = 0;
    bool pendingBlack = false;
    // Matches the active-state default (NTSC before VIInit) and the pending
    // 16666us interval below; a flush before any VIConfigure must not commit
    // PAL timing onto an NTSC-interval state.
    uint32_t pendingTvFormat = 0;
    uint32_t pendingRenderWidth = 640;
    uint32_t pendingRenderHeight = 480;
    uint32_t pendingViXOrigin = 0;
    uint32_t pendingViYOrigin = 0;
    uint32_t pendingXfbWidth = 640;
    uint32_t pendingXfbHeight = 480;
    std::chrono::microseconds pendingRetraceInterval{16666us};
    // V-Sync: the interval the display lock wants for the next retrace (0 = the game's own interval).
    std::chrono::nanoseconds lockedInterval{0};
    
    // Set by VIFlush(); cleared after commit in AdvanceRetrace
    bool flushArmed = false;
};

std::mutex g_viMutex;
ViState g_vi;


// Guest-side state addresses used by the SDK's VI globals.
constexpr uint32_t kViInitializedFlagAddr   = 0x8056dd00;  // IsInitialized (NSMBW)
constexpr uint32_t kViTvFormatAddr          = 0x8056dd70;  // CurrTvMode (NSMBW)
constexpr uint32_t kViRenderWidthAddr       = 0x8050baf4;  // HorVer.DispSizeX (NSMBW)
constexpr uint32_t kViRenderHeightAddr      = 0x8050baf6;  // HorVer.DispSizeY (NSMBW)
constexpr uint32_t kViXfbWidthAddr          = 0x8050bb02;  // HorVer.FBSizeX (NSMBW)
constexpr uint32_t kViXfbHeightAddr         = 0x8050bb0c;  // HorVer+0x1C (PanSizeY) (NSMBW)
constexpr uint32_t kViRetraceCountAddr      = 0x8056ddac;  // retraceCount (NSMBW); // matches VIWaitForRetrace/handler
constexpr uint32_t kViTimingGuardAddr       = 0x8056dd0c;  // IsInitialized+0xC = __VIDimming_All_Clear in NSMBW (offset-matched; MKW name unverified)
constexpr uint32_t kViPreRetraceCallback    = 0x8056dd80;  // PreCB (NSMBW)
constexpr uint32_t kViPostRetraceCallback   = 0x8056dd7c;  // PostCB (NSMBW)
constexpr uint32_t kViNextFrameBufferAddr   = 0x8056dd68;  // CurrBufAddr in the SDK layout (offset-matched to MKW) (NSMBW)
constexpr uint32_t kViNextFrameBufferHwAddr = 0x8050bb20;  // HorVer.bufAddr (NSMBW)
constexpr uint32_t kViRetraceQueueAddr      = 0x8056dd88;  // retraceQueue (NSMBW); // Thread queue for VIWaitForRetrace

// (MKW gated the post-retrace callback on EGG::BaseSystem::sSystem at 0x80386F60; not used for NSMBW.)


// ============================================================================
// V-Sync display lock (Windows)
// ============================================================================
// With V-Sync on, presents wait for vblank. A game clock of exactly 60.000 Hz against a display that
// really runs at 59.94 (or 179.96 / 3) drops or repeats a frame every few seconds, and a full flip
// queue makes Present block. So the VI clock runs at exactly k display periods per retrace (k = 1 on a
// 60 Hz display, 3 at 180 Hz), with its phase pulled gently toward the middle of a vblank interval so
// each frame lands on the same vblank every time. The compositor reports the display's exact period
// and the time of its last vblank. Displays with no whole fraction near 60 (75, 144 Hz) keep the
// game's own clock.

struct DisplayLock {
    bool valid = false;
    int k = 0;
    std::chrono::nanoseconds period{0};
    Clock::time_point vblank{};
    bool phaseKnown = false;  // vblank is a real vblank time (DWM's period agreed with the display mode)
    Clock::time_point lastQuery{};
    bool logged = false;
};
DisplayLock g_displayLock;  // under g_viMutex

void RefreshDisplayLockLocked(Clock::time_point now) {
    DisplayLock& d = g_displayLock;
    if (!settings_overlay::VSync()) {
        d.valid = false;
        d.logged = false;
        return;
    }
    if (d.lastQuery != Clock::time_point{} && now - d.lastQuery < 250ms) {
        return;
    }
    d.lastQuery = now;
#if defined(_WIN32)
    using GetTimingFn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);
    static const GetTimingFn getTiming = [] {
        HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
        return dwm != nullptr ? reinterpret_cast<GetTimingFn>(
                                    reinterpret_cast<void*>(::GetProcAddress(dwm, "DwmGetCompositionTimingInfo")))
                              : nullptr;
    }();
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    LARGE_INTEGER frequency{}, qpcNow{};
    const HRESULT result = getTiming != nullptr ? getTiming(nullptr, &info) : E_NOTIMPL;
    const bool dwmOk = SUCCEEDED(result) && info.qpcRefreshPeriod != 0 && ::QueryPerformanceFrequency(&frequency) &&
                       ::QueryPerformanceCounter(&qpcNow);
    // The game window's display mode: DWM's timing is one clock for all displays and was 288-468 Hz on a
    // 240 Hz screen next to a 60 Hz one, which left the lock off ("120 FPS needs to truly work").
    double modeHz = 0.0;
    int windowCount = 0;
    if (SDL_Window** windows = SDL_GetWindows(&windowCount)) {
        if (windowCount > 0)
            if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(windows[0])))
                modeHz = mode->refresh_rate;
        SDL_free(windows);
    }
    const auto steadyNow = Clock::now();
    const double nsPerTick = dwmOk ? 1e9 / static_cast<double>(frequency.QuadPart) : 0.0;
    const double dwmHz = dwmOk ? 1e9 / (info.qpcRefreshPeriod * nsPerTick) : 0.0;
    d.phaseKnown = dwmOk && (modeHz <= 1.0 || std::fabs(dwmHz - modeHz) < modeHz * 0.02);
    const double hz = d.phaseKnown ? dwmHz : modeHz;
    if (hz <= 1.0) {
        if (!d.logged) {
            d.logged = true;
            RT_LOGF(RT_TAG_VI, "V-Sync: no display timing (0x%08lX); the game keeps its own clock\n",
                    static_cast<unsigned long>(result));
        }
        d.valid = false;
        return;
    }
    const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / hz));
    const int k = static_cast<int>(std::lround(hz / 60.0));
    d.valid = k >= 1 && k <= 8 && std::fabs(hz / k - 60.0) < 60.0 * 0.02;
    d.k = k;
    d.period = period;
    if (d.phaseKnown)
        d.vblank = steadyNow - std::chrono::nanoseconds(static_cast<int64_t>(
                                   static_cast<double>(qpcNow.QuadPart - static_cast<LONGLONG>(info.qpcVBlank)) * nsPerTick));
    if (!d.logged) {
        d.logged = true;
        RT_LOGF(RT_TAG_VI, "V-Sync: display %.3f Hz (DWM %.3f Hz, mode %.3f Hz%s), %s\n", hz, dwmHz, modeHz,
                d.phaseKnown ? "" : ", no vblank phase",
                d.valid ? (std::string("game clock locked at ") + std::to_string(k) + " refresh(es) per frame").c_str()
                        : "no whole fraction near 60 Hz; the game keeps its own clock");
    }
#else
    d.valid = false;
#endif
}

// The interval to the retrace after `retrace`: k display periods, nudged toward the target phase.
std::chrono::nanoseconds LockedIntervalAfter(Clock::time_point retrace) {
    const DisplayLock& d = g_displayLock;
    if (!d.valid || d.period.count() <= 0) {
        return std::chrono::nanoseconds(0);
    }
    const int64_t period = d.period.count();
    if (!d.phaseKnown) return std::chrono::nanoseconds(period * d.k);  // the rate alone, no phase pull
    // Presents go out about 6.5 ms after their retrace (aurora's kNativePresentOffsetNanos): put that
    // moment halfway between two vblanks, the most room either way.
    constexpr int64_t kPresentOffset = 6'500'000;
    int64_t target = (period / 2 - kPresentOffset) % period;
    if (target < 0) target += period;
    int64_t phase = (std::chrono::duration_cast<std::chrono::nanoseconds>(retrace - d.vblank).count()) % period;
    if (phase < 0) phase += period;
    int64_t error = phase - target;
    if (error > period / 2) error -= period;
    if (error < -period / 2) error += period;
    const int64_t correction = std::clamp<int64_t>(-error / 20, -100'000, 100'000);
    return std::chrono::nanoseconds(period * d.k + correction);
}

std::chrono::microseconds IntervalForFormat(uint32_t tvFormat) {
    // NTSC-ish defaults to 60 Hz; PAL uses 50 Hz.
    return tvFormat == 1 ? 20000us : 16666us;
}

// Shared busy-wait budget for deadline-precise sleeps (matches Aurora's
// presenter spin window). Larger windows burn a core for no visible gain.
constexpr std::chrono::microseconds kFinalSpinWindow{500};

void SleepPreciselyUntil(Clock::time_point deadline, bool finishWithSpin = false,
                         std::chrono::microseconds spinWindow = 750us) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return;
    }
    const auto timerDeadline =
        finishWithSpin && deadline - now > spinWindow ? deadline - spinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    struct HighResolutionTimer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_MODIFY_STATE | SYNCHRONIZE);
        ~HighResolutionTimer() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    static thread_local HighResolutionTimer timer;
    if (timer.handle != nullptr) {
        const auto remaining100ns =
            std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timerDeadline - now);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(remaining100ns.count(), 1);
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            WaitForSingleObject(timer.handle, INFINITE);
            if (!finishWithSpin) {
                return;
            }
        }
    }
#endif
    if (!finishWithSpin) {
        std::this_thread::sleep_until(deadline);
        return;
    }
    if (Clock::now() < timerDeadline) {
        std::this_thread::sleep_until(timerDeadline);
    }
    // This runs only for the final fraction of a VI interval. Do not yield the
    // host thread here: a scheduler quantum is larger than the remaining
    // budget and would recreate the 17-18 ms sawtooth this path removes.
    // The pause hint keeps a spin on one SMT thread from starving its sibling core thread (a Steam Deck held the
    // game at 30 FPS until a CPU thread was toggled, issue #43); Linux had a bare spin.
    while (Clock::now() < deadline) {
#if defined(_WIN32)
        YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
}

void ViSetR3(CpuContext* ctx, uint32_t value)
{
    if (ctx) {
        ctx->gpr[3] = value;
    }
}

void WriteGuestStateLocked() {
    try {
        Memory::Write8(kViInitializedFlagAddr, 1);
        Memory::Write8(kViTimingGuardAddr, 1);
        Memory::Write32(kViTvFormatAddr, g_vi.tvFormat);
        Memory::Write16(kViRenderWidthAddr, static_cast<uint16_t>(g_vi.renderWidth));
        Memory::Write16(kViRenderHeightAddr, static_cast<uint16_t>(g_vi.renderHeight));
        Memory::Write16(kViXfbWidthAddr, static_cast<uint16_t>(g_vi.xfbWidth));
        Memory::Write16(kViXfbHeightAddr, static_cast<uint16_t>(g_vi.xfbHeight));
        Memory::Write32(kViRetraceCountAddr, g_vi.retraceCount);
        Memory::Write32(kViPreRetraceCallback, g_vi.preRetraceCallback);
        Memory::Write32(kViPostRetraceCallback, g_vi.postRetraceCallback);
        // Write PENDING frame buffer to guest memory so SDK code sees the queued value
        Memory::Write32(kViNextFrameBufferAddr, g_vi.pendingNextFrameBuffer);
        Memory::Write32(kViNextFrameBufferHwAddr, g_vi.pendingNextFrameBuffer);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_VI, "WriteGuestStateLocked", e);
    }
}

// Fast-forward: the game's clock runs this many times faster (1 = normal).
std::atomic<uint32_t> g_viSpeed{1};

// The interval to the next retrace: the display lock's while V-Sync holds one (NTSC only).
std::chrono::nanoseconds ActiveIntervalLocked() {
    if (const uint32_t speed = g_viSpeed.load(std::memory_order_relaxed); speed > 1) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(g_vi.retraceInterval) / speed;
    }
    if (g_vi.lockedInterval.count() > 0 && g_vi.tvFormat != 1) {
        return g_vi.lockedInterval;
    }
    return g_vi.retraceInterval;
}

uint64_t IntervalTicks(std::chrono::microseconds interval) {
    return TimeBaseContract::NanosecondsToTicks(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count()));
}

void EnsureInitializedLocked() {
    if (g_vi.initialized) {
        return;
    }
    g_vi.initialized = true;
    g_vi.retraceInterval = IntervalForFormat(g_vi.tvFormat);
    g_vi.lastRetrace = Clock::now();
    g_vi.detLastRetraceTicks = DetClock::Now();
    WriteGuestStateLocked();
}

// GXRenderModeObj::viTVmode encodes the output family (0 NTSC, 1 PAL, 2 MPAL,
// 5 EURGB60) in bits [4:2].
uint32_t ExtractTvFormat(uint32_t tvMode) {
    return (tvMode >> 2) & 0x7;
}

// Re-entry guard to prevent AdvanceRetrace calling itself via OSWakeupThread -> SelectThread
static std::atomic<bool> s_inAdvanceRetrace{false};

// Set while VI_HLE_PresentFrame runs its seal/pace/pre-warm sequence. Guest callbacks
// serviced during that window (pace-loop alarms, GX timing polls) still see the stale
// hasValidXfb/g_auroraFrameActive flags; a retrace-context present fired from them would
// end the freshly pre-warmed empty frame and show it as a black frame group.
static std::atomic<bool> s_presentSequenceActive{false};

// NSMBW bring-up: frames the guest actually handed to the presenter (an XFB was ready), and a one-shot
// watchdog that dumps every guest thread's state if none arrived after 12 s and again after 30 s, so
// a boot that stalls in a wait loop shows where each thread is parked.
static std::atomic<uint32_t> s_presentedXfbFrames{0};
extern "C" { extern int g_spmDrawDump; extern int g_spmCullOff; extern int g_spmSweepDump; extern unsigned g_spmDrawIdx; extern unsigned g_spmDrawLimit; }  // aurora command_processor (SPM bring-up diagnostics)
static uint32_t SpmPresentedFramesLocal() { return s_presentedXfbFrames.load(std::memory_order_relaxed); }
static uint32_t (*const g_spmFramesFn)() = &SpmPresentedFramesLocal;

void StartStallWatchdogOnce() {
    if (RuntimeFrontend::Active()) return;
    static std::once_flag once;
    std::call_once(once, [] {
        // Unattended bring-up: autoexit.txt beside the executable holds a run length in seconds.
        if (const auto dir = RuntimeConfigFile::ExecutableDirectory()) {
            std::ifstream in(*dir / "autoexit.txt");
            int seconds = 0;
            if (in >> seconds && seconds > 0) {
                std::thread([seconds] {
                    std::this_thread::sleep_for(std::chrono::seconds(seconds));
                    std::cerr << "[runtime] autoexit after " << seconds << " s" << std::endl;
                    std::_Exit(0);
                }).detach();
            }
        }
        std::thread([] {
            const auto start = std::chrono::steady_clock::now();
            // SPM_WATCHDOG_AT=<s>: one more thread dump then (e.g. to see where a hang sits)
            const char* extra = std::getenv("SPM_WATCHDOG_AT");
            for (const int seconds : {12, 30, extra ? std::atoi(extra) : 0}) {
                if (seconds == 0) break;
                std::this_thread::sleep_until(start + std::chrono::seconds(seconds));
                const uint32_t frames = s_presentedXfbFrames.load(std::memory_order_relaxed);
                const bool black = g_vi.black;  // racy read; diagnostics only
                RT_LOGF(RT_TAG_VI, "[watchdog] %d s: %u guest frame(s) presented, VI black=%d\n", seconds, frames,
                        black ? 1 : 0);
                if (frames != 0 && !black && seconds != 30) {  // SPM bring-up: always dump at 30 s
                    continue;
                }
                RT_LOGF(RT_TAG_VI, "[watchdog] nothing visible yet; guest threads:\n");
                Fiber::GuestFiberManager::DumpGuestThreads(std::cerr);
            }
        }).detach();
    });
}

// NSMBW: report where the game's own loader places its four boot RELs. The translated build was
// pre-linked for fixed heap addresses (projects/nsmbw/tools/prelink_rels.py); a mismatch means the
// guest heap layout differs from hardware and translated REL code would run against wrong data.
// Walks __OSModuleInfoList (0x800030C8) once per retrace until all four have been reported.
void LogGuestModulesOnce() {
    struct ExpectedModule { uint32_t id; const char* name; uint32_t image; uint32_t bss; };
    static constexpr ExpectedModule kExpected[] = {
        {1, "relF", 0x80BF6840u, 0x80F12640u},  // SPM: projects/spm/tools/prelink_spm_rel.py
    };
    static uint32_t s_reported = 0;  // one bit per module id
    if (s_reported == 0x2u) {
        return;
    }
    uint32_t module = 0;
    if (!Memory::TryRead32(0x800030C8u, module)) {
        return;
    }
    for (int guard = 0; module != 0 && guard < 16; ++guard) {
        uint32_t id = 0, next = 0, sectionInfo = 0, sectionBytes = 0, bss = 0;
        if (!Memory::TryRead32(module + 0x00u, id) || !Memory::TryRead32(module + 0x04u, next) ||
            !Memory::TryRead32(module + 0x10u, sectionInfo) || !Memory::TryRead32(module + 0x30u, sectionBytes)) {
            return;
        }
        const uint32_t bssSection = sectionBytes & 0xFFu;  // OSModuleHeader.bssSection (+0x33)
        if (bssSection != 0) {
            Memory::TryRead32(sectionInfo + bssSection * 8u, bss);  // absolute after OSLink
        }
        if (id < 32 && (s_reported & (1u << id)) == 0) {
            s_reported |= 1u << id;
            const ExpectedModule* expected = nullptr;
            for (const auto& candidate : kExpected) {
                if (candidate.id == id) {
                    expected = &candidate;
                }
            }
            const bool match = expected != nullptr && expected->image == module &&
                               (bss == 0 || (bss & ~1u) == expected->bss);
            RT_LOGF(RT_TAG_OS, "REL module %u (%s) linked at image 0x%08X bss 0x%08X: %s\n", id,
                    expected ? expected->name : "unexpected", module, bss,
                    match ? "MATCH" : "MISMATCH vs the pre-linked layout");
        }
        module = next;
    }
}

// NSMBW: log every scene switch (boot, title, world map, stage, ...) with the game's own profile name,
// so automated bring-up runs show how far the game got without screenshots. dScene_c::m_nextScene and
// m_nowScene are adjacent halfwords at 0x80428730; dProf_getName reads names from a pointer table.
void LogSceneChanges() {
    // SPM: seqdrv now_seq / next_seq (.sdata, r13-0x7E48 / -0x7E44) and seqWork.stage (0x804CF364).
    static constexpr const char* kSeqNames[] = {"logo", "title", "game", "mapchange", "gameover", "load"};
    static uint32_t s_last = 0xFFFFFFFFu;
    uint32_t now = 0, next = 0, stage = 0;
    if (!Memory::TryRead32(0x8056D0D8u, now) || !Memory::TryRead32(0x8056D0DCu, next) ||
        !Memory::TryRead32(0x804CF364u, stage)) {
        return;
    }
    const uint32_t key = (now & 0xFFu) << 16 | (next & 0xFFu) << 8 | (stage & 0xFFu);
    if (key == s_last) {
        return;
    }
    s_last = key;
    const auto name = [](uint32_t v) { return v < 6 ? kSeqNames[v] : "none"; };
    RT_LOGF(RT_TAG_VI, "seq -> %s (next %s) stage %u (frame %u)\n", name(now), name(next), stage,
            s_presentedXfbFrames.load(std::memory_order_relaxed));
}

void AdvanceRetrace(CpuContext* ctx, Clock::time_point retraceStamp, bool serviceAurora) {
    // Prevent re-entry - this can happen if OSWakeupThread triggers SelectThread
    // which goes idle and calls ProcessTimerEvents again
    if (s_inAdvanceRetrace.exchange(true)) {
        return;
    }
    StartStallWatchdogOnce();
    LogGuestModulesOnce();
    LogSceneChanges();
    SpmMapSweep::Tick(ctx, s_presentedXfbFrames.load(std::memory_order_relaxed));

    uint32_t preCb = 0;
    uint32_t postCb = 0;
    uint32_t retraceValue = 0;
    bool hasXfbReady = false;
    uint32_t readyXfb = 0;
    uint32_t currentFb = 0;
    bool isBlack = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        
        // Commit pending state if VIFlush armed it (see ViState).
        if (g_vi.flushArmed) {
            // Commit pending -> active
            g_vi.nextFrameBuffer = g_vi.pendingNextFrameBuffer;
            g_vi.black = g_vi.pendingBlack;
            g_vi.tvFormat = g_vi.pendingTvFormat;
            g_vi.renderWidth = g_vi.pendingRenderWidth;
            g_vi.renderHeight = g_vi.pendingRenderHeight;
            g_vi.viXOrigin = g_vi.pendingViXOrigin;
            g_vi.viYOrigin = g_vi.pendingViYOrigin;
            g_vi.xfbWidth = g_vi.pendingXfbWidth;
            g_vi.xfbHeight = g_vi.pendingXfbHeight;
            g_vi.retraceInterval = g_vi.pendingRetraceInterval;
            
            // Clear flush armed flag
            g_vi.flushArmed = false;
            
        }
        
        g_vi.retraceCount++;
        g_vi.fieldOdd = !g_vi.fieldOdd;
        g_vi.currentFrameBuffer = g_vi.nextFrameBuffer;
        currentFb = g_vi.currentFrameBuffer;
        g_vi.lastRetrace = retraceStamp;
        RefreshDisplayLockLocked(Clock::now());
        g_vi.lockedInterval = LockedIntervalAfter(retraceStamp);
        retraceValue = g_vi.retraceCount;
        preCb = g_vi.preRetraceCallback;
        postCb = g_vi.postRetraceCallback;
        hasXfbReady = g_vi.hasValidXfb;
        readyXfb = g_vi.readyXfb;
        isBlack = g_vi.black;
        WriteGuestStateLocked();
    }

    // Deterministic mode: this retrace starts a frame. Fingerprint guest memory as the previous
    // frame left it, then latch every channel's input for the new one before any guest code reads it.
    if (DetClock::Enabled()) {
        DetHash::OnRetrace(retraceValue);
        // A libretro frontend latches when it resumes the guest with that frame's input instead
        // (AdvanceOneFrame); latching here would hand the game the previous frame's buttons.
        if (!RuntimeFrontend::Active()) FrameInput::Latch(retraceValue);
    }

    // Wake up threads sleeping on the VI retrace queue (VIWaitForRetrace).
    // The retrace count has been incremented and written to guest memory.
    if (ctx) {
        // Bring-up trace: the first wakeups show whether a VIWaitForRetrace sleeper was queued and
        // whether waking it made the OS see a runnable thread.
        static int s_wakeLogs = 0;
        const bool logWake = s_wakeLogs < 6;
        uint32_t queueHead = 0;
        if (logWake) {
            Memory::TryRead32(kViRetraceQueueAddr, queueHead);
        }
        ctx->gpr[3] = kViRetraceQueueAddr;
        OSWakeupThread_HLE_801aaaa4(ctx);
        if (logWake) {
            ++s_wakeLogs;
            uint32_t runQueueBits = 0;
            Memory::TryRead32(0x8056DBB0u, runQueueBits);  // RunQueueBits (NSMBW)
            RT_LOGF(RT_TAG_VI, "[retrace %u] woke retrace queue (head 0x%08X) -> RunQueueBits 0x%08X\n",
                    retraceValue, queueHead, runQueueBits);
        }
    }

    if (serviceAurora) {
        // Process window events (but don't present - that happens in GXCopyDisp)
        UpdateAuroraAndProcessEvents();

        // Start a new Aurora frame if one isn't already active
        if (!g_auroraFrameActive.load(std::memory_order_acquire)) {
            if (BeginAuroraFrame()) {
                g_auroraFrameActive.store(true, std::memory_order_release);
            }
        }
    }

    if (ctx) {
        ctx->gpr[3] = retraceValue;
        if (preCb) {
            InvokeIndirectCpu(preCb, ctx);
        }
        if (postCb) {
            // Mario Kart Wii gated this on EGG::BaseSystem::sSystem being non-null because its
            // callback dereferenced it during early boot. NSMBW has no BaseSystem (its EGG is the
            // older TSystem<...>), and none of its direct VISetPostRetraceCallback calls install a
            // callback, so follow the SDK: call whatever callback is set.
            InvokeIndirectCpu(postCb, ctx);
        }
    }

    // VISetBlack(TRUE) keeps frame submission running but shows only the clear color: GX render work is
    // skipped and Aurora's end_frame() clears to black, matching the hardware manual's "signal continues,
    // pixels go black" behavior. When not black, submission waits for hasXfbReady (GXCopyDisp done).
    if (serviceAurora && !s_presentSequenceActive.load(std::memory_order_acquire)) {
        const bool frameActive = g_auroraFrameActive.load(std::memory_order_acquire);
        const bool xfbMatches = (readyXfb != 0 && readyXfb == currentFb);
        const bool shouldPresentXfb = hasXfbReady && !isBlack && xfbMatches;
        const bool shouldPresentBlack = isBlack && frameActive;
        const bool shouldSubmit = frameActive && (shouldPresentXfb || shouldPresentBlack);

        if (shouldSubmit) {
            if (!RuntimeFrontend::Active() && (!isBlack || settings_overlay::StartupScreenVisible())) {
                // Normal presentation: draw overlay on top of GX content
                settings_overlay::Draw();
            }
            // Outside startup, VI black remains a pure black presentation.
            // Unpaced: this present already runs in retrace context.
            VI_HLE_PresentFrame(shouldPresentXfb, false);
        } else if (g_auroraFrameHadWork.load(std::memory_order_acquire) && !shouldPresentXfb && !isBlack) {
            // GX work is in progress but frame not complete - just poll window events
            // Don't call aurora_end_frame() as that would present incomplete work
            UpdateAuroraAndProcessEvents();
        }
    }

    // Clear re-entry guard
    s_inAdvanceRetrace.store(false);
    // A libretro frontend's frame ends here; the scheduler's idle loop suspends right after its
    // idle step (RuntimeFrontend::SuspendIfRequested), a resume point a save state can rebuild.
    RuntimeFrontend::RequestSuspendAtVI();
}

bool AdvanceDueRetraces(CpuContext* ctx, int maxToProcess, bool serviceAurora)
{
    bool advancedAny = false;

    if (DetClock::Enabled()) {
        for (int catchUpCount = 0; catchUpCount < maxToProcess; ++catchUpCount) {
            {
                std::lock_guard<std::mutex> lock(g_viMutex);
                if (!g_vi.initialized) {
                    return advancedAny;
                }
                const uint64_t due = g_vi.detLastRetraceTicks + IntervalTicks(g_vi.retraceInterval);
                if (DetClock::Now() < due) {
                    return advancedAny;
                }
                g_vi.detLastRetraceTicks = due;
            }
            AdvanceRetrace(ctx, Clock::now(), serviceAurora);
            advancedAny = true;
            if (RuntimeFrontend::SuspendRequested()) break;  // one retrace per libretro frame
        }
        return advancedAny;
    }

    for (int catchUpCount = 0; catchUpCount < maxToProcess; ++catchUpCount) {
        Clock::time_point target;
        auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            if (!g_vi.initialized) {
                return advancedAny;
            }
            target = g_vi.lastRetrace + ActiveIntervalLocked();
            if (now < target) {
                return advancedAny;
            }
        }


        AdvanceRetrace(ctx, target, serviceAurora);
        advancedAny = true;
    }

    return advancedAny;
}

} // namespace

// Force one retrace boundary to pass, whether or not its wall-clock deadline
// has arrived, so the guest's retrace callbacks (AsyncDisplay's counters and
// friends) run. VI_HLE_PollRetrace below is the time-driven counterpart.
void VI_HLE_ForceRetrace(CpuContext* ctx) {
    Clock::time_point target;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        target = g_vi.lastRetrace + ActiveIntervalLocked();
    }
    AdvanceRetrace(ctx, target, true);
}

void VI_HLE_SetSpeed(uint32_t speed) {
    g_viSpeed.store(speed < 1 ? 1 : speed, std::memory_order_relaxed);
}

uint32_t VI_HLE_Speed() {
    return g_viSpeed.load(std::memory_order_relaxed);
}

bool VI_HLE_IsAdvancingRetrace() {
    return s_inAdvanceRetrace.load(std::memory_order_acquire);
}

// Advance every retrace whose interval has already elapsed. Safe to call from
// busy loops (GX drawing, the scheduler's idle spin) to keep VBlank ticking.
void VI_HLE_PollRetrace(CpuContext* ctx) {
    AdvanceDueRetraces(ctx, 8, true);
}

void VI_HLE_ProcessRetracesDeferred(int maxToProcess) {
    // A wall-clock pump (GX work, host frame waits); deterministic mode delivers retraces only from
    // the scheduler and the guest's own VI calls.
    if (maxToProcess <= 0 || !OS_HLE_InterruptsEnabled() || DetClock::Enabled()) {
        return;
    }

    // This entry point runs from the middle of an arbitrary translated function
    // (GX__Begin's timing service, the host frame loop). Retrace callbacks are
    // an interrupt from that function's point of view, so they get a private
    // copy of its register file exactly as the hardware interrupt would.
    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    // Host renderer ownership is not a guest critical section. Deliver due VI
    // callbacks at wall-clock cadence while suppressing thread switches and
    // all recursive Aurora/event work until the native GX call has unwound.
    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        AdvanceDueRetraces(cpu, maxToProcess, false);
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}

uint64_t VI_HLE_DetNextRetraceTicks() {
    std::lock_guard<std::mutex> lock(g_viMutex);
    if (!g_vi.initialized) {
        return DetClock::kNever;
    }
    return g_vi.detLastRetraceTicks + IntervalTicks(g_vi.retraceInterval);
}

void VI_HLE_WaitForNextRetracePoll() {
    Clock::time_point retraceDeadline;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        retraceDeadline = g_vi.lastRetrace + ActiveIntervalLocked();
    }

    const auto now = Clock::now();
    if (now >= retraceDeadline) {
        return;
    }
    // Audio DMA and alarm queues still need regular service even when the VI
    // deadline is farther away. The high-resolution wait removes the 1 ms
    // scheduler overshoot when retrace is the next event.
    const bool retraceIsNext = retraceDeadline <= now + 1ms;
    SleepPreciselyUntil(retraceIsNext ? retraceDeadline : now + 1ms, retraceIsNext,
                        kFinalSpinWindow);
}

namespace {

// Retrace count consumed by the most recent paced present. This is a slot memo
// against the VI timeline, not a second clock: it only answers "did a retrace
// already elapse while this frame was being produced?". Producer-thread only.
uint32_t s_lastPacedRetraceCount = ~0u;

// Last presentation anchor handed to Aurora, in nanoseconds on the VI retrace
// grid. Guarantees consecutive sealed frames never share an anchor (see the
// comment at the stamping site). Producer-thread only, like the memo above.
uint64_t s_lastPresentAnchorNanos = 0;

// Sleeps to the same VI retrace boundary VIWaitForRetrace targets, servicing alarms every 1 ms so audio
// DMA and timers keep running, then delivers that retrace so guest logic starts exactly on the grid.
void PaceToRetraceBoundary(Clock::time_point deadline) {
    constexpr auto kServiceSlice = 1ms;
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        if (deadline - now > kServiceSlice + kFinalSpinWindow) {
            SleepPreciselyUntil(now + kServiceSlice);
            OS_HLE_ProcessAlarmsDeferred(8);
            // Audio DMA is a 3 ms cadence and this wait is up to a full display
            // period long. Without a pump here the blocks that came due during
            // the wait are all delivered at once when the guest next reaches
            // idle, which the guest observes as AI slack jitter.
            Audio_HLE_PollDeferred();
            continue;
        }
        SleepPreciselyUntil(deadline, true, kFinalSpinWindow);
        break;
    }
    VI_HLE_ProcessRetracesDeferred(1);
}

} // namespace

// Single owner of the Aurora frame presentation sequence: seals the active frame, optionally paces the
// producer to the VI retrace boundary, and pre-warms the next frame. Paced from GXCopyDisp; unpaced for
// the retrace-context black/boot present path in AdvanceRetrace.
// NSMBW_LOG_FPS (debug.nsmbw.logfps on Android): every two seconds, the frames presented and where
// each frame's time went: the game (guest code and GX command building), aurora_end_frame (encoding,
// submission, and waiting on the GPU when it falls behind) and pacing to the retrace. A game share near
// the frame budget is CPU-bound; a large end_frame share is GPU-bound.
struct FrameTimes {
    Clock::time_point windowStart{};
    Clock::time_point lastPresentEnd{};
    double gameMs = 0, endFrameMs = 0, paceMs = 0, worstMs = 0, sumSq = 0;
    int slow = 0;
    int frames = 0;
};

static bool LogFpsEnabled() {
    static const bool on = std::getenv("NSMBW_LOG_FPS") != nullptr;
    return on;
}

// dvd.cpp: time in the game's disc reads since start, and their count.
uint64_t DvdReadNanosTotal();
uint32_t DvdReadCount();

void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace) {
    if (s_presentSequenceActive.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // Frontend frame timing: encode/submit cost, reported apart from guest CPU time.
    struct PresentTimer {
        Clock::time_point start = Clock::now();
        ~PresentTimer() {
            if (RuntimeFrontend::Active())
                RuntimeFrontend::AddPresentTime(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        }
    } presentTimer;
    GuestProfiler::OnFramePresented();
    static FrameTimes times;
    const Clock::time_point presentStart = Clock::now();
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    struct SequenceGuard {
        ~SequenceGuard() { s_presentSequenceActive.store(false, std::memory_order_release); }
    } sequenceGuard;
    if (presentedXfb) {
        {
            const uint32_t pf = s_presentedXfbFrames.fetch_add(1, std::memory_order_relaxed) + 1;
            static const int32_t dumpTarget = [] { std::ifstream in("drawdump.txt"); int32_t f = -1; in >> f; return f; }();
            static const bool cullTest = std::ifstream("depth_test.txt").good();
            g_spmDrawDump = ((dumpTarget >= 0 && pf == static_cast<uint32_t>(dumpTarget)) || g_spmSweepDump) ? 1 : 0;
            static const int envTest = [] { const char* e = std::getenv("SPM_CULL_TEST"); return e ? std::atoi(e) : 0; }();  // 1 = no culling, 2 = no depth test (diagnostic)
            static const unsigned drawLimit = [] { const char* e = std::getenv("SPM_DRAW_LIMIT"); return e ? static_cast<unsigned>(std::atoi(e)) : ~0u; }();
            g_spmDrawLimit = drawLimit;
            g_spmDrawIdx = 0;
            g_spmCullOff = envTest ? envTest : ((cullTest && pf >= 6900u) ? 2 : 0);
        }
        // SPM bring-up: with drawdump.txt present, report any guest write into the DOL's
        // read-only sections (.rodata/.sdata2) once the game has been running a while.
        if (s_presentedXfbFrames.load(std::memory_order_relaxed) == 3000u && std::ifstream("drawdump.txt").good()) {
            std::ifstream dol(RuntimeConfigFile::ResolvedDvdRoot() / "sys" / "main.dol", std::ios::binary);
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(dol)), {});
            auto be = [&](size_t o) { return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3]; };
            for (int sec = 7; d.size() > 0xE4 && sec < 18; ++sec) {
                const uint32_t off = be(sec * 4), addr = be(0x48 + sec * 4), size = be(0x90 + sec * 4);
                if (addr != 0x80319200u && addr != 0x8056E280u) continue;  // .rodata, .sdata2
                int reported = 0;
                for (uint32_t i = 0; i + 4 <= size && off + i + 4 <= d.size() && reported < 40; i += 4) {
                    uint32_t v = 0;
                    if (Memory::TryRead32(addr + i, v) && v != be(off + i)) {
                        RT_LOGF(RT_TAG_VI, "[rocheck] 0x%08X dol %08X mem %08X\n", addr + i, be(off + i), v);
                        ++reported;
                    }
                }
                RT_LOGF(RT_TAG_VI, "[rocheck] section 0x%08X checked, %d diffs shown\n", addr, reported);
            }
        }
        // NSMBW: lift the runtime's startup cover at the game's first visible frame. Mario Kart's
        // runtime lifted it from a StrapScene::CheckInput hook that auto-accepted its strap screen;
        // NSMBW shows its own Wii Strap screen instead, and the player presses A as on a console.
        static std::once_flag firstVisibleFrame;
        if (!g_vi.black) {
            std::call_once(firstVisibleFrame, [] {
                // The cover now stays up until the player presses A or 2 (hle/input/kpad.cpp).
                RT_LOGF(RT_TAG_VI, "first visible guest frame presented; waiting for A or 2 to lift the startup cover\n");
            });
        }
    }
    Clock::time_point paceDeadline{};
    bool paceThisFrame = false;
    // Deterministic mode paces in the scheduler's idle wait instead, and must not deliver the
    // retrace from here (PaceToRetraceBoundary does), so frames present as soon as they are sealed.
    if (DetClock::Enabled() || RuntimeFrontend::Active()) {
        paceToRetrace = false;
    }
    if (paceToRetrace) {
        uint64_t baseNanos = 0;
        uint64_t intervalNanos = 0;
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            paceDeadline = g_vi.lastRetrace + ActiveIntervalLocked();
            retraceCount = g_vi.retraceCount;
            baseNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g_vi.lastRetrace.time_since_epoch())
                    .count());
            intervalNanos = static_cast<uint64_t>(
                ActiveIntervalLocked().count());
        }
        // Hold the frame to its boundary only when no retrace elapsed during
        // its production. A frame that missed its boundary presents
        // immediately: hardware would quantize down to the next vblank here,
        // but free-running late frames matches the previous pacer and keeps a
        // heavy scene at e.g. 50 fps instead of hard 30.
        const uint32_t retracesElapsed = retraceCount - s_lastPacedRetraceCount;
        paceThisFrame = retracesElapsed == 0;
        s_lastPacedRetraceCount = retraceCount;
        // aurora_report_producer_paced needs a different signal than the pace-wait above: the guest
        // self-paces via VIWaitForRetrace, so one retrace per produced frame is the healthy locked-60
        // cadence, and zero only happens when production outruns VI. "Kept up" means <=1 retrace
        // elapsed; 2+ means a boundary was missed, so Aurora seals that frame without its interpolated
        // slots (a windowed backstop lowers the slot target only under sustained overload).
        aurora_report_producer_paced(retracesElapsed <= 1);
        // Stamp the sealed frame's presentation schedule so Aurora paces interpolated slots against
        // this same VI timeline. Anchor to the NEXT retrace boundary, not the period just produced,
        // since slots anchored to the current period would already be expired by seal time. Encoding
        // overruns are corrected by sliding the whole slot group forward onto a later boundary of this
        // same grid, so this stays the single cadence authority. Anchors must also be strictly
        // monotonic: two frames sealed before lastRetrace advances would collide on one boundary and
        // burst-present, so a colliding anchor steps onto the next grid point instead of repeating it.
        uint64_t anchorNanos = baseNanos + intervalNanos;
        if (s_lastPresentAnchorNanos != 0 && anchorNanos <= s_lastPresentAnchorNanos) {
            anchorNanos = s_lastPresentAnchorNanos + intervalNanos;
        }
        s_lastPresentAnchorNanos = anchorNanos;
        aurora_set_present_schedule(anchorNanos, intervalNanos);
    } else {
        // Retrace-context presents (VI black, boot) have no display period of
        // their own to subdivide; present as soon as the frame is ready. The
        // schedule grid is gone, so the anchor cursor must not constrain the
        // next paced frame.
        s_lastPresentAnchorNanos = 0;
        aurora_set_present_schedule(0, 0);
    }

    const Clock::time_point endFrameStart = Clock::now();
    aurora_end_frame();
    const Clock::time_point paceStart = Clock::now();
    if (paceThisFrame) {
        PaceToRetraceBoundary(paceDeadline);
        std::lock_guard<std::mutex> lock(g_viMutex);
        s_lastPacedRetraceCount = g_vi.retraceCount;
    }
    // Always on: a frame that took over 20 ms (a dropped frame at 60) goes to the log with where the
    // time went, so a report of drops on someone's machine comes with its cause. At most 30 a minute.
    if (presentedXfb) {
        static Clock::time_point s_lastEnd{}, s_windowStart{};
        static int s_logged = 0;
        static uint64_t s_lastGx = 0, s_lastDvd = 0;
        static uint32_t s_lastDvdReads = 0;
        const uint64_t gxTotal = AuroraGxProcessNanosTotal();
        const double gxMs = (gxTotal - s_lastGx) / 1e6;
        s_lastGx = gxTotal;
        const uint64_t dvdTotal = DvdReadNanosTotal();
        const uint32_t dvdReads = DvdReadCount();
        const double dvdMs = (dvdTotal - s_lastDvd) / 1e6;
        const uint32_t frameReads = dvdReads - s_lastDvdReads;
        s_lastDvd = dvdTotal;
        s_lastDvdReads = dvdReads;
        const Clock::time_point now = Clock::now();
        if (s_lastEnd != Clock::time_point{}) {
            const double frame = ms(now - s_lastEnd);
            if (now - s_windowStart > std::chrono::minutes(1)) {
                s_windowStart = now;
                s_logged = 0;
            }
            if (frame > 20.0 && frame < 2000.0 && s_logged++ < 30) {
                uint32_t sceneWord = 0;
                Memory::TryRead32(0x80428730u, sceneWord);
                RT_LOGF(RT_TAG_VI,
                        "slow frame %.1f ms (scene 0x%03X): game %.2f (gx %.2f, disc %.2f in %u reads), present %.2f, "
                        "end_frame %.2f, pace %.2f\n",
                        frame, sceneWord & 0xFFFFu, ms(presentStart - s_lastEnd), gxMs, dvdMs, frameReads,
                        ms(endFrameStart - presentStart),
                        ms(paceStart - endFrameStart), ms(now - paceStart));
            }
        }
        s_lastEnd = now;
    }
    if (LogFpsEnabled() && presentedXfb) {
        const Clock::time_point now = Clock::now();
        if (times.lastPresentEnd != Clock::time_point{}) {
            const double game = ms(presentStart - times.lastPresentEnd) + ms(endFrameStart - presentStart);
            const double frame = ms(now - times.lastPresentEnd);
            times.gameMs += game;
            times.endFrameMs += ms(paceStart - endFrameStart);
            times.paceMs += ms(now - paceStart);
            times.worstMs = std::max(times.worstMs, frame);
            times.sumSq += frame * frame;
            times.slow += frame > 20.0 ? 1 : 0;
            ++times.frames;
        } else {
            times.windowStart = now;
        }
        times.lastPresentEnd = now;
        const double window = ms(now - times.windowStart);
        if (window >= 2000.0 && times.frames > 0) {
            const double n = times.frames;
            uint64_t gxNanos = 0, lockNanos = 0, draws = 0;
            AuroraTakeGxStats(&gxNanos, &lockNanos, &draws);
            uint64_t copies = 0, creates = 0;
            AuroraTakeCopyStats(&copies, &creates);
            RT_LOGF(RT_TAG_VI,
                    "fps %.1f: game %.2f ms (gx %.2f ms, lock wait %.2f ms, %.0f draws, %.1f copies, %.1f new copy "
                    "textures), end_frame %.2f ms, pace %.2f ms, worst frame %.1f ms, jitter %.2f ms, %d over 20 ms\n",
                    n * 1000.0 / window, times.gameMs / n, gxNanos / 1e6 / n, lockNanos / 1e6 / n, draws / n,
                    copies / n, creates / n, times.endFrameMs / n, times.paceMs / n, times.worstMs,
                    std::sqrt(std::max(0.0, times.sumSq / n - (window / n) * (window / n))), times.slow);
            if (aurora_get_frame_interpolation_fps() != 0) {  // what the screen shows vs the 60 Hz game
                AuroraPresentTiming present{};
                aurora_get_present_timing(&present);
                AuroraFrameInterpolationDiagnostics fi{};
                aurora_get_frame_interpolation_diagnostics(&fi);
                RT_LOGF(RT_TAG_VI, "interp: displayed %.1f fps (motion %.1f), target %u, slots %u/%u, matched %u/%u of %u draws, "
                        "eligible %u, low-match %llu/%llu frames, reductions %llu, late drops %llu\n",
                        present.framesPerSecond, present.effectiveFramesPerSecond, fi.targetFps, fi.activeSamples,
                        fi.targetSamples, fi.matches, fi.matchable, fi.candidates, fi.eligible,
                        (unsigned long long)fi.framesLowMatch, (unsigned long long)fi.framesSealed,
                        (unsigned long long)fi.slotReductions, (unsigned long long)fi.lateSealDrops);
            }
            times = FrameTimes{};
            times.windowStart = now;
            times.lastPresentEnd = now;
        }
    }
    if (!RuntimeFrontend::Active()) settings_overlay::AdvancePresentedFrame();
    g_auroraFrameActive.store(false, std::memory_order_release);
    g_auroraFrameHadWork.store(false, std::memory_order_release);
    if (presentedXfb) {
        std::lock_guard<std::mutex> lock(g_viMutex);
        g_vi.hasValidXfb = false;
        g_vi.readyXfb = 0;
    }
    // Pre-warm the next frame so subsequent GX work has a valid frame context.
    {
        UpdateAuroraAndProcessEvents();
        if (BeginAuroraFrame()) {
            g_auroraFrameActive.store(true, std::memory_order_release);
        }
    }
}

// -----------------------------------------------------------------------------
// VI_HLE_SetXfbReady - Called by GXCopyDisp to signal EFB->XFB copy completed.
// This marks that we now have valid framebuffer data to present.
// -----------------------------------------------------------------------------
void VI_HLE_SetXfbReady(uint32_t xfbAddr) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    g_vi.hasValidXfb = true;
    g_vi.readyXfb = xfbAddr;
    if (g_vi.currentFrameBuffer == 0 && g_vi.nextFrameBuffer == 0) {
        g_vi.currentFrameBuffer = xfbAddr;
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
        WriteGuestStateLocked();
    } else if (g_vi.nextFrameBuffer != xfbAddr) {
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
    }
}

// VIInit (0x801BC7A0) and its lower-level helper __VIInit (0x801BC5A0) both
// program MMIO at 0xCC0020xx on hardware. We skip all hardware access and seed
// the same defaults instead, so the two entry points share one body.
static void SeedViStateForInit(CpuContext* ctx, const char* who)
{
    RT_LOG(RT_TAG_VI) << who << " called: seeding VI state (HLE)" << std::endl;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
    }
    ViSetR3(ctx, 0);
}

extern "C" void VIInit_HLE_801b94a4(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "VIInit_801b94a4");
}
PPC_NATIVE_OVERRIDE_VOID(802808C4, VIInit_HLE_801b94a4, (CpuContext* ctx), (ctx));

extern "C" void __VIInit_HLE_801b9294(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "__VIInit_801b9294");
}
PPC_NATIVE_OVERRIDE_VOID(802806B4, __VIInit_HLE_801b9294, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// Helper stubs referenced by VIInit switch cases (case D variants).
// These are hardware-specific; treat as no-ops to keep control flow intact.
// -----------------------------------------------------------------------------
extern "C" void VIInit_caseD_0_HLE_801b9934(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_0_801b9934 stubbed" << std::endl;
}
#if 0  // NSMBW: MKW switch-case label; its function is native in NSMBW (was MKW-only)
#if 0  // SPM: 0x801b9934: no NSMBW symbol at this address
PPC_NATIVE_OVERRIDE_VOID(801B9934, VIInit_caseD_0_HLE_801b9934, (CpuContext* ctx), (ctx));
#endif
#endif

extern "C" void VIInit_caseD_1_HLE_801b993c(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_1_801b993c stubbed" << std::endl;
}
#if 0  // NSMBW: MKW switch-case label; its function is native in NSMBW (was MKW-only)
#if 0  // SPM: 0x801b993c: no NSMBW symbol at this address
PPC_NATIVE_OVERRIDE_VOID(801B993C, VIInit_caseD_1_HLE_801b993c, (CpuContext* ctx), (ctx));
#endif
#endif

extern "C" void VIInit_caseD_2_HLE_801b9944(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_2_801b9944 stubbed" << std::endl;
}
#if 0  // NSMBW: MKW switch-case label; its function is native in NSMBW (was MKW-only)
#if 0  // SPM: 0x801b9944: no NSMBW symbol at this address
PPC_NATIVE_OVERRIDE_VOID(801B9944, VIInit_caseD_2_HLE_801b9944, (CpuContext* ctx), (ctx));
#endif
#endif

// -----------------------------------------------------------------------------
// VISetPreRetraceCallback (0x801BC3E0)
// -----------------------------------------------------------------------------
extern "C" void VISetPreRetraceCallback_HLE_801b90f4(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.preRetraceCallback;
        g_vi.preRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(80280524, VISetPreRetraceCallback_HLE_801b90f4, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPostRetraceCallback (0x801BC430)
// -----------------------------------------------------------------------------
extern "C" void VISetPostRetraceCallback_HLE_801b9138(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.postRetraceCallback;
        g_vi.postRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(80280568, VISetPostRetraceCallback_HLE_801b9138, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIGetDTVStatus (0x801BE070)
// Reads DTV status from VI hardware (MMIO 0xCC00206E). Stub to "not ready".
// -----------------------------------------------------------------------------
extern "C" void VIGetDTVStatus_HLE_801bad38(CpuContext* ctx)
{
    ViSetR3(ctx, 0); // return 0 -> not ready / disabled
}
PPC_NATIVE_OVERRIDE_VOID(8028217C, VIGetDTVStatus_HLE_801bad38, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIConfigure & related helpers: translate GXRenderModeObj into guest globals.
// -----------------------------------------------------------------------------
extern "C" void VIConfigure_HLE_801b9f6c(CpuContext* ctx)
{
    // One read of the guest GXRenderModeObj serves both the VI pending state and
    // aurora, rather than unpacking the same 0x39 bytes twice.
    const uint32_t renderModePtr = ctx ? ctx->gpr[3] : 0;
    GXRenderModeObj renderMode{};
    if (!ReadGuestRenderModeObj(renderModePtr, renderMode)) {
        RT_LOG(RT_TAG_VI) << "VIConfigure: invalid GXRenderModeObj pointer 0x"
                  << std::hex << renderModePtr << std::dec << std::endl;
        ViSetR3(ctx, 0);
        return;
    }

    const uint32_t decodedTvFormat =
        ExtractTvFormat(static_cast<uint32_t>(renderMode.viTVmode));
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingTvFormat = decodedTvFormat;
        g_vi.pendingRetraceInterval = IntervalForFormat(g_vi.pendingTvFormat);
        g_vi.pendingRenderWidth =
            renderMode.viWidth != 0 ? renderMode.viWidth : renderMode.fbWidth;
        g_vi.pendingRenderHeight =
            renderMode.viHeight != 0 ? renderMode.viHeight : renderMode.xfbHeight;
        g_vi.pendingViXOrigin = renderMode.viXOrigin;
        g_vi.pendingViYOrigin = renderMode.viYOrigin;
        g_vi.pendingXfbWidth = renderMode.fbWidth;
        g_vi.pendingXfbHeight =
            renderMode.xfbHeight != 0 ? renderMode.xfbHeight : renderMode.efbHeight;
    }

    ::VIConfigure(&renderMode);

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(802812A8, VIConfigure_HLE_801b9f6c, (CpuContext* ctx), (ctx));

extern "C" void VIFlush_HLE_801ba9a4(CpuContext* ctx)
{
    uint32_t guestNextFb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        if (g_vi.pendingNextFrameBuffer == 0) {
            try {
                guestNextFb = Memory::Read32(kViNextFrameBufferAddr);
            } catch (const Memory::AccessViolation&) {
                guestNextFb = 0;
            }
            if (guestNextFb == 0) {
                try {
                    guestNextFb = Memory::Read32(kViNextFrameBufferHwAddr);
                } catch (const Memory::AccessViolation&) {
                    guestNextFb = 0;
                }
            }
            if (guestNextFb != 0) {
                g_vi.pendingNextFrameBuffer = guestNextFb;
            }
        }
        
        // Arm only; the commit happens at the next retrace (see ViState).
        g_vi.flushArmed = true;
    }

    // NOTE: We do NOT set hasValidXfb here. Frame readiness is signaled ONLY by
    // GXCopyDisp (which sets hasValidXfb = true), as that's when the EFB->XFB
    // copy is complete and we have a valid frame to present.

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(80281D90, VIFlush_HLE_801ba9a4, (CpuContext* ctx), (ctx));

extern "C" void VISetNextFrameBuffer_HLE_801baab8(CpuContext* ctx)
{
    const uint32_t fbPtr = ctx ? ctx->gpr[3] : 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingNextFrameBuffer = fbPtr;
        // Also update guest memory for SDK code that reads this directly
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(80281EA4, VISetNextFrameBuffer_HLE_801baab8, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextFrameBuffer_HLE_801bab24(CpuContext* ctx)
{
    uint32_t fb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Return PENDING value - what was set by VISetNextFrameBuffer
        fb = g_vi.pendingNextFrameBuffer;
    }
    ViSetR3(ctx, fb);
    VI_HLE_PollRetrace(ctx);
}
#if 0  // NSMBW: VIGetNextFrameBuffer is not linked into NSMBW (was MKW-only)
#if 0  // SPM: 0x801bab24: no NSMBW symbol at this address
PPC_NATIVE_OVERRIDE_VOID(801BAB24, VIGetNextFrameBuffer_HLE_801bab24, (CpuContext* ctx), (ctx));
#endif
#endif

extern "C" void VISetBlack_HLE_801bab2c(CpuContext* ctx)
{
    const bool makeBlack = ctx ? (ctx->gpr[3] != 0) : false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingBlack = makeBlack;
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(80281F10, VISetBlack_HLE_801bab2c, (CpuContext* ctx), (ctx));

extern "C" void VIGetRetraceCount_HLE_801baba4(CpuContext* ctx)
{
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        count = g_vi.retraceCount;
    }
    ViSetR3(ctx, count);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(80281F88, VIGetRetraceCount_HLE_801baba4, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextField_HLE_801babac(CpuContext* ctx)
{
    bool fieldOdd = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        fieldOdd = g_vi.fieldOdd;
    }
    ViSetR3(ctx, fieldOdd ? 1 : 0);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(80281F90, VIGetNextField_HLE_801babac, (CpuContext* ctx), (ctx));

extern "C" void VIGetCurrentLine_HLE_801bac48(CpuContext* ctx)
{
    uint32_t height = 480;
    std::chrono::nanoseconds interval{16666us};
    Clock::time_point last;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        height = g_vi.xfbHeight;
        interval = ActiveIntervalLocked();
        last = g_vi.lastRetrace;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - last);
    if (DetClock::Enabled()) {
        uint64_t lastTicks = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            lastTicks = g_vi.detLastRetraceTicks;
        }
        const uint64_t now = DetClock::Now();
        elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            TimeBaseContract::TicksToDuration(now > lastTicks ? now - lastTicks : 0));
    }
    uint32_t line = 0;
    if (interval.count() > 0 && height > 0) {
        const uint64_t scaled = static_cast<uint64_t>(elapsed.count()) * height;
        const uint64_t intervalUs = static_cast<uint64_t>(std::max<int64_t>(1, interval.count() / 1000));
        line = static_cast<uint32_t>(std::min<uint64_t>(height - 1, scaled / intervalUs));
    }
    ViSetR3(ctx, line);
}
PPC_NATIVE_OVERRIDE_VOID(8028202C, VIGetCurrentLine_HLE_801bac48, (CpuContext* ctx), (ctx));

extern "C" void VIWaitForRetrace_HLE_801b99ec(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    
    if (Fiber::GuestFiberManager::IsInitialized()) {
        bool resumedRunning = false;
        const SaveState::YieldRecord* resumed = SaveState::TakeYield(SaveState::YieldKind::ViWaitForRetrace, resumedRunning);
        const CpuContext entry = *cpu;
        const int32_t irqState = resumed ? static_cast<int32_t>(resumed->extra[1]) : OS__DisableInterrupts_801a65ac();
        uint32_t retraceCount = 0;
        if (resumed) {
            retraceCount = resumed->extra[0];
        } else {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            retraceCount = g_vi.retraceCount;
        }
        // Save states: the wait every frame's main loop parks in; a restored thread continues after its sleep.
        std::optional<SaveState::YieldScope> yieldScope;
        if (resumed) yieldScope.emplace(*resumed);
        else yieldScope.emplace(SaveState::YieldKind::ViWaitForRetrace, entry,
                                std::initializer_list<uint32_t>{retraceCount, static_cast<uint32_t>(irqState)});
        bool continuing = resumed != nullptr;

        do {
            cpu->gpr[3] = kViRetraceQueueAddr;
            if (continuing) {
                OS_HLE_SleepResume(cpu, resumedRunning, 0);  // the sleep ran with interrupts already off
                continuing = false;
            } else {
                OSSleepThread_HLE_801aa9b8(cpu);
            }

            {
                std::lock_guard<std::mutex> lock(g_viMutex);
                EnsureInitializedLocked();
                if (g_vi.retraceCount - retraceCount >= RuntimeFrontend::FrameStep()) {
                    break;
                }
            }
        } while (true);

        OS__RestoreInterrupts_801a65d4(irqState);
    } else {
        std::chrono::nanoseconds interval{16666us};
        Clock::time_point target;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            interval = ActiveIntervalLocked();
            target = g_vi.lastRetrace + interval;
        }

        const auto now = Clock::now();
        if (now < target) {
            SleepPreciselyUntil(target, true);
        }
        AdvanceRetrace(cpu, target, true);
    }
    ViSetR3(cpu, 0);
}
PPC_NATIVE_OVERRIDE_VOID(80280E0C, VIWaitForRetrace_HLE_801b99ec, (CpuContext* ctx), (ctx));

// VIGetScanMode (SPM 0x8028211C): reads the VI clock/DTV registers on hardware. The host has no
// cable, so report interlaced (VI_INTERLACE = 0), the mode every NTSC console supports.
extern "C" uint32_t VIGetScanMode_HLE() { return 0; }
PPC_NATIVE_OVERRIDE(8028211C, VIGetScanMode_HLE, uint32_t, (), ());


uint32_t SpmPresentedFrames() { return g_spmFramesFn(); }

// Called only after guest producers stop, before the next content session starts.
void VI_HLE_ResetFrontend() {
    std::lock_guard lock(g_viMutex);
    g_vi = ViState{};
    g_auroraFrameActive = false;
    g_auroraFrameHadWork = false;
    s_inAdvanceRetrace = false;
    s_presentSequenceActive = false;
    s_presentedXfbFrames = 0;
    s_lastPacedRetraceCount = ~0u;
    s_lastPresentAnchorNanos = 0;
}

void VI_HLE_SaveState(SaveState::Writer& out) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    out.Pod(g_vi);
}

void VI_HLE_LoadState(SaveState::Reader& in) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    in.Pod(g_vi);
}
