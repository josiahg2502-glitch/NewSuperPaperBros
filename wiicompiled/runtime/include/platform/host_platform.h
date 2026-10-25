#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Small host-services boundary for functionality that must not leak Win32
// assumptions into runtime or game code. Guest execution, virtual memory, and
// cooperative contexts remain outside this layer until dedicated macOS
// prototypes establish a safe abstraction.
namespace RuntimePlatform {

// Explicit frontend paths take precedence over process environment and working directory.
void SetFrontendDirectories(std::filesystem::path data, std::filesystem::path resources);
void ClearFrontendDirectories();
std::optional<std::filesystem::path> FrontendDataDirectory();

std::optional<std::filesystem::path> ExecutableDirectory() noexcept;

// Returns the platform's conventional per-user application-data directory.
// It does not create the directory, leaving that policy to the caller.
std::filesystem::path ApplicationDataDirectory(std::string_view applicationName);

// The root for per-run diagnostics. Keeping this here ensures log placement
// follows the same host convention as configuration and other user data.
std::filesystem::path LogDirectory(std::string_view applicationName);

uint64_t CurrentProcessId() noexcept;

// Big.LITTLE Linux/Android: keep the calling thread off the cores with the lowest maximum clock, where
// schedulers otherwise often place the game thread. Returns a log line, empty when nothing was done.
std::string PinCurrentThreadToFastCores();

} // namespace RuntimePlatform
