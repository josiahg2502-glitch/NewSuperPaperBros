#include "frontend/runtime_contract.h"
#include "frontend/runtime_mode.h"
#include "save_state.h"
#include "isa/ppc_isa_context.h"
#include "frontend/hle_lifecycle.h"
#include "abi_bridge.h"
#include "audio_backend.h"
#include "det_clock.h"
#include "display_settings.h"
#include "fiber_manager.h"
#include "frame_input.h"
#include "gx_guest_write.h"
#include "hle/audio/ax_dsp.h"
#include "hle_stubs.h"
#include "host_context.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "system_bridge.h"
#include "virtual_remote.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <cstdarg>
#include <cstring>
#include <optional>
#include <thread>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace RuntimeFrontend {
namespace {
std::atomic<bool> active{false};
bool stopping = false, finished = false, failed = false, graphics = false;
HostContext::Handle frontend = nullptr, boot = nullptr, suspended = nullptr;
ContentPaths paths;
Callbacks callbacks;
InputSnapshot input;
std::optional<RuntimeUserConfig> previousConfig;
std::thread::id owner;
unsigned shakeFrame = 9;
std::string lastError;
uint64_t presentTime = 0;
bool suspendRequested = false;
uint32_t frameStep = 1;          // VI retraces per frontend frame (2: half rate)
uint32_t retracesThisFrame = 0;
bool atCanonicalSuspend = false;
uint32_t bootThread = 0;  // guest thread whose fiber is the boot context (restored boot root resumes it)

// The gamepad as a sideways Wii Remote, with the desktop build's rules (virtual_remote.cpp PadIntent
// and UpdatePointer), stepped once per frontend frame.
struct Remote {
    uint32_t hold = 0;           // WPAD bits, D-pad already rotated for the sideways remote
    float pointer[2] = {};       // -1..1, +y down
    unsigned pointerFrames = 0;  // frames the pointer stays on screen
    float tilt = 0;              // -1 counter-clockwise .. 1 clockwise
} remote;
constexpr float kStickThreshold = 48.f / 127.f;  // left stick moves beyond this radius
constexpr float kDiagonalSlope = 0.4142136f;     // tan(22.5 degrees): eight-way sectors
constexpr float kPointerDeadZone = 0.25f, kPointerSpeed = 1.6f / 60.f;  // half-screens per frame at full tilt
constexpr unsigned kPointerLinger = 120;          // the pointer hides 2 s after the last aim
constexpr float kTiltDeadZone = 0.25f, kTiltFollow = 0.3f, kMaxTiltRadians = 0.7853982f;

void StepRemote() {
    uint32_t screen = input.buttons & (Left | Right | Up | Down);
    const float sx = input.leftStick[0], sy = -input.leftStick[1];  // +y up
    if (sx * sx + sy * sy >= kStickThreshold * kStickThreshold) {
        if (std::fabs(sx) >= kDiagonalSlope * std::fabs(sy)) screen |= sx < 0 ? Left : Right;
        if (std::fabs(sy) >= kDiagonalSlope * std::fabs(sx)) screen |= sy < 0 ? Down : Up;
    }
    remote.hold = input.buttons & ~15u;
    if (screen & Left) remote.hold |= Up;  // held sideways: the D-pad's up points left
    if (screen & Right) remote.hold |= Down;
    if (screen & Up) remote.hold |= Right;
    if (screen & Down) remote.hold |= Left;

    // Tilt Island and the other arcade games steer by tilting: there the left stick tilts too.
    const bool arcade = VirtualRemote::InArcade();
    const float tilt = arcade && std::fabs(sx) >= kTiltDeadZone ? std::clamp(sx, -1.f, 1.f) : 0.f;
    remote.tilt += (tilt - remote.tilt) * kTiltFollow;
    if (std::fabs(remote.tilt) < 0.001f) remote.tilt = 0;

    // Touch aims directly; the right stick moves the pointer, which appears in the middle and stays
    // a moment after. Never keep it up permanently outside the arcade: Mario freezes under it.
    const float rx = input.rightStick[0], ry = input.rightStick[1];
    if (input.pointerValid) {
        remote.pointer[0] = input.pointerX;
        remote.pointer[1] = input.pointerY;
        remote.pointerFrames = kPointerLinger;
    } else if (rx * rx + ry * ry >= kPointerDeadZone * kPointerDeadZone) {
        if (!remote.pointerFrames) remote.pointer[0] = remote.pointer[1] = 0;
        remote.pointer[0] = std::clamp(remote.pointer[0] + rx * kPointerSpeed, -1.f, 1.f);
        remote.pointer[1] = std::clamp(remote.pointer[1] + ry * kPointerSpeed, -1.f, 1.f);
        remote.pointerFrames = kPointerLinger;
    } else if (remote.pointerFrames && !arcade) {
        --remote.pointerFrames;
    }
}

void SampleInput(uint32_t, FrameInput::Frame& frame) {
    frame = {};
    auto& state = frame[0];
    state.connected = true;
    state.kind = WiiRemoteInput::Kind::Remote;
    auto& sample = state.sample;
    sample.hold = remote.hold;
    static constexpr float flick[] = {-1.6f, -4.4f, -1.2f, .8f, 2.8f, 4.8f, 4.8f, 2.4f, 0.f};
    if (input.shake && shakeFrame >= 9) shakeFrame = 0;
    const float angle = remote.tilt * kMaxTiltRadians;
    sample.acc[1] = -std::cos(angle) + (shakeFrame < 9 ? flick[shakeFrame++] : 0.f);
    sample.acc[2] = std::sin(angle);
    sample.pointerValid = remote.pointerFrames > 0;
    sample.pointer[0] = remote.pointer[0];
    sample.pointer[1] = remote.pointer[1];
}

void Boot(void*) {
    try {
        Fiber::GuestFiberManager::Initialize();
        CheckCancellation();
        auto& cpu = GetPersistentCpuContext();
        cpu.gpr[1] = 0x81700000u;
        CpuContextScope scope(&cpu);
        InvokeIndirectCpu(0x80006124u, &cpu);
    } catch (const Cancelled&) {
    } catch (const std::exception& error) {
        failed = true;
        lastError = error.what();
        RT_LOGF(RT_TAG_RUNTIME, "Frontend guest failed: %s\n", error.what());
    }
    stopping = true;
    Fiber::GuestFiberManager::CancelFrontendFibers();
    finished = true;
    suspended = HostContext::Current();
    HostContext::Switch(frontend);
    for (;;) HostContext::Switch(frontend); // Completed contexts are never scheduled again.
}
}

