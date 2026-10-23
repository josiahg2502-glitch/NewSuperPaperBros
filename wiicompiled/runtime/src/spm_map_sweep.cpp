extern "C" unsigned g_spmAudit[8];
extern "C" unsigned g_spmGameClear;
extern "C" int g_spmSweepDump;
#include "spm_map_sweep.h"
#include "abi_bridge.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <tuple>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <string>
#include <vector>

namespace SpmMapSweep {
namespace {
using Clock = std::chrono::steady_clock;
// Diagnostic (env SPM_MTX_SELFTEST): runs the game's own matrix/vector routines through the recompiled code and compares
// with plain C++ math. Any "MISMATCH" line names a routine whose recompiled result is wrong.
// SPM_MATH_SELFTEST=1: recompiled math helpers vs host math, logged as [mathtest] (movement debugging).
void RunMathSelfTest(CpuContext* cpu) {
    CpuContext a = *cpu;
    a.gpr[3] = 0;
    a.gpr[4] = 64;
    InvokeIndirectCpu(0x801A5634, &a);  // __memAlloc(HEAP_MAIN, 64)
    const uint32_t base = a.gpr[3];
    if (!base || !Memory::Contains(base, 64)) return;
    auto call = [&](uint32_t fn, std::initializer_list<double> f, uint32_t r3 = 0, uint32_t r4 = 0) {
        CpuContext c = *cpu;
        c.gpr[3] = r3; c.gpr[4] = r4;
        int i = 1;
        for (double d : f) c.fpr[i++].d = d;
        InvokeIndirectCpu(fn, &c);
        return c.fpr[1].d;
    };
    for (double d : {0.0, 0.5, 1.5707963, 2.0, 3.1415927, 4.712389, -1.5707963, 6.0}) {
        RT_LOGF(RT_TAG_RUNTIME, "[mathtest] x=%g sin %g (host %g) cos %g (host %g) floor %g fmod2 %g\n", d, call(0x80261A5C, {d}), std::sin(d),
                call(0x80261650, {d}), std::cos(d), call(0x80261724, {d}), call(0x80261BB4, {d, 2.0}));
    }
    for (double deg : {0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0, -90.0, 450.0}) {
        call(0x8019BA28, {deg}, base, base + 4);  // sincosf(angle, &s, &c)
        RT_LOGF(RT_TAG_RUNTIME, "[mathtest] deg=%g sincosf s %g c %g | reviseAngle %g | angleABf(0,0,sin,-cos) %g\n", deg, Memory::ReadFloat32(base),
                Memory::ReadFloat32(base + 4), call(0x8019B7A8, {deg}),
                call(0x8019B874, {0.0, 0.0, std::sin(deg * 3.14159265 / 180), -std::cos(deg * 3.14159265 / 180)}));
    }
}

void RunMtxSelfTest(CpuContext* cpu) {
    CpuContext a = *cpu;
    a.gpr[3] = 0;
    a.gpr[4] = 1024;
    InvokeIndirectCpu(0x801A5634, &a);  // __memAlloc(HEAP_MAIN, 1024)
    const uint32_t base = a.gpr[3];
    if (!base || !Memory::Contains(base, 1024)) {
        RT_LOGF(RT_TAG_RUNTIME, "[mtxtest] alloc failed\n");
        return;
    }
    auto put = [&](uint32_t off, const float* v, int n) { for (int i = 0; i < n; ++i) Memory::WriteFloat32(base + off + i * 4, v[i]); };
    auto get = [&](uint32_t off, float* v, int n) { for (int i = 0; i < n; ++i) v[i] = Memory::ReadFloat32(base + off + i * 4); };
    auto call = [&](uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, std::initializer_list<double> f) {
        CpuContext c = *cpu;
        c.gpr[3] = r3; c.gpr[4] = r4; c.gpr[5] = r5; c.gpr[6] = r6;
        int i = 1;
        for (double d : f) c.fpr[i++].d = d;
        InvokeIndirectCpu(fn, &c);
    };
    int bad = 0;
    auto cmp = [&](const char* name, uint32_t off, const float* exp, int n) {
        float got[16] = {};
        get(off, got, n);
        float md = 0, mx = 1;
        for (int i = 0; i < n; ++i) { md = std::max(md, std::fabs(got[i] - exp[i])); mx = std::max(mx, std::fabs(exp[i])); }
        const bool ok = md <= 1e-3f * mx;
        bad += !ok;
        RT_LOGF(RT_TAG_RUNTIME, "[mtxtest] %s %s maxdiff=%g got=[%g %g %g %g] exp=[%g %g %g %g]\n", ok ? "ok      " : "MISMATCH", name, md,
                got[0], got[1], got[2], got[3], exp[0], exp[1], exp[2], exp[3]);
    };
    const uint32_t A = 0x000, B = 0x040, R = 0x080, V = 0x0C0, VO = 0x0D0, CAM = 0x100, UP = 0x110, TGT = 0x120, M4 = 0x140;
    const float ma[12] = {0.8f, 0.1f, -0.3f, 5.f, 0.2f, 0.9f, 0.4f, -7.f, -0.1f, 0.3f, 1.1f, 2.f};
    const float mb[12] = {1.2f, -0.4f, 0.5f, 3.f, 0.3f, 0.7f, -0.2f, 11.f, 0.6f, 0.1f, 0.9f, -4.f};
    put(A, ma, 12); put(B, mb, 12);
    float e[16] = {};
    // Concat
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) { float v = j == 3 ? ma[i * 4 + 3] : 0; for (int k = 0; k < 3; ++k) v += ma[i * 4 + k] * mb[k * 4 + j]; e[i * 4 + j] = v; }
    call(0x80277934, base + A, base + B, base + R, 0, {});
    cmp("PSMTXConcat", R, e, 12);
    // Inverse
    {
        const float* m = ma;
        float c00 = m[5] * m[10] - m[6] * m[9], c01 = m[2] * m[9] - m[1] * m[10], c02 = m[1] * m[6] - m[2] * m[5];
        float c10 = m[6] * m[8] - m[4] * m[10], c11 = m[0] * m[10] - m[2] * m[8], c12 = m[2] * m[4] - m[0] * m[6];
        float c20 = m[4] * m[9] - m[5] * m[8], c21 = m[1] * m[8] - m[0] * m[9], c22 = m[0] * m[5] - m[1] * m[4];
        const float det = m[0] * c00 + m[1] * c10 + m[2] * c20;
        const float r[9] = {c00 / det, c01 / det, c02 / det, c10 / det, c11 / det, c12 / det, c20 / det, c21 / det, c22 / det};
        for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) e[i * 4 + j] = r[i * 3 + j]; e[i * 4 + 3] = -(r[i * 3] * m[3] + r[i * 3 + 1] * m[7] + r[i * 3 + 2] * m[11]); }
        call(0x80277A00, base + A, base + R, 0, 0, {});
        cmp("PSMTXInverse", R, e, 12);
    }
    // MultVec
    {
        const float v[3] = {3.f, -2.f, 7.f};
        put(V, v, 3);
        for (int i = 0; i < 3; ++i) e[i] = ma[i * 4] * v[0] + ma[i * 4 + 1] * v[1] + ma[i * 4 + 2] * v[2] + ma[i * 4 + 3];
        call(0x80278354, base + A, base + V, base + VO, 0, {});
        cmp("PSMTXMultVec", VO, e, 3);
        const float cr[3] = {v[1] * 4.f - v[2] * 0.5f, v[2] * 1.f - v[0] * 4.f, v[0] * 0.5f - v[1] * 1.f};
        const float w[3] = {1.f, 0.5f, 4.f};
        put(R, w, 3);
        call(0x80278978, base + V, base + R, base + VO, 0, {});
        const float ce[3] = {v[1] * w[2] - v[2] * w[1], v[2] * w[0] - v[0] * w[2], v[0] * w[1] - v[1] * w[0]};
        (void)cr;
        cmp("PSVECCrossProduct", VO, ce, 3);
        call(0x802788B8, base + V, base + VO, 0, 0, {});
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        const float ne[3] = {v[0] / len, v[1] / len, v[2] / len};
        cmp("PSVECNormalize", VO, ne, 3);
    }
    // LookAt
    {
        const float cam[3] = {10.f, 20.f, 830.f}, up[3] = {0.f, 1.f, 0.f}, tgt[3] = {10.f, 20.f, 30.f};
        put(CAM, cam, 3); put(UP, up, 3); put(TGT, tgt, 3);
        float look[3] = {cam[0] - tgt[0], cam[1] - tgt[1], cam[2] - tgt[2]};
        auto norm = [](float* v) { const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); v[0] /= l; v[1] /= l; v[2] /= l; };
        norm(look);
        float right[3] = {up[1] * look[2] - up[2] * look[1], up[2] * look[0] - up[0] * look[2], up[0] * look[1] - up[1] * look[0]};
        norm(right);
        const float u[3] = {look[1] * right[2] - look[2] * right[1], look[2] * right[0] - look[0] * right[2], look[0] * right[1] - look[1] * right[0]};
        const float* rows[3] = {right, u, look};
        for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) e[i * 4 + j] = rows[i][j]; e[i * 4 + 3] = -(rows[i][0] * cam[0] + rows[i][1] * cam[1] + rows[i][2] * cam[2]); }
        call(0x80277FBC, base + R, base + CAM, base + UP, base + TGT, {});
        cmp("C_MTXLookAt", R, e, 12);
    }
    // Trans / Scale / RotRad(y)
    {
        call(0x80277E18, base + R, 0, 0, 0, {12.5, -34.25, 800.0});
        const float te[12] = {1, 0, 0, 12.5f, 0, 1, 0, -34.25f, 0, 0, 1, 800.f};
        cmp("PSMTXTrans", R, te, 12);
        call(0x80277E98, base + R, 0, 0, 0, {2.0, 3.0, 0.5});
        const float se[12] = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0.5f, 0};
        cmp("PSMTXScale", R, se, 12);
        const double ang = 0.6;
        call(0x80277BC0, base + R, 'y', 0, 0, {ang});
        const float c = std::cos(static_cast<float>(ang)), sn = std::sin(static_cast<float>(ang));
        const float re[12] = {c, 0, sn, 0, 0, 1, 0, 0, -sn, 0, c, 0};
        cmp("PSMTXRotRad(y)", R, re, 12);
    }
    // Ortho (4x4): t,b,l,r,n,f
    {
        call(0x80278610, base + M4, 0, 0, 0, {143.0, -143.0, -250.0, 250.0, 0.0, 10000.0});
        const float oe[16] = {2.f / 500.f, 0, 0, 0, 0, 2.f / 286.f, 0, 0, 0, 0, -1.f / 10000.f, -1.f, 0, 0, 0, 1};
        float tmp[16] = {2.f / 500.f, 0, 0, 0, 0, 2.f / 286.f, 0, 0, 0, 0, -1.f / 10000.f, -0.f, 0, 0, 0, 1};
        (void)tmp;
        float got[16];
        get(M4, got, 16);
        const float exp[16] = {oe[0], 0, 0, 0, 0, oe[5], 0, 0, 0, 0, oe[10], -0.f, 0, 0, 0, 1};
        RT_LOGF(RT_TAG_RUNTIME, "[mtxtest] C_MTXOrtho raw: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g (expect m00=%g m11=%g m22=%g m23=%g)\n",
                got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7], got[8], got[9], got[10], got[11], got[12], got[13], got[14], got[15],
                exp[0], exp[5], exp[10], -0.f);
    }
    RT_LOGF(RT_TAG_RUNTIME, "[mtxtest] DONE mismatches=%d\n", bad);
}


