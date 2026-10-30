#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Mods live in <UserData>/mods/<name>: files/ replaces game files (riivolution.cpp), textures/ replaces textures.
// A .zip dropped in the mods folder is unpacked into its own mod folder at the next start; a file named "disabled"
// inside a mod folder turns it off (the Options menu's Mods page).
namespace ModInstall {
struct Mod {
    std::string name;
    bool enabled = true;
};
std::filesystem::path Folder();
void InstallDroppedZips();  // at start, before the game reads its files
std::vector<Mod> List();
void SetEnabled(const std::string& name, bool enabled);
// Unpacks `zip` into `dest`, refusing entries that would land outside it. False with `error` on failure.
bool ExtractZip(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string& error);
// SPM_SELFTEST=1: a wrapped mod zip unpacks, and one slipping out of its folder is refused.
bool SelfCheck();
} // namespace ModInstall
