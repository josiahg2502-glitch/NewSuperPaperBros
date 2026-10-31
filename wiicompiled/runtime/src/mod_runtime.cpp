// Code mods, Gecko cheat codes and Riivolution memory patches (mod_runtime.h).
#include "mod_runtime.h"

#include "abi_bridge.h"
#include "hle/storage/riivolution.h"
#include "memory.h"
#include "mod_install.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "spm_mod_api.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifndef SPM_REGION_PAL
#define SPM_REGION_PAL 0
#endif

extern "C" {
extern const uint32_t kGuestMapSymbolCount;
extern const uint32_t kGuestMapSymbolAddresses[];
extern const char* const kGuestMapSymbolNames[];
}

namespace fs = std::filesystem;

namespace {

// ---- code ranges: a write there changes an instruction, which the translated game never reads ----
struct Range { uint32_t start, end; };
std::vector<Range> g_codeRanges;
bool g_relRangesAdded = false;

void AddDolTextRanges() {
    std::ifstream dol(RuntimeConfigFile::ResolvedDvdRoot() / "sys" / "main.dol", std::ios::binary);
    std::array<uint8_t, 0xE4> header{};
    if (!dol.read(reinterpret_cast<char*>(header.data()), header.size())) return;
    const auto be = [&](size_t at) { return uint32_t(header[at]) << 24 | uint32_t(header[at + 1]) << 16 | uint32_t(header[at + 2]) << 8 | header[at + 3]; };
    for (int i = 0; i < 7; ++i)
        if (const uint32_t size = be(0x90 + 4 * i); size) g_codeRanges.push_back({be(0x48 + 4 * i), be(0x48 + 4 * i) + size});
}

// The loaded REL modules (OS module queue at 0x800030C8): their executable sections, once the game has linked them.
void AddRelTextRanges() {
    uint32_t module = 0;
    if (!Memory::TryRead32(0x800030C8u, module)) return;
    for (int guard = 0; module && guard < 16 && Memory::Contains(module, 0x20); ++guard) {
        const uint32_t count = Memory::Read32(module + 0x0C), table = Memory::Read32(module + 0x10);
        for (uint32_t s = 0; s < count && s < 32 && Memory::Contains(table + 8 * s, 8); ++s) {
            const uint32_t offset = Memory::Read32(table + 8 * s), size = Memory::Read32(table + 8 * s + 4);
            if (!(offset & 1) || !size) continue;
            const uint32_t start = (offset & ~1u) >= 0x80000000u ? (offset & ~1u) : module + (offset & ~1u);
            g_codeRanges.push_back({start, start + size});
            g_relRangesAdded = true;
        }
        module = Memory::Read32(module);  // link.next
    }
}

bool IsCode(uint32_t address) {
    return std::any_of(g_codeRanges.begin(), g_codeRanges.end(), [&](const Range& r) { return address >= r.start && address < r.end; });
}

// ---- Gecko codes ----
struct Gecko {
    std::string name, note;
    std::vector<std::pair<uint32_t, uint32_t>> lines;
    bool enabledByDefault = false;
    bool supported = true;
    bool checkedCode = false;
};
std::mutex g_cheatMutex;
std::vector<Gecko> g_codes;
std::set<std::string> g_enabled;  // names switched on

fs::path CheatDir() { return RuntimeConfigFile::ApplicationDataDirectory() / "cheats"; }

// The code types run here: RAM writes, conditionals, base / pointer loads, terminators.
bool SupportedType(uint8_t cmd) {
    switch (cmd) {
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x08:
    case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A: case 0x2C: case 0x2E:
    case 0x40: case 0x42: case 0x44: case 0x48: case 0x4A: case 0x4C:
    case 0xDE: case 0xE0: case 0xE2: case 0xF0:
        return true;
    default:
        return false;
    }
}

uint8_t Cmd(uint32_t a) { return uint8_t(a >> 24) & 0xEE; }  // without the pointer bit (0x10) and address bit 24

// Lines a code takes after its first one.
size_t ExtraLines(uint32_t a, uint32_t v) {
    switch (Cmd(a)) {
    case 0x06: return (v + 7) / 8;
    case 0x08: return 1;
    default: return 0;
    }
}

// Runs one code against game memory (`write` false: only reports the addresses it would write, for IsCode).
template <class Read32, class Write>
void RunGecko(const Gecko& code, Read32 read32, Write write) {
    uint32_t ba = 0x80000000u, po = 0x80000000u;
    std::vector<bool> stack;  // conditionals: true while their code runs
    const auto active = [&] { return std::all_of(stack.begin(), stack.end(), [](bool b) { return b; }); };
    for (size_t i = 0; i < code.lines.size(); ++i) {
        const auto [a, v] = code.lines[i];
        const uint8_t cmd = Cmd(a);
        const bool usePo = (a >> 24) & 0x10;
        const uint32_t address = (a & 0x01FFFFFFu) + (usePo ? po : ba);
        const bool run = active();
        switch (cmd) {
        case 0x00: case 0x02: case 0x04: {
            if (!run) break;
            const uint32_t size = cmd == 0x00 ? 1 : cmd == 0x02 ? 2 : 4;
            const uint32_t count = cmd == 0x04 ? 1 : (v >> 16) + 1;
            for (uint32_t n = 0; n < count; ++n) write(address + n * size, size, size == 1 ? (v & 0xFF) : size == 2 ? (v & 0xFFFF) : v);
            break;
        }
        case 0x06:
            for (uint32_t n = 0; run && n < v; ++n) {
                const auto& line = code.lines[std::min(code.lines.size() - 1, i + 1 + n / 8)];
                const uint32_t word = (n % 8) < 4 ? line.first : line.second;
                write(address + n, 1, (word >> (24 - 8 * (n % 4))) & 0xFF);
            }
            break;
        case 0x08:
            if (run && i + 1 < code.lines.size()) {
                const auto [p, inc] = code.lines[i + 1];
                const uint32_t type = p >> 28, count = ((p >> 16) & 0xFFF) + 1, step = p & 0xFFFF;
                const uint32_t size = type == 0 ? 1 : type == 1 ? 2 : 4;
                uint32_t value = v;
                for (uint32_t n = 0; n < count; ++n, value += inc)
                    write(address + n * step, size, size == 1 ? (value & 0xFF) : size == 2 ? (value & 0xFFFF) : value);
            }
            break;
        case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A: case 0x2C: case 0x2E: {
            if (a & 1) { if (!stack.empty()) stack.pop_back(); }  // an endif first
            const bool parent = active();
            bool cond = false;
            const uint32_t at = address & ~1u;
            if (parent) {
                if (cmd <= 0x26) {
                    const uint32_t m = read32(at);
                    cond = cmd == 0x20 ? m == v : cmd == 0x22 ? m != v : cmd == 0x24 ? m > v : m < v;
                } else {
                    const uint32_t m = (read32(at & ~3u) >> ((at & 2) ? 0 : 16)) & 0xFFFF & ~(v >> 16);
                    const uint32_t want = v & 0xFFFF;
                    cond = cmd == 0x28 ? m == want : cmd == 0x2A ? m != want : cmd == 0x2C ? m > want : m < want;
                }
            }
            stack.push_back(parent && cond);
            break;
        }
        case 0x40: case 0x48: {  // ba / po = [X (+ ba or po)]
            if (!run) break;
            uint32_t at = v;
            if (a & 0x00100000u) at += usePo ? po : ba;
            (cmd == 0x40 ? ba : po) = read32(at);
            break;
        }
        case 0x42: case 0x4A: {  // ba / po = X (+ ba or po)
            if (!run) break;
            uint32_t value = v;
            if (a & 0x00100000u) value += cmd == 0x42 ? ba : po;
            (cmd == 0x42 ? ba : po) = value;
            break;
        }
        case 0x44: case 0x4C:
            if (run) write(v, 4, cmd == 0x44 ? ba : po);
            break;
        case 0xDE: {
            const bool parent = active();
            const uint32_t lo = v & 0xFFFF0000u, hi = v << 16;
            stack.push_back(parent && po >= lo && po < hi);
            break;
        }
        case 0xE0:
            stack.clear();
            if (v >> 16) ba = v & 0xFFFF0000u;
            if (v & 0xFFFF) po = v << 16;
            break;
        case 0xE2: {
            for (uint32_t n = 0; n < (a & 0xFF) && !stack.empty(); ++n) stack.pop_back();  // E2T000VV: VV endifs
            if ((a & 0x00100000u) && !stack.empty()) {  // else
                const bool top = stack.back();
                stack.pop_back();
                stack.push_back(active() && !top);
            }
            break;
        }
        case 0xF0:
            return;
        default:
            break;
        }
        i += ExtraLines(a, v);
    }
}

void LoadCheatFile(const fs::path& path, const std::string& source) {
    std::ifstream in(path);
    std::string line, section;
    std::set<std::string> defaults;
    std::vector<Gecko> codes;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[') {
            section = line;
            continue;
        }
        if (section == "[Gecko_Enabled]" && line.front() == '$') {
            defaults.insert(line.substr(1));
        } else if (section == "[Gecko]") {
            if (line.front() == '$') {
                codes.push_back({});
                codes.back().name = line.substr(1);
            } else if (!codes.empty() && line.front() == '*') {
                if (!codes.back().note.empty()) codes.back().note += ' ';
                codes.back().note += line.substr(1);
            } else if (!codes.empty()) {
                unsigned a = 0, v = 0;
                char extra = 0;
                if (std::sscanf(line.c_str(), "%8x %8x%c", &a, &v, &extra) == 2) codes.back().lines.emplace_back(a, v);
            }
        }
    }
    for (auto& code : codes) {
        if (code.lines.empty()) continue;
        for (size_t i = 0; i < code.lines.size(); ++i) {
            if (!SupportedType(Cmd(code.lines[i].first))) {
                code.supported = false;
                code.note = "Runs PowerPC code, which this version can't do.";
                break;
            }
            i += ExtraLines(code.lines[i].first, code.lines[i].second);
        }
        code.enabledByDefault = defaults.count(code.name) != 0;
        if (code.note.empty()) code.note = "From " + source;
        const bool duplicate = std::any_of(g_codes.begin(), g_codes.end(), [&](const Gecko& g) { return g.name == code.name; });
        if (!duplicate) g_codes.push_back(std::move(code));
    }
}