// US0 seqSetSeq (0x8017B3C8) stores seq/map/entry; seq_gameInit reads gp->mapName at +0x44.
constexpr uint32_t kNowSeq = 0x8056D0D8, kNextSeq = 0x8056D0DC, kGp = 0x8056D198;

// Debug menu warps (settings_overlay.cpp): the request is stored by the UI and served here, on the game's own frame.
std::mutex g_warpMutex;
bool g_warpPending = false, g_warpHasDoor = false;
bool g_warpHasPos = false;
float g_warpPos[3] = {};
int g_arrivalStage = 0;        // 1: warp issued, 2: new map loading, then Mario is placed at g_arrivalPos
float g_arrivalPos[3] = {};
char g_warpMap[32], g_warpDoor[32];
std::vector<std::function<void(CpuContext*)>> g_jobs;
std::atomic<int> g_lastResult{0};

uint32_t CallGuest(CpuContext* cpu, uint32_t fn, uint32_t r3, uint32_t r4 = 0) {
    CpuContext call = *cpu;
    call.gpr[3] = r3;
    call.gpr[4] = r4;
    InvokeIndirectCpu(fn, &call);
    return call.gpr[3];
}

void Enqueue(std::function<void(CpuContext*)> job) {
    std::lock_guard<std::mutex> lock(g_warpMutex);
    g_jobs.push_back(std::move(job));
}

void RunJobs(CpuContext* cpu) {
    std::vector<std::function<void(CpuContext*)>> jobs;
    {
        std::lock_guard<std::mutex> lock(g_warpMutex);
        uint32_t now = 0, after = 0;
        if (g_jobs.empty() || !Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after) || now != 2 || after != 2) return;
        jobs.swap(g_jobs);
    }
    for (auto& job : jobs) job(cpu);
}

