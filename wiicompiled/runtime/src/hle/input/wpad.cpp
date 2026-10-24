#include "frame_input.h"
#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "wii_remote_input.h"

#include <cstdint>

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg);

namespace {

constexpr uint32_t kDefaultWorkMemSize = 0x20000;
constexpr uint8_t kDefaultDpdSensitivity = 3;
constexpr int32_t kStatusOk = 0;

struct WpadStubState {
    bool initSubRan = false;
    bool simpleSyncActive = false;
    uint32_t syncDeviceCallback = 0;
    uint32_t workMemSize = kDefaultWorkMemSize;
    uint8_t dpdSensitivity = kDefaultDpdSensitivity;
    WpadContract::State contract{};
};

WpadStubState g_state{};

void InvokeWpadCallback(uint32_t callback, uint32_t chan, int32_t result)
{
    if (callback == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callback)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = chan;
    cpu.gpr[4] = static_cast<uint32_t>(result);
    InvokeIndirectCpu(callback, &cpu);
}

int32_t InitializeWpadLibrary()
{
    g_state.contract.Initialize();
    return kStatusOk;
}

// The async WPAD entry points all report their outcome to the guest callback and
// then return the same value.
int32_t CompleteWpadRequest(uint32_t chan, uint32_t callback, int32_t result)
{
    InvokeWpadCallback(callback, chan, result);
    return result;
}

} // namespace

extern "C" int32_t WPADGetStatus_HLE()
{
    // RVL::WPADGetStatus takes no channel arg; it reports global WUD library state.
    // Reading r3 here would leak a caller's stale register value into the result.
    return g_state.contract.GetLibraryStatus();
}
PPC_NATIVE_OVERRIDE(802CCBB4, WPADGetStatus_HLE, int32_t, (), ());

extern "C" uint32_t WPADGetDpdSensitivity_HLE()
{
    return static_cast<uint32_t>(g_state.dpdSensitivity);
}
PPC_NATIVE_OVERRIDE(802D0874, WPADGetDpdSensitivity_HLE, uint32_t, (), ());

// The emulated remotes always have their pointer camera on (KPAD pos comes from the right stick or mouse). The real
// library function read WPAD's own channel state, which this HLE never sets, so SPM's arcade minigames (Mansion
// Patrol, Forget Me Not) saw the pointer as off and ignored it (issue #35).
extern "C" uint32_t WPADIsDpdEnabled_HLE()
{
    return 1;
}
PPC_NATIVE_OVERRIDE(802D087C, WPADIsDpdEnabled_HLE, uint32_t, (), ());

extern "C" int32_t WPADInitSub_HLE()
{
    if (!g_state.initSubRan) {
        g_state.initSubRan = true;
        return InitializeWpadLibrary();
    }
    return kStatusOk;
}
#if 0  // NSMBW: WPADInitSub is not linked into NSMBW (was MKW-only)
#if 0  // SPM: 0x801bf3b4: no NSMBW symbol at this address
PPC_NATIVE_OVERRIDE(801BF3B4, WPADInitSub_HLE, int32_t, (), ());
#endif
#endif

extern "C" int32_t WPADInit_HLE()
{
    return InitializeWpadLibrary();
}
PPC_NATIVE_OVERRIDE(802CC950, WPADInit_HLE, int32_t, (), ());

extern "C" int32_t WUDGetStatus_HLE()
{
    return WpadContract::kStatusReady;
}
PPC_NATIVE_OVERRIDE(802D97B0, WUDGetStatus_HLE, int32_t, (), ());

// WPADGetDataFormat reads the per-channel format set by WPADSetDataFormat. Can't reuse the
// translated SDK implementation: it dereferences Bluetooth control blocks that HLE'd WPADInit
// never constructs.
extern "C" int32_t WPADGetDataFormat_HLE(uint32_t chan)
{
    return g_state.contract.GetDataFormat(chan);
}
PPC_NATIVE_OVERRIDE(802CE3CC, WPADGetDataFormat_HLE, int32_t, (uint32_t chan), (chan));

// WPADSetDataFormat: records the per-channel data format the game asked for.
extern "C" int32_t WPADSetDataFormat_HLE(uint32_t chan, int32_t format)
{
    return g_state.contract.SetDataFormat(chan, format);
}
PPC_NATIVE_OVERRIDE(802CE414, WPADSetDataFormat_HLE, int32_t, (uint32_t chan, int32_t format), (chan, format));

