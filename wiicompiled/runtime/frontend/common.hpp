#pragma once
#include <android/log.h>
#include <vulkan/vulkan.h>
#include <cstdarg>
#include <stdexcept>
#include <string>

namespace poc {
inline void log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, "DawnInterop", format, args);
    va_end(args);
}
inline void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
inline void vkcheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult=" + std::to_string(result));
}
#define VK_CHECK(call) ::poc::vkcheck((call), #call)
}
