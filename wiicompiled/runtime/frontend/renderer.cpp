#include "renderer.hpp"
#include <dawn/native/VulkanBackend.h>
#include <limits>

namespace poc {
namespace {
std::string str(wgpu::StringView s) { return s.data ? std::string(s.data, s.length == WGPU_STRLEN ? std::char_traits<char>::length(s.data) : s.length) : ""; }
}
Renderer::Renderer() {
    try { initialize(); }
    catch(...) {
        if(device) { device.Destroy(); instance.ProcessEvents(); }
        throw;
    }
}
void Renderer::initialize() {
    dawn::native::DawnInstanceDescriptor backend{};
    backend.backendValidationLevel = dawn::native::Disabled;
    backend.SetLoggingCallback([](wgpu::LoggingType, wgpu::StringView message) { log("Dawn: %s", str(message).c_str()); });
    wgpu::InstanceFeatureName instance_features[] = {wgpu::InstanceFeatureName::TimedWaitAny};
    wgpu::InstanceDescriptor instance_desc{};
    instance_desc.nextInChain = &backend;
    instance_desc.requiredFeatureCount = 1;
    instance_desc.requiredFeatures = instance_features;
    native = std::make_unique<dawn::native::Instance>(&instance_desc);
    instance = wgpu::Instance(native->Get());
    wgpu::RequestAdapterOptions options{};
    options.backendType = wgpu::BackendType::Vulkan;
    options.forceFallbackAdapter = false;
    const char* toggles[] = {"allow_unsafe_apis"};
    wgpu::DawnTogglesDescriptor toggles_desc{};
    toggles_desc.enabledToggleCount = 1;
    toggles_desc.enabledToggles = toggles;
    // Experimental native sharing features must be visible on the adapter too.
    options.nextInChain = &toggles_desc;
    auto adapters = native->EnumerateAdapters(&options);
    for (auto& a : adapters) {
        wgpu::Adapter adapter(a.Get());
        wgpu::AdapterInfo info{};
        require(adapter.GetInfo(&info) == wgpu::Status::Success, "Dawn adapter info failed");
        if (info.adapterType == wgpu::AdapterType::CPU || info.adapterType == wgpu::AdapterType::Unknown) continue;
        const wgpu::FeatureName features[] = {wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer, wgpu::FeatureName::SharedFenceSyncFD};
        if (!adapter.HasFeature(features[0]) || !adapter.HasFeature(features[1])) continue;
        wgpu::DeviceDescriptor desc{};
        desc.nextInChain = &toggles_desc;
        desc.requiredFeatureCount = 2;
        desc.requiredFeatures = features;
        desc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message, Renderer* self) {
            log("Dawn ERROR type=%u %s", unsigned(type), str(message).c_str()); self->failed = true;
        }, this);
        desc.SetDeviceLostCallback(wgpu::CallbackMode::AllowProcessEvents,
            [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message, Renderer* self) {
                if (reason != wgpu::DeviceLostReason::Destroyed) {
                    log("Dawn DEVICE LOST reason=%u %s", unsigned(reason), str(message).c_str()); self->failed = true;
                }
            }, this);
        device = wgpu::Device::Acquire(a.CreateDevice(&desc));
        if (device) break;
    }
    require(bool(device), "No hardware Vulkan Dawn adapter with the requested features; software adapters rejected");
    events();
}

Renderer::Renderer(wgpu::Instance existing_instance, wgpu::Device existing_device,
                   std::function<void(const wgpu::Texture&)> output)
    : device(std::move(existing_device)), instance(std::move(existing_instance)),
      external_output(std::move(output)), owns_device(false) {
    require(device && instance && bool(external_output), "Missing external Dawn device/output");
    require(device.HasFeature(wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer) &&
            device.HasFeature(wgpu::FeatureName::SharedFenceSyncFD), "External Dawn device lacks AHB/SyncFD");
}
Renderer::~Renderer() {
    if (device && owns_device) {
        // Callers drain first; Destroy also cancels any loss callbacks while `this` lives.
        device.Destroy();
        instance.ProcessEvents();
    }
}
void Renderer::events() {
    instance.ProcessEvents();
    require(!failed, "Dawn validation/device error (see logcat)");
}
void Renderer::drain() {
    if (!device) return;
    bool done = false;
    auto future = device.GetQueue().OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
        [](wgpu::QueueWorkDoneStatus status, wgpu::StringView, bool* success) { *success = status == wgpu::QueueWorkDoneStatus::Success; }, &done);
    auto status = instance.WaitAny(future, std::numeric_limits<uint64_t>::max());
    require(status == wgpu::WaitStatus::Success && done, "Dawn queue drain failed");
}
void Renderer::render(const wgpu::Texture& target) {
    events();
    require(bool(external_output), "Renderer has no output");
    external_output(target);
    events();
}
}
