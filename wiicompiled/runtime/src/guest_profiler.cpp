// NSMBW_PROFILE=<start s>:<length s> (Windows): a sampling profiler for the game thread. From
// <start> seconds after the first presented frame, for <length> seconds, the thread that presents
// frames is suspended about a thousand times a second and its instruction pointer recorded. At the
// end, samples are mapped back to the translated guest function whose code they fall in (by each
// translated function's host entry point) and the top entries are logged with their guest address,
// so a slow scene can be traced to the game code (or the runtime and aurora code) it spends time in.
#include "guest_profiler.h"

#include "abi_bridge.h"
#include "runtime_log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {
extern const uint32_t kGuestMapSymbolCount;
extern const uint32_t kGuestMapSymbolAddresses[];
extern const char* const kGuestMapSymbolNames[];
}

namespace GuestProfiler {
namespace {

#ifdef _WIN32
struct Entry {
    uintptr_t host;
    uint32_t guest;
};

const char* GuestName(uint32_t address) {
    const uint32_t* begin = kGuestMapSymbolAddresses;
    const uint32_t* end = kGuestMapSymbolAddresses + kGuestMapSymbolCount;
    const uint32_t* it = std::lower_bound(begin, end, address);
    return it != end && *it == address ? kGuestMapSymbolNames[it - begin] : "";
}

void Report(const std::unordered_map<uintptr_t, uint32_t>& samples, const std::vector<std::vector<uintptr_t>>& stacks,
            uint32_t total) {
    // Every translated function's host entry point, sorted, to find which one a sample falls in.
    std::vector<Entry> entries;
    for (const auto& [lo, hi] : {std::pair{0x80004000u, 0x80319180u}, std::pair{0x80BF691Cu, 0x80C5A648u}}) {  // SPM main.dol text, relF text
        for (uint32_t address = lo; address < hi; address += 4) {
            if (const TranslatedFunctionInfo* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
                info != nullptr && info->entryPoint != nullptr && info->address == address) {
                entries.push_back({reinterpret_cast<uintptr_t>(info->entryPoint), address});
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
    // The engine module (engine_usa.dll / engine_pal.dll since the loader split), not the small loader exe.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&OnFramePresented), &self);
    const auto exeBase = reinterpret_cast<uintptr_t>(self ? self : GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exeBase + dos->e_lfanew);
    const uintptr_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;

    // A host address as a translated guest function, or exe+RVA (resolve with llvm-nm), or [dll].
    const auto describe = [&](uintptr_t rip) -> std::string {
        if (rip >= exeBase && rip < exeEnd) {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            // Translated code sits in its own range: past the last entry point it is runtime code.
            if (it != entries.begin() && it != entries.end()) {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                return text;
            }
            char text[40];
            std::snprintf(text, sizeof(text), "exe+%llX", static_cast<unsigned long long>(rip - exeBase));
            return text;
        }
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &module)) {
            GetModuleFileNameA(module, path, MAX_PATH);
        }
        const char* slash = std::strrchr(path, '\\');
        return std::string("[") + (slash ? slash + 1 : path) + "]";
    };
    // Raw stacks (exe RVAs, "-" for other modules) for offline symbolization: nm the exe, see AGENTS.md.
    if (FILE* raw = std::fopen("profile_stacks.txt", "w")) {
        for (const auto& stack : stacks) {
            for (const uintptr_t rip : stack) {
                if (rip >= exeBase && rip < exeEnd) {
                    std::fprintf(raw, "%llX ", static_cast<unsigned long long>(rip - exeBase));
                } else {
                    std::fputs("- ", raw);
                }
            }
            std::fputc('\n', raw);
        }
        std::fclose(raw);
    }
    // Where each sample's stack enters system code from ours: the first frames inside the exe.
    std::unordered_map<std::string, uint32_t> chains;
    for (const auto& stack : stacks) {
        std::string chain;
        int exeFrames = 0;
        for (const uintptr_t rip : stack) {
            if (rip >= exeBase && rip < exeEnd) {
                chain += (chain.empty() ? "" : " < ") + describe(rip);
                if (++exeFrames == 5) break;
            } else if (exeFrames > 0) {
                break;
            }
        }
        if (stack.empty() || (stack[0] >= exeBase && stack[0] < exeEnd)) {
            continue;  // already in our code: the leaf table covers it
        }
        chains[describe(stack[0]) + " from " + (chain.empty() ? "?" : chain)]++;
    }
    std::vector<std::pair<std::string, uint32_t>> sortedChains(chains.begin(), chains.end());
    std::sort(sortedChains.begin(), sortedChains.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sortedChains.size() && i < 25; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile chain: %5.1f%%  %s\n", 100.0 * sortedChains[i].second / total,
                sortedChains[i].first.c_str());
    }

    std::unordered_map<std::string, uint32_t> byName;
    for (const auto& [rip, count] : samples) {
        std::string key;
        if (rip < exeBase || rip >= exeEnd) {
            HMODULE module = nullptr;
            char path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(rip), &module)) {
                GetModuleFileNameA(module, path, MAX_PATH);
            }
            const char* slash = std::strrchr(path, '\\');
            key = std::string("[") + (slash ? slash + 1 : path) + "]";
        } else {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            if (it == entries.begin()) {
                key = "[exe, before translated code]";
            } else {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                key = text;
            }
        }
        byName[key] += count;
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(byName.begin(), byName.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    RT_LOGF(RT_TAG_RUNTIME, "profile: %u samples, %zu translated functions\n", total, entries.size());
    for (size_t i = 0; i < sorted.size() && i < 60; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile: %5.1f%%  %s\n", 100.0 * sorted[i].second / total, sorted[i].first.c_str());
    }
}

void Run(HANDLE thread, double startSeconds, double lengthSeconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
    std::unordered_map<uintptr_t, uint32_t> samples;
    std::vector<std::vector<uintptr_t>> stacks;
    uint32_t total = 0;
    auto next = std::chrono::steady_clock::now();
    const auto end = next + std::chrono::duration<double>(lengthSeconds);
    while (std::chrono::steady_clock::now() < end) {
        // About 1000 samples a second; Sleep() is too coarse for that, so wait by yielding.
        next += std::chrono::microseconds(1000);
        while (std::chrono::steady_clock::now() < next) {
            std::this_thread::yield();
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            // No heap use while the game thread is suspended: it may hold the heap lock (deadlock).
            uintptr_t frames[24];
            int depth = 0;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                // The call stack, by the unwind data every x64 function carries.
                for (; depth < 24 && context.Rip != 0; ++depth) {
                    frames[depth] = static_cast<uintptr_t>(context.Rip);
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                    if (function == nullptr) {
                        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);  // a leaf: return address on top
                        context.Rsp += 8;
                        continue;
                    }
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                     &establisher, nullptr);
                }
            }
            ResumeThread(thread);
            if (depth > 0) {
                ++samples[frames[0]];
                ++total;
                stacks.emplace_back(frames, frames + depth);
            }
        }
    }
    Report(samples, stacks, total);
}
#endif

} // namespace

void OnFramePresented() {
#ifdef _WIN32
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    const char* value = std::getenv("NSMBW_PROFILE");
    // SPM: also read "<start>:<length>" from profile.txt beside the executable (the test loop's switch).
    static char fileValue[64] = {};
    if (value == nullptr) {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        path = path.substr(0, path.find_last_of("\\/") + 1) + "profile.txt";
        if (FILE* f = std::fopen(path.c_str(), "r")) {
            if (std::fgets(fileValue, sizeof(fileValue), f)) value = fileValue;
            std::fclose(f);
        }
    }
    double start = 0, length = 0;
    if (value == nullptr || std::sscanf(value, "%lf:%lf", &start, &length) != 2 || length <= 0) {
        return;
    }
    HANDLE thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::thread(Run, thread, start, length).detach();
#endif
}

} // namespace GuestProfiler
