#include <xxhash.h>
#include <algorithm>
#include <vector>
#include <cstring>
#include <unordered_map>
#include <mutex>
// gx_init.cpp - GX Initialization and FIFO Management
#include "gx_internal.h"
#include "runtime_log.h"
#include "save_state.h"

#include <cstdio>

// Forward declarations for HLE functions used by GXInit
extern "C" void __GX__FifoInit_8016d180();
extern "C" void __GX__PEInit_8016ee14();
extern "C" void __GX__SetTmemConfig_80171458(uint32_t mode);
extern "C" void GX__InitFifoBase_8016c7c8(uint32_t fa, uint32_t ba, uint32_t s);
// GX__SetCPUFifo_8016c94c is already declared in gx_internal.h.
extern "C" void GX__SetGPFifo_8016cb2c(uint32_t fa);
extern "C" GXFifoObj* GXInit(void* base, u32 size);

// ============================================================================
// GXInit
// ============================================================================


/**
 * GXInit HLE - Initialize the Graphics subsystem
 * This replaces the translated function that writes to MMIO addresses.
 */
extern "C" uint32_t GX__Init_8016b850(uint32_t fifoBase, uint32_t fifoSize)
{
    constexpr uint32_t kFifoObjAddr = 0x80521520u;  // FifoObj (NSMBW)
    constexpr uint32_t kGXDataAddr = 0x805215A0u;   // gxData (NSMBW)
    constexpr uint32_t kGXDataSize = 0x600u;

    GXInit(GuestToHostPtr(fifoBase, fifoSize), fifoSize);

    // Initialize GXData structure in guest memory
    try {
        for (uint32_t offset = 0; offset < kGXDataSize; offset += 4) {
            Memory::Write32(kGXDataAddr + offset, 0);
        }
        Memory::Write8(kGXDataAddr + 0x5f8, 0);
        Memory::Write8(kGXDataAddr + 0x5f9, 1);
        Memory::Write8(kGXDataAddr + 0x5fa, 1);
        Memory::Write32(kGXDataAddr + 0x5e4, 0);
        Memory::Write32(kGXDataAddr + 0x5e8, 0);
        Memory::Write32(kGXDataAddr + 0x5fc, 0);
        Memory::Write32(kGXDataPtrAddr, kGXDataAddr);
        // GXData+0x5F8 was just cleared; drop any stale recording shadow with it.
        BeginDisplayListRecording(0, 0);
    } catch (const ::Memory::AccessViolation& e) {
        RT_LOGF(RT_TAG_GX, "Memory access violation during GXData init: %s\n", e.what());
    }

    __GX__FifoInit_8016d180();
    GX__InitFifoBase_8016c7c8(kFifoObjAddr, fifoBase, fifoSize);
    GX__SetCPUFifo_8016c94c(kFifoObjAddr);
    GX__SetGPFifo_8016cb2c(kFifoObjAddr);
    __GX__PEInit_8016ee14();

    try {
        uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            Memory::Write32(gd + 0x254, 0);
            Memory::Write32(gd + 0x174, 0x0f0000ff);
            Memory::Write32(gd + 0x7c, 0x22000000);
            Memory::Write32(gd + 0x170, 0x27000000);
            const uint32_t baseRegs[] = {0x30, 0x38};
            for (int i = 0; i < 2; ++i) {
                uint32_t r = baseRegs[i];
                Memory::Write32(gd + 0x108 + i * 0x10, r << 24);
                Memory::Write32(gd + 0x128 + i * 0x10, (r + 1) << 24);
                Memory::Write32(gd + 0x10c + i * 0x10, (r + 2) << 24);
                Memory::Write32(gd + 0x12c + i * 0x10, (r + 3) << 24);
                Memory::Write32(gd + 0x110 + i * 0x10, (r + 4) << 24);
                Memory::Write32(gd + 0x130 + i * 0x10, (r + 5) << 24);
                Memory::Write32(gd + 0x114 + i * 0x10, (r + 6) << 24);
                Memory::Write32(gd + 0x134 + i * 0x10, (r + 7) << 24);
            }
        }
    } catch (...) {}

    __GX__SetTmemConfig_80171458(2);

    RT_LOGF(RT_TAG_GX, "GX initialized, FIFO at 0x%08X (base=0x%08X size=0x%08X)\n",
            kFifoObjAddr, fifoBase, fifoSize);

    return kFifoObjAddr;
}
PPC_NATIVE_OVERRIDE(80289cb4, GX__Init_8016b850, uint32_t, (uint32_t fifoBase, uint32_t fifoSize), (fifoBase, fifoSize));

