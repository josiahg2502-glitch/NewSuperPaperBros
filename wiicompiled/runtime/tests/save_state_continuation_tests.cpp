// Two "translated" functions written the way the translator emits them (guest registers in C++ locals,
// ctx->lr set to the guest return address before each call, PPC EABI frames on a guest stack) call a
// yielding HLE function, which switches to the scheduler fiber like SelectThread does. A save state is
// guest memory plus the register file captured at the yield; nothing native is kept. Restoring rebuilds
// the native stack: a fresh fiber walks the guest back chain, then re-enters each function through a
// resume entry at its call site, outermost first.
//
// Checks: (1) save -> advance -> restore matches an uninterrupted run, (2) restoring the same state
// repeatedly does too, (3) restoring in a separate process (different allocations/ASLR) does too, and
// (4) the negative control: with callee-saved registers kept only in native locals across calls, the
// restored run diverges.
#include "host_context.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint32_t kOuter = 0x1000, kOuterCallMid = 0x1010;  // OUTER and the return address of its call
constexpr uint32_t kMid = 0x2000, kMidCallYield = 0x2010;
constexpr uint32_t kRootReturn = 0xDEAD0000;                   // LR of the thread entry
constexpr uint32_t kStackTop = 0xF000, kLogCount = 0x100, kLog = 0x104, kTick = 0x80;
constexpr int kSteps = 40, kSaveAt = 10, kAdvance = 10;

struct Ctx { uint32_t gpr[32]; uint32_t lr; };
struct Memory {
    std::array<uint32_t, 0x10000 / 4> words{};
    uint32_t& operator[](uint32_t address) { return words.at(address / 4); }
};

Memory mem;
Ctx live;                       // the shared register file every frame works on
Ctx yieldCtx;                   // register file captured at the most recent yield
bool coherent = true;           // flush callee-saved registers to ctx around calls
HostContext::Handle scheduler = nullptr, worker = nullptr;

void Log(uint32_t value) { mem[kLog + 4 * mem[kLogCount]] = value; ++mem[kLogCount]; }

// HLE yield (like OSSleepThread -> SelectThread): park the fiber, return the scheduler's tick in r3.
void Yield(Ctx* ctx) {
    yieldCtx = *ctx;
    HostContext::Switch(scheduler);
    ctx->gpr[3] = mem[kTick];
}

// MID(r3): r31 = r3 * 7; yield; return r31 + tick. Saves LR and r31 in its frame, as compiled PPC does.
void Mid(Ctx* ctx) {
    uint32_t r1 = ctx->gpr[1], r3 = ctx->gpr[3], r31 = ctx->gpr[31], r0 = ctx->lr;
    mem[r1 - 16] = r1; r1 -= 16; mem[r1 + 20] = r0; mem[r1 + 12] = r31;  // stwu, mflr/stw r0, stw r31
    r31 = r3 * 7;
    ctx->gpr[1] = r1; ctx->gpr[3] = r3; if (coherent) ctx->gpr[31] = r31;
    ctx->lr = kMidCallYield;
    Yield(ctx);
    r3 = ctx->gpr[3]; if (coherent) r31 = ctx->gpr[31];
    r3 = r31 + r3;
    r31 = mem[r1 + 12]; r0 = mem[r1 + 20]; r1 += 16;                     // epilogue
    ctx->gpr[1] = r1; ctx->gpr[3] = r3; ctx->gpr[31] = r31; ctx->lr = r0;
}

// OUTER: r31 accumulates, r30 counts iterations; each one calls MID and logs r31. Never returns.
void Outer(Ctx* ctx) {
    uint32_t r1 = ctx->gpr[1], r3 = 0, r30 = 0, r31 = 1, r0 = ctx->lr;
    mem[r1 - 16] = r1; r1 -= 16; mem[r1 + 20] = r0;
    for (;;) {
        r3 = r31 + r30;
        ctx->gpr[1] = r1; ctx->gpr[3] = r3;
        if (coherent) { ctx->gpr[30] = r30; ctx->gpr[31] = r31; }
        ctx->lr = kOuterCallMid;
        Mid(ctx);
        r3 = ctx->gpr[3]; if (coherent) { r30 = ctx->gpr[30]; r31 = ctx->gpr[31]; }
        r31 = r31 * 3 + r3;
        Log(r31);
        ++r30;
    }
}

