#pragma once
#include <filesystem>
#include <functional>
#include <string>

// A Wii disc image (.iso, .ciso or .wbfs): decodes the container, decrypts the game partition and extracts it to
// the layout the game reads (sys/ and files/). Port of the old launcher's WiiDisc.cs (tools/extract_wbfs.py).
namespace DiscImage {
struct Info {
    std::string id;      // "R8PE01"
    int revision = 0;
    std::string title;
};
// Reads the header and checks the game partition decrypts. False with `error` when it isn't a usable Wii image.
bool Probe(const std::filesystem::path& image, Info& info, std::string& error);
// Writes sys/ and files/ under `outDir`; `progress(done, total)` after each file.
bool Extract(const std::filesystem::path& image, const std::filesystem::path& outDir,
             const std::function<void(int, int)>& progress, std::string& error);
} // namespace DiscImage
