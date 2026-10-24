#include "libretro.h"
#include "bridge.hpp"
#include "adapters.h"
#include "disc_image.h"
#include "prepare_screen.h"
#include "frontend/runtime_contract.h"
#include "frontend/runtime_mode.h"
#include "platform/host_platform.h"
#include <aurora/external.h>
#include <dawn/native/VulkanBackend.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

namespace {
using namespace poc;
retro_environment_t environment = nullptr;
retro_video_refresh_t video = nullptr;
retro_audio_sample_t audio = nullptr;
retro_audio_sample_batch_t audioBatch = nullptr;
retro_input_poll_t inputPoll = nullptr;
retro_input_state_t inputState = nullptr;
const retro_hw_render_interface_vulkan* frontend = nullptr;
std::unique_ptr<Renderer> renderer;
std::unique_ptr<Bridge> bridge;
RuntimeFrontend::InputAdapter input;
RuntimeFrontend::PcmAdapter pcm;
RuntimeFrontend::ContentPaths paths;
bool loaded = false, running = false, stopped = false, cpuWait = false;
uint32_t frames = 0;

// One-time .wbfs unpacking: a worker thread extracts while retro_run shows the Preparing screen.
constexpr uint32_t kPrepareWidth = 1280, kPrepareHeight = 960;  // largest option; geometry maximum
struct Preparation {
    std::thread worker;
    RuntimeFrontend::DiscImage::Progress progress;
    std::atomic<int> state{0};  // 0 working, 1 succeeded, 2 failed
    std::mutex mutex;
    std::string error;
    std::filesystem::path root;
    RuntimeFrontend::PrepareTimeline timeline;
    uint32_t frames = 0;
    RuntimeFrontend::FrameStats stats;
    std::chrono::steady_clock::time_point lastRun{};
    // Shown progress is elapsed / (elapsed + remaining at the last few seconds' work rate): per-file
    // cost drifts, so a fixed bytes/files weighting would run ahead of the real finish.
    std::deque<std::pair<double, double>> recent;  // (seconds, work fraction)
    double shown = 0;
    double Estimate(double seconds, double fraction) {
        recent.emplace_back(seconds, fraction);
        while (recent.size() > 2 && seconds - recent.front().first > 4.0) recent.pop_front();
        const double span = seconds - recent.front().first, rate = (fraction - recent.front().second) / span;
        double estimate = fraction;
        if (seconds > 3.0 && span > 2.0 && rate > 0) estimate = seconds / (seconds + (1.0 - fraction) / rate);
        shown = std::max(shown, std::min(estimate, 0.99));
        return shown;
    }
    // Timing per phase: unpacking, chime, hold+fade.
    struct Phase { uint32_t frames = 0, slow = 0, underruns = 0; double total = 0, work = 0, worst = 0, minAudio = 1; };
    Phase phases[3];
    std::unique_ptr<Renderer> renderer;  // private Dawn device; Aurora's starts after this is gone
    std::unique_ptr<RuntimeFrontend::PrepareScreen> screen;
};
std::unique_ptr<Preparation> preparation;
bool frameStats = true;
bool fastCores = true;

// The game runs on the frontend's thread.
void pinToFastCores() {
    static thread_local bool pinned = false;
    if (pinned || !fastCores) return;
    pinned = true;
    if (const auto line = RuntimePlatform::PinCurrentThreadToFastCores(); !line.empty()) log("SPM fast cores %s", line.c_str());
}
uint32_t frameStep = 1;  // spm_frame_rate 30: two VI retraces per retro_run
uint32_t prepareWidth = 640, prepareHeight = 480;
float audioOccupancy = -1;
bool audioUnderrun = false;
void RETRO_CALLCONV audioBufferStatus(bool active, unsigned occupancy, bool underrunLikely) {
    audioOccupancy = active ? float(occupancy) / 100.f : -1.f;
    audioUnderrun = active && underrunLikely;
}
void logPreparationTiming(const Preparation& prep) {
    static const char* names[] = {"unpacking", "chime", "hold+fade"};
    for (int i = 0; i < 3; ++i) {
        const auto& phase = prep.phases[i];
        if (!phase.frames) continue;
        log("SPM prepare timing %s at %ux%u: %u frames, avg %.1f ms (%.1f fps), in core %.1f ms, worst %.1f ms, >20 ms %u, "
            "underrun-likely %u, min audio %.0f%%", names[i], prepareWidth, prepareHeight, phase.frames, phase.total / phase.frames,
            1000.0 * phase.frames / phase.total, phase.work / phase.frames, phase.worst,
            phase.slow, phase.underruns, phase.minAudio * 100);
    }
}
void releasePrepareGraphics() {
    if (!preparation) return;
    // The bridge borrows the preparation renderer, so it goes first.
    bridge.reset();
    preparation->screen.reset();
    preparation->renderer.reset();
}
void stopPreparation() {
    if (!preparation) return;
    preparation->progress.cancel = true;
    if (preparation->worker.joinable()) preparation->worker.join();
    releasePrepareGraphics();
    preparation.reset();
}
void report(const char* error) {
    stopped = true;
    log("SPM STOPPED: %s", error);
    if (environment) { retro_message message{error, 600}; environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message); }
}
void teardown() {
    // Order matters: producers, then the shared images, then Aurora's device that owns them.
    RuntimeFrontend::Stop();
    releasePrepareGraphics();
    bridge.reset(); renderer.reset();
    RuntimeFrontend::Unload();
    pcm.Clear(); running = false;
}
void destroy() {
    // Only presentation belongs to the frontend context; the guest stays suspended at its last VI.
    try { bridge.reset(); } catch (const std::exception& error) { report(error.what()); }
    frontend = nullptr;
}
void resetContext() {
    destroy();
    const retro_hw_render_interface* generic = nullptr;
    if (!environment || !environment(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &generic) ||
        !generic || generic->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN || generic->interface_version != 5) {
        report("Vulkan render interface v5 unavailable"); return;
    }
    frontend = reinterpret_cast<const retro_hw_render_interface_vulkan*>(generic);
}
const VkApplicationInfo* application() {
    static const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
        "Super Paper Mario", 1, "Aurora", 1, VK_API_VERSION_1_1};
    return &app;
}
bool rejectLegacy(retro_vulkan_context*, VkInstance, VkPhysicalDevice, VkSurfaceKHR,
    PFN_vkGetInstanceProcAddr, const char**, unsigned, const char**, unsigned, const VkPhysicalDeviceFeatures*) { return false; }