// WPADProbe: reports the extension type of a Bluetooth remote on `chan`, or no controller.
extern "C" int32_t WPADProbe_HLE(uint32_t chan, uint32_t typePtr)
{
    if (chan >= WpadContract::kChannelCount) {
        return WpadContract::kErrorBadChannel;
    }

    // Drive the rescan state machine here too: a reconnect probe can arrive
    // before the next PADRead, and only Poll() brings a dropped remote back.
    WiiRemoteInput::Poll();

    if (FrameInput::Active()) {
        // Deterministic mode: connections change only at frame boundaries, like the input itself.
        const FrameInput::Remote& remote = FrameInput::Get(chan);
        if (typePtr != 0) {
            uint32_t type = WpadContract::kExtensionCore;
            if (remote.connected && remote.kind == WiiRemoteInput::Kind::RemoteWithNunchuk) type = 1u;
            if (remote.connected && remote.kind == WiiRemoteInput::Kind::RemoteWithClassic) type = 2u;
            Memory::Write32(typePtr, type);
        }
        return remote.connected ? kStatusOk : WpadContract::kErrorNoController;
    }

    // A real Bluetooth remote: WPAD_DEV_CORE (0) for a bare remote,
    // WPAD_DEV_FREESTYLE (1) with a Nunchuk, WPAD_DEV_CLASSIC (2) with a Classic
    // Controller. The game reads the type from here (not from
    // KPADStatus.dev_type) to pick its control scheme, and re-reads it when it
    // changes, which is what makes an extension swap mid-game work like on the
    // console. EffectiveKind keeps the last type through SDL's re-creation of
    // the joystick after a swap.
    const WiiRemoteInput::Kind kind = WiiRemoteInput::EffectiveKind(chan);
    if (WiiRemoteInput::IsRemoteChannel(chan)) {
        if (typePtr != 0) {
            uint32_t type = WpadContract::kExtensionCore;
            if (kind == WiiRemoteInput::Kind::RemoteWithNunchuk) type = 1u;
            if (kind == WiiRemoteInput::Kind::RemoteWithClassic) type = 2u;
            Memory::Write32(typePtr, type);
        }
        return kStatusOk;
    }
    if (typePtr != 0) {
        Memory::Write32(typePtr, WpadContract::kExtensionCore);
    }
    return WpadContract::kErrorNoController;
}
PPC_NATIVE_OVERRIDE(802CE208, WPADProbe_HLE, int32_t, (uint32_t chan, uint32_t typePtr), (chan, typePtr));

extern "C" void WPADControlMotor_HLE(uint32_t chan, uint32_t command)
{
    (void)chan;
    (void)command;
}
PPC_NATIVE_OVERRIDE(802CE684, WPADControlMotor_HLE, void, (uint32_t chan, uint32_t command), (chan, command));

extern "C" int32_t WPADGetInfoAsync_HLE(uint32_t chan, uint32_t infoPtr, uint32_t callback)
{
    if (chan >= WpadContract::kChannelCount) {
        return CompleteWpadRequest(chan, callback, WpadContract::kErrorBadChannel);
    }

    (void)infoPtr;
    return CompleteWpadRequest(chan, callback, WpadContract::kErrorNoController);
}
PPC_NATIVE_OVERRIDE(802CE4C4, WPADGetInfoAsync_HLE, int32_t,
         (uint32_t chan, uint32_t infoPtr, uint32_t callback), (chan, infoPtr, callback));

extern "C" int32_t WPADControlLed_HLE(uint32_t chan, uint32_t ledMask, uint32_t callback)
{
    if (chan >= WpadContract::kChannelCount) {
        return CompleteWpadRequest(chan, callback, WpadContract::kErrorBadChannel);
    }
    (void)ledMask;
    return CompleteWpadRequest(chan, callback, WpadContract::kErrorNoController);
}
#if 0  // SPM: WPADControlLed: name not in SPM
PPC_NATIVE_OVERRIDE(801E1630, WPADControlLed_HLE, int32_t,
         (uint32_t chan, uint32_t ledMask, uint32_t callback), (chan, ledMask, callback));
#endif

extern "C" int32_t WPADStartSimpleSync_HLE()
{
    if (g_state.simpleSyncActive) {
        return 0;
    }
    g_state.simpleSyncActive = true;
    return 1;
}
#if 0  // NSMBW: WPADStartSimpleSync is not linked into NSMBW (was MKW-only)
#if 0  // SPM: 0x801bf634: no NSMBW symbol at this address
PPC_NATIVE_OVERRIDE(801BF634, WPADStartSimpleSync_HLE, int32_t, (), ()); // WUDStartSyncSimple
#endif
#endif
#if 0  // NSMBW: WPADStartSimpleSync is not linked into NSMBW (was MKW-only)
#if 0  // SPM: 0x801bf638: no NSMBW symbol at this address
PPC_NATIVE_OVERRIDE(801BF638, WPADStartSimpleSync_HLE, int32_t, (), ()); // WPADStartSimpleSync (HBM)
#endif
#endif

// due to multiplayer controller screen hle this to avoid startsyncdevice to return fail every frame
// causing you to get softlocked in the game
extern "C" int32_t WPADStopSimpleSync_HLE()
{
    if (g_state.simpleSyncActive) {
        g_state.simpleSyncActive = false;
        const uint32_t callback = g_state.syncDeviceCallback;
        if (callback != 0 && TranslatedFunctionRegistry::FindByAddressPtr(callback)) {
            NandQueueIosCallback(callback, 1, 0); // callback(WUD_SYNC_DONE, devicesSynced=0)
        }
    }
    return 1;
}
PPC_NATIVE_OVERRIDE(802CCBA8, WPADStopSimpleSync_HLE, int32_t, (), ());

extern "C" uint32_t WPADSetSyncDeviceCallback_HLE(uint32_t callback)
{
    const uint32_t previous = g_state.syncDeviceCallback;
    g_state.syncDeviceCallback = callback;
    return previous;
}
#if 0  // NSMBW: WPADSetSyncDeviceCallback is not linked into NSMBW (was MKW-only)
#if 0  // SPM: 0x801bf640: no NSMBW symbol at this address
PPC_NATIVE_OVERRIDE(801BF640, WPADSetSyncDeviceCallback_HLE, uint32_t, (uint32_t callback), (callback));
#endif
#endif

void WPAD_HLE_ResetFrontend() {
    g_state = WpadStubState{};
}
