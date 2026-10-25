#pragma once
// Save states: guest-thread continuations.
//
// At a frontend VI suspend every started guest thread is parked in an HLE wait it entered from
// translated code. A YieldScope records that wait (its kind, the registers at entry and the wait's own
// state). Capture walks each thread's guest call chain from those registers and checks it against the
// resumable function list; restore rebuilds each thread's native stack the first time it runs again by
// re-entering the chain's functions at their call sites (SaveStateResume) down to the wait, which then
// continues after its sleep.
#include "ppc_runtime.h"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <initializer_list>
#include <string>
#include <vector>

namespace SaveState {

enum class YieldKind : uint32_t { None = 0, ViWaitForRetrace = 1, MessageQueue = 2, Sleep = 3 };

struct YieldRecord {
    YieldKind kind = YieldKind::None;
    CpuContext entry{};      // registers when the wait was entered from translated code
    uint32_t extra[6] = {};  // the wait's own state (retrace count, queue, message pointer, ...)
};

// Held for the whole HLE wait; only the outermost wait of a guest thread is recorded.
class YieldScope {
public:
    YieldScope(YieldKind kind, const CpuContext& entry, std::initializer_list<uint32_t> extra);
    explicit YieldScope(const YieldRecord& resumed);  // a wait continuing after a restore
    ~YieldScope();
    YieldScope(const YieldScope&) = delete;
    YieldScope& operator=(const YieldScope&) = delete;

private:
    uint32_t thread_ = 0;
    bool owner_ = false;
};

// Called at the top of each HLE wait: when this thread's rebuild has reached it, ends the rebuild and
// returns the recorded wait (to continue after its sleep), else nullptr. `running` tells whether the
// thread was the one in the scheduler's idle loop at capture.
const YieldRecord* TakeYield(YieldKind kind, bool& running);

// Byte stream of a state: tagged sections of trivially copyable values (same core build only).
class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : out_(out) {}
    void Bytes(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        out_.insert(out_.end(), bytes, bytes + size);
    }
    template <class T> void Pod(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        Bytes(&value, sizeof(T));
    }
    // A section: tag, payload size, payload. Begin returns what End needs to fill in the size.
    size_t Begin(uint32_t tag) {
        Pod(tag);
        const size_t at = out_.size();
        Pod(uint64_t(0));
        return at;
    }
    void End(size_t at) {
        const uint64_t size = out_.size() - at - sizeof(uint64_t);
        std::memcpy(out_.data() + at, &size, sizeof(size));
    }

private:
    std::vector<uint8_t>& out_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    void Bytes(void* data, size_t size) {
        if (!ok_ || size_t(end_ - p_) < size) { ok_ = false; return; }
        std::memcpy(data, p_, size);
        p_ += size;
    }
    template <class T> void Pod(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        Bytes(&value, sizeof(T));
    }
    template <class T> T Get() { T value{}; Pod(value); return value; }
    // The next section, which must be `tag`: a reader over its payload (failed if absent or truncated).
    Reader Section(uint32_t tag) {
        const uint32_t found = Get<uint32_t>();
        const uint64_t size = Get<uint64_t>();
        const uint8_t* payload = ok_ && found == tag && size <= uint64_t(end_ - p_) ? Take(size_t(size)) : nullptr;
        if (!payload) { ok_ = false; return Reader(nullptr, 0, false); }
        return Reader(payload, size_t(size));
    }
    // A view of the next `size` bytes (nullptr past the end), consumed.
    const uint8_t* Take(size_t size) {
        if (!ok_ || size_t(end_ - p_) < size) { ok_ = false; return nullptr; }
        const uint8_t* at = p_;
        p_ += size;
        return at;
    }
    bool Ok() const { return ok_; }
    bool AtEnd() const { return ok_ && p_ == end_; }

private:
    Reader(const uint8_t* data, size_t size, bool ok) : p_(data), end_(data + size), ok_(ok) {}
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

constexpr uint32_t Tag(const char (&name)[5]) {
    return uint32_t(uint8_t(name[0])) | uint32_t(uint8_t(name[1])) << 8 | uint32_t(uint8_t(name[2])) << 16 |
           uint32_t(uint8_t(name[3])) << 24;
}

struct Frame { uint32_t function = 0, returnAddress = 0; };

struct ThreadContinuation {
    uint32_t thread = 0;
    std::vector<Frame> frames;  // outermost first
    YieldRecord yield;
    bool running = false;
};

// Capture: this thread's recorded wait and its chain, validated. False with a reason when the state
// cannot be captured here (unknown wait, unlisted function, indirect or inconsistent call).
bool CaptureThread(uint32_t thread, uint32_t rootFunction, bool running, ThreadContinuation& out, std::string& error);
bool IsResumable(uint32_t function) noexcept;

// Restore: forget live wait records (their fibers are gone) and queue each thread's continuation for
// its resume root, which calls BeginResume and then the returned outermost function.
void ResetForRestore();
void SetPendingResume(ThreadContinuation continuation);
uint32_t BeginResume(uint32_t thread);

}  // namespace SaveState

// The rest of an OSSleepThread for a restored thread (os_sleep.cpp): called by each HLE wait's resume.
void OS_HLE_SleepResume(CpuContext* cpu, bool running, int32_t irqState);