retro_hw_render_context_negotiation_interface_vulkan negotiation{
    RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN, 2, application, rejectLegacy,
    destroy, create_instance, create_device2
};
void matchGpu(WGPUDevice device) {
    const auto instance = dawn::native::vulkan::GetInstance(device);
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    // Dawn's private device and the frontend's must be the same GPU to share AHardwareBuffers.
    require(count == 1, "Expected one Aurora Vulkan GPU for verified AHB identity");
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, &gpu));
    VkPhysicalDeviceProperties a{}, b{};
    vkGetPhysicalDeviceProperties(gpu, &a); vkGetPhysicalDeviceProperties(frontend->gpu, &b);
    require(a.vendorID == b.vendorID && a.deviceID == b.deviceID && a.driverVersion == b.driverVersion &&
        a.apiVersion == b.apiVersion && std::memcmp(a.pipelineCacheUUID, b.pipelineCacheUUID, VK_UUID_SIZE) == 0,
        "Aurora/frontend GPU identities differ");
}
const char* option(const char* key, const char* fallback) {
    retro_variable value{key, nullptr};
    return environment && environment(RETRO_ENVIRONMENT_GET_VARIABLE, &value) && value.value ? value.value : fallback;
}
}

extern "C" {
RETRO_API unsigned retro_api_version() { return RETRO_API_VERSION; }
RETRO_API void retro_set_environment(retro_environment_t cb) {
    environment = cb;
    static const retro_variable options[] = {
        {"spm_video_contract", "Video contract (restart); ahb-gpu|ahb-cpu-wait"},
        {"spm_frame_stats", "Frame timing panel on the Preparing screen; enabled|disabled"},
        {"spm_fast_cores", "Run the game on the fast CPU cores; enabled|disabled"},
        {"spm_frame_rate", "Frame rate (restart): 30 keeps full game speed when 60 can't be held; 60|30"},
        {"spm_prepare_resolution", "Preparing screen resolution; 640x480|960x720|1280x960"},
        {nullptr, nullptr}};
    if (environment) environment(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(options));
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audioBatch = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { inputPoll = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { inputState = cb; }
RETRO_API void retro_init() {}
RETRO_API void retro_deinit() { retro_unload_game(); }
RETRO_API void retro_get_system_info(retro_system_info* info) {
    *info = {"Super Paper Mario", "0.1-dev", "wbfs|bin", true, false};
}
RETRO_API void retro_get_system_av_info(retro_system_av_info* info) {
    // The game is 640x480; the one-time Preparing screen renders at twice that.
    *info = {{640, 480, kPrepareWidth, kPrepareHeight, 4.f / 3.f}, {60. / frameStep, 48000.}};
}
RETRO_API bool retro_load_game(const retro_game_info* content) {
    try {
        require(!loaded && content && content->path && environment, "Load a Super Paper Mario .wbfs disc image");
        const char *save = nullptr, *system = nullptr;
        require(environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save) && save && *save, "Frontend save directory required");
        require(environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system) && system && *system, "Frontend system directory required");
        paths.saveRoot = std::filesystem::path(save) / "spm-r8pe01";
        paths.systemRoot = std::filesystem::path(system) / "spm-r8pe01";
        for (const char* name : {"dsp_coef.bin", "cacert.pem", "wii_bootstrap"})
            if (!std::filesystem::exists(paths.systemRoot / name))
                throw std::runtime_error("Missing " + std::string(name) + " in " + paths.systemRoot.string() +
                                         ": copy the core package's system folder into RetroArch's system directory");
        const std::filesystem::path path = std::filesystem::absolute(content->path);
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (extension == ".wbfs") {
            namespace Disc = RuntimeFrontend::DiscImage;
            const Disc::Identity identity = Disc::ReadIdentity(path);
            if (identity.gameId != "R8PE01" || identity.revision != 0)
                throw std::runtime_error("This core runs Super Paper Mario (USA, R8PE01 revision 0); this disc is " +
                                         identity.gameId + " revision " + std::to_string(identity.revision));
            paths.discRoot = Disc::CachedRoot(paths.systemRoot, identity);
            if (paths.discRoot.empty()) {
                preparation = std::make_unique<Preparation>();
                preparation->worker = std::thread([path, identity, parent = paths.systemRoot, prep = preparation.get()] {
                    try {
                        prep->root = Disc::ExtractToCache(path, parent, identity, prep->progress);
                        log("SPM disc prepared: %llu bytes in %s (decrypt %.1fs, write %.1fs)",
                            static_cast<unsigned long long>(prep->progress.done.load()), prep->root.c_str(),
                            prep->progress.decryptNanos / 1e9, prep->progress.writeNanos / 1e9);
                        prep->state = 1;
                    } catch (const std::exception& error) {
                        std::lock_guard lock(prep->mutex);
                        prep->error = error.what();
                        prep->state = 2;
                    }
                });
            }
        } else {
            std::ifstream file(path, std::ios::binary);
            char id[8]{}; file.read(id, sizeof(id));
            require(file && std::memcmp(id, "R8PE01\0\0", 8) == 0 && path.filename() == "boot.bin",
                "Content must be a USA R8PE01 revision 0 .wbfs, or an extracted disc's sys/boot.bin");
            paths.discRoot = path.parent_path().parent_path();
            require(std::filesystem::is_regular_file(paths.discRoot / "files/rel/relF.bin"), "Extracted disc files missing");
        }
        cpuWait = std::strcmp(option("spm_video_contract", "ahb-gpu"), "ahb-cpu-wait") == 0;
        frameStats = std::strcmp(option("spm_frame_stats", "enabled"), "enabled") == 0;
        fastCores = std::strcmp(option("spm_fast_cores", "enabled"), "enabled") == 0;
        frameStep = std::strcmp(option("spm_frame_rate", "60"), "30") == 0 ? 2 : 1;
        const char* resolution = option("spm_prepare_resolution", "640x480");
        prepareWidth = std::strcmp(resolution, "1280x960") == 0 ? 1280 : std::strcmp(resolution, "960x720") == 0 ? 960 : 640;
        prepareHeight = prepareWidth * 3 / 4;
        // States are whole-machine snapshots of one core build: no netplay, run-ahead or rewind.
        uint64_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE | RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT;
        environment(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
        static const retro_audio_buffer_status_callback status{audioBufferStatus};
        if (!environment(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, const_cast<retro_audio_buffer_status_callback*>(&status)))
            audioOccupancy = -1;
        frontend_device_negotiated = false;
        retro_hw_render_context_negotiation_interface support{RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN, 0};
        require(environment(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &support) && support.interface_version >= 2,
            "Vulkan negotiation v2 required");
        retro_hw_render_callback hw{};
        hw.context_type = RETRO_HW_CONTEXT_VULKAN; hw.context_reset = resetContext;
        hw.context_destroy = destroy; hw.version_major = 1; hw.version_minor = 1;
        require(environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw), "Frontend rejected Vulkan contract");
        require(environment(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &negotiation), "Frontend rejected Vulkan negotiation");
        environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, const_cast<retro_input_descriptor*>(RuntimeFrontend::InputAdapter::Descriptors()));
        loaded = true; stopped = false; frames = 0;
        return true;
    } catch (const std::exception& error) { report(error.what()); stopPreparation(); teardown(); destroy(); return false; }
}
RETRO_API void retro_unload_game() {
    try { stopPreparation(); teardown(); } catch (const std::exception& error) { report(error.what()); }
    frontend = nullptr; loaded = false; stopped = false; frames = 0;
}
namespace {
// Fixed size (length, state, zero padding) so the frontend sees one size before the game runs, which
// auto-load at start needs. A load before the first frame boundary is applied by retro_run.
constexpr size_t kStateHeadroom = 16u << 20;  // beyond guest memory: threads, HLE state, display lists
constexpr size_t kStateSize = sizeof(uint64_t) + 24u * 1024 * 1024 + 128u * 1024 * 1024 + kStateHeadroom;
std::vector<uint8_t> pendingState;
bool applyState(const std::vector<uint8_t>& state) {
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    const bool restored = RuntimeFrontend::RestoreState(state, error);
    log("SPM state load at frame=%u %s%s (%.1f ms)", frames, restored ? "ok" : "refused: ", error.c_str(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    if (restored) pcm.Clear();
    return restored;
}
// Logged every 600 frames as one "SPM perf" line.
struct GameplayTiming {
    using Clock = std::chrono::steady_clock;
    Clock::time_point last{};
    uint32_t count = 0, slow = 0, underruns = 0;
    float minAudio = 2;
    double interval = 0, worst = 0, guest = 0, present = 0, handoff = 0, other = 0;
    void Add(Clock::time_point start, Clock::time_point advanced, Clock::time_point bridged, Clock::time_point end, double presentMs) {
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        if (last.time_since_epoch().count()) {
            const double gap = ms(last, start);
            interval += gap; worst = std::max(worst, gap); slow += gap > 17.5 * frameStep; ++count;
            underruns += audioUnderrun; if (audioOccupancy >= 0) minAudio = std::min(minAudio, audioOccupancy);
            const double advance = ms(start, advanced);
            guest += advance - presentMs; present += presentMs; handoff += ms(advanced, bridged); other += ms(bridged, end);
        }
        last = start;
        if (count < 600) return;
        auto& b = bridge->timing;
#if defined(__linux__)
        const int cpu = sched_getcpu();
#else
        const int cpu = -1;
#endif
        log("SPM perf frame=%u fps=%.1f speed=%.0f%% interval avg=%.2f worst=%.1f slow=%u | guest=%.2f present=%.2f bridge=%.2f other=%.2f "
            "core=%.2f ms | bridge sync=%.2f fence=%.2f render=%.2f submit=%.2f | cpu=%d audio=%.0f%% min=%.0f%% underrun-likely=%u", frames,
            1000.0 * count / interval, 100.0 * 1000.0 * count / interval * frameStep / 60.0, interval / count, worst, slow, guest / count,
            present / count, handoff / count, other / count, (guest + present + handoff + other) / count, b.sync / count, b.fence / count, b.render / count, b.submit / count, cpu,
            audioOccupancy * 100, minAudio > 1 ? -100.f : minAudio * 100, underruns);
        b = {};
        *this = GameplayTiming{last};
    }
} gameplayTiming;
}
RETRO_API void retro_run() {
    if (!loaded || stopped || !frontend) return;
    try {
        pinToFastCores();
        if (inputPoll) inputPoll();
        if (preparation) {
            Preparation& prep = *preparation;
            if (prep.state == 2) {
                std::lock_guard lock(prep.mutex);
                throw std::runtime_error("Could not prepare the game data: " + prep.error);
            }
            if (!prep.renderer) {
                prep.renderer = std::make_unique<Renderer>();
                prep.screen = std::make_unique<RuntimeFrontend::PrepareScreen>(prep.renderer->device);
                prep.renderer->set_output([&prep](const wgpu::Texture& target) {
                    prep.screen->Draw(target, prep.timeline.Frame(), frameStats ? &prep.stats : nullptr);
                });
            }
            if (!bridge) bridge = std::make_unique<Bridge>(frontend, *prep.renderer, cpuWait);
            const auto runStart = std::chrono::steady_clock::now();
            const double intervalMs = prep.lastRun.time_since_epoch().count()
                ? std::chrono::duration<double, std::milli>(runStart - prep.lastRun).count() : 0.0;
            prep.lastRun = runStart;
            prep.timeline.Advance(prep.Estimate(prep.timeline.Frame().time, prep.progress.Fraction()), prep.state == 1);
            ++prep.frames;
            bridge->frame(prepareWidth, prepareHeight);
            if (video) video(RETRO_HW_FRAME_BUFFER_VALID, prepareWidth, prepareHeight, 0);
            int16_t samples[1024 * 2];
            if (prep.frames == 1 && audioBatch) {
                // ~64 ms of silence as headroom; without it the frontend's buffer runs near empty and crackles.
                std::memset(samples, 0, sizeof(samples));
                for (int i = 0; i < 3; ++i) audioBatch(samples, 1024);
            }
            const size_t count = prep.timeline.Audio(samples, 1024);
            if (audioBatch) audioBatch(samples, count);
            if (intervalMs > 0) {
                RuntimeFrontend::FrameStats::Sample sample;
                sample.intervalMs = float(intervalMs);
                sample.workMs = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - runStart).count());
                sample.audio = audioOccupancy;
                sample.underrun = audioUnderrun ? 1.f : 0.f;
                prep.stats.Push(sample);
                const float done = prep.timeline.Frame().done;
                auto& phase = prep.phases[done < 0 ? 0 : done < 1.6f ? 1 : 2];
                ++phase.frames;
                phase.total += intervalMs;
                phase.work += sample.workMs;
                phase.worst = std::max(phase.worst, intervalMs);
                phase.slow += intervalMs > 20.0;
                phase.underruns += audioUnderrun;
                if (audioOccupancy >= 0) phase.minAudio = std::min<double>(phase.minAudio, audioOccupancy);
            }
            if (prep.timeline.Complete()) {
                logPreparationTiming(prep);
                prep.worker.join();
                paths.discRoot = prep.root;
                releasePrepareGraphics();
                preparation.reset();
                log("SPM preparing screen complete; starting game from %s", paths.discRoot.c_str());
            }
            return;
        }
        if (!running) {
            RuntimeFrontend::SetFrameStep(frameStep);
            if (!RuntimeFrontend::Initialize(paths, {RuntimeFrontend::PcmAdapter::Sink, &pcm}))
                throw std::runtime_error(RuntimeFrontend::LastError());
            const wgpu::Device device(aurora_external_device());
            const wgpu::Instance instance(aurora_external_instance());
            matchGpu(device.Get());
            renderer = std::make_unique<Renderer>(instance, device, [](const wgpu::Texture& target) {
                require(aurora_external_copy_output(target.Get()), "Aurora output copy failed");
            });
            bridge = std::make_unique<Bridge>(frontend, *renderer, cpuWait);
            running = true;
        }
        if (!bridge) {
            bridge = std::make_unique<Bridge>(frontend, *renderer, cpuWait);
            log("SPM presentation resumed at frame=%u", frames);
        }
        require(aurora_external_healthy(), "Aurora Dawn device was lost");
        const auto runStart = std::chrono::steady_clock::now();
        const auto snapshot = input.Poll(inputState);
        const auto result = RuntimeFrontend::AdvanceOneFrame(snapshot, {});
        require(result != RuntimeFrontend::FrameResult::Failed && result != RuntimeFrontend::FrameResult::Stopped, RuntimeFrontend::LastError());
        require(aurora_external_healthy(), "Aurora Dawn device was lost");
        if (!pendingState.empty()) {
            applyState(pendingState);
            pendingState = {};
        }
        const auto advanced = std::chrono::steady_clock::now();
        if (aurora_external_output()) {
            bridge->frame(640, 480);
            if (video) video(RETRO_HW_FRAME_BUFFER_VALID, 640, 480, 0);
        } else if (video) video(nullptr, 640, 480, 0);
        const auto bridged = std::chrono::steady_clock::now();
        pcm.Drain(audioBatch, audio);
        gameplayTiming.Add(runStart, advanced, bridged, std::chrono::steady_clock::now(), RuntimeFrontend::TakePresentTime() / 1e6);
        ++frames;
    } catch (const std::exception& error) { report(error.what()); }
      catch (...) { report("Unhandled runtime failure"); }
}
RETRO_API void retro_reset() {
    try { teardown(); stopped = false; frames = 0; } catch (const std::exception& error) { report(error.what()); }
}
RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}
RETRO_API unsigned retro_get_region() { return RETRO_REGION_NTSC; }
RETRO_API size_t retro_serialize_size() { return kStateSize; }
RETRO_API bool retro_serialize(void* data, size_t size) {
    if (!running || frames == 0 || size < kStateSize) return false;
    static std::vector<uint8_t> state;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    const bool captured = RuntimeFrontend::CaptureState(state, error);
    if (captured && state.size() + sizeof(uint64_t) > size) error = "state larger than the reported size";
    log("SPM state save at frame=%u %s%s (%zu bytes, %.1f ms)", frames, captured && error.empty() ? "ok" : "refused: ",
        error.c_str(), state.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    if (!captured || !error.empty()) return false;
    const uint64_t length = state.size();
    auto* out = static_cast<uint8_t*>(data);
    std::memcpy(out, &length, sizeof(length));
    std::memcpy(out + sizeof(length), state.data(), state.size());
    std::memset(out + sizeof(length) + state.size(), 0, size - sizeof(length) - state.size());
    return true;
}
RETRO_API bool retro_unserialize(const void* data, size_t size) {
    uint64_t length = 0;
    if (!data || size < sizeof(length)) return false;
    std::memcpy(&length, data, sizeof(length));
    if (length == 0 || length > size - sizeof(length)) return false;
    const auto* bytes = static_cast<const uint8_t*>(data) + sizeof(length);
    std::vector<uint8_t> state(bytes, bytes + length);
    if (!running || frames == 0) {
        log("SPM state load deferred until the game's first frame");
        pendingState = std::move(state);
        return true;
    }
    return applyState(state);
}
RETRO_API void retro_cheat_reset() {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}
RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }
RETRO_API void* retro_get_memory_data(unsigned) { return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned) { return 0; }
}