void LoadCheats() {
    std::lock_guard<std::mutex> lock(g_cheatMutex);
    g_codes.clear();
    g_enabled.clear();
    std::error_code ec;
    fs::create_directories(CheatDir(), ec);
    std::vector<std::pair<fs::path, std::string>> files;
    for (const auto& entry : fs::directory_iterator(CheatDir(), ec)) {
        const auto ext = entry.path().extension().string();
        if (ext == ".ini" || ext == ".txt") files.emplace_back(entry.path(), RuntimeConfigFile::PathToUtf8(entry.path().filename()));
    }
    for (const auto& mod : ModInstall::List()) {
        if (!mod.enabled) continue;
        for (const auto& dir : {ModInstall::Folder() / mod.name, ModInstall::Folder() / mod.name / "codes"})
            for (const auto& entry : fs::directory_iterator(dir, ec))
                if (entry.path().extension() == ".ini") files.emplace_back(entry.path(), mod.name);
    }
    std::sort(files.begin(), files.end());
    for (const auto& [path, source] : files)
        if (path.filename() != "enabled.txt") LoadCheatFile(path, source);
    // enabled.txt: "+name" / "-name" lines from the Options menu override the files' [Gecko_Enabled].
    std::map<std::string, bool> chosen;
    std::ifstream state(CheatDir() / "enabled.txt");
    for (std::string line; std::getline(state, line);) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 1 && (line[0] == '+' || line[0] == '-')) chosen[line.substr(1)] = line[0] == '+';
    }
    for (const auto& code : g_codes) {
        const auto it = chosen.find(code.name);
        if (it != chosen.end() ? it->second : code.enabledByDefault) g_enabled.insert(code.name);
    }
    if (!g_codes.empty()) RT_LOGF(RT_TAG_RUNTIME, "cheats: %zu code(s), %zu on\n", g_codes.size(), g_enabled.size());
}

