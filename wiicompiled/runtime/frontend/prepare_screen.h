#pragma once
#include <webgpu/webgpu_cpp.h>
#include <array>
#include <cstddef>
#include <cstdint>

// The one-time "Preparing game data" screen shown while the core unpacks a .wbfs: a paper card
// flipping above a progress bar, a check mark and a short "plink" once the data is ready.
namespace RuntimeFrontend {
// Everything the shader needs for one frame; produced by PrepareTimeline.
struct PrepareFrame {
    float time = 0;       // seconds since the screen appeared
    float progress = 0;   // displayed (eased) progress, 0..1
    float done = -1;      // seconds since completion, negative while working
    float fade = 0;       // fade to black before the game starts, 0..1
    float cardAngle = 0;  // card flip angle in radians; multiples of 2*pi show the front
    float cardLift = 0;   // 0..1 rise while a flip is in motion
};

// Recent frame timing for the corner indicator: how evenly frames arrive and how full the
// frontend's audio buffer is (it reports underruns that make sound choppy).
struct FrameStats {
    static constexpr size_t kHistory = 128;
    struct Sample {
        float intervalMs = 0;  // wall time since the previous retro_run
        float workMs = 0;      // time spent inside retro_run
        float audio = -1;      // frontend audio buffer occupancy 0..1, negative if unreported
        float underrun = 0;    // 1 when the frontend expects an underrun
    };
    void Push(const Sample& sample) { samples[next] = sample; next = (next + 1) % kHistory; count += count < kHistory; }
    // Oldest first.
    const Sample& At(size_t i) const { return samples[(next + kHistory - count + i) % kHistory]; }
    size_t Count() const { return count; }
    float Fps(size_t window = 30) const;

private:
    std::array<Sample, kHistory> samples{};
    size_t next = 0, count = 0;
};

// Frame-stepped animation and sound; deterministic, GPU-free and host-testable.
class PrepareTimeline {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr double kFrameSeconds = 1.0 / 60.0;
    // Advances one video frame. `fraction` is the real work progress; `finished` once it succeeded.
    void Advance(double fraction, bool finished);
    // Stereo int16 samples for the frame just advanced (silence, or the completion sound).
    size_t Audio(int16_t* stereo, size_t maxFrames);
    const PrepareFrame& Frame() const { return frame; }
    // True once the screen has fully faded out and the game can start.
    bool Complete() const { return frame.fade >= 1.f; }

private:
    PrepareFrame frame;
    double time = 0, progress = 0, angle = 0, settleFrom = -1, settleStart = 0, done = -1;
    uint64_t soundSample = 0;
    bool sounding = false;
    double audioDebt = 0;
};

class PrepareScreen {
public:
    explicit PrepareScreen(wgpu::Device device);
    // Records and submits one frame into `target` (RGBA8Unorm, RenderAttachment usage). With
    // `stats`, a small frame-timing panel is drawn in the top-right corner.
    void Draw(const wgpu::Texture& target, const PrepareFrame& frame, const FrameStats* stats = nullptr);

private:
    wgpu::Device device;
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer uniforms, statsBuffer;
    wgpu::BindGroup bindings;
};
}  // namespace RuntimeFrontend