void WatchStep(CpuContext* cpu);

// A map change started before the game has finished loading its effects hangs at stage 11 of the map change (nothing
// is ever shown). The sweep tool waits for the same state before it warps, so the Debug menu does too, and it also
// waits for the map to have been running for a moment.
[[maybe_unused]] bool WarpEffectsReady() {
    uint32_t effects = 0, pending = 0, ready = 0, data = 0, a = 0, b = 0;
    Memory::TryRead32(0x8056CFB0, effects);
    if (!effects || !Memory::Contains(effects, 16)) return false;
    Memory::TryRead32(effects + 8, pending);
    Memory::TryRead32(effects + 12, ready);
    Memory::TryRead32(0x8056D804, data);
    if (!pending || !ready || !data || !Memory::Contains(data, 20)) return false;
    Memory::TryRead32(data + 12, a);
    Memory::TryRead32(data + 16, b);
    return a && b;
}

void ServiceWarp(CpuContext* cpu) {
    if (!cpu) return;
    RunJobs(cpu);
    WatchStep(cpu);
    static uint32_t inMapFrames = 0;
    {
        uint32_t now = 0, after = 0;
        if (Memory::TryRead32(kNowSeq, now) && Memory::TryRead32(kNextSeq, after) && now == 2 && after == 2) ++inMapFrames;
        else inMapFrames = 0;
    }
    if (g_arrivalStage == 1 && inMapFrames == 0) g_arrivalStage = 2;
    if (g_arrivalStage == 2) {
        // Place Mario only once the arrival is over: the door/pipe entrance moves Mario itself and waits for him, so a
        // teleport during it left the event waiting forever. Wait for marioWork.ctrl (0x34) and keyOff (0x35) at 0
        // and a position unchanged for 10 frames; give up after 15 s.
        static float last[3] = {};
        static uint32_t still = 0;
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);                                                  // marioGetPtr
        if (inMapFrames > 900 || !Memory::Contains(mw, 0x70)) {
            g_arrivalStage = 0;
        } else if (inMapFrames >= 30) {
            float pos[3];
            for (int i = 0; i < 3; ++i) pos[i] = Memory::ReadFloat32(mw + 0x5C + 4 * i);
            const bool control = Memory::Read8(mw + 0x34) == 0 && Memory::Read8(mw + 0x35) == 0;
            still = control && pos[0] == last[0] && pos[1] == last[1] && pos[2] == last[2] ? still + 1 : 0;
            std::memcpy(last, pos, sizeof last);
            if (still >= 10) {
                g_arrivalStage = 0;
                still = 0;
                for (int i = 0; i < 3; ++i) Memory::WriteFloat32(mw + 0x5C + 4 * i, g_arrivalPos[i]);
            }
        }
    }
    char map[32], door[32];
    bool hasDoor;
    {
        std::lock_guard<std::mutex> lock(g_warpMutex);
        if (!g_warpPending) return;
        uint32_t now = 0, after = 0;
        if (!Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after) || now != 2 || after != 2) return;
        if (inMapFrames < 120) return;   // WarpEffectsReady() is only true during cutscenes, so it cannot gate normal play
        std::memcpy(map, g_warpMap, sizeof map);
        std::memcpy(door, g_warpDoor, sizeof door);
        hasDoor = g_warpHasDoor;
        g_warpPending = false;
        g_arrivalStage = g_warpHasPos ? 1 : 0;
        std::memcpy(g_arrivalPos, g_warpPos, sizeof g_arrivalPos);
    }
    static uint32_t buffer = 0;
    if (!buffer) {
        CpuContext call = *cpu;
        call.gpr[3] = 0;  // __memAlloc(HEAP_MAIN, 64), kept across map changes
        call.gpr[4] = 64;
        InvokeIndirectCpu(0x801A5634, &call);
        buffer = call.gpr[3];
    }
    if (!buffer || !Memory::Contains(buffer, 64)) return;
    for (uint32_t i = 0; i < 32; ++i) {
        Memory::Write8(buffer + i, uint8_t(map[i]));
        Memory::Write8(buffer + 32 + i, uint8_t(door[i]));
    }
    CpuContext call = *cpu;
    call.gpr[3] = 3;  // seqSetSeq(SEQ_MAPCHANGE, mapName, doorName)
    call.gpr[4] = buffer;
    call.gpr[5] = hasDoor ? buffer + 32 : 0;
    InvokeIndirectCpu(0x8017B3C8, &call);
    RT_LOGF(RT_TAG_RUNTIME, "[spmwarp] %s %s\n", map, hasDoor ? door : "(default door)");
}

struct Sweep {
    bool enabled = false, pending = false, changed = false, sampling = false;
    bool startOnly = false;  // start_map.txt: one warp for the player, no sampling, no exits
    int startGsw0 = -1;
    std::vector<std::string> maps;
    size_t next = 0;
    uint32_t buffer = 0, firstFrame = 0;
    int dwellSeconds = 3;
    Clock::time_point started = Clock::now(), requested{}, stable{};