// Resume entries, as the translator emits them per non-leaf function.
struct Frame { uint32_t function, returnAddress; };
using Chain = std::vector<Frame>;
void ResumeAt(Ctx* ctx, const Chain& chain, size_t depth);

void MidResume(Ctx* ctx, const Chain& chain, size_t depth) {
    uint32_t r1 = ctx->gpr[1], r3 = ctx->gpr[3], r31 = ctx->gpr[31], r0 = 0;
    switch (chain[depth].returnAddress) {
    case kMidCallYield:
        ResumeAt(ctx, chain, depth + 1);
        r1 = ctx->gpr[1]; r3 = ctx->gpr[3]; r31 = ctx->gpr[31];  // resume reloads every local
        break;
    default: std::abort();
    }
    r3 = r31 + r3;
    r31 = mem[r1 + 12]; r0 = mem[r1 + 20]; r1 += 16;
    ctx->gpr[1] = r1; ctx->gpr[3] = r3; ctx->gpr[31] = r31; ctx->lr = r0;
}

void OuterResume(Ctx* ctx, const Chain& chain, size_t depth) {
    uint32_t r1 = ctx->gpr[1], r3 = 0, r30 = 0, r31 = 0;
    switch (chain[depth].returnAddress) {
    case kOuterCallMid:
        ResumeAt(ctx, chain, depth + 1);
        r1 = ctx->gpr[1]; r3 = ctx->gpr[3]; r30 = ctx->gpr[30]; r31 = ctx->gpr[31];
        break;
    default: std::abort();
    }
    // The rest of the loop body after the call, then the ordinary loop (the same code as Outer).
    r31 = r31 * 3 + r3;
    Log(r31);
    ++r30;
    for (;;) {
        r3 = r31 + r30;
        ctx->gpr[1] = r1; ctx->gpr[3] = r3;
        if (coherent) { ctx->gpr[30] = r30; ctx->gpr[31] = r31; }
        ctx->lr = kOuterCallMid;
        Mid(ctx);
        r3 = ctx->gpr[3]; if (coherent) { r30 = ctx->gpr[30]; r31 = ctx->gpr[31]; }
        r31 = r31 * 3 + r3;
        Log(r31);
        ++r30;
    }
}

// The innermost frame is the HLE yield: its resume finishes the HLE's post-switch work.
void YieldResume(Ctx* ctx) {
    HostContext::Switch(scheduler);  // parked until the scheduler resumes this rebuilt fiber
    ctx->gpr[3] = mem[kTick];
}

void ResumeAt(Ctx* ctx, const Chain& chain, size_t depth) {
    if (depth == chain.size()) return YieldResume(ctx);
    switch (chain[depth].function) {
    case kOuter: return OuterResume(ctx, chain, depth);
    case kMid: return MidResume(ctx, chain, depth);
    default: std::abort();
    }
}

// Call-site table (from translation metadata): guest return address -> function containing it.
uint32_t FunctionOf(uint32_t returnAddress) {
    switch (returnAddress) {
    case kOuterCallMid: return kOuter;
    case kMidCallYield: return kMid;
    default: return 0;
    }
}

// Chain from guest state only: the yield's LR, then saved LRs along the EABI back chain.
bool WalkGuestStack(const Ctx& ctx, Chain& chain) {
    chain.clear();
    uint32_t returnAddress = ctx.lr, sp = ctx.gpr[1];
    while (returnAddress != kRootReturn) {
        const uint32_t function = FunctionOf(returnAddress);
        if (!function || chain.size() > 64) return false;  // unknown frame: capture must be refused
        chain.insert(chain.begin(), Frame{function, returnAddress});
        const uint32_t back = mem[sp];
        returnAddress = mem[back + 4];
        sp = back;
    }
    return true;
}

Ctx restoreCtx;
void WorkerEntry(void*) {
    live = {};
    live.gpr[1] = kStackTop; live.lr = kRootReturn;
    mem[kStackTop] = 0; mem[kStackTop + 4] = kRootReturn;  // root frame: back chain end, LR of the entry
    Outer(&live);
}
void ResumeEntry(void*) {
    Chain chain;
    if (!WalkGuestStack(restoreCtx, chain)) std::abort();
    live = restoreCtx;
    ResumeAt(&live, chain, 0);
}