// ============================================================================
// FIFO Management
// ============================================================================

extern "C" void GX__InitFifoBase_8016c7c8(uint32_t fa, uint32_t ba, uint32_t s) { GXInitFifoBase((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj)), GuestToHostPtr(ba, s), s); }

extern "C" void GX__SetCPUFifo_8016c94c(uint32_t fa) { GXSetCPUFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(8028adac, GX__SetCPUFifo_8016c94c, (uint32_t fa), (fa));

extern "C" void GX__SetGPFifo_8016cb2c(uint32_t fa) { GXSetGPFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(8028af8c, GX__SetGPFifo_8016cb2c, (uint32_t fa), (fa));

extern "C" void __GX__SaveFifo_8016cdbc(uint32_t fa) { GXSaveCPUFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(8028b21c, __GX__SaveFifo_8016cdbc, (uint32_t fa), (fa));

extern "C" void GX__GetCPUFifo_8016cf10(uint32_t fa) { auto* d=(GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj)); auto* s=GXGetCPUFifo(); if(d&&s) std::memcpy(d,s,sizeof(GXFifoObj)); }
PPC_NATIVE_OVERRIDE_VOID(8028b324, GX__GetCPUFifo_8016cf10, (uint32_t fa), (fa));

extern "C" void __GX__FifoInit_8016d180()
{
    constexpr uint32_t kCpInterruptId = 0x11u;
    constexpr uint32_t kCpInterruptMask = 0x4000u;
    constexpr uint32_t kCpInterruptHandlerAddr = 0x8028aac8u;
    constexpr uint32_t kGxCurrentThreadPtrAddr = 0x8056dfa4u;  // __GXCurrentThread (NSMBW)
    constexpr uint32_t kGxThreadQueueAddr = 0x8056dfa0u;       // GXOverflowSuspendInProgress (NSMBW)
    constexpr uint32_t kCpuFifoObjAddr = 0x80521BC4u;  // CPUFifo (NSMBW)
    constexpr uint32_t kGpFifoObjAddr = 0x80521BA0u;   // GPFifo (NSMBW)
    constexpr size_t kFifoObjSize = 0x24u;
    constexpr uint32_t kFifoWrapFlagAddr = 0x8056df90u;   // CPUFifoReady (NSMBW)
    constexpr uint32_t kFifoWrapFlag2Addr = 0x8056df91u;  // GPFifoReady (NSMBW)

    __OSSetInterruptHandler_801a65f8_hle(kCpInterruptId, kCpInterruptHandlerAddr);
    __OSUnmaskInterrupts_801a69bc_hle(kCpInterruptMask);

    try { Memory::Write32(kGxCurrentThreadPtrAddr, OS__GetCurrentThread_801a98b0_hle()); } catch (const ::Memory::AccessViolation&) {}
    try { Memory::Write32(kGxThreadQueueAddr, 0); } catch (const ::Memory::AccessViolation&) {}

    auto zeroBlock = [](uint32_t addr, size_t sizeBytes) {
        for (size_t offset = 0; offset < sizeBytes; offset += 4) {
            Memory::Write32(addr + static_cast<uint32_t>(offset), 0);
        }
    };

    try { zeroBlock(kCpuFifoObjAddr, kFifoObjSize); } catch (const ::Memory::AccessViolation&) {}
    try { zeroBlock(kGpFifoObjAddr, kFifoObjSize); } catch (const ::Memory::AccessViolation&) {}

    try { Memory::Write8(kFifoWrapFlagAddr, 0); } catch (const ::Memory::AccessViolation&) {}
    try { Memory::Write8(kFifoWrapFlag2Addr, 0); } catch (const ::Memory::AccessViolation&) {}
}

extern "C" void __GX__PEInit_8016ee14()
{
    constexpr uint32_t kPeTokenInterruptId = 0x12u;
    constexpr uint32_t kPeFinishInterruptId = 0x13u;
    constexpr uint32_t kPeTokenInterruptMask = 0x1000u;
    constexpr uint32_t kPeFinishInterruptMask = 0x2000u;
    constexpr uint32_t kPeTokenHandlerAddr = 0x8028cf90;
    constexpr uint32_t kPeFinishHandlerAddr = 0x8028d058;
    constexpr uint32_t kPeThreadQueueAddr = 0x8056dfb0u;  // FinishQueue (NSMBW)

    __OSSetInterruptHandler_801a65f8_hle(kPeTokenInterruptId, kPeTokenHandlerAddr);
    __OSSetInterruptHandler_801a65f8_hle(kPeFinishInterruptId, kPeFinishHandlerAddr);
    __OSUnmaskInterrupts_801a69bc_hle(kPeTokenInterruptMask);
    __OSUnmaskInterrupts_801a69bc_hle(kPeFinishInterruptMask);

    try { Memory::Write32(kPeThreadQueueAddr, 0); } catch (const ::Memory::AccessViolation&) {}
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            const uint16_t cur = Memory::Read16(gd + 0x0Au);
            Memory::Write16(gd + 0x0Au, static_cast<uint16_t>(cur | 0x000Fu));
        }
    } catch (const ::Memory::AccessViolation&) {}
}
PPC_NATIVE_OVERRIDE_VOID(8028d0d8, __GX__PEInit_8016ee14, (), ());