    Sweep() {
        const char* path = std::getenv("SPM_MAP_SWEEP_FILE");
        if (!path || !*path) {
            // start_map.txt next to the exe: "<map> [gsw0]". Once the player is in any map, warp there once and delete
            // the file (a test shortcut; saving afterwards keeps the story value it sets).
            std::ifstream start("start_map.txt");
            std::string map;
            if (start >> map && map.size() < 32 && map.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos) {
                if (!(start >> startGsw0)) startGsw0 = -1;
                enabled = startOnly = true;
                maps.push_back(map);
                RT_LOGF(RT_TAG_RUNTIME, "[spmstart] will warp to %s (gsw0 %d) once a map is loaded\n", map.c_str(), startGsw0);
            }
            start.close();
            if (startOnly) std::remove("start_map.txt");
            return;
        }
        enabled = true;
        std::ifstream in(path);
        std::string name;
        while (std::getline(in, name)) {
            if (!name.empty() && name.back() == '\r') name.pop_back();
            if (name.empty() || name.size() >= 32 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) {
                RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] invalid-map-list\n");
                Finish(3);
            }
            maps.push_back(name);
        }
        if (!in.eof() || maps.empty()) {
            RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] unreadable-or-empty-map-list\n");
            Finish(3);
        }
        if (const char* value = std::getenv("SPM_MAP_SWEEP_SECONDS")) {
            const int seconds = std::atoi(value);
            if (seconds >= 1 && seconds <= 60) dwellSeconds = seconds;
        }
        RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] ready %zu maps dwell=%d\n", maps.size(), dwellSeconds);
    }

    [[noreturn]] static void Finish(int code) {
        std::cerr.flush();
        std::fflush(nullptr);
        std::_Exit(code);
    }

    // GSW(0) to set before entering `map`: longest matching prefix in SPM_SWEEP_GSW0_TABLE ("prefix value" lines, # comments,
    // tools\sweep-gsw0.txt), else SPM_SWEEP_GSW0, else -1 (leave the save alone).
    static int Gsw0For(const std::string& map) {
        static const auto table = [] {
            std::vector<std::pair<std::string, int>> t;
            if (const char* path = std::getenv("SPM_SWEEP_GSW0_TABLE")) {
                std::ifstream in(path);
                std::string prefix;
                int value = 0;
                while (in >> prefix) {
                    if (prefix[0] == '#') { std::getline(in, prefix); continue; }
                    if (in >> value) t.emplace_back(prefix, value);
                }
                RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] gsw0 table %s: %zu entries\n", path, t.size());
            }
            return t;
        }();
        size_t best = 0;
        int value = -1;
        for (const auto& [prefix, v] : table)
            if (prefix.size() > best && map.compare(0, prefix.size(), prefix) == 0) best = prefix.size(), value = v;
        if (value < 0)
            if (const char* g0 = std::getenv("SPM_SWEEP_GSW0")) value = std::atoi(g0);
        return value;
    }

    static std::string CurrentMap() {
        uint32_t gp = 0;
        if (!Memory::TryRead32(kGp, gp) || gp < 0x80004000 || !Memory::Contains(gp, 0x64)) return {};
        std::string name;
        for (uint32_t i = 0; i < 32; ++i) {
            const char c = static_cast<char>(Memory::Read8(gp + 0x44 + i));
            if (!c) return name;
            name += c;
        }
        return {};
    }

    static bool EffectsReady() {
        uint32_t effects = 0, pending = 0, ready = 0, data = 0, a = 0, b = 0;
        Memory::TryRead32(0x8056CFB0, effects);
        if (!effects || !Memory::Contains(effects, 16)) return false;
        Memory::TryRead32(effects + 8, pending);
        Memory::TryRead32(effects + 12, ready);
        Memory::TryRead32(0x8056D804, data);  // effSync's final check: r13-0x771C.
        if (!pending || !ready || !data || !Memory::Contains(data, 20)) return false;
        Memory::TryRead32(data + 12, a);
        Memory::TryRead32(data + 16, b);
        return a && b;
    }

    void Tick(CpuContext* cpu, uint32_t frames) {
        if (!enabled || !cpu) return;
        uint32_t now = 0, after = 0;
        if (!Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after)) return;
        const auto time = Clock::now();
        static Clock::time_point holdUntil{};
        if (!pending) {
            if (now != 2 || after != 2 || CurrentMap().empty() || (!buffer && !EffectsReady())) {
                if (!startOnly && time - started > std::chrono::seconds(90)) {
                    RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] boot-timeout seq=%u next=%u\n", now, after);
                    Finish(2);
                }
                return;
            }
            if (next == maps.size()) {
                if (time < holdUntil) return;  // the last map's screenshot needs the window too
                RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] complete\n");
                Finish(0);
            }
            if (!buffer) {
                CpuContext call = *cpu;
                call.gpr[3] = 0;  // __memAlloc(HEAP_MAIN, 64), persistent across map changes.
                call.gpr[4] = 64;
                InvokeIndirectCpu(0x801A5634, &call);
                buffer = call.gpr[3];
                if (!buffer || !Memory::Contains(buffer, 64)) {
                    RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] allocation-failed\n");
                    Finish(3);
                }
            }
            { static bool mathDone = false; if (!mathDone && std::getenv("SPM_MATH_SELFTEST")) { mathDone = true; RunMathSelfTest(cpu); } }
            { static bool mtxDone = false; if (!mtxDone && std::getenv("SPM_MTX_SELFTEST")) { mtxDone = true; RunMtxSelfTest(cpu); } }
            if (time < holdUntil) return;
            const auto& name = maps[next++];
            for (uint32_t i = 0; i < 32; ++i) Memory::Write8(buffer + i, i < name.size() ? name[i] : 0);
            pending = true;
            changed = sampling = false;
            requested = time;
            RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] attempt %s\n", name.c_str());
            if (const int g0 = startOnly ? startGsw0 : Gsw0For(name); g0 >= 0) {  // story progress (gp->gsw0) the map scripts branch on
                uint32_t gp = 0;
                if (Memory::TryRead32(kGp, gp) && Memory::Contains(gp, 0x144)) Memory::Write32(gp + 0x140, uint32_t(g0));
            }
            if (const char* g1 = std::getenv("SPM_SWEEP_GSW1")) {  // Pit floor (gp->gsw[1], u8): chest rooms need 9,19.. / 109,119..
                uint32_t gp = 0;
                if (Memory::TryRead32(kGp, gp) && Memory::Contains(gp, 0x546)) Memory::Write8(gp + 0x545, uint8_t(std::atoi(g1)));
            }
            if (const char* item = std::getenv("SPM_GIVE_ITEM")) {  // test: pouchAddItem(id) once, e.g. 67 Thunder Rage
                static bool given = false;
                if (!given) {
                    given = true;
                    CpuContext add = *cpu;
                    add.gpr[3] = static_cast<uint32_t>(std::atoi(item));
                    InvokeIndirectCpu(0x8014CB04, &add);
                    RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] gave item %s\n", item);
                }
            }
            CpuContext call = *cpu;
            call.gpr[3] = 3;  // seqSetSeq(SEQ_MAPCHANGE, mapName, nullptr).
            call.gpr[4] = buffer;
            call.gpr[5] = 0;
            InvokeIndirectCpu(0x8017B3C8, &call);
            if (startOnly) enabled = false;  // the game takes it from here
            return;
        }
        const auto& name = maps[next - 1];
        changed = changed || now == 3;
        if (changed && now == 2 && after == 2) {
            const std::string actual = CurrentMap();
            if (actual == name) {
                if (!sampling) {
                    stable = time;
                    firstFrame = frames;
                    sampling = true;
                    for (unsigned& c : g_spmAudit) c = 0;
                    RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] loaded %s\n", name.c_str());
                }
                {   // draw dump of 2 frames, SPM_SWEEP_DUMP_FRAME (default 200) frames after load, for the map named in SPM_SWEEP_DUMP
                    static const char* dumpMap = std::getenv("SPM_SWEEP_DUMP");
                    static const unsigned dumpAt = [] { const char* e = std::getenv("SPM_SWEEP_DUMP_FRAME"); return e ? unsigned(std::atoi(e)) : 200u; }();
                    const unsigned rel = frames - firstFrame;
                    g_spmSweepDump = (dumpMap && name == dumpMap && rel >= dumpAt && rel < dumpAt + 2) ? 1 : 0;
                }
                {   // SPM_SWEEP_WPAD: wpadmgr state once a second (cursor flag, pointingPos[0], KPAD pos/dpd of the newest status)
                    static const bool wpadLog = std::getenv("SPM_SWEEP_WPAD") != nullptr;
                    uint32_t wp = 0;
                    if (wpadLog && (frames - firstFrame) % 60 == 0 && Memory::TryRead32(0x80574F20u - 0x7D68u, wp) && Memory::Contains(wp, 0x9DC8)) {
                        const uint32_t st = wp + 0x6C;
                        RT_LOGF(RT_TAG_RUNTIME, "[spmwpad] flags %08X pointing %.3f,%.3f kpad pos %.3f,%.3f dpd %u hold %08X\n",
                                Memory::Read32(wp), Memory::ReadFloat32(wp + 0x4C), Memory::ReadFloat32(wp + 0x50),
                                Memory::ReadFloat32(st + 0x20), Memory::ReadFloat32(st + 0x24), Memory::Read8(st + 0x5E), Memory::Read32(st));
                    }
                }
                if (time - stable >= std::chrono::seconds(dwellSeconds) && frames - firstFrame >= 10) {
                    // fadeIsFinish(): 0 means a screen fade/curtain is still active, so a black shot is not a render bug.
                    CpuContext fadeCall = *cpu;
                    InvokeIndirectCpu(0x800676E0, &fadeCall);
                    RT_LOGF(RT_TAG_RUNTIME, "[spmaudit] %s draws=%u dropped=%u nohandle=%u badtexmap=%u clear=%06X fade=%u xfskip=%u nesteddl=%u texnull=%u palnull=%u\n", name.c_str(),
                            g_spmAudit[0], g_spmAudit[1], g_spmAudit[2], g_spmAudit[3], g_spmGameClear, fadeCall.gpr[3] & 0xFF,
                            g_spmAudit[4], g_spmAudit[5], g_spmAudit[6], g_spmAudit[7]);
                    RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] sampled %s frames=%u\n", name.c_str(), frames - firstFrame);
                    { uint32_t g = 0, gsw0 = 0; if (Memory::TryRead32(kGp, g) && Memory::Contains(g, 0x144) && Memory::TryRead32(g + 0x140, gsw0)) RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] gsw0 %d\n", int32_t(gsw0)); }
                    { // MapObj flag0 of interesting objects (mapdrv_wp at r13-0x7F10; spm-headers MapEntry/MapObj layout)
                      uint32_t wp = 0, hidden = 0;
                      if (Memory::TryRead32(0x8056D010u, wp) && Memory::Contains(wp, 0x6AC)) {
                          const uint32_t grp = Memory::Read32(wp + 0x698) & 1u;
                          const uint32_t e = wp + grp * 0x34C;
                          const uint32_t n = Memory::Read32(e + 0x180), objs = Memory::Read32(e + 0x184);
                          for (uint32_t k = 0; k < n && k < 4096 && Memory::Contains(objs + k * 0x144, 0x144); ++k) {
                              const uint32_t o = objs + k * 0x144, f0 = Memory::Read32(o), j = Memory::Read32(o + 0xC);
                              uint32_t np = 0; if (!Memory::Contains(j, 4) || !Memory::TryRead32(j, np) || !Memory::Contains(np, 16)) continue;
                              std::string nm; for (int c = 0; c < 31; ++c) { const char ch = char(Memory::Read8(np + c)); if (!ch || ch < 0x20 || ch > 0x7e) break; nm += ch; }
                              if (f0 & 1u) ++hidden;
                              if (nm == "sky_msk" || nm == "group4" || nm == "dai_k_ani_01" || nm == "sc_doa_02" || nm == "polySurface62_q00197" || nm == "mae_kusa_10")
                                  RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] mapobj %s f0 %08X\n", nm.c_str(), f0);
                          }
                          RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] mapobjs %u hidden %u\n", n, hidden);
                          const uint32_t na = Memory::Read32(e + 0x190), anims = Memory::Read32(e + 0x194);
                          for (uint32_t k = 0; k < na && k < 64 && Memory::Contains(anims + k * 0x28, 0x28); ++k) {
                              const uint32_t a = anims + k * 0x28, fd = Memory::Read32(a + 0x18);
                              uint32_t np = 0; std::string nm;
                              if (Memory::Contains(fd, 4) && Memory::TryRead32(fd, np) && Memory::Contains(np, 16))
                                  for (int c = 0; c < 31; ++c) { const char ch = char(Memory::Read8(np + c)); if (!ch || ch < 0x20 || ch > 0x7e) break; nm += ch; }
                              RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] anim %s flags %04X time %.1f rate %.2f\n", nm.c_str(), Memory::Read16(a), Memory::ReadFloat32(a + 0x10), Memory::ReadFloat32(a + 0x14));
                          }
                      } }
                    holdUntil = time + std::chrono::milliseconds(4000);  // let the audit screenshot see this map (the runner polls the log, then PrintWindows a full frame)
                    pending = false;
                    return;
                }
            } else if (!actual.empty()) {
                RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] redirected %s actual=%s\n", name.c_str(), actual.c_str());
                pending = false;
                return;
            }
        } else {
            sampling = false;
        }
        if (time - requested > std::chrono::seconds(30 + dwellSeconds)) {
            uint32_t stage = 0, rel = 0, effects = 0, effectPending = 0, effectReady = 0;
            Memory::TryRead32(0x804CF364, stage);
            Memory::TryRead32(0x8056D1C0, rel);  // isRelLoaded: r13-0x7D60, byte +8.
            Memory::TryRead32(0x8056CFB0, effects);  // effSync: r13-0x7F70, words +8/+12.
            if (effects && Memory::Contains(effects, 16)) {
                Memory::TryRead32(effects + 8, effectPending);
                Memory::TryRead32(effects + 12, effectReady);
            }
            const uint32_t loaded = rel && Memory::Contains(rel, 9) ? Memory::Read8(rel + 8) : 0;
            RT_LOGF(RT_TAG_RUNTIME, "[spmsweep] timeout %s seq=%u next=%u stage=%u rel=%u effects=%u/%u\n",
                    name.c_str(), now, after, stage, loaded, effectPending, effectReady);
            Finish(2);  // Restart externally; forcing another warp during a failed transition is unsafe.
        }
    }
};
} // namespace

