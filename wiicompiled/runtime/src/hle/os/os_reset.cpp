#include "memory.h"
#include "aurora_events.h"
#include "hle/audio/ax_dsp.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "system_bridge.h"

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <string>
#include <chrono>
#include <thread>

namespace {

static std::string ReadGuestStringSafe(uint32_t addr)
{
    if (addr == 0) {
        return "";
    }
    std::string out;
    out.reserve(64);
    try {
        for (size_t i = 0; i < 512; ++i) {
            uint8_t b = Memory::Read8(addr + static_cast<uint32_t>(i));
            if (b == 0) break;
            out.push_back(static_cast<char>(b));
        }
    } catch (const Memory::AccessViolation&) {
        out = "<invalid_guest_ptr>";
    }
    return out;
}

} // namespace

// 0x801A9BA0 -> PPCHalt
extern "C" void PPCHalt_8012E5A4()
{
    // A halt is never a clean shutdown: the guest reaches it after OSPanic or an
    // unrecoverable OS error. Report it like every other fatal path so the run
    // folder gets the same artifact set, and exit non-zero.
    RT_LOGF(RT_TAG_OS, "PPCHalt called, exiting process\n");
    std::fflush(stderr);
    RuntimeCrash::WriteCrashArtifacts("halt", "The guest executed PPCHalt.");
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system halted the console",
                          "Super Paper Mario executed PPCHalt, which the console only reaches after an "
                          "unrecoverable error.");
    MarkFatalErrorReported();
    if (RuntimeFrontend::Active()) RuntimeFrontend::Fail("Guest panic or halt");
    std::exit(EXIT_FAILURE);
}

PPC_NATIVE_OVERRIDE_VOID(8026B334, PPCHalt_8012E5A4, (), ());

