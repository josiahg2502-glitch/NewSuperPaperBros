#pragma once

#include "frontend/runtime_contract.h"
#include "libretro.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace RuntimeFrontend {

class InputAdapter {
public:
    InputSnapshot Poll(retro_input_state_t input);
    static const retro_input_descriptor* Descriptors();

};

class PcmAdapter {
public:
    size_t Push(const int16_t* samples, size_t frames, uint32_t rate);
    static size_t Sink(void* opaque, const int16_t* samples, size_t frames, uint32_t rate);
    size_t Drain(retro_audio_sample_batch_t batch, retro_audio_sample_t sample);
    void Clear();
    size_t QueuedFrames() const;
    uint64_t DroppedFrames() const;

private:
    static constexpr size_t Capacity = 8192;
    mutable std::mutex mutex_;
    std::array<int16_t, Capacity * 2> queue_{};
    size_t head_ = 0;
    size_t size_ = 0;
    uint64_t dropped_ = 0;
    uint32_t phase_ = 0;
    uint32_t lastRate_ = 0;
    int16_t previousLeft_ = 0;
    int16_t previousRight_ = 0;
    bool havePrevious_ = false;
    uint64_t generation_ = 0;
};

} // namespace RuntimeFrontend