void Tick(CpuContext* cpu, uint32_t presentedFrames) {
    // SPM_MARIO_LOG=1: Mario's position / speed / motion / buttons every 15 frames in a map (spm-headers MarioWork).
    static const bool marioLog = std::getenv("SPM_MARIO_LOG") != nullptr;
    uint32_t seq = 0;
    if (marioLog && cpu && presentedFrames % 15 == 0 && Memory::TryRead32(kNowSeq, seq) && seq == 2) {
        CpuContext call = *cpu;
        InvokeIndirectCpu(0x801214EC, &call);  // marioGetPtr
        const uint32_t mw = call.gpr[3];
        if (Memory::Contains(mw, 0x320)) {
            // The map object Mario stands on (hitGetName of MarioWork+0x1C4), e.g. he3_08's statue pedestal "A2D_dai".
            CpuContext hit = *cpu;
            hit.gpr[3] = Memory::Read32(mw + 0x1C4);
            InvokeIndirectCpu(0x80070780, &hit);  // hitGetName
            char ground[24] = {};
            for (int i = 0; i < 23 && Memory::Contains(hit.gpr[3] + i, 1) && (ground[i] = char(Memory::Read8(hit.gpr[3] + i))); ++i) {}
            RT_LOGF(RT_TAG_RUNTIME, "[mario] ground %s\n", ground);
        }
        if (Memory::Contains(mw, 0x320))
            RT_LOGF(RT_TAG_RUNTIME, "[mario] f=%u pos %.2f %.2f %.2f xz %.3f walk %.2f mot %u sub %d held %04X rep %04X joy %u,%u ctrl %u keyoff %u flags %08X dir %.1f wpad %08X\n",
                    presentedFrames, Memory::ReadFloat32(mw + 0x5C), Memory::ReadFloat32(mw + 0x60), Memory::ReadFloat32(mw + 0x64),
                    Memory::ReadFloat32(mw + 0x148), Memory::ReadFloat32(mw + 0x14C), Memory::Read16(mw + 0x2C), int32_t(Memory::Read32(mw + 0x3C)),
                    Memory::Read16(mw + 0x30C), Memory::Read16(mw + 0x310), Memory::Read8(mw + 0x316), Memory::Read8(mw + 0x317),
                    Memory::Read8(mw + 0x34), Memory::Read8(mw + 0x35), Memory::Read32(mw), Memory::ReadFloat32(mw + 0x174),
                    Memory::Contains(Memory::Read32(0x8056D1B8u), 4) ? Memory::Read32(Memory::Read32(0x8056D1B8u)) : 0u);
    }
    // SPM_MARIO_POS=x,y,z: once Mario has been in a map for a second, put him there (tests at a spot; one time).
    static const char* posEnv = std::getenv("SPM_MARIO_POS");
    static uint32_t inMapFrames = 0;
    if (posEnv && cpu && Memory::TryRead32(kNowSeq, seq) && seq == 2 && ++inMapFrames == 60) {
        float p[3] = {};
        if (std::sscanf(posEnv, "%f,%f,%f", &p[0], &p[1], &p[2]) == 3) {
            CpuContext call = *cpu;
            InvokeIndirectCpu(0x801214EC, &call);  // marioGetPtr
            const uint32_t mw = call.gpr[3];
            if (Memory::Contains(mw, 0x70)) {
                for (int i = 0; i < 3; ++i) Memory::WriteFloat32(mw + 0x5C + 4 * i, p[i]);
                RT_LOGF(RT_TAG_RUNTIME, "[mario] placed at %.1f %.1f %.1f\n", p[0], p[1], p[2]);
            }
        }
    }
    ServiceWarp(cpu);
    static Sweep sweep;
    sweep.Tick(cpu, presentedFrames);
}
void RequestWarp(const char* map, const char* door, const float* arrivalPos) {
    std::lock_guard<std::mutex> lock(g_warpMutex);
    g_warpHasPos = arrivalPos != nullptr;
    if (arrivalPos) std::memcpy(g_warpPos, arrivalPos, sizeof g_warpPos);
    std::memset(g_warpMap, 0, sizeof g_warpMap);
    std::memset(g_warpDoor, 0, sizeof g_warpDoor);
    std::strncpy(g_warpMap, map, sizeof g_warpMap - 1);
    g_warpHasDoor = door && *door;
    if (g_warpHasDoor) std::strncpy(g_warpDoor, door, sizeof g_warpDoor - 1);
    g_warpPending = true;
}
void AddItem(int itemId) {
    Enqueue([=](CpuContext* cpu) { g_lastResult = int(CallGuest(cpu, 0x8014CB04, uint32_t(itemId))); });      // pouchAddItem
}
void RemoveItem(int itemId) {
    Enqueue([=](CpuContext* cpu) { CallGuest(cpu, 0x8014D458, uint32_t(itemId)); });                          // pouchRemoveItem
}
// Tippi is the game's "guide", not a pouch item: the open-world mod only switches her on, it never adds item 220, and
// she is not in the game's Pixl menu. As a pouch Pixl she showed up there and selecting her crashed (mot_fairy_other.c).
// guideWork.flag0 bit 0x80 (GUIDE_FLAG0_DISABLE, spm-decomp guide.h) turns her off; saving copies it into GSWF 403.
static void SetTippiEnabled(CpuContext* cpu, bool enabled) {
    const uint32_t guide = CallGuest(cpu, 0x8011EFD0, 0);                                                    // guideGetWork
    if (!Memory::Contains(guide, 2)) return;
    const uint16_t flag0 = Memory::Read16(guide);
    Memory::Write16(guide, enabled ? uint16_t(flag0 & ~0x80u) : uint16_t(flag0 | 0x80u));
}
void SetTippi(bool enabled) {
    Enqueue([=](CpuContext* cpu) {
        SetTippiEnabled(cpu, enabled);
        // An item 220 left by an earlier version of this menu: take it out of the Pixl list (slot emptied, rest moved up).
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);                                                // pouchGetPtr
        if (!Memory::Contains(pouch, 0x104)) return;
        const uint32_t base = pouch + 0xC4;
        for (uint32_t i = 0; i < 16; ++i) {
            if (int16_t(Memory::Read16(base + 4 * i + 2)) != 220) continue;
            for (uint32_t j = i; j + 1 < 16; ++j) Memory::Write32(base + 4 * j, Memory::Read32(base + 4 * (j + 1)));
            Memory::Write32(base + 4 * 15, 0);
            break;
        }
    });
}
void ReturnToTitle() {
    Enqueue([](CpuContext* cpu) {
        uint32_t now = 0, after = 0;
        if (!Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after) || now != 2 || after != 2) return;
        CpuContext call = *cpu;
        call.gpr[3] = 1;  // seqSetSeq(SEQ_TITLE, nullptr, nullptr): the game over "quit" path
        call.gpr[4] = 0;
        call.gpr[5] = 0;
        InvokeIndirectCpu(0x8017B3C8, &call);
    });
}
void RemoveCharOrPixl(int itemId) {
    // The game has no remove for these slots (pouchRemoveItem only handles item lists): make it not selectable with
    // the game's own call, then empty the slot (Pixls after it move up). The character played is left alone; the Pixl
    // in use hands over to another Pixl first.
    Enqueue([=](CpuContext* cpu) {
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);                                                // pouchGetPtr
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);                                                  // marioGetPtr
        if (!Memory::Contains(pouch, 0x104) || !Memory::Contains(mw, 0x40)) return;
        if (itemId == 216 + int8_t(Memory::Read8(mw + 0x38))) return;                                        // the character played
        if (itemId == int(CallGuest(cpu, 0x8014E828, 0))) {                                                  // pouchGetCurPixl
            for (uint32_t i = 1; i < 16; ++i) {   // hand over to another Pixl first, if there is one
                const int other = int16_t(Memory::Read16(pouch + 0xC4 + 4 * i + 2));
                if (other != 0 && other != 220 && other != itemId) {
                    CallGuest(cpu, 0x8014E914, uint32_t(other));                                             // pouchSetPixlSelected
                    break;
                }
            }
        }
        for (const auto [base, count, pixl] : {std::tuple{0xB4u, 4u, false}, std::tuple{0xC4u, 16u, true}}) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t slot = pouch + base + 4 * i;
                if (int16_t(Memory::Read16(slot + 2)) != itemId) continue;
                CallGuest(cpu, pixl ? 0x8014E660u : 0x8014E524u, uint32_t(itemId));                          // pouchMake{Pixl,Char}NotSelectable
                if (itemId == 220) SetTippiEnabled(cpu, false);
                if (!pixl) {   // a character keeps its slot index: the slot is just emptied
                    Memory::Write32(slot, 0);
                    return;
                }
                for (uint32_t j = i; j + 1 < count; ++j) Memory::Write32(pouch + base + 4 * j, Memory::Read32(pouch + base + 4 * (j + 1)));
                Memory::Write32(pouch + base + 4 * (count - 1), 0);
                return;
            }
        }
    });
}
void Unlock(int itemId, bool pixl) {
    if (itemId == 220) {   // Tippi: the guide, not a pouch Pixl
        SetTippi(true);
        return;
    }
    Enqueue([=](CpuContext* cpu) {
        CallGuest(cpu, 0x8014CB04, uint32_t(itemId));                                                         // pouchAddItem
        CallGuest(cpu, pixl ? 0x8014E5A4u : 0x8014E4BCu, uint32_t(itemId));                                   // pouchMake{Pixl,Char}Selectable
    });
}
void AddCoins(int amount) {
    Enqueue([=](CpuContext* cpu) { CallGuest(cpu, 0x8014CA98, uint32_t(amount)); });                          // pouchAddCoin
}
void SetHp(int hp, int maxHp) {
    Enqueue([=](CpuContext* cpu) {
        CallGuest(cpu, 0x8014C9C0, uint32_t(maxHp));                                                          // pouchSetMaxHp
        CallGuest(cpu, 0x8014C920, uint32_t(hp));                                                             // pouchSetHp
    });
}
void SetSystemFlag(int flag, bool on) {
    Enqueue([=](CpuContext* cpu) { CallGuest(cpu, on ? 0x80037E90u : 0x80037F18u, uint32_t(flag)); });         // swSet / swClear
}
void SetGsw(int index, int value) {
    Enqueue([=](CpuContext* cpu) {
        if (index == 0) {                                                                                     // GSW(0) is a full s32 in the game work
            uint32_t gp = 0;
            if (Memory::TryRead32(kGp, gp) && Memory::Contains(gp, 0x144)) Memory::Write32(gp + 0x140, uint32_t(value));
        } else {
            CallGuest(cpu, 0x80037F58, uint32_t(index), uint32_t(value));                                     // swByteSet
        }
    });
}
void ReadSystemFlag(int flag) {
    Enqueue([=](CpuContext* cpu) { g_lastResult = int(CallGuest(cpu, 0x80037ED0, uint32_t(flag))); });         // swGet
}
int Gsw0() {
    uint32_t gp = 0;
    if (!Memory::TryRead32(kGp, gp) || gp < 0x80004000 || !Memory::Contains(gp, 0x144)) return -1;
    return int(Memory::Read32(gp + 0x140));
}
int LastResult() { return g_lastResult.load(); }

