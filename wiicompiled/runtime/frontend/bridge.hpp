#pragma once
#include "renderer.hpp"
#include <libretro_vulkan.h>
#include <android/hardware_buffer.h>
#include <array>
#include <memory>
#include <unistd.h>

namespace poc {
struct Fd {
    int value = -1;
    ~Fd() { reset(); }
    Fd() = default;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    void reset(int fd = -1) { if (value >= 0) close(value); value = fd; }
    int release() { int fd = value; value = -1; return fd; }
};
struct Slot {
    AHardwareBuffer* ahb = nullptr;
    wgpu::SharedTextureMemory memory;
    wgpu::Texture texture;
    Fd returned_fd;
    VkImage shared = VK_NULL_HANDLE, output = VK_NULL_HANDLE;
    VkDeviceMemory shared_memory = VK_NULL_HANDLE, output_memory = VK_NULL_HANDLE;
    retro_vulkan_image image{};
    VkSemaphore incoming = VK_NULL_HANDLE, returned = VK_NULL_HANDLE, ready = VK_NULL_HANDLE;
    VkFence complete = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    bool submitted = false, output_initialized = false, initialized = false;
    VkImageLayout return_old = VK_IMAGE_LAYOUT_UNDEFINED, return_new = VK_IMAGE_LAYOUT_UNDEFINED;
};
class Bridge {
public:
    Bridge(const retro_hw_render_interface_vulkan* frontend, Renderer& renderer, bool cpu_wait);
    ~Bridge();
    void frame(uint32_t w, uint32_t h);
    // Accumulated milliseconds per phase of frame(); the caller reads and clears them.
    struct Timing { double sync = 0, fence = 0, render = 0, submit = 0; } timing;
    void drain();
    void resize(uint32_t w, uint32_t h);
private:
    const retro_hw_render_interface_vulkan* vk;
    Renderer& dawn;
    bool cpu;
    uint32_t width = 0, height = 0, mask = 0;
    std::array<std::unique_ptr<Slot>,32> slots;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb;
    PFN_vkImportSemaphoreFdKHR import_fd;
    PFN_vkGetSemaphoreFdKHR export_fd;
    void create(Slot& slot);
    void destroy(Slot& slot);
    void clear();
};
// Used in negotiation, before requesting a frontend device with these features.
void probe_gpu(VkPhysicalDevice gpu);
bool create_device2(retro_vulkan_context*, VkInstance, VkPhysicalDevice, VkSurfaceKHR,
    PFN_vkGetInstanceProcAddr, retro_vulkan_create_device_wrapper_t, void*);
VkInstance create_instance(PFN_vkGetInstanceProcAddr, const VkApplicationInfo*,
    retro_vulkan_create_instance_wrapper_t, void*);
extern bool frontend_device_negotiated;
}