// ============================================================================
// Display List Recording
// ============================================================================

static bool g_spmAuroraDlActive = false;
// Guest lists Aurora encoded (they carry its GX_LOAD_AURORA commands, which the runtime's list walker
// does not model), keyed by address -> size; GX__CallDisplayList hands these straight to Aurora.
static std::mutex g_auroraDlMutex;
static std::unordered_map<uint32_t, uint32_t> g_auroraDls;
bool SpmAuroraDlRecording() { return g_spmAuroraDlActive; }
void RegisterAuroraDisplayList(uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> lock(g_auroraDlMutex);
    if (size) g_auroraDls[addr] = size; else g_auroraDls.erase(addr);
}
bool IsAuroraDisplayList(uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> lock(g_auroraDlMutex);
    auto it = g_auroraDls.find(addr);
    return it != g_auroraDls.end() && size <= it->second + 32u;
}
static uint32_t g_spmAuroraDlAddr = 0;
static uint32_t g_spmAuroraDlSize = 0;
extern "C" bool GXDisplayListIsEmpty();
void SpmRecordRawVertexDisplayList() {
    // shortcut: mixed state/geometry lists retain host encoding; defer their vertices if a failing case appears.
    if (!g_spmAuroraDlActive || !GXDisplayListIsEmpty()) return;
    // Geometry-only lists inherit their vertex layout at playback, not at recording.
    ::GXEndDisplayList();
    g_spmAuroraDlActive = false;
    BeginDisplayListRecording(g_spmAuroraDlAddr, g_spmAuroraDlSize);
}
// Aurora's list encoding is larger than real GX (SPM's extdrv asserts dl_size < 128), so lists are
// recorded host-side and the guest buffer only receives a 32-byte stub naming the recording. The stub
// travels with guest copies (mapdrv records into scratch and copies), unlike an address key.
static std::vector<uint8_t> g_spmDlScratch;
static std::vector<std::vector<uint8_t>> g_hostDls;
static std::unordered_map<uint64_t, uint32_t> g_hostDlByHash;
static constexpr uint8_t kHostDlMagic[8] = {0, 0, 0, 'S', 'P', 'M', 'D', 'L'};  // leading GX NOPs
bool SpmHostDisplayList(const uint8_t* list, uint32_t n, const uint8_t*& data, uint32_t& size) {
    if (n < 12 || std::memcmp(list, kHostDlMagic, 8) != 0) return false;
    const uint32_t id = (uint32_t(list[8]) << 24) | (uint32_t(list[9]) << 16) | (uint32_t(list[10]) << 8) | list[11];
    std::lock_guard<std::mutex> lock(g_auroraDlMutex);
    if (id >= g_hostDls.size()) return false;
    data = g_hostDls[id].data();
    size = static_cast<uint32_t>(g_hostDls[id].size());
    return true;
}

extern "C" void aurora_gx_get_object_ids(uint32_t* tex, uint32_t* tlut);
extern "C" void aurora_gx_set_object_ids(uint32_t tex, uint32_t tlut);

