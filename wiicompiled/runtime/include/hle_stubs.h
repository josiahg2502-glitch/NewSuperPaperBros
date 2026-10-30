#pragma once
#include <string>

#include "abi_bridge.h"

// hle/gx/gx_fatal_stubs.cpp includes nothing but this header and reaches
// std::fprintf / std::snprintf / std::abort through it.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// VI Utils
void VI_HLE_ForceRetrace(CpuContext* ctx);
void VI_HLE_PollRetrace(CpuContext* ctx);
void VI_HLE_ProcessRetracesDeferred(int maxToProcess);
void VI_HLE_WaitForNextRetracePoll();
// Deterministic mode (det_clock.h): the virtual tick of the next VI retrace, DetClock::kNever
// before VIInit.
uint64_t VI_HLE_DetNextRetraceTicks();
namespace SaveState { class Writer; class Reader; }
void VI_HLE_SaveState(SaveState::Writer& out);
void VI_HLE_LoadState(SaveState::Reader& in);
void OS_HLE_SaveState(SaveState::Writer& out);
void OS_HLE_LoadState(SaveState::Reader& in);
void AI_HLE_SaveState(SaveState::Writer& out);
void AI_HLE_LoadState(SaveState::Reader& in);
void KPAD_HLE_SaveState(SaveState::Writer& out);
void KPAD_HLE_LoadState(SaveState::Reader& in);
bool NAND_HLE_SaveState(SaveState::Writer& out, std::string& error);
void NAND_HLE_LoadState(SaveState::Reader& in);
bool GXDL_HLE_SaveState(SaveState::Writer& out, std::string& error);
void GXDL_HLE_LoadState(SaveState::Reader& in);
// Single owner of the Aurora frame presentation sequence (seal, optional pace
// to the VI retrace boundary, pre-warm the next frame). paceToRetrace is true
// for the GXCopyDisp producer path and false for retrace-context presents.
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace);
bool VI_HLE_IsAdvancingRetrace();
// Fast-forward: the game's clock runs `speed` times faster (1 = normal).
void VI_HLE_SetSpeed(uint32_t speed);
uint32_t VI_HLE_Speed();
void VI_HLE_SetXfbReady(uint32_t xfbAddr); // Called by GXCopyDisp to signal EFB→XFB copy
void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros);
void Audio_HLE_Poll(CpuContext* ctx);
// Deferred twin of Audio_HLE_Poll for the long host waits that already service
// retraces and alarms (the VI retrace pacing loop, the Aurora frame-worker wait
// callback). Runs the AI DMA tick on an isolated register file the way
// OS_HLE_ProcessAlarmsDeferred does, so it is safe to call from the middle of an
// arbitrary translated function.
void Audio_HLE_PollDeferred();
// Deterministic mode: the virtual tick at which the AI DMA block in flight completes, or
// DetClock::kNever while audio DMA is stopped.
uint64_t Audio_HLE_DetNextEventTicks();
bool OS_HLE_InterruptsEnabled() noexcept;
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess);
extern "C" void OS_HLE_BeginDeferredGuestCallbacks();
extern "C" void OS_HLE_EndDeferredGuestCallbacks();


// Defines and registers a faithful native reimplementation that REPLACES the translated function
// at a PPC address (not a stub; genuine not-yet-implemented entries live in hle/gx/gx_fatal_stubs.cpp
// and abort). The translator regex-parses these macro names to skip that address at build time, so
// renaming requires updating Translator.Cli/Program.cs, RuntimeNativeGuestEffectAnalyzer.cs,
// TranslatedBuildShardEmitter.cs and RuntimeNativeFunctionAbiProvider.cs together.

#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list) \
    extern "C" ret_type func_##addr_hex arg_list { return name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list) \
    extern "C" void func_##addr_hex arg_list { name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)