namespace {
std::mutex g_stateMutex;
DebugState g_state;
std::atomic<bool> g_statePending{false};
}

void RequestState() {
    if (g_statePending.exchange(true)) return;
    Enqueue([](CpuContext* cpu) {
        DebugState st;
        st.valid = true;
        Memory::TryRead32(kNowSeq, st.seq);
        uint32_t gp = 0;
        if (Memory::TryRead32(kGp, gp) && Memory::Contains(gp, 0x64))
            for (uint32_t i = 0; i < 31; ++i) st.map[i] = static_cast<char>(Memory::Read8(gp + 0x44 + i));
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);                     // marioGetPtr
        if (Memory::Contains(mw, 0x320)) {
            for (int i = 0; i < 3; ++i) st.pos[i] = Memory::ReadFloat32(mw + 0x5C + 4 * i);
            st.held = Memory::Read16(mw + 0x30C);
            st.curChar = 216 + int8_t(Memory::Read8(mw + 0x38));                    // marioWork.character
        }
        st.curPixl = int(CallGuest(cpu, 0x8014E828, 0));                        // pouchGetCurPixl
        const uint32_t guide = CallGuest(cpu, 0x8011EFD0, 0);                   // guideGetWork
        st.tippi = Memory::Contains(guide, 2) && !(Memory::Read16(guide) & 0x80u);
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);                  // pouchGetPtr
        if (Memory::Contains(pouch, sizeof st.pouch))
            for (uint32_t i = 0; i < sizeof st.pouch; ++i) st.pouch[i] = Memory::Read8(pouch + i);
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state = st;
        }
        g_statePending = false;
    });
}
bool GetState(DebugState& out) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    out = g_state;
    return g_state.valid;
}
void SetPouch(int offset, int value) {
    Enqueue([=](CpuContext* cpu) {
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);
        if (offset >= 0 && offset + 4 <= 0x6A0 && Memory::Contains(pouch, 0x6A0)) Memory::Write32(pouch + offset, uint32_t(value));
    });
}
void SetVar(int type, int index, int value) {
    if (type == 0) { SetGsw(index, value); return; }
    Enqueue([=](CpuContext* cpu) {
        switch (type) {
        case 1: CallGuest(cpu, value ? 0x80037E90u : 0x80037F18u, uint32_t(index)); break;                   // swSet / swClear
        case 2: CallGuest(cpu, 0x800380C8, uint32_t(index), uint32_t(value)); break;                          // _swByteSet
        case 3: CallGuest(cpu, value ? 0x80038000u : 0x80038088u, uint32_t(index)); break;                    // _swSet / _swClear
        }
    });
}
void ReadVar(int type, int index) {
    Enqueue([=](CpuContext* cpu) {
        switch (type) {
        case 0: g_lastResult = index == 0 ? Gsw0() : int(CallGuest(cpu, 0x80037FDC, uint32_t(index))); break;  // swByteGet
        case 1: g_lastResult = int(CallGuest(cpu, 0x80037ED0, uint32_t(index))); break;                        // swGet
        case 2: g_lastResult = int(CallGuest(cpu, 0x800380D8, uint32_t(index))); break;                        // _swByteGet
        case 3: g_lastResult = int(CallGuest(cpu, 0x80038040, uint32_t(index))); break;                        // _swGet
        }
    });
}