// ---- native code mods ----
struct Hook { SpmHookFn fn; void* user; };
std::unordered_map<uint32_t, std::vector<Hook>> g_hooks;
std::vector<uint64_t> g_hookBits((0x01800000u >> 2) / 64);  // hooked addresses in MEM1
std::vector<std::pair<SpmFrameFn, void*>> g_frameFns;
bool g_loading = false;
thread_local uint32_t g_bypass = 0;  // call_original: the next call to this address skips the hooks
std::vector<std::string> g_modFolders;  // kept alive for api.mod_folder

CpuContext* Ctx(SpmCpu* cpu) { return reinterpret_cast<CpuContext*>(cpu); }

uint32_t FindFunction(const char* name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < kGuestMapSymbolCount; ++i)
        if (std::strcmp(kGuestMapSymbolNames[i], name) == 0) return kGuestMapSymbolAddresses[i];
    return 0;
}

uint32_t ApiCall(SpmCpu* cpu, uint32_t fn, const uint32_t* args, int count) {
    if (!cpu || !fn) return 0;
    CpuContext call = *Ctx(cpu);
    for (int i = 0; i < count && i < 8; ++i) call.gpr[3 + i] = args[i];
    InvokeIndirectCpu(fn, &call);
    return call.gpr[3];
}

