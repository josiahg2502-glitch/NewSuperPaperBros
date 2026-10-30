#pragma once
#include <string>

// Built-in updater. A packaged game has engine/version.txt ("v1.0.5"); at start the latest GitHub release is
// checked in the background (curl, which Windows 10+ and Linux both ship). Install() downloads that release,
// unpacks it, and hands over to a small script that copies it over the game folder once the game has quit, then
// starts the game again. Saves, settings, mods and the disc are not in the download, so they are kept.
namespace Updater {
enum class State { Off, Checking, UpToDate, Available, Downloading, Failed };
void Start();               // the background check, once (does nothing without version.txt or when turned off)
State GetState();
std::string Current();      // "v1.0.5", empty for a development build
std::string Latest();       // the newer release's tag while State::Available
void Install();             // download in the background; on success the game quits into the update script
bool CheckEnabled();
void SetCheckEnabled(bool on);
} // namespace Updater