// Save states: the host-side display list recordings the guest's stubs name (their texture and array
// pointers address the fixed flat guest mapping, so they stay valid in another process), the Aurora
// list registry and Aurora's texture/TLUT object id counters. Refused while a list is being recorded.
bool GXDL_HLE_SaveState(SaveState::Writer& out, std::string& error) {
    if (g_spmAuroraDlActive) {
        error = "a display list is being recorded";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_auroraDlMutex);
    out.Pod(uint32_t(g_hostDls.size()));
    for (const auto& list : g_hostDls) {
        out.Pod(uint32_t(list.size()));
        out.Bytes(list.data(), list.size());
    }
    std::vector<std::pair<uint64_t, uint32_t>> byHash(g_hostDlByHash.begin(), g_hostDlByHash.end());
    std::sort(byHash.begin(), byHash.end());
    out.Pod(uint32_t(byHash.size()));
    for (const auto& [hash, id] : byHash) { out.Pod(hash); out.Pod(id); }
    std::vector<std::pair<uint32_t, uint32_t>> aurora(g_auroraDls.begin(), g_auroraDls.end());
    std::sort(aurora.begin(), aurora.end());
    out.Pod(uint32_t(aurora.size()));
    for (const auto& [address, size] : aurora) { out.Pod(address); out.Pod(size); }
    uint32_t tex = 0, tlut = 0;
    aurora_gx_get_object_ids(&tex, &tlut);
    out.Pod(tex);
    out.Pod(tlut);
    return true;
}

void GXDL_HLE_LoadState(SaveState::Reader& in) {
    std::lock_guard<std::mutex> lock(g_auroraDlMutex);
    g_hostDls.assign(in.Get<uint32_t>(), {});
    for (auto& list : g_hostDls) {
        const uint32_t size = in.Get<uint32_t>();
        if (const uint8_t* bytes = in.Take(size)) list.assign(bytes, bytes + size);
    }
    g_hostDlByHash.clear();
    for (uint32_t i = 0, n = in.Get<uint32_t>(); i < n && in.Ok(); ++i) {
        const uint64_t hash = in.Get<uint64_t>();
        g_hostDlByHash[hash] = in.Get<uint32_t>();
    }
    g_auroraDls.clear();
    for (uint32_t i = 0, n = in.Get<uint32_t>(); i < n && in.Ok(); ++i) {
        const uint32_t address = in.Get<uint32_t>();
        g_auroraDls[address] = in.Get<uint32_t>();
    }
    const uint32_t tex = in.Get<uint32_t>(), tlut = in.Get<uint32_t>();
    if (in.Ok()) aurora_gx_set_object_ids(tex, tlut);
    g_spmAuroraDlActive = false;
}

