#pragma once
// Save states: the calls a resumable translated function makes (ResumableFunctionRewriter in the
// translator). While a guest thread's native stack is being
// rebuilt, each resumable frame on its chain jumps to the call site the state recorded for it.
#include <cstdint>

struct CpuContext;

namespace SaveStateResume {
// A rebuild is under way on this guest thread and has not reached its HLE wait yet.
bool Active() noexcept;
// Consume this frame: returns the guest return address of its suspended call.
uint32_t Enter(uint32_t function);
// The target of a resumed indirect call (the next frame's function), else `original`.
uint32_t Target(uint32_t original) noexcept;
// The registers the thread had when it entered its HLE wait: the values a frame kept native.
const CpuContext& YieldEntry() noexcept;
[[noreturn]] void Mismatch(uint32_t function);
}  // namespace SaveStateResume
