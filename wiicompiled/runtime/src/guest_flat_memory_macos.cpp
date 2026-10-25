#include "guest_flat_memory.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace GuestFlat {
bool g_requiresCheckedAccess = false;
namespace {
struct Mapping { uint32_t base; uint64_t size; uint8_t* host; };
std::mutex g_mutex;
std::vector<Mapping> g_mappings;
std::vector<RegionRequest> g_layout;
uint8_t* g_base = nullptr;
bool g_active = false;

uint64_t Offset(const RegionRequest& r) {
    if (r.backing == Backing::Mem1) return r.base & 0x1fffffffu;
    if (r.backing == Backing::Mem2) return (r.base & 0x1fffffffu) - 0x10000000u;
    return 0;
}
bool Same(const std::vector<RegionRequest>& a, const std::vector<RegionRequest>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) { return x.base == y.base && x.size == y.size && x.backing == y.backing; });
}
int BackingFile(size_t size) {
    char name[] = "/tmp/wiicompiled-guest-XXXXXX";
    const int fd = mkstemp(name);
    if (fd >= 0) { unlink(name); if (ftruncate(fd, static_cast<off_t>(size)) != 0) { close(fd); return -1; } }
    return fd;
}
} // namespace

bool IsActive() { return g_active; }
void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard lock(g_mutex);
    g_requiresCheckedAccess = static_cast<size_t>(getpagesize()) > kGuestPageSize;
    if (g_active) {
        if (!Same(g_layout, regions)) throw std::runtime_error("flat guest layout cannot be remapped");
        // Keep fixed aliases, but restore power-on RAM just like the other flat-memory backend.
        for (const auto& mapping : g_mappings) std::memset(mapping.host, 0, mapping.size);
        return;
    }
    mach_vm_address_t address = kFixedFlatGuestBase;
    if (mach_vm_allocate(mach_task_self(), &address, kGuestSpaceSize, VM_FLAGS_FIXED) != KERN_SUCCESS || address != kFixedFlatGuestBase)
        throw std::runtime_error("unable to reserve fixed 4 GiB macOS guest address space");
    g_base = reinterpret_cast<uint8_t*>(address);
    struct Store { Backing kind; uint32_t owned; uint64_t size; int fd; };
    std::vector<Store> stores;
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        auto it = std::find_if(stores.begin(), stores.end(), [&](const Store& s) { return s.kind == r.backing && s.owned == owned; });
        const uint64_t need = Offset(r) + r.size;
        if (it == stores.end()) stores.push_back({r.backing, owned, need, -1}); else it->size = std::max(it->size, need);
    }
    for (auto& s : stores) { s.fd = BackingFile(s.size); if (s.fd < 0) throw std::runtime_error("unable to create macOS guest backing store"); }
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        const auto& s = *std::find_if(stores.begin(), stores.end(), [&](const Store& x) { return x.kind == r.backing && x.owned == owned; });
        auto* host = static_cast<uint8_t*>(mmap(nullptr, r.size, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, Offset(r)));
        auto* guest = mmap(g_base + r.base, r.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, s.fd, Offset(r));
        if (host == MAP_FAILED || guest != g_base + r.base) throw std::runtime_error("unable to map macOS guest alias");
        g_mappings.push_back({r.base, r.size, host});
    }
    for (auto& s : stores) close(s.fd);
    g_layout = regions; g_active = true;
}
uint8_t* HostPointer(uint32_t a) { for (const auto& m : g_mappings) if (a >= m.base && uint64_t(a - m.base) < m.size) return m.host + (a - m.base); return nullptr; }
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}
FaultCounters Counters() { return {}; }
void LogFaultSummary() noexcept {}
bool HandleAccessViolation(void*, bool) noexcept { return false; }
} // namespace GuestFlat
