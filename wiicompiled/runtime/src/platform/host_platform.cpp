#include "platform/host_platform.h"
#include <algorithm>
#include <fstream>
#include <string>
#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <pwd.h>
#endif

namespace RuntimePlatform {
namespace {
std::optional<std::filesystem::path> frontendData, frontendResources;
}
void SetFrontendDirectories(std::filesystem::path data, std::filesystem::path resources) {
    frontendData = std::move(data); frontendResources = std::move(resources);
}
void ClearFrontendDirectories() { frontendData.reset(); frontendResources.reset(); }
std::optional<std::filesystem::path> FrontendDataDirectory() { return frontendData; }


std::optional<std::filesystem::path> ExecutableDirectory() noexcept {
    if (frontendResources) return frontendResources;
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) {
        return std::nullopt;
    }
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return std::nullopt;
    }
    path.resize(std::char_traits<char>::length(path.c_str()));
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(path, ec);
    return (ec ? std::filesystem::path(path) : resolved).parent_path();
#elif defined(__ANDROID__)
    // The game is a library inside an APK, so there is no executable directory. The app unpacks
    // the files a desktop build keeps beside its executable (DSP ROM, pipeline cache, Wii
    // bootstrap NAND, CA bundle) and names that directory before SDL starts the game.
    if (const char* directory = std::getenv("WIICOMPILED_RUNTIME_DIR"); directory && *directory) {
        return std::filesystem::path(directory);
    }
    return std::nullopt;
#else
    return std::nullopt;
#endif
}

std::filesystem::path ApplicationDataDirectory(std::string_view applicationName) {
    if (frontendData) return *frontendData;
#if defined(_WIN32)
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath)) && rawPath) {
        const std::filesystem::path directory = std::filesystem::path(rawPath) / applicationName;
        CoTaskMemFree(rawPath);
        return directory;
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / applicationName;
    }
    if (const passwd* user = getpwuid(getuid()); user && user->pw_dir && *user->pw_dir) {
        return std::filesystem::path(user->pw_dir) / "Library" / "Application Support" / applicationName;
    }
#elif defined(__ANDROID__)
    // The app's own storage (its external files directory, which a PC can reach over USB) already
    // belongs to this game alone, so it is used as-is rather than gaining an application subfolder.
    (void)applicationName;
    if (const char* directory = std::getenv("WIICOMPILED_DATA_DIR"); directory && *directory) {
        return std::filesystem::path(directory);
    }
#endif
    return std::filesystem::current_path() / applicationName;
}

std::filesystem::path LogDirectory(std::string_view applicationName) {
    return ApplicationDataDirectory(applicationName) / "Logs";
}

uint64_t CurrentProcessId() noexcept {
#if defined(_WIN32)
    return static_cast<uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

std::string PinCurrentThreadToFastCores() {
#if defined(__linux__)
    long frequency[CPU_SETSIZE] = {};
    long lowest = 0, highest = 0;
    const int count = std::min<int>(static_cast<int>(sysconf(_SC_NPROCESSORS_CONF)), CPU_SETSIZE);
    for (int cpu = 0; cpu < count; ++cpu) {
        std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/cpuinfo_max_freq");
        if (!(in >> frequency[cpu])) frequency[cpu] = 0;
        if (frequency[cpu] && (!lowest || frequency[cpu] < lowest)) lowest = frequency[cpu];
        highest = std::max(highest, frequency[cpu]);
    }
    if (!lowest || lowest == highest) return {};  // one cluster: nothing to avoid
    cpu_set_t set;
    CPU_ZERO(&set);
    std::string list;
    for (int cpu = 0; cpu < count; ++cpu)
        if (frequency[cpu] > lowest) { CPU_SET(cpu, &set); list += (list.empty() ? "" : ",") + std::to_string(cpu); }
    const bool pinned = sched_setaffinity(0, sizeof(set), &set) == 0;
    return std::string(pinned ? "pinned" : "pin failed") + ": cpus " + list + " (max " + std::to_string(highest) +
           " kHz, avoided " + std::to_string(lowest) + " kHz)";
#else
    return {};
#endif
}

} // namespace RuntimePlatform