void Step(int step) { mem[kTick] = uint32_t(step) * 5 + 1; HostContext::Switch(worker); }
void Run(int from, int to) { for (int step = from; step < to; ++step) Step(step); }
std::vector<uint32_t> Output() {
    return {mem.words.begin() + kLog / 4, mem.words.begin() + kLog / 4 + mem[kLogCount]};
}
void StartFresh() {
    if (worker) HostContext::Destroy(worker);
    mem = {};
    worker = HostContext::Create(64 * 1024, WorkerEntry, nullptr);
    HostContext::Switch(worker);  // runs to the first yield
}
void StartFromState(const Memory& savedMemory, const Ctx& savedCtx) {
    if (worker) HostContext::Destroy(worker);
    mem = savedMemory; restoreCtx = savedCtx;
    worker = HostContext::Create(64 * 1024, ResumeEntry, nullptr);
    HostContext::Switch(worker);  // rebuilds the stack and parks in YieldResume
}

struct State { Memory memory; Ctx ctx; };
bool WriteState(const char* path, const State& state) {
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const bool ok = std::fwrite(&state, sizeof(state), 1, file) == 1;
    return std::fclose(file) == 0 && ok;
}

// Reference, then save at kSaveAt, advance, restore (three times). Returns mismatching restores.
int RoundTrips(std::vector<uint32_t>& reference, State& saved, int restores) {
    StartFresh(); Run(0, kSteps); reference = Output();
    StartFresh(); Run(0, kSaveAt);
    saved = {mem, yieldCtx};
    Run(kSaveAt, kSaveAt + kAdvance);
    int failures = 0;
    for (int i = 0; i < restores; ++i) {
        StartFromState(saved.memory, saved.ctx);
        Run(kSaveAt, kSteps);
        if (Output() != reference) ++failures;
    }
    return failures;
}
}  // namespace

int main(int argc, char** argv) {
    if (!HostContext::InitializeScheduler(&scheduler)) return 1;
    if (argc == 3 && std::string(argv[1]) == "--restore") {
        // Separate process: only the state file is shared with the parent.
        State state{};
        FILE* file = std::fopen(argv[2], "rb");
        if (!file || std::fread(&state, sizeof(state), 1, file) != 1) return 2;
        std::fclose(file);
        StartFromState(state.memory, state.ctx);
        Run(kSaveAt, kSteps);
        const auto output = Output();
        std::fwrite(output.data(), sizeof(uint32_t), output.size(), stdout);
        return 0;
    }

    std::vector<uint32_t> reference;
    State saved;
    const int sameProcess = RoundTrips(reference, saved, 3);
    std::printf("coherent calls: %d/3 restores diverged (reference %zu values)\n", sameProcess, reference.size());

    // Fresh process from the saved state.
    const std::string path = "/tmp/spm-continuation-state.bin";  // NOLINT: test scratch file
    if (!WriteState(path.c_str(), saved)) return 3;
    const std::string command = std::string(argv[0]) + " --restore " + path;
    FILE* child = popen(command.c_str(), "r");
    std::vector<uint32_t> childOutput(reference.size() + 8);
    const size_t got = child ? std::fread(childOutput.data(), sizeof(uint32_t), childOutput.size(), child) : 0;
    const int childStatus = child ? pclose(child) : -1;
    childOutput.resize(got);
    const bool freshOk = childStatus == 0 && childOutput == reference;
    std::printf("coherent calls: fresh-process restore %s\n", freshOk ? "matches" : "DIVERGED");
    std::remove(path.c_str());

    // Negative control: callee-saved registers live only in native locals across calls.
    coherent = false;
    std::vector<uint32_t> nativeReference;
    State nativeSaved;
    const int native = RoundTrips(nativeReference, nativeSaved, 1);
    std::printf("native callee-saved (control): %d/1 restores diverged\n", native);

    HostContext::Destroy(worker);
    HostContext::ShutdownScheduler(scheduler);
    return sameProcess == 0 && freshOk && native == 1 ? 0 : 1;
}
