#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

// Frontend callbacks and guest scheduling are owned by the retro_run thread.
// The audio producer may run on a worker; the PCM sink must copy before returning.
// GPU targets are borrowed for one frame and must remain live until its completion fence.
namespace RuntimeFrontend {

enum Button : uint32_t {
    Left = 1u << 0, Right = 1u << 1, Down = 1u << 2, Up = 1u << 3,
    Plus = 1u << 4, Two = 1u << 8, One = 1u << 9, B = 1u << 10,
    A = 1u << 11, Minus = 1u << 12, Home = 1u << 15,
};
struct InputSnapshot {
    uint32_t buttons = 0;  // D-pad as screen directions; the sticks are separate
    float leftStick[2] = {};   // -1..1, +x right, +y down
    float rightStick[2] = {};  // -1..1, +x right, +y down; moves the pointer
    float pointerX = 0.f; // touch position, normalized [-1, 1], left to right
    float pointerY = 0.f; // normalized [-1, 1], top to bottom
    bool pointerValid = false; // touching: the pointer jumps to pointerX/Y
    bool shake = false;
};
struct ContentPaths {
    std::filesystem::path discRoot; // extracted DATA folder containing sys/ and files/
    std::filesystem::path saveRoot; // frontend save directory / spm-r8pe01
    std::filesystem::path systemRoot; // frontend system directory / spm-r8pe01
};
struct VideoTarget {
    void* texture = nullptr; // borrowed WGPUTexture on Aurora's existing device
    uint32_t width = 640;
    uint32_t height = 480;
};
using PcmSink = size_t (*)(void* opaque, const int16_t* interleavedStereo,
                          size_t frames, uint32_t sampleRate);
struct Callbacks {
    PcmSink pcm = nullptr; // signed native-endian LR, 32000 or 48000 Hz
    void* opaque = nullptr;
};
enum class FrameResult { Presented, NoVideo, Stopped, Failed };

// Initialization must not enter the blocking standalone RuntimeMain entry.
// AdvanceOneFrame executes one original VI interval; the frontend owns pacing.
// Stop joins guest/render/audio workers; Unload releases all content-owned state.
// Reset is Stop + reinitialization with the same paths and callbacks.
bool Initialize(const ContentPaths&, const Callbacks&);
FrameResult AdvanceOneFrame(const InputSnapshot&, const VideoTarget&);
void Stop();
bool Reset();
void Unload();

} // namespace RuntimeFrontend
