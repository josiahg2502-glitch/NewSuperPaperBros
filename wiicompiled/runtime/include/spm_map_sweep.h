#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct CpuContext;

namespace SpmMapSweep {
void Tick(CpuContext* cpu, uint32_t presentedFrames);
// Debug menu: warp to `map` through seqSetSeq on the next frame the game is in a map (door may be null).
void RequestWarp(const char* map, const char* door);

// Debug menu actions, run on the game's own frame (the UI thread only queues them). Each one calls the game's own
// function (pouchAddItem, swSet, ...) so the game's state stays consistent.
void AddItem(int itemId);
void RemoveItem(int itemId);
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
    uint8_t pouch[0x6A0] = {};   // MarioPouchWork (spm-docs mario_pouch.h)
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
bool HasSavedPosition();
void Heal();             // HP = max HP
}