uint32_t ApiCallOriginal(SpmCpu* cpu, uint32_t fn) {
    if (!cpu || !fn) return 0;
    g_bypass = fn;
    InvokeIndirectCpu(fn, Ctx(cpu));
    g_bypass = 0;
    return Ctx(cpu)->gpr[3];
}

int ApiHook(uint32_t fn, SpmHookFn cb, void* user) {
    if (!g_loading || !cb || !TranslatedFunctionRegistry::FindByAddressPtr(fn)) return 0;
    g_hooks[fn].push_back({cb, user});
    const uint32_t slot = (fn - 0x80000000u) >> 2;
    if (slot < g_hookBits.size() * 64) g_hookBits[slot >> 6] |= uint64_t(1) << (slot & 63);
    ModHooks::g_active = true;
    return 1;
}

void ApiOnFrame(SpmFrameFn cb, void* user) {
    if (g_loading && cb) g_frameFns.emplace_back(cb, user);
}

const SpmModApi& BaseApi() {
    static const SpmModApi api = [] {
        SpmModApi a{};
        a.version = SPM_MOD_API_VERSION;
        a.region = SPM_REGION_PAL;
        a.log = [](const char* text) { RT_LOGF(RT_TAG_RUNTIME, "[mod] %s\n", text ? text : ""); };
        a.read8 = [](uint32_t at) { return Memory::Contains(at, 1) ? Memory::Read8(at) : uint8_t(0); };
        a.read16 = [](uint32_t at) { return Memory::Contains(at, 2) ? Memory::Read16(at) : uint16_t(0); };
        a.read32 = [](uint32_t at) { return Memory::Contains(at, 4) ? Memory::Read32(at) : 0u; };
        a.read_f32 = [](uint32_t at) { return Memory::Contains(at, 4) ? Memory::ReadFloat32(at) : 0.0f; };
        a.write8 = [](uint32_t at, uint8_t v) { if (Memory::Contains(at, 1)) Memory::Write8(at, v); };
        a.write16 = [](uint32_t at, uint16_t v) { if (Memory::Contains(at, 2)) Memory::Write16(at, v); };
        a.write32 = [](uint32_t at, uint32_t v) { if (Memory::Contains(at, 4)) Memory::Write32(at, v); };
        a.write_f32 = [](uint32_t at, float v) { if (Memory::Contains(at, 4)) Memory::WriteFloat32(at, v); };
        a.gpr = [](SpmCpu* cpu) { return Ctx(cpu)->gpr; };
        a.get_fpr = [](SpmCpu* cpu, int i) { return i >= 0 && i < 32 ? Ctx(cpu)->fpr[i].d : 0.0; };
        a.set_fpr = [](SpmCpu* cpu, int i, double v) { if (i >= 0 && i < 32) Ctx(cpu)->fpr[i].d = v; };
        a.call = ApiCall;
        a.call_original = ApiCallOriginal;
        a.hook = ApiHook;
        a.on_frame = ApiOnFrame;
        a.find_function = FindFunction;
        return a;
    }();
    return api;
}

void LoadCodeMods() {
    std::error_code ec;
    for (const auto& mod : ModInstall::List()) {
        if (!mod.enabled) continue;
        for (const auto& entry : fs::directory_iterator(ModInstall::Folder() / mod.name / "code", ec)) {
#ifdef _WIN32
            if (entry.path().extension() != ".dll") continue;
            HMODULE lib = LoadLibraryW(entry.path().wstring().c_str());
            auto init = lib ? reinterpret_cast<int (*)(const SpmModApi*)>(GetProcAddress(lib, "spm_mod_init")) : nullptr;
#else
            if (entry.path().extension() != ".so") continue;
            void* lib = dlopen(entry.path().c_str(), RTLD_NOW | RTLD_LOCAL);
            auto init = lib ? reinterpret_cast<int (*)(const SpmModApi*)>(dlsym(lib, "spm_mod_init")) : nullptr;
#endif
            const std::string file = RuntimeConfigFile::PathToUtf8(entry.path().filename());
            if (!init) {
                RT_LOGF(RT_TAG_RUNTIME, "code mod %s/%s: not loaded (no spm_mod_init)\n", mod.name.c_str(), file.c_str());
                continue;
            }
            g_modFolders.push_back(RuntimeConfigFile::PathToUtf8(ModInstall::Folder() / mod.name));
            SpmModApi api = BaseApi();
            api.mod_folder = g_modFolders.back().c_str();
            static std::vector<std::unique_ptr<SpmModApi>> keep;  // a mod may keep the pointer
            keep.push_back(std::make_unique<SpmModApi>(api));
            g_loading = true;
            const int ok = init(keep.back().get());
            g_loading = false;
            RT_LOGF(RT_TAG_RUNTIME, "code mod %s/%s: %s\n", mod.name.c_str(), file.c_str(), ok ? "loaded" : "init failed");
        }
    }
}

