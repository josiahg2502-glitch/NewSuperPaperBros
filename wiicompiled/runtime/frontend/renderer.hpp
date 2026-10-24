#pragma once
#include "common.hpp"
#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>
#include <atomic>
#include <memory>
#include <functional>
#include <vector>

namespace poc {
class Renderer {
public:
    // A private device that can share AHardwareBuffers; draws through set_output.
    Renderer();
    // Borrow an existing renderer device. Never create or destroy the caller's device.
    Renderer(wgpu::Instance instance, wgpu::Device device,
             std::function<void(const wgpu::Texture&)> output);
    ~Renderer();
    wgpu::Device device;
    std::atomic<bool> failed{false};
    void render(const wgpu::Texture& target);
    void set_output(std::function<void(const wgpu::Texture&)> output) { external_output = std::move(output); }
    void drain();
    void events();
private:
    void initialize();
    std::unique_ptr<dawn::native::Instance> native;
    wgpu::Instance instance;
    std::function<void(const wgpu::Texture&)> external_output;
    bool owns_device = true;
};
}
