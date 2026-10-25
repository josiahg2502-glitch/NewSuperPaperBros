#include "platform/host_platform.h"

#include <iostream>

int main() {
    if (!RuntimePlatform::ExecutableDirectory()) {
        std::cerr << "unable to resolve the current executable directory\n";
        return 1;
    }

    const auto userData = RuntimePlatform::ApplicationDataDirectory("WiiCompiledPlatformPathsTest");
    if (userData.filename() != "WiiCompiledPlatformPathsTest") {
        std::cerr << "application-data directory lost its application name: " << userData << '\n';
        return 1;
    }
    if (RuntimePlatform::LogDirectory("WiiCompiledPlatformPathsTest") != userData / "Logs") {
        std::cerr << "log directory is not derived from application data\n";
        return 1;
    }
#if defined(__APPLE__)
    if (userData.parent_path().filename() != "Application Support" ||
        userData.parent_path().parent_path().filename() != "Library") {
        std::cerr << "macOS application-data directory is not under ~/Library/Application Support: "
                  << userData << '\n';
        return 1;
    }
#endif

    const auto executable = RuntimePlatform::ExecutableDirectory();
    RuntimePlatform::SetFrontendDirectories("/frontend/saves", "/frontend/system");
    if (RuntimePlatform::ApplicationDataDirectory("ignored") != "/frontend/saves" ||
        RuntimePlatform::ExecutableDirectory() != std::filesystem::path("/frontend/system") ||
        RuntimePlatform::LogDirectory("ignored") != "/frontend/saves/Logs") return 1;
    RuntimePlatform::ClearFrontendDirectories();
    if (RuntimePlatform::FrontendDataDirectory() ||
        RuntimePlatform::ApplicationDataDirectory("WiiCompiledPlatformPathsTest") != userData ||
        RuntimePlatform::ExecutableDirectory() != executable) return 1;
    return 0;
}