namespace {
std::atomic<bool> g_watch{false};
std::mutex g_watchMutex;
std::vector<std::string> g_watchLog;
uint8_t g_flagPrev[8192];
uint8_t g_gswPrev[512];
bool g_watchPrimed = false;
uint32_t g_watchTick = 0;
float g_savedPos[3];
std::atomic<bool> g_hasSavedPos{false};

void WatchStep(CpuContext* cpu) {
    if (!g_watch) { g_watchPrimed = false; return; }
    if (++g_watchTick % 20) return;
    uint32_t now = 0, after = 0;
    if (!Memory::TryRead32(kNowSeq, now) || !Memory::TryRead32(kNextSeq, after) || now != 2 || after != 2) return;
    std::vector<std::string> changes;
    char line[64];
    for (int n = 0; n < 8192; ++n) {
        const uint8_t v = uint8_t(CallGuest(cpu, 0x80037ED0, uint32_t(n)) & 1);          // swGet
        if (g_watchPrimed && v != g_flagPrev[n]) {
            std::snprintf(line, sizeof line, "GSWF %d: %d -> %d", n, g_flagPrev[n], v);
            changes.emplace_back(line);
        }
        g_flagPrev[n] = v;
    }
    for (int n = 1; n < 512; ++n) {
        const uint8_t v = uint8_t(CallGuest(cpu, 0x80037FDC, uint32_t(n)));              // swByteGet
        if (g_watchPrimed && v != g_gswPrev[n]) {
            std::snprintf(line, sizeof line, "GSW %d: %d -> %d", n, g_gswPrev[n], v);
            changes.emplace_back(line);
        }
        g_gswPrev[n] = v;
    }
    static int lastGsw0 = -1;
    const int gsw0 = Gsw0();
    if (g_watchPrimed && gsw0 != lastGsw0) {
        std::snprintf(line, sizeof line, "GSW 0: %d -> %d", lastGsw0, gsw0);
        changes.emplace_back(line);
    }
    lastGsw0 = gsw0;
    g_watchPrimed = true;
    if (!changes.empty()) {
        std::lock_guard<std::mutex> lock(g_watchMutex);
        for (auto& c : changes) g_watchLog.push_back(std::move(c));
        if (g_watchLog.size() > 400) g_watchLog.erase(g_watchLog.begin(), g_watchLog.begin() + (g_watchLog.size() - 400));
    }
}
}