// Save states: the boot context of a restored state; the main thread's continuation instead of __start.
void BootResume(void*) {
    try {
        auto& cpu = GetPersistentCpuContext();
        CpuContextScope scope(&cpu);
        const uint32_t root = SaveState::BeginResume(bootThread);
        CheckCancellation();
        InvokeIndirectCpu(root, &cpu);
    } catch (const Cancelled&) {
    } catch (const std::exception& error) {
        failed = true;
        lastError = error.what();
        RT_LOGF(RT_TAG_RUNTIME, "Frontend guest failed: %s\n", error.what());
    }
    stopping = true;
    Fiber::GuestFiberManager::CancelFrontendFibers();
    finished = true;
    suspended = HostContext::Current();
    HostContext::Switch(frontend);
    for (;;) HostContext::Switch(frontend);
}

struct Continuations {
    std::vector<SaveState::ThreadContinuation> threads;
    uint32_t running = 0, boot = 0;
    CpuContext live{};
    std::vector<Fiber::FiberRecord> fibers;
    uint32_t currentThread = 0;
};

bool CaptureContinuations(Continuations& out, std::string& error) {
    if (!active || finished || !atCanonicalSuspend || !suspended) { error = "guest not at a frame boundary"; return false; }
    out = {};
    struct Visit { Continuations* out; std::vector<std::pair<uint32_t, uint32_t>> started; } visit{&out, {}};
    const bool listed = Fiber::GuestFiberManager::ForEachFiber(
        [](uint32_t thread, const Fiber::GuestFiber& fiber, bool, void* user) {
            auto& v = *static_cast<Visit*>(user);
            if (fiber.terminated || !fiber.fiber) return;
            if (fiber.fiber == suspended) v.out->running = thread;
            if (fiber.fiber == Fiber::GuestFiberManager::SchedulerFiber()) v.out->boot = thread;
            if (fiber.started) v.started.emplace_back(thread, fiber.fiber == Fiber::GuestFiberManager::SchedulerFiber() ? 0x80006124u : fiber.entryPoint);
        },
        &visit);
    if (!listed) { error = "fiber table busy"; return false; }
    if (!out.running || !out.boot) {
        char text[160];
        std::snprintf(text, sizeof(text), "running %08X or boot %08X thread unknown (%zu started, suspended=%p boot=%p)",
                      out.running, out.boot, visit.started.size(), suspended, boot);
        error = text;
        return false;
    }
    for (const auto& [thread, root] : visit.started) {
        SaveState::ThreadContinuation continuation;
        std::string reason;
        if (!SaveState::CaptureThread(thread, root, thread == out.running, continuation, reason)) {
            char text[160];
            std::snprintf(text, sizeof(text), "thread %08X: %s", thread, reason.c_str());
            error = text;
            return false;
        }
        out.threads.push_back(std::move(continuation));
    }
    out.live = GetPersistentCpuContext();
    Fiber::GuestFiberManager::ExportTable(out.fibers, out.currentThread);
    return true;
}

