#pragma once
// Mods that change the game while it runs, beyond replaced files and textures:
//  - native code mods: <mods>/<name>/code/*.dll|*.so built against spm_mod_api.h (hooks on game functions, a frame
//    callback, memory access, calling game functions);
//  - Gecko cheat codes in Dolphin's .ini format ([Gecko] / [Gecko_Enabled]) from <UserData>/cheats or a mod folder:
//    the RAM-write, conditional and pointer code types; ones that run PowerPC code (C0/C2...) are listed as unsupported;
//  - Riivolution <memory> patches of a mod's XML (riivolution.cpp collects them).
// The recomp runs translated code, so a patch or code that rewrites the game's own instructions has no effect; those
// are marked as such.
#include <cstdint>
#include <string>
#include <vector>

struct CpuContext;

namespace ModRuntime {
// At start, before the game runs: loads the enabled code mods (their hooks are set up here) and the cheat codes.
void Load();
// After OSInit: Riivolution memory patches inside the main program (before its data is used).
void ApplyEarlyPatches();
// Every game frame on the game's thread: frame callbacks, Riivolution patches once the game is up, cheat codes.
void Frame(CpuContext* cpu);

struct CheatCode {
    std::string name;
    std::string note;        // the code's own comment lines, or why it cannot run here
    bool enabled = false;
    bool supported = true;   // false: it runs PowerPC code, or only rewrites the game's instructions
};
std::vector<CheatCode> CheatCodes();
void SetCheatEnabled(const std::string& name, bool enabled);  // saved to <UserData>/cheats/enabled.txt
std::string CheatFolder();                                   // UTF-8, for the Options menu

// SPM_SELFTEST: the Gecko interpreter on a scratch buffer.
bool SelfCheck();
} // namespace ModRuntime
// The hook check on every game function call is ModHooks (abi_bridge.h ApplyRuntimeCallOptions), only consulted
// once a code mod has hooked something.
