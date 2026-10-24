#include "adapters.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace RuntimeFrontend {
namespace {
int16_t Read(retro_input_state_t input, unsigned device, unsigned index, unsigned id) {
    return input ? input(0, device, index, id) : 0;
}
float Normalize(int16_t value) {
    return std::clamp(static_cast<float>(value) / 32767.f, -1.f, 1.f);
}
}

InputSnapshot InputAdapter::Poll(retro_input_state_t input) {
    InputSnapshot result;
    auto down = [input](unsigned id) { return Read(input, RETRO_DEVICE_JOYPAD, 0, id) != 0; };
    auto set = [&result](bool pressed, Button button) {
        if (pressed) result.buttons |= static_cast<uint32_t>(button);
    };
    // Sticks stay raw: the runtime applies the desktop build's rules (eight-way sectors, a moving
    // pointer that lingers, arcade tilt), see frontend_runtime.cpp.
    result.leftStick[0] = Normalize(Read(input, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X));
    result.leftStick[1] = Normalize(Read(input, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y));
    result.rightStick[0] = Normalize(Read(input, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X));
    result.rightStick[1] = Normalize(Read(input, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));
    set(down(RETRO_DEVICE_ID_JOYPAD_UP), Up);
    set(down(RETRO_DEVICE_ID_JOYPAD_DOWN), Down);
    set(down(RETRO_DEVICE_ID_JOYPAD_LEFT), Left);
    set(down(RETRO_DEVICE_ID_JOYPAD_RIGHT), Right);
    set(down(RETRO_DEVICE_ID_JOYPAD_B), Two);
    set(down(RETRO_DEVICE_ID_JOYPAD_A) || down(RETRO_DEVICE_ID_JOYPAD_Y), One);
    set(down(RETRO_DEVICE_ID_JOYPAD_X), A);
    set(down(RETRO_DEVICE_ID_JOYPAD_L) || down(RETRO_DEVICE_ID_JOYPAD_SELECT), Minus);
    set(down(RETRO_DEVICE_ID_JOYPAD_START), Plus);
    set(down(RETRO_DEVICE_ID_JOYPAD_L2), B);
    result.shake = down(RETRO_DEVICE_ID_JOYPAD_R) || down(RETRO_DEVICE_ID_JOYPAD_R2);
    set(down(RETRO_DEVICE_ID_JOYPAD_L3), Home);

    if (Read(input, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0) {
        result.pointerX = Normalize(Read(input, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X));
        result.pointerY = Normalize(Read(input, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y));
        result.pointerValid = true;
    }
    return result;
}

const retro_input_descriptor* InputAdapter::Descriptors() {
    static const retro_input_descriptor descriptors[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Move X (tilt in minigames)"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Move Y"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "One"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Two (jump)"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "A"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "One"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Minus"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Minus"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Plus"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "B"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Shake"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Shake"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Home"},
        {0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED, "Touch"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Pointer X"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Pointer Y"},
        {0, 0, 0, 0, nullptr}
    };
    return descriptors;
}

size_t PcmAdapter::Push(const int16_t* samples, size_t frames, uint32_t rate) {
    if (!samples || (rate != 32000 && rate != 48000)) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (rate != lastRate_) {
        phase_ = 0;
        havePrevious_ = false;
        lastRate_ = rate;
    }
    for (size_t i = 0; i < frames; ++i) {
        const int16_t left = samples[2 * i], right = samples[2 * i + 1];
        const uint32_t phaseBefore = phase_;
        phase_ += 48000;
        uint32_t emitted = 0;
        while (phase_ >= rate) {
            phase_ -= rate;
            ++emitted;
            float fraction = static_cast<float>(emitted * rate - phaseBefore) / 48000.f;
            fraction = std::clamp(fraction, 0.f, 1.f);
            const int16_t outLeft = havePrevious_ ? static_cast<int16_t>(std::lrint(previousLeft_ + (left - previousLeft_) * fraction)) : left;
            const int16_t outRight = havePrevious_ ? static_cast<int16_t>(std::lrint(previousRight_ + (right - previousRight_) * fraction)) : right;
            if (size_ == Capacity) {
                ++dropped_;
            } else {
                size_t tail = (head_ + size_) % Capacity;
                queue_[2 * tail] = outLeft;
                queue_[2 * tail + 1] = outRight;
                ++size_;
            }
        }
        previousLeft_ = left;
        previousRight_ = right;
        havePrevious_ = true;
    }
    return frames;
}

size_t PcmAdapter::Sink(void* opaque, const int16_t* samples, size_t frames, uint32_t rate) {
    return opaque ? static_cast<PcmAdapter*>(opaque)->Push(samples, frames, rate) : 0;
}

size_t PcmAdapter::Drain(retro_audio_sample_batch_t batch, retro_audio_sample_t sample) {
    if (!batch && !sample) return 0;
    std::vector<int16_t> pending;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.resize(size_ * 2);
        for (size_t i = 0; i < size_; ++i) {
            size_t at = (head_ + i) % Capacity;
            pending[2 * i] = queue_[2 * at];
            pending[2 * i + 1] = queue_[2 * at + 1];
        }
        generation = generation_;
    }
    size_t consumed = 0;
    if (batch) {
        size_t offered = pending.size() / 2;
        size_t reported = batch(pending.data(), offered);
        consumed = std::min(reported, offered);
    } else {
        size_t offered = pending.size() / 2;
        for (; consumed < offered; ++consumed)
            sample(pending[2 * consumed], pending[2 * consumed + 1]);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ == generation) {
            size_t remove = std::min(consumed, size_);
            head_ = (head_ + remove) % Capacity;
            size_ -= remove;
        }
    }
    return consumed;
}

void PcmAdapter::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = size_ = 0;
    phase_ = lastRate_ = 0;
    previousLeft_ = previousRight_ = 0;
    havePrevious_ = false;
    ++generation_;
}
size_t PcmAdapter::QueuedFrames() const { std::lock_guard<std::mutex> lock(mutex_); return size_; }
uint64_t PcmAdapter::DroppedFrames() const { std::lock_guard<std::mutex> lock(mutex_); return dropped_; }

} // namespace RuntimeFrontend
