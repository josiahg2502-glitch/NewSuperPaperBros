#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct CpuContext;

namespace SpmMapSweep {
void Tick(CpuContext* cpu, uint32_t presentedFrames);
// Debug menu: warp to `map` through seqSetSeq on the next frame the game is in a map (door may be null).
void RequestWarp(const char* map, const char* door, const float* arrivalPos = nullptr);   // arrivalPos: x y z, or none

// Debug menu actions, run on the game's own frame (the UI thread only queues them). Each one calls the game's own
// function (pouchAddItem, swSet, ...) so the game's state stays consistent.
void AddItem(int itemId);
void RemoveItem(int itemId);
void SetTippi(bool enabled);          // Tippi slot + guide on/off
void ReturnToTitle();                 // back to the title screen (from a map only)
// Options menu: freeze the game in a map (spmarioSystemLevel, as its own menus do), then put the level back.
void SetGamePaused(bool paused);
// The game's sequence: 0 logo, 1 title, 2 in a map, 3 map change, 4 game over, 5 file load; -1 before boot.
int Seq();
// Pause menu (pausewin) open, for faster menus.
bool PauseMenuOpen();
// Options > Save game: what a save block does (nandUpdateSave + nandWriteSave on the file in play), so loading the
// file puts Mario where he stood. Status: 0 idle, 1 writing, 2 saved, 3 could not save here.
void SaveGame();
int SaveStatus();
// Options > Cheats, applied on every game frame in a map.
void SetInfiniteHp(bool on);
void SetMaxCoins(bool on);
void RemoveCharOrPixl(int itemId);   // character / Pixl slot; not the character played or the Pixl in use
void Unlock(int itemId, bool pixl);   // add the character / Pixl item, then make it selectable
void AddCoins(int amount);
void SetHp(int hp, int maxHp);
void SetSystemFlag(int flag, bool on);   // GSWF
void SetGsw(int index, int value);       // GSW(n); n = 0 is the story position (sequence)
void ReadSystemFlag(int flag);           // result in LastResult()
int Gsw0();                              // current story position, -1 if the game is not running
int LastResult();

// Snapshot of the running game, refreshed on the game's frame while the UI keeps calling RequestState().
struct DebugState {
    bool valid = false;
    uint32_t seq = 0;
    char map[32] = {};
    float pos[3] = {};
    uint16_t held = 0;
    int curChar = -1;            // item id of the character played (216 Mario .. 219 Luigi)
    int curPixl = -1;            // item id of the Pixl in use
    bool tippi = false;          // Tippi (the guide) enabled
    uint8_t pouch[0x6A0] = {};   // the game's pouch work
};
void RequestState();
bool GetState(DebugState& out);
void SetPouch(int offset, int value);              // write a 32-bit field of the pouch (level, coins, ...)
void SetVar(int type, int index, int value);       // type: 0 GSW, 1 GSWF, 2 LSW, 3 LSWF
void ReadVar(int type, int index);

// Flag watch: while on, every 20 frames all GSWF / GSW values are compared with the previous pass and each change
// is logged ("GSWF 611: 0 -> 1"). Picking up a chest or an item shows which flag it sets.
void SetWatch(bool on);
bool WatchEnabled();
std::vector<std::string> GetWatchLog();
void ClearWatchLog();

// All 8192 GSWF values, refreshed on the game's frame while the UI keeps calling RequestFlagSnapshot().
void RequestFlagSnapshot();
bool FlagValue(int flag);

void SavePosition();     // remember Mario's x y z
void LoadPosition();     // put Mario back there
void TeleportTo(float x, float y, float z);
void SetGravity(int dir);   // 0 down, 1 up, 2 left, 3 right (marioSetGravity: the upside-down rooms of 4-4)
bool HasSavedPosition();
void Heal();             // HP = max HP
}