bool RestoreContinuations(const Continuations& state, std::string& error) {
    SaveState::ResetForRestore();
    for (const auto& thread : state.threads) SaveState::SetPendingResume(thread);
    bootThread = state.boot;
    void* newBoot = nullptr;
    void* newRunning = nullptr;
    if (!Fiber::GuestFiberManager::RebuildForRestore(state.fibers, boot, BootResume, state.running, &newBoot, &newRunning)) {
        error = "fiber rebuild failed";
        return false;
    }
    HostContext::Destroy(boot);
    boot = newBoot;
    suspended = newRunning;
    GetPersistentCpuContext() = state.live;
    return true;
}

void Printf(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
#if defined(__ANDROID__)
    __android_log_vprint(ANDROID_LOG_INFO, "DawnInterop", format, arguments);
#else
    std::vfprintf(stderr, format, arguments);
#endif
    va_end(arguments);
}
bool Active() noexcept { return active; }
void AddPresentTime(uint64_t nanoseconds) noexcept { presentTime += nanoseconds; }
uint64_t TakePresentTime() noexcept { const uint64_t value = presentTime; presentTime = 0; return value; }
const char* LastError() noexcept { return lastError.c_str(); }
void CheckCancellation() { if (active && stopping) throw Cancelled{}; }
[[noreturn]] void Fail(const char* message) {
    failed = true;
    lastError = message;
    RT_LOGF(RT_TAG_RUNTIME, "Frontend stopped: %s\n", message);
    stopping = true;
    throw Cancelled{};
}
void RequestSuspendAtVI() noexcept {
    if (!active || ++retracesThisFrame < frameStep) return;
    retracesThisFrame = 0;
    suspendRequested = true;
}
void SetFrameStep(uint32_t retraces) noexcept { frameStep = retraces < 1 ? 1 : retraces; }
uint32_t FrameStep() noexcept { return active ? frameStep : 1; }
bool SuspendRequested() noexcept { return active && suspendRequested; }
void SuspendIfRequested() {
    if (!active || !suspendRequested) return;
    suspendRequested = false;
    atCanonicalSuspend = true;  // the only point a save state captures or restores
    SuspendAtVI();
    atCanonicalSuspend = false;
}
void SuspendAtVI() {
    if (!active) return;
    CheckCancellation();
    suspended = HostContext::Current();
    HostContext::Switch(frontend);
    CheckCancellation();
}

