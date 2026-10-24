#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

// Reads a Wii .wbfs disc image and unpacks its data partition into the extracted layout the DVD
// layer serves (sys/ and files/). A C++ port of tools/extract_wbfs.py for the libretro core.
namespace RuntimeFrontend::DiscImage {
struct Identity {
    std::string gameId;  // six characters, e.g. R8PE01
    int revision = -1;
};
struct Progress {
    std::atomic<uint64_t> done{0}, total{0};  // bytes
    std::atomic<uint32_t> filesDone{0}, files{0};
    std::atomic<bool> cancel{false};
    // Fraction of the expected time spent so far. Each file also costs a fixed open/close time
    // (~17 ms on Android shared storage, as much as ~1.5 MB of data), so bytes alone run ahead.
    double Fraction() const {
        constexpr double kFileBytes = 1.5 * 1024 * 1024;
        const double whole = double(total) + kFileBytes * files;
        return whole > 0 ? (double(done) + kFileBytes * filesDone) / whole : 0.0;
    }
    uint64_t decryptNanos = 0, writeNanos = 0;  // summed over unpacking threads, for logs
};

// The unencrypted disc header. Throws if the file is not a readable WBFS image.
Identity ReadIdentity(const std::filesystem::path& wbfs);
// Unpacks into `destination`, which must not exist yet. Throws on cancel, write failure or file
// data that the image does not store (an incomplete dump).
void Extract(const std::filesystem::path& wbfs, const std::filesystem::path& destination, Progress& progress);

// `<parent>/disc` once a completed extraction of this identity is there, else empty.
std::filesystem::path CachedRoot(const std::filesystem::path& parent, const Identity& identity);
// Extracts into `<parent>/disc` and writes the completion marker last, so an interrupted run is
// never mistaken for a complete one. A stale or interrupted cache is removed first. (Renaming a
// finished folder into place costs tens of seconds on Android shared storage.)
std::filesystem::path ExtractToCache(const std::filesystem::path& wbfs, const std::filesystem::path& parent,
                                     const Identity& identity, Progress& progress);
}  // namespace RuntimeFrontend::DiscImage
