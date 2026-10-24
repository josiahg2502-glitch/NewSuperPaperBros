#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "ppc_runtime.h"

// Forward declarations
struct CpuContext;

// GuestFiberManager: each guest OSThread maps to a host context. A scheduler context picks
// which guest fiber runs; a real timer thread queues VI retraces at the VI cadence. Guest
// threads only switch at explicit yield points (OSSleepThread, OSYieldThread, ...), matching
// Wii cooperative semantics exactly.

namespace Fiber {

// Thread states matching guest OS
enum class ThreadState : uint32_t {
    READY = 1,
    RUNNING = 2,
    WAITING = 4,
    MORIBUND = 8,
};

// Information about a guest fiber
struct GuestFiber {
    void* fiber = nullptr;              // Host context handle
    uint32_t entryPoint = 0;            // Thread entry function
    uint32_t entryArg = 0;              // Argument to entry function
    CpuContext cpuContext{};            // Saved CPU context for this fiber
    ThreadState state = ThreadState::READY;
    bool isSchedulerFiber = false;      // True for the main scheduler fiber
    bool terminated = false;            // Thread has exited
    bool started = false;               // Its fiber has run (save states rebuild started threads)
};

// Save states: one guest thread's fiber bookkeeping, without its host context.
struct FiberRecord {
    uint32_t thread = 0, entryPoint = 0, entryArg = 0;
    CpuContext cpuContext{};
    ThreadState state = ThreadState::READY;
    bool isSchedulerFiber = false, terminated = false, started = false;
};

class GuestFiberManager {
public:
    // Initialize the fiber system - must be called from main thread
    static void Initialize();
    
    // Shutdown the fiber system
    static void Shutdown();
    // Unwind suspended guest stacks while frontend cancellation is active.
    static void CancelFrontendFibers();
    
    // Check if fiber system is initialized
    static bool IsInitialized();
    
    // Create a fiber for a guest thread (called from OSCreateThread HLE).
    // OSCreateThread's stackSize/priority are not passed: the host fiber models
    // neither, only the guest SP seeded from stackBase. Returns true on success.
    static bool CreateGuestFiber(uint32_t guestThreadAddr, uint32_t entryPoint,
                                  uint32_t entryArg, uint32_t stackBase);
    
    // Resume a guest thread (called from OSResumeThread HLE)
    // This marks the fiber as runnable
    static void ResumeGuestThread(uint32_t guestThreadAddr);
    
    // Suspend a guest thread (called from OSSuspendThread HLE)
    static void SuspendGuestThread(uint32_t guestThreadAddr);
    
    // Called when a guest thread exits or is canceled
    static void ExitGuestThread(uint32_t guestThreadAddr, ThreadState finalState);
    
    // Switch to a specific guest thread's fiber (called from OSLoadContext)
    // This saves the current fiber's state and switches to the target
    static void SwitchToThread(uint32_t guestThreadAddr, CpuContext* cpu);
    
    // Get the currently running guest thread address (0 if in scheduler)
    static uint32_t GetCurrentGuestThread();
    
    // Get fiber info for a guest thread (may be null)
    static GuestFiber* GetFiber(uint32_t guestThreadAddr);
    
    // Check for VI retrace and process it (called from scheduler idle)
    static void ProcessTimerEvents(CpuContext* cpu);
    
    // Register the current host fiber as a guest thread fiber
    // This is used for the default/main thread that exists before OSCreateThread
    static bool RegisterMainThreadAsFiber(uint32_t guestThreadAddr, CpuContext* cpu);
    
    // Check if a fiber exists for a guest thread
    static bool HasFiber(uint32_t guestThreadAddr);
    // Diagnostics: every guest thread's state, saved registers and stack backchain. Uses try_lock so
    // a watchdog can call it while guest code holds the fiber mutex without deadlocking.
    static void DumpGuestThreads(std::ostream& os);
    static bool IsTerminated(uint32_t guestThreadAddr);
    // Save states. The bookkeeping SwitchToThread does once the switched-out thread runs again, for a
    // restored thread whose rebuilt fiber is starting there.
    static void CompleteSwitchIn(uint32_t guestThreadAddr, CpuContext* cpu);
    // The boot thread's own host context: Initialize, run on the boot fiber, makes a new one for it.
    static void* SchedulerFiber() { return s_schedulerFiber; }
    // Replace every live fiber's host context: started threads get a resume root (their continuation
    // was queued with SaveState::SetPendingResume), unstarted ones start afresh, and the boot thread
    // (isSchedulerFiber; `boot` is the frontend's own handle to its stack) gets `bootRoot`. Returns the
    // new contexts of the boot and running threads.
    static bool RebuildForRestore(const std::vector<FiberRecord>& table, void* boot, void (*bootRoot)(void*),
                                  uint32_t runningThread, void** newBoot, void** newRunning);
    // Save states: every guest thread's record (sorted by thread) and the current guest thread.
    static void ExportTable(std::vector<FiberRecord>& table, uint32_t& currentThread);
    // Save states: every fiber; try_lock, false when busy.
    static bool ForEachFiber(void (*visit)(uint32_t guestThreadAddr, const GuestFiber& fiber, bool current, void* user),
                             void* user);

private:
    // The fiber entry point wrapper
#if defined(_WIN32)
    static void CALLBACK FiberProc(void* param);
#else
    static void FiberProc(void* param);
#endif
    static void ResumeProc(void* param);
    static void FinishGuestThread(uint32_t guestThreadAddr);
    // Switch from whichever fiber is currently active straight to the scheduler fiber, without
    // the SwitchToThread bookkeeping (CPU context save/restore, s_currentGuestThread). Used for
    // in-fiber yields that aren't a real guest thread switch: waiting out the EGG::Thread::start
    // deferral loop, and returning control on natural thread exit.
    static void SwitchToScheduler();

    // Internal state
    static std::mutex s_mutex;
    static std::unordered_map<uint32_t, GuestFiber> s_fibers;
    static std::vector<void*> s_fibersPendingDelete;
    // The scheduler's own host context. Its opaque handle is supplied by the
    // active HostContext backend, so one field serves every supported host.
    static void* s_schedulerFiber;
    static bool s_schedulerFiberRebuilt;  // s_schedulerFiber came from RebuildForRestore
    static uint32_t s_currentGuestThread;
    static bool s_initialized;
    // Stored CPU context pointer for fiber switches
    static thread_local CpuContext* s_cpuContext;

    static void PurgePendingFibers();
};

// Global pending retrace count. GuestFiberManager::ProcessTimerEvents drains
// this on the guest thread so VI callbacks still run with guest state/locking
// expectations.
extern std::atomic<uint32_t> g_viRetracePendingCount;

} // namespace Fiber