bool Initialize(const ContentPaths& content, const Callbacks& sinks) {
    if (active || graphics) return false;
    owner = std::this_thread::get_id();
    paths = content; callbacks = sinks;
    stopping = finished = failed = false;
    lastError.clear();
    try {
        std::filesystem::create_directories(paths.saveRoot / "NAND");
        std::filesystem::create_directories(paths.systemRoot / "Cache");
        RuntimePlatform::SetFrontendDirectories(paths.saveRoot, paths.systemRoot);
        previousConfig = RuntimeConfigFile::Get();
        auto& config = RuntimeConfigFile::Mutable();
        config.dvdRoot = RuntimeConfigFile::PathToUtf8(paths.discRoot);
        config.nandRoot = RuntimeConfigFile::PathToUtf8(paths.saveRoot / "NAND");
        config.widescreen = false;
        config.frameInterpolationFps = 0;
        config.textureReplacements = false;
        config.audioMixWorker = false;
        active = true;
        VI_HLE_ResetFrontend();
        DetClock::Reset();
        DetClock::Enable();
        AudioBackend::Instance().SetFrontendSink(callbacks.pcm, callbacks.opaque);
        AxDspHle::SetMixWorkerEnabled(false);
        SystemBridge::Initialize();
        TranslatedFunctionRegistry::Finalize();
        DisplaySettings::Initialize({DisplaySettings::kAspect4x3, 1.f});
        const std::string save = paths.saveRoot.string(), cache = (paths.systemRoot / "Cache").string();
        const std::string resources = paths.systemRoot.string();
        AuroraConfig aurora{};
        aurora.appName = "Super Paper Mario";
        aurora.userPath = save.c_str(); aurora.cachePath = cache.c_str(); aurora.resourcesPath = resources.c_str();
#if defined(__APPLE__)
        aurora.desiredBackend = BACKEND_METAL;
#else
        aurora.desiredBackend = BACKEND_VULKAN;
#endif
        aurora.windowWidth = 640; aurora.windowHeight = 480;
        aurora.externalPresentation = true;
        aurora.disableCopyFilter = true;
        aurora.logLevel = LOG_INFO;
#if defined(__ANDROID__)
        aurora.logCallback = [](AuroraLogLevel level, const char* module, const char* message, unsigned length) {
            __android_log_print(level >= LOG_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                "DawnInterop", "Aurora[%s] %.*s", module, int(length), message);
        };
#endif
        const auto info = aurora_initialize(0, nullptr, &aurora);
        if (info.initializationStatus != AURORA_INITIALIZATION_SUCCESS)
            throw std::runtime_error(info.initializationError ? info.initializationError : "Windowless Aurora failed");
        graphics = true;
        GxGuestWrite::InstallAuroraHooks();
        DisplaySettings::OnFrameBoundary(640, 480);
        aurora_set_frame_interpolation_fps(0);
        FrameInput::SetSource(SampleInput);
        FrameInput::Latch(0);
        if (!HostContext::InitializeScheduler(&frontend)) throw std::runtime_error("Frontend context failed");
        boot = HostContext::Create(4 * 1024 * 1024, Boot, nullptr);
        if (!boot) throw std::runtime_error("Guest boot context allocation failed");
        suspended = boot;
        suspendRequested = false;
        retracesThisFrame = 0;
        return true;
    } catch (const std::exception& error) {
        lastError = error.what();
        RT_LOGF(RT_TAG_RUNTIME, "Frontend initialization failed: %s\n", error.what());
        failed = true;
        Unload();
        return false;
    }
}

FrameResult AdvanceOneFrame(const InputSnapshot& snapshot, const VideoTarget&) {
    if (!active || finished) return failed ? FrameResult::Failed : FrameResult::Stopped;
    if (std::this_thread::get_id() != owner) return FrameResult::Failed;
    input = snapshot;
    StepRemote();
    FrameInput::Latch(0);  // the guest reads it after this switch, in this frame's wpadMain
    HostContext::Switch(suspended);
    if (finished) return failed ? FrameResult::Failed : FrameResult::Stopped;
    return FrameResult::Presented;
}

void Stop() {
    if (!active) return;
    if (std::this_thread::get_id() != owner) throw std::runtime_error("Guest stop called from a different host thread");
    stopping = true;
    if (!finished && boot && suspended) HostContext::Switch(suspended);
    AxDspHle::ShutdownMixWorker();
    AudioBackend::Instance().Shutdown();
    AudioBackend::Instance().SetFrontendSink(nullptr, nullptr);
    FrameInput::SetSource(nullptr);
    Fiber::GuestFiberManager::Shutdown();
    if (boot) HostContext::Destroy(boot);
    boot = suspended = nullptr;
    if (frontend) HostContext::ShutdownScheduler(frontend);
    frontend = nullptr;
    active = false;
}