// ---- Riivolution memory patches ----
void ApplyRiivolution(uint32_t below) {
    for (const auto& overlay : RuntimeRiivolution::Overlays()) {
        if (!overlay.patches) continue;
        for (const auto& write : overlay.patches->memory) {
            const uint32_t size = uint32_t(write.value.size());
            if (write.address >= below || !Memory::Contains(write.address, size)) continue;
            bool matches = true;
            for (uint32_t i = 0; i < write.original.size() && matches; ++i)
                matches = Memory::Contains(write.address + i, 1) && Memory::Read8(write.address + i) == write.original[i];
            if (!matches) continue;
            for (uint32_t i = 0; i < size; ++i) Memory::Write8(write.address + i, write.value[i]);
        }
    }
}

uint32_t Read32Safe(uint32_t at) { return Memory::Contains(at, 4) ? Memory::Read32(at) : 0u; }

void WriteSafe(uint32_t at, uint32_t size, uint32_t value) {
    if (!Memory::Contains(at, size)) return;
    if (size == 1) Memory::Write8(at, uint8_t(value));
    else if (size == 2) Memory::Write16(at, uint16_t(value));
    else Memory::Write32(at, value);
}

} // namespace

namespace ModHooks {
bool g_active = false;
bool Run(uint32_t target, CpuContext* ctx) {
    // Most calls are to functions no mod hooked: one bit per 4-byte address of MEM1 answers that.
    const uint32_t slot = (target - 0x80000000u) >> 2;
    if (slot >= g_hookBits.size() * 64 || !(g_hookBits[slot >> 6] >> (slot & 63) & 1)) return false;
    if (g_bypass == target) {
        g_bypass = 0;
        return false;
    }
    const auto it = g_hooks.find(target);
    if (it == g_hooks.end()) return false;
    for (const Hook& hook : it->second)
        if (hook.fn(reinterpret_cast<SpmCpu*>(ctx), hook.user)) return true;
    return false;
}
} // namespace ModHooks

