#include "det_clock.h"
#include "frontend/runtime_mode.h"

#include "runtime_log.h"
#include "timebase_contract.h"

#include <algorithm>
#include <atomic>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace DetClock {
namespace detail {
bool g_enabled = false;
} // namespace detail

namespace {

using Clock = std::chrono::steady_clock;

// About a microsecond of guest time per mftb read: enough for a loop that polls the time base to
// get anywhere, too little to matter for the few hundred reads a frame makes.
constexpr uint64_t kTicksPerGuestRead = 61;
// How far real time may run ahead of the timeline before pacing gives up catching up and starts
// over from the present (the backlog would otherwise play back as a burst of fast frames).
constexpr auto kMaxLag = std::chrono::milliseconds(100);

std::atomic<uint64_t> g_now{0};

// Real-time anchor of the pacing: virtual instant g_anchorTicks happens at real time g_anchorReal.
// Guest thread only.
bool g_anchored = false;
Clock::time_point g_anchorReal{};
uint64_t g_anchorTicks = 0;

void SleepUntil(Clock::time_point deadline) {
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    // The high-resolution waitable timer lands within about half a millisecond; spin the rest so
    // frames keep an even cadence instead of the 1 ms scheduler sawtooth.
    constexpr auto kSpin = std::chrono::microseconds(500);
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
    const auto now = Clock::now();
    if (deadline - now > kSpin && timer.handle != nullptr) {
        const auto wait = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
            deadline - kSpin - now);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(wait.count(), 1);
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            WaitForSingleObject(timer.handle, INFINITE);
        }
    }
    while (Clock::now() < deadline) {
        YieldProcessor();
    }
#else
    // clock_nanosleep is precise to well under a millisecond on Android and Linux; no spin, which
    // would only cost a phone battery.
    std::this_thread::sleep_until(deadline);
#endif
}

} // namespace

void Enable() noexcept {
    if (!detail::g_enabled) {
        detail::g_enabled = true;
        RT_LOGF(RT_TAG_RUNTIME, "deterministic timing on: guest clocks run on a virtual timeline\n");
    }
}

void Reset() noexcept { detail::g_enabled = false; g_now.store(0); g_anchored = false; }

uint64_t Now() noexcept {
    return g_now.load(std::memory_order_relaxed);
}

uint64_t ReadForGuest() noexcept {
    return g_now.fetch_add(kTicksPerGuestRead, std::memory_order_relaxed);
}

void AdvanceTo(uint64_t ticks) noexcept {
    uint64_t now = g_now.load(std::memory_order_relaxed);
    while (ticks > now && !g_now.compare_exchange_weak(now, ticks, std::memory_order_relaxed)) {
    }
}

void PaceTo(uint64_t ticks) {
    if (RuntimeFrontend::Active()) return;
    const auto now = Clock::now();
    if (!g_anchored || ticks < g_anchorTicks) {
        g_anchored = true;
        g_anchorReal = now;
        g_anchorTicks = ticks;
        return;
    }
    const auto target = g_anchorReal + TimeBaseContract::TicksToDuration(ticks - g_anchorTicks);
    if (now >= target) {
        if (now - target > kMaxLag) {
            g_anchorReal = now;
            g_anchorTicks = ticks;
        }
        return;
    }
    SleepUntil(target);
}

void ResetPacing() noexcept {
    g_anchored = false;
}

Clock::time_point NowTimePoint() noexcept {
    return Clock::time_point{TimeBaseContract::TicksToDuration(Now())};
}

uint64_t TicksAtOrAfter(Clock::time_point point) noexcept {
    const auto sinceEpoch = std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count();
    if (sinceEpoch <= 0) {
        return 0;
    }
    const uint64_t ns = static_cast<uint64_t>(sinceEpoch);
    constexpr uint64_t kNum = TimeBaseContract::kTickRatioNumerator;    // 243 ticks
    constexpr uint64_t kDen = TimeBaseContract::kTickRatioDenominator;  // per 4000 ns
    return (ns / kDen) * kNum + ((ns % kDen) * kNum + kDen - 1) / kDen;
}

} // namespace DetClock