void Unload() {
    Stop();
    if (graphics) { aurora_shutdown(); graphics = false; }
    DVD_HLE_ResetFrontend();
    NAND_HLE_ResetFrontendCallbacks();
    NAND_HLE_ResetFrontendFiles();
    NAND_HLE_ResetFrontendIos();
    OS_HLE_ResetFrontendSleep();
    WPAD_HLE_ResetFrontend();
    AI_HLE_ResetFrontend();
    Memory::Reset();
    DetClock::Reset();
    if (previousConfig) { RuntimeConfigFile::Mutable() = *previousConfig; previousConfig.reset(); }
    RuntimePlatform::ClearFrontendDirectories();
    shakeFrame = 9;
    remote = {};
}

namespace {
constexpr uint32_t kStateMagic = SaveState::Tag("SPMS"), kStateVersion = 1;
// States resume translated code at its call sites, so they load only into the build that wrote them.
constexpr char kStateBuild[32] = __DATE__ " " __TIME__;
constexpr uint32_t kMem1Base = 0x80000000u, kMem2Base = 0x90000000u;

void WriteContinuations(SaveState::Writer& out, const Continuations& state) {
    out.Pod(state.running);
    out.Pod(state.boot);
    out.Pod(state.live);
    out.Pod(state.currentThread);
    out.Pod(uint32_t(state.fibers.size()));
    for (const auto& fiber : state.fibers) out.Pod(fiber);
    out.Pod(uint32_t(state.threads.size()));
    for (const auto& thread : state.threads) {
        out.Pod(thread.thread);
        out.Pod(thread.running);
        out.Pod(thread.yield);
        out.Pod(uint32_t(thread.frames.size()));
        for (const auto& frame : thread.frames) out.Pod(frame);
    }
}

void ReadContinuations(SaveState::Reader& in, Continuations& state) {
    in.Pod(state.running);
    in.Pod(state.boot);
    in.Pod(state.live);
    in.Pod(state.currentThread);
    state.fibers.resize(std::min<uint32_t>(in.Get<uint32_t>(), 4096));
    for (auto& fiber : state.fibers) in.Pod(fiber);
    state.threads.resize(std::min<uint32_t>(in.Get<uint32_t>(), 4096));
    for (auto& thread : state.threads) {
        in.Pod(thread.thread);
        in.Pod(thread.running);
        in.Pod(thread.yield);
        thread.frames.resize(std::min<uint32_t>(in.Get<uint32_t>(), 64));
        for (auto& frame : thread.frames) in.Pod(frame);
    }
}
}  // namespace

bool CaptureState(std::vector<uint8_t>& out, std::string& error) {
    Continuations continuations;
    if (!CaptureContinuations(continuations, error)) return false;
    out.clear();
    out.reserve(Memory::kMem1Size + Memory::kMem2Size + (1 << 20));
    SaveState::Writer writer(out);
    writer.Pod(kStateMagic);
    writer.Pod(kStateVersion);
    writer.Pod(kStateBuild);
    size_t at = writer.Begin(SaveState::Tag("THRD"));
    WriteContinuations(writer, continuations);
    writer.End(at);
    at = writer.Begin(SaveState::Tag("FRNT"));
    writer.Pod(retracesThisFrame);
    writer.Pod(remote);
    writer.Pod(DetClock::Now());
    writer.End(at);
    at = writer.Begin(SaveState::Tag("VI  "));
    VI_HLE_SaveState(writer);
    writer.End(at);
    at = writer.Begin(SaveState::Tag("OS  "));
    OS_HLE_SaveState(writer);
    writer.End(at);
    at = writer.Begin(SaveState::Tag("AI  "));
    AI_HLE_SaveState(writer);
    writer.End(at);
    at = writer.Begin(SaveState::Tag("INPT"));
    KPAD_HLE_SaveState(writer);
    VirtualRemote::SaveState(writer);
    writer.End(at);
    at = writer.Begin(SaveState::Tag("NAND"));
    if (!NAND_HLE_SaveState(writer, error)) return false;
    writer.End(at);
    at = writer.Begin(SaveState::Tag("GXDL"));
    if (!GXDL_HLE_SaveState(writer, error)) return false;
    writer.End(at);
    at = writer.Begin(SaveState::Tag("MEM1"));
    writer.Bytes(Memory::GetPointer(kMem1Base, Memory::kMem1Size), Memory::kMem1Size);
    writer.End(at);
    at = writer.Begin(SaveState::Tag("MEM2"));
    writer.Bytes(Memory::GetPointer(kMem2Base, Memory::kMem2Size), Memory::kMem2Size);
    writer.End(at);
    return true;
}