namespace ModRuntime {

void Load() {
    AddDolTextRanges();
    // Built in (issue #57): at King Sammer's "Vassals to me!" in 6-1 the frame's animation ("ext") textures add up
    // past 1 MB, and extdrv.c asserts twice about it: :364 `anim_ext_tex_size < 1024*1024` (their image buffer) and
    // :839 `bank_adr+size < 2*512*1024` (the Wii's 1 MB texture cache, TMEM). The recomp keeps those frame-buffer
    // copies on the GPU and never uses TMEM (textures are read from RAM), so going past either is harmless here.
    g_loading = true;
    const int hooked = ApiHook(0x8019B6C8u, [](SpmCpu* cpu, void*) -> int {  // __assert2(file, line, ..., expression)
        const uint32_t* r = Ctx(cpu)->gpr;
        if (r[4] != 364 && r[4] != 839) return 0;
        bool match = false;
        for (const uint32_t at : {r[5], r[6]}) {  // the expression is in one of these, depending on the call
            char text[48] = {};
            for (uint32_t i = 0; i + 1 < sizeof text && Memory::Contains(at + i, 1); ++i)
                if (!(text[i] = char(Memory::Read8(at + i)))) break;
            match |= std::strstr(text, "anim_ext_tex_size") != nullptr || std::strstr(text, "bank_adr+size") != nullptr;
        }
        if (!match) return 0;
        static uint32_t logged = 0;
        if (!(logged & (r[4] == 364 ? 1u : 2u)))
            RT_LOGF(RT_TAG_RUNTIME, "extdrv.c:%u animation textures over 1 MB: continuing (issue #57)\n", r[4]);
        logged |= r[4] == 364 ? 1u : 2u;
        return 1;
    }, nullptr);
    g_loading = false;
    if (!hooked) RT_LOGF(RT_TAG_RUNTIME, "issue #57 guard: __assert2 not hookable\n");
    LoadCodeMods();
    LoadCheats();
}

void ApplyEarlyPatches() {
    uint32_t arenaLo = 0;  // the end of the main program (boot info ArenaLo)
    if (Memory::TryRead32(0x80000030u, arenaLo) && arenaLo > 0x80004000u) ApplyRiivolution(arenaLo);
}

void Frame(CpuContext* cpu) {
    static uint32_t seq = 0;
    Memory::TryRead32(0x8056D0D8u, seq);  // the game's sequence: the REL is linked from the title screen on
    if (seq == 0 || seq > 5) return;
    static bool patched = false;
    if (!patched) {
        patched = true;
        AddRelTextRanges();
        ApplyRiivolution(0xFFFFFFFFu);
    }
    for (const auto& [fn, user] : g_frameFns) fn(reinterpret_cast<SpmCpu*>(cpu), user);
    std::lock_guard<std::mutex> lock(g_cheatMutex);
    for (auto& code : g_codes) {
        if (!code.supported || !g_enabled.count(code.name)) continue;
        if (!code.checkedCode && g_relRangesAdded) {
            // A code that only rewrites the game's instructions does nothing in a recomp: say so instead.
            code.checkedCode = true;
            bool any = false, data = false;
            RunGecko(code, Read32Safe, [&](uint32_t at, uint32_t, uint32_t) { any = true; data |= !IsCode(at); });
            if (any && !data) {
                code.supported = false;
                code.note = "Changes the game's code, which this version can't do.";
                RT_LOGF(RT_TAG_RUNTIME, "cheat \"%s\" only rewrites game code: turned off\n", code.name.c_str());
                continue;
            }
        }
        RunGecko(code, Read32Safe, WriteSafe);
    }
}

std::vector<CheatCode> CheatCodes() {
    std::lock_guard<std::mutex> lock(g_cheatMutex);
    std::vector<CheatCode> out;
    for (const auto& code : g_codes) out.push_back({code.name, code.note, g_enabled.count(code.name) != 0, code.supported});
    return out;
}

void SetCheatEnabled(const std::string& name, bool enabled) {
    std::lock_guard<std::mutex> lock(g_cheatMutex);
    if (enabled) g_enabled.insert(name);
    else g_enabled.erase(name);
    std::ofstream state(CheatDir() / "enabled.txt", std::ios::trunc);
    for (const auto& code : g_codes) state << (g_enabled.count(code.name) ? '+' : '-') << code.name << '\n';
}

std::string CheatFolder() { return RuntimeConfigFile::PathToUtf8(CheatDir()); }

bool SelfCheck() {
    // 04 write, a 16-bit fill, a string, and an if-equal that is false (its write must not happen) then an endif.
    std::array<uint8_t, 64> ram{};
    const uint32_t base = 0x80001000u;
    auto read32 = [&](uint32_t at) {
        const uint32_t o = at - base;
        return o + 4 <= ram.size() ? uint32_t(ram[o]) << 24 | uint32_t(ram[o + 1]) << 16 | uint32_t(ram[o + 2]) << 8 | ram[o + 3] : 0u;
    };
    auto write = [&](uint32_t at, uint32_t size, uint32_t value) {
        const uint32_t o = at - base;
        for (uint32_t i = 0; i < size && o + i < ram.size(); ++i) ram[o + i] = uint8_t(value >> (8 * (size - 1 - i)));
    };
    Gecko code;
    code.lines = {{0x04001000u, 0x11223344u}, {0x02001004u, 0x0001ABCDu}, {0x06001008u, 0x00000003u}, {0x41424300u, 0},
                  {0x20001000u, 0x00000000u}, {0x04001010u, 0xDEADBEEFu}, {0xE2000001u, 0}, {0x04001014u, 0x55555555u}};
    RunGecko(code, read32, write);
    return read32(base) == 0x11223344u && read32(base + 4) == 0xABCDABCDu && ram[8] == 'A' && ram[10] == 'C' &&
           read32(base + 0x10) == 0 && read32(base + 0x14) == 0x55555555u;
}

} // namespace ModRuntime