// 0x801AD4E0 -> OS::Panic
extern "C" void OS__Panic_801A2660_Cpu(CpuContext* ctx)
{
    const uint32_t file_ptr = ctx ? ctx->gpr[3] : 0;
    const int line = ctx ? static_cast<int>(ctx->gpr[4]) : 0;
    const uint32_t fmt_ptr = ctx ? ctx->gpr[5] : 0;

    const std::string file = ReadGuestStringSafe(file_ptr);
    const std::string fmt = ReadGuestStringSafe(fmt_ptr);

    std::fprintf(stderr, "\n\n");
    std::fprintf(stderr, "========================================\n");
    RT_LOGF(RT_TAG_OS, "OS::Panic called!\n");
    std::fprintf(stderr, "========================================\n");
    RT_LOGF(RT_TAG_OS, "Location: %s:%d\n", file.empty() ? "<null>" : file.c_str(), line);
    if (!fmt.empty()) {
        RT_LOGF(RT_TAG_OS, "Message: %s\n", fmt.c_str());
        // SPM asserts pass the failed condition (and sometimes a name) as %s arguments in r6/r7.
        if (ctx) {
            const std::string a6 = ReadGuestStringSafe(ctx->gpr[6]);
            const std::string a7 = ReadGuestStringSafe(ctx->gpr[7]);
            RT_LOGF(RT_TAG_OS, "Args: r6=\"%s\" r7=\"%s\"\n", a6.c_str(), a7.c_str());
        }
    }
    std::fprintf(stderr, "========================================\n");
    std::fflush(stderr);
    
    RT_LOGF(RT_TAG_OS, "Dumping CPU state...\n");
    std::fflush(stderr);
    
    
    RT_LOGF(RT_TAG_OS, "CPU registers:\n");
    if (ctx) {
        std::fprintf(stderr, "  PC: 0x%08X  LR: 0x%08X  CTR: 0x%08X\n", ctx->pc, ctx->lr, ctx->ctr);
        std::fprintf(stderr, "  r1: 0x%08X  r2: 0x%08X  r13: 0x%08X\n", ctx->gpr[1], ctx->gpr[2], ctx->gpr[13]);
        std::fprintf(stderr, "  r3: 0x%08X  r4: 0x%08X  r5: 0x%08X\n", ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
    } else {
        std::fprintf(stderr, "  (CPU context is NULL)\n");
    }
    
    std::fprintf(stderr, "========================================\n");
    RT_LOGF(RT_TAG_OS, "Flushing output and exiting...\n");
    std::fflush(stderr);
    std::fflush(stdout);

    std::string details = "Super Paper Mario reported an OS panic";
    if (!file.empty()) {
        details += " at ";
        details += file;
        details += ":";
        details += std::to_string(line);
    }
    if (!fmt.empty()) {
        details += "\n\n";
        details += fmt;
    }
    RuntimeCrash::WriteCrashArtifacts("panic", details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system reported a panic", details);
    MarkFatalErrorReported();

    // Give extra time for output to flush with PowerShell redirection
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    if (RuntimeFrontend::Active()) RuntimeFrontend::Fail("Guest panic or halt");
    std::exit(EXIT_FAILURE);
}

PPC_NATIVE_OVERRIDE_VOID(8019D5B0, OS__Panic_801A2660_Cpu, (CpuContext* ctx), (ctx));

// 0x801B3CC0 -> OSResetSystem
extern "C" uint32_t OSResetSystem()
{
    RT_LOGF(RT_TAG_OS, "OSResetSystem: simulating console reset\n");
    std::fflush(stderr);
    // The AX mix worker holds resolved host pointers into the guest regions;
    // it must be stopped before those mappings are torn down.
    if (RuntimeFrontend::Active()) RuntimeFrontend::Fail("Guest requested reset");
    AxDspHle::ShutdownMixWorker();
    Memory::Reset();
    SetRuntimeExitCode(0);
    // HOME menu "Reset"/"Wii Menu": exit like the window's close button. std::exit ran the engine DLL's static
    // destructors with threads still joinable and showed a std::terminate error box (issue #40, like #38).
    ExitForAuroraWindowClose();
    return 0; // unreachable, but keeps the signature consistent with callers
}

PPC_NATIVE_OVERRIDE(80273B38, OSResetSystem, uint32_t, (), ());

// HOME menu "Reset" (OSRestart), "Wii Menu" (OSReturnToMenu) and power off (OSShutdownSystem), plus the
// other reboot paths: on a Wii they shut the hardware down and spin until the console resets, so the game
// hung after "Yes" (issue #40). Here they quit the game like its close button. Names from projects/spm/MAP.txt;
// the PAL build maps the addresses.
// The PAL game's spsndExit (USA has no such step) unregisters the AI DMA callback and then calls AXQuit, whose
// __AXOutQuit sleeps until the next audio frame. Here audio frames run from that callback, so none comes and the
// HOME menu's Reset/Wii Menu froze on a black screen (issue #40). The game quits right after, so skip it.
// make_pal_runtime.py sets SPM_REGION_PAL to 1 and keeps 802875F0 (PAL AXQuit) as it is.
#define SPM_REGION_PAL 0
#if SPM_REGION_PAL
extern "C" void AXQuit_PAL_HLE() { RT_LOGF(RT_TAG_OS, "AXQuit: skipped at shutdown\n"); }
PPC_NATIVE_OVERRIDE_VOID(802875F0, AXQuit_PAL_HLE, (), ());
#endif

// The game's reset callbacks (spmario.c): each sets a flag that spmarioMain acts on. Same effect as the
// originals (li r0,1; stb r0,flag(r13)), plus a log line so a reset request shows up in the log.
extern "C" void resetButtonRestart_HLE(CpuContext* ctx) { RT_LOGF(RT_TAG_OS, "resetButtonRestart\n"); ctx->gpr[0] = 1; Memory::Write8(ctx->gpr[13] - 0x7540, 1); }
PPC_NATIVE_OVERRIDE_VOID(801A82C8, resetButtonRestart_HLE, (CpuContext* ctx), (ctx));
extern "C" void shutdownConsole_HLE(CpuContext* ctx) { RT_LOGF(RT_TAG_OS, "shutdownConsole\n"); ctx->gpr[0] = 1; Memory::Write8(ctx->gpr[13] - 0x753F, 1); }
PPC_NATIVE_OVERRIDE_VOID(801A82D4, shutdownConsole_HLE, (CpuContext* ctx), (ctx));
extern "C" void homeButtonRestart_HLE(CpuContext* ctx) { RT_LOGF(RT_TAG_OS, "homeButtonRestart\n"); ctx->gpr[0] = 1; Memory::Write8(ctx->gpr[13] - 0x753E, 1); }
PPC_NATIVE_OVERRIDE_VOID(801A82E0, homeButtonRestart_HLE, (CpuContext* ctx), (ctx));

extern "C" void OSShutdownSystem_HLE() { RT_LOGF(RT_TAG_OS, "OSShutdownSystem: quitting\n"); ExitForAuroraWindowClose(); }
PPC_NATIVE_OVERRIDE_VOID(80273778, OSShutdownSystem_HLE, (), ());
extern "C" void OSRebootForNANDAPP_HLE() { RT_LOGF(RT_TAG_OS, "__OSRebootForNANDAPP: quitting\n"); ExitForAuroraWindowClose(); }
PPC_NATIVE_OVERRIDE_VOID(8027388C, OSRebootForNANDAPP_HLE, (), ());
extern "C" void OSRestart_HLE() { RT_LOGF(RT_TAG_OS, "OSRestart: quitting\n"); ExitForAuroraWindowClose(); }
PPC_NATIVE_OVERRIDE_VOID(8027393C, OSRestart_HLE, (), ());
extern "C" void OSReturnToMenu_HLE() { RT_LOGF(RT_TAG_OS, "OSReturnToMenu: quitting\n"); ExitForAuroraWindowClose(); }
PPC_NATIVE_OVERRIDE_VOID(802739C8, OSReturnToMenu_HLE, (), ());
extern "C" void OSReturnToMenuForError_HLE() { RT_LOGF(RT_TAG_OS, "__OSReturnToMenuForError: quitting\n"); ExitForAuroraWindowClose(); }
PPC_NATIVE_OVERRIDE_VOID(80273A9C, OSReturnToMenuForError_HLE, (), ());

// 0x801B89E0 -> exit(int status)
extern "C" uint32_t Exit_801AE58C(int status)
{
    RT_LOGF(RT_TAG_OS, "exit(status=%d) called via 0x80277888\n", status);
    std::fflush(stderr);
    if (status != 0) {
        const std::string details =
            "Super Paper Mario called exit(" + std::to_string(status) + ").";
        RuntimeCrash::WriteCrashArtifacts("exit", details);
        ShowRuntimeFatalPopup("the game exited with an error status", details);
        MarkFatalErrorReported();
    }
    SetRuntimeExitCode(status);
    if (status == 0) ExitForAuroraWindowClose();
    if (RuntimeFrontend::Active()) RuntimeFrontend::Fail("Guest called exit");
    std::exit(status);
    return static_cast<uint32_t>(status);
}

PPC_NATIVE_OVERRIDE(80277888, Exit_801AE58C, uint32_t, (int status), (status));