extern "C" void GX__BeginDisplayList_80172e00(uint32_t la, uint32_t s) {
    try {
        uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (!gd) return;
        if (Memory::Read32(gd + 0x5FCu)) GX__SetDirtyState_8016ee78();
        if (Memory::Read8(gd + 0x5F9u)) std::memcpy(Memory::GetPointer(0x80521EF0, 0x600), Memory::GetPointer(gd, 0x600), 0x600);
        Memory::Write32(0x80521E74, la + s - 4u);
        Memory::Write32(0x80521E8C, 0);
        Memory::Write32(0x80521E70, la);
        Memory::Write32(0x80521E78, s);
        Memory::Write32(0x80521E84, la);
        Memory::Write32(0x80521E88, la);
        Memory::Write8(gd + 0x5F8u, 1u);
        // Mirror the guest fifo-object fields the FIFO write path consumes so
        // HleFifoWrite never has to read them back out of guest memory.
        GXFlush();
        // SPM compiles map materials into display lists from native GX state calls (GXSetTevOp,
        // GXLoadTexObj, GXSetCullMode...), which the vertex-only shadow recorder never captured, so
        // GXEndDisplayList returned 0 and mapdrv asserted. Aurora encodes every GX call into its FIFO,
        // so let it record the whole list straight into the guest buffer.
        if (GuestToHostPtr(la, s) && s >= 32u) {
            if (g_spmDlScratch.empty()) g_spmDlScratch.resize(4u << 20);
            g_spmAuroraDlSize = s;
            ::GXBeginDisplayList(g_spmDlScratch.data(), static_cast<u32>(g_spmDlScratch.size()));
            g_boundTexMaps.fill(BoundTexInfo{});  // every texture load must land in the list, not be skipped
            g_spmAuroraDlActive = true;
            g_spmAuroraDlAddr = la;
        } else {
            BeginDisplayListRecording(la, s);
        }
        GX__GetCPUFifo_8016cf10(0x805224F0);
        GX__SetCPUFifo_8016c94c(0x80521E70);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(802912fc, GX__BeginDisplayList_80172e00, (uint32_t la, uint32_t s), (la, s));

extern "C" uint32_t GX__EndDisplayList_80172eb4() {
    try {
        uint32_t auroraBytes = 0;
        const bool auroraDl = g_spmAuroraDlActive;
        if (auroraDl) {
            const uint32_t recorded = ::GXEndDisplayList();
            g_spmAuroraDlActive = false;
            uint32_t id = 0;
            {
                std::lock_guard<std::mutex> lock(g_auroraDlMutex);
                const uint64_t h = XXH3_64bits(g_spmDlScratch.data(), recorded) ^ (uint64_t(recorded) << 40);
                auto it = g_hostDlByHash.find(h);
                if (it != g_hostDlByHash.end()) {
                    id = it->second;
                } else {
                    id = static_cast<uint32_t>(g_hostDls.size());
                    g_hostDls.emplace_back(g_spmDlScratch.begin(), g_spmDlScratch.begin() + recorded);
                    g_hostDlByHash.emplace(h, id);
                }
            }
            uint8_t stub[32] = {};
            std::memcpy(stub, kHostDlMagic, 8);
            stub[8] = uint8_t(id >> 24); stub[9] = uint8_t(id >> 16); stub[10] = uint8_t(id >> 8); stub[11] = uint8_t(id);
            if (uint8_t* dst = static_cast<uint8_t*>(GuestToHostPtr(g_spmAuroraDlAddr, 32))) std::memcpy(dst, stub, 32);
            auroraBytes = 32u;
            RegisterAuroraDisplayList(g_spmAuroraDlAddr, auroraBytes);
            static int s_dlLog = 0;
            if (s_dlLog++ < 40) RT_LOGF(RT_TAG_GX, "[spmdl] recorded list 0x%08X: %u bytes\n", g_spmAuroraDlAddr, auroraBytes);
            if (std::getenv("SPM_DL_HEX") && id < 64) {  // diagnostic: hex of each recorded host list
                static bool seen[64] = {};
                if (!seen[id]) { seen[id] = true; std::string h; for (uint32_t b = 0; b < g_hostDls[id].size() && b < 1024; ++b) { char t[4]; std::snprintf(t, sizeof t, "%02x", g_hostDls[id][b]); h += t; }
                    RT_LOGF(RT_TAG_GX, "[spmdlhex] id %u %s\n", id, h.c_str()); }
            }
            g_boundTexMaps.fill(BoundTexInfo{});  // live texture bindings are unknown after recording
        }
        GXFlush();
        GX__GetCPUFifo_8016cf10(0x80521E70);
        const uint8_t wrapped = Memory::Read8(kDlFifoAddr + kDlWrapFlagOffset);
        GX__SetCPUFifo_8016c94c(0x805224F0);

        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            if (Memory::Read8(gd + 0x5F9u) != 0) {
                const int32_t interruptLevel = OS__DisableInterrupts_801a65ac();
                const uint32_t savedWord8 = Memory::Read32(gd + 0x08u);
                std::memcpy(Memory::GetPointer(gd, 0x600),
                            Memory::GetPointer(0x80521EF0, 0x600),
                            0x600);
                Memory::Write32(gd + 0x08u, savedWord8);
                OS__RestoreInterrupts_801a65d4(interruptLevel);
            }
            Memory::Write8(gd + 0x5F8u, 0);
        }

        // Publish the cached cursor/count before the count is read back below.
        // Done unconditionally: the original read of GXData+0x5F8 returned
        // "inactive" whenever the GXData pointer was null, so recording must
        // stop here even on the gd == 0 path.
        EndDisplayListRecording();

        if (auroraDl) {
            Memory::Write32(kDlCountAddr, auroraBytes);
            return auroraBytes;
        }
        return wrapped == 0 ? Memory::Read32(kDlCountAddr) : 0;
    } catch (...) {
        // Leaving the shadow active would silently swallow every subsequent
        // FIFO write into a dead list.
        EndDisplayListRecording();
        return 0;
    }
}
PPC_NATIVE_OVERRIDE(802913AC, GX__EndDisplayList_80172eb4, uint32_t, (), ());

// ============================================================================
// Flush
// ============================================================================

extern "C" void GX__Flush_8016e654() { GXFlush(); }
PPC_NATIVE_OVERRIDE_VOID(8028c8e8, GX__Flush_8016e654, (), ());