bool RestoreState(const std::vector<uint8_t>& data, std::string& error) {
    if (!active || finished || !suspended) { error = "guest not running"; return false; }
    // Find and check every section before touching the running guest.
    SaveState::Reader in(data.data(), data.size());
    char build[sizeof(kStateBuild)] = {};
    const bool header = in.Get<uint32_t>() == kStateMagic && in.Get<uint32_t>() == kStateVersion;
    in.Pod(build);
    if (!header || !in.Ok()) { error = "not a Super Paper Mario state"; return false; }
    if (std::memcmp(build, kStateBuild, sizeof(build)) != 0) { error = "state was saved by a different core build"; return false; }
    SaveState::Reader threads = in.Section(SaveState::Tag("THRD"));
    SaveState::Reader frontendState = in.Section(SaveState::Tag("FRNT"));
    SaveState::Reader vi = in.Section(SaveState::Tag("VI  "));
    SaveState::Reader os = in.Section(SaveState::Tag("OS  "));
    SaveState::Reader ai = in.Section(SaveState::Tag("AI  "));
    SaveState::Reader input = in.Section(SaveState::Tag("INPT"));
    SaveState::Reader nand = in.Section(SaveState::Tag("NAND"));
    SaveState::Reader gxdl = in.Section(SaveState::Tag("GXDL"));
    SaveState::Reader mem1 = in.Section(SaveState::Tag("MEM1"));
    SaveState::Reader mem2 = in.Section(SaveState::Tag("MEM2"));
    Continuations continuations;
    ReadContinuations(threads, continuations);
    const auto savedRetraces = frontendState.Get<uint32_t>();
    const auto savedRemote = frontendState.Get<Remote>();
    const auto clock = frontendState.Get<uint64_t>();
    const uint8_t* mem1Bytes = mem1.Take(Memory::kMem1Size);
    const uint8_t* mem2Bytes = mem2.Take(Memory::kMem2Size);
    if (!in.AtEnd() || !threads.AtEnd() || !frontendState.AtEnd() || !mem1.AtEnd() || !mem2.AtEnd() || !vi.Ok() || !os.Ok() || !ai.Ok() || !input.Ok() || !nand.Ok() || !gxdl.Ok()) {
        error = "malformed state";
        return false;
    }
    std::memcpy(Memory::GetPointer(kMem1Base, Memory::kMem1Size), mem1Bytes, Memory::kMem1Size);
    std::memcpy(Memory::GetPointer(kMem2Base, Memory::kMem2Size), mem2Bytes, Memory::kMem2Size);
    DetClock::RestoreNow(clock);
    VI_HLE_LoadState(vi);
    OS_HLE_LoadState(os);
    AI_HLE_LoadState(ai);
    KPAD_HLE_LoadState(input);
    VirtualRemote::LoadState(input);
    NAND_HLE_LoadState(nand);
    GXDL_HLE_LoadState(gxdl);
    retracesThisFrame = savedRetraces;
    remote = savedRemote;
    suspendRequested = false;
    return RestoreContinuations(continuations, error);
}

bool RebuildContinuationsInPlace(std::string& error) {
    Continuations state;
    return CaptureContinuations(state, error) && RestoreContinuations(state, error);
}

bool Reset() {
    const auto content = paths;
    const auto sinks = callbacks;
    Unload();
    return Initialize(content, sinks);
}
} // namespace RuntimeFrontend