void SetWatch(bool on) { g_watch = on; }
bool WatchEnabled() { return g_watch.load(); }
std::vector<std::string> GetWatchLog() {
    std::lock_guard<std::mutex> lock(g_watchMutex);
    return g_watchLog;
}
void ClearWatchLog() {
    std::lock_guard<std::mutex> lock(g_watchMutex);
    g_watchLog.clear();
}
void SavePosition() {
    Enqueue([](CpuContext* cpu) {
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);
        if (!Memory::Contains(mw, 0x70)) return;
        for (int i = 0; i < 3; ++i) g_savedPos[i] = Memory::ReadFloat32(mw + 0x5C + 4 * i);
        g_hasSavedPos = true;
    });
}
void SetGravity(int dir) {
    Enqueue([=](CpuContext* cpu) { CallGuest(cpu, 0x8012AA90, uint32_t(dir)); });                           // marioSetGravity
}
void TeleportTo(float x, float y, float z) {
    Enqueue([=](CpuContext* cpu) {
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);                                                  // marioGetPtr
        if (!Memory::Contains(mw, 0x70)) return;
        Memory::WriteFloat32(mw + 0x5C, x);
        Memory::WriteFloat32(mw + 0x60, y);
        Memory::WriteFloat32(mw + 0x64, z);
    });
}
void LoadPosition() {
    Enqueue([](CpuContext* cpu) {
        if (!g_hasSavedPos) return;
        const uint32_t mw = CallGuest(cpu, 0x801214EC, 0);
        if (!Memory::Contains(mw, 0x70)) return;
        for (int i = 0; i < 3; ++i) Memory::WriteFloat32(mw + 0x5C + 4 * i, g_savedPos[i]);
    });
}
bool HasSavedPosition() { return g_hasSavedPos.load(); }
void Heal() {
    Enqueue([](CpuContext* cpu) {
        const uint32_t pouch = CallGuest(cpu, 0x8014B594, 0);
        if (Memory::Contains(pouch, 0x14)) Memory::Write32(pouch + 0x0C, Memory::Read32(pouch + 0x10));
    });
}

namespace {
std::mutex g_flagSnapMutex;
uint8_t g_flagSnap[8192];
std::atomic<bool> g_flagSnapPending{false};
}

void RequestFlagSnapshot() {
    if (g_flagSnapPending.exchange(true)) return;
    Enqueue([](CpuContext* cpu) {
        static uint8_t fresh[8192];
        for (int n = 0; n < 8192; ++n) fresh[n] = uint8_t(CallGuest(cpu, 0x80037ED0, uint32_t(n)) & 1);   // swGet
        {
            std::lock_guard<std::mutex> lock(g_flagSnapMutex);
            std::memcpy(g_flagSnap, fresh, sizeof g_flagSnap);
        }
        g_flagSnapPending = false;
    });
}
bool FlagValue(int flag) {
    if (flag < 0 || flag >= 8192) return false;
    std::lock_guard<std::mutex> lock(g_flagSnapMutex);
    return g_flagSnap[flag] != 0;
}
} // namespace SpmMapSweep
