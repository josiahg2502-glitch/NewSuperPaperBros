// Save states: guest-thread continuations; see save_state.h.
#include "save_state.h"
#include "save_state_resume.h"

#include "fiber_manager.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdlib>
#include <map>
#include <unordered_set>

namespace SaveState {
namespace {
constexpr uint32_t kOSCurrentContextAddr = 0x800000d4u;

// The resumable translated functions (projects/spm/resumable_functions.inc, which the translator reads too).
const std::unordered_set<uint32_t>& Resumable() {
    static const std::unordered_set<uint32_t> functions = {
#include "../../projects/spm/resumable_functions.inc"
    };
    return functions;
}

// The guest address each HLE wait overrides: the innermost call of a recorded chain must target it.
uint32_t WaitFunction(YieldKind kind) {
    switch (kind) {
    case YieldKind::ViWaitForRetrace: return 0x80280E0Cu;  // VIWaitForRetrace
    case YieldKind::MessageQueue: return 0x80272904u;      // OSReceiveMessage
    case YieldKind::Sleep: return 0x802757F4u;             // OSSleepThread
    default: return 0;
    }
}

struct Live { YieldRecord record; int depth = 0; };
std::map<uint32_t, Live> g_live;                     // guest thread -> its current outermost wait
std::map<uint32_t, ThreadContinuation> g_pending;   // restored threads not yet run again
std::map<uint32_t, CpuContext> g_resumeEntry;        // YieldEntry of each restored thread

struct Active { bool on = false; uint32_t thread = 0; size_t next = 0; ThreadContinuation continuation; } g_active;

// The guest thread whose fiber is running. The fiber manager's view, since the guest's current context
// is the idle context while a thread idles in SelectThread; the guest's view before the first switch.
uint32_t CurrentThread() {
    if (const uint32_t thread = Fiber::GuestFiberManager::GetCurrentGuestThread()) return thread;
    uint32_t thread = 0;
    return Memory::TryRead32(kOSCurrentContextAddr, thread) ? thread : 0;
}

bool Word(uint32_t address, uint32_t& value) { return (address & 3) == 0 && Memory::TryRead32(address, value); }

// The direct call before a return address: its target, or 0 for anything else.
uint32_t DirectCallBefore(uint32_t returnAddress) {
    uint32_t op = 0;
    if (!Word(returnAddress - 4, op) || (op >> 26) != 18 || !(op & 1)) return 0;
    int32_t offset = int32_t(op & 0x03FFFFFC);
    if (offset & 0x02000000) offset -= 0x04000000;
    return (op & 2) ? uint32_t(offset) : returnAddress - 4 + uint32_t(offset);
}
}  // namespace

YieldScope::YieldScope(YieldKind kind, const CpuContext& entry, std::initializer_list<uint32_t> extra)
    : thread_(CurrentThread()) {
    Live& live = g_live[thread_];
    if (live.depth++ != 0) return;
    owner_ = true;
    live.record.kind = kind;
    live.record.entry = entry;
    size_t i = 0;
    for (const uint32_t value : extra) if (i < 6) live.record.extra[i++] = value;
    for (; i < 6; ++i) live.record.extra[i] = 0;
}

YieldScope::YieldScope(const YieldRecord& resumed) : thread_(CurrentThread()) {
    Live& live = g_live[thread_];
    if (live.depth++ != 0) return;
    owner_ = true;
    live.record = resumed;
}

YieldScope::~YieldScope() {
    const auto it = g_live.find(thread_);
    if (it != g_live.end() && --it->second.depth <= 0) g_live.erase(it);
}

const YieldRecord* TakeYield(YieldKind kind, bool& running) {
    if (!g_active.on || g_active.thread != CurrentThread()) return nullptr;
    if (g_active.next != g_active.continuation.frames.size() || g_active.continuation.yield.kind != kind) {
        SaveStateResume::Mismatch(WaitFunction(kind));
    }
    g_active.on = false;
    running = g_active.continuation.running;
    return &g_active.continuation.yield;
}

bool IsResumable(uint32_t function) noexcept { return Resumable().count(function) != 0; }

bool CaptureThread(uint32_t thread, uint32_t rootFunction, bool running, ThreadContinuation& out, std::string& error) {
    const auto live = g_live.find(thread);
    if (live == g_live.end() || live->second.record.kind == YieldKind::None) {
        error = "thread not in a recorded wait";
        return false;
    }
    out = {};
    out.thread = thread;
    out.running = running;
    out.yield = live->second.record;
    // Innermost first: the wait's caller, then saved LRs along the EABI back chain to the root.
    std::vector<uint32_t> returns;
    uint32_t returnAddress = out.yield.entry.lr, sp = out.yield.entry.gpr[1];
    while (returnAddress != 0) {
        returns.push_back(returnAddress);
        if (returns.size() > 64) { error = "chain too deep"; return false; }
        uint32_t back = 0;
        if (!Word(sp, back)) { error = "unreadable stack"; return false; }
        if (back == 0) break;
        if (back <= sp || !Word(back + 4, returnAddress)) { error = "broken back chain"; return false; }
        sp = back;
        if (returnAddress == 0x8027510Cu) break;  // OSExitThread: a thread entry's root LR
    }
    if (!returns.empty() && returns.back() == 0x8027510Cu) returns.pop_back();
    if (returns.empty()) { error = "empty chain"; return false; }
    // Frames outermost first: the root is the thread entry; each next frame is the direct call's target.
    uint32_t function = rootFunction;
    for (size_t i = returns.size(); i-- > 0;) {
        if (!IsResumable(function)) {
            char text[64];
            std::snprintf(text, sizeof(text), "function 0x%08X not resumable", function);
            error = text;
            return false;
        }
        out.frames.push_back({function, returns[i]});
        function = DirectCallBefore(returns[i]);
        if (function == 0) { error = "indirect or missing call on the chain"; return false; }
    }
    if (function != WaitFunction(out.yield.kind)) { error = "innermost call is not the recorded wait"; return false; }
    return true;
}

void ResetForRestore() {
    g_live.clear();
    g_pending.clear();
    g_resumeEntry.clear();
    g_active = {};
}

void SetPendingResume(ThreadContinuation continuation) {
    const uint32_t thread = continuation.thread;
    g_resumeEntry[thread] = continuation.yield.entry;
    g_pending[thread] = std::move(continuation);
}

uint32_t BeginResume(uint32_t thread) {
    const auto it = g_pending.find(thread);
    if (it == g_pending.end() || it->second.frames.empty()) {
        RT_LOGF(RT_TAG_RUNTIME, "save state: thread %08X has no continuation\n", thread);
        SaveStateResume::Mismatch(0);
    }
    g_active.continuation = std::move(it->second);
    g_pending.erase(it);
    g_active.thread = thread;
    g_active.next = 0;
    g_active.on = true;
    return g_active.continuation.frames.front().function;
}
}  // namespace SaveState

namespace SaveStateResume {
bool Active() noexcept { return SaveState::g_active.on && SaveState::g_active.thread == SaveState::CurrentThread(); }

uint32_t Enter(uint32_t function) {
    auto& active = SaveState::g_active;
    if (!active.on || active.next >= active.continuation.frames.size() ||
        active.continuation.frames[active.next].function != function) {
        Mismatch(function);
    }
    return active.continuation.frames[active.next++].returnAddress;
}

uint32_t Target(uint32_t original) noexcept {
    const auto& active = SaveState::g_active;
    return Active() && active.next < active.continuation.frames.size() ? active.continuation.frames[active.next].function
                                                                         : original;
}

const CpuContext& YieldEntry() noexcept {
    static const CpuContext none{};
    const auto it = SaveState::g_resumeEntry.find(SaveState::CurrentThread());
    return it != SaveState::g_resumeEntry.end() ? it->second : none;
}

void Mismatch(uint32_t function) {
    RT_LOGF(RT_TAG_RUNTIME, "save state: resume mismatch at function 0x%08X\n", function);
    std::abort();
}
}  // namespace SaveStateResume
