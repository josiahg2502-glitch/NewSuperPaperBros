#pragma once
#include <cstdint>
#include <stdexcept>

namespace RuntimeFrontend {
// Internal runtime hooks; inactive in standalone builds.
bool Active() noexcept;
const char* LastError() noexcept;
void Printf(const char* format, ...);
struct Cancelled {};
void CheckCancellation();
void SuspendAtVI();
// The retrace that ends a frontend frame requests the suspend; the scheduler's idle loop performs it.
void RequestSuspendAtVI() noexcept;
bool SuspendRequested() noexcept;
void SuspendIfRequested();
// Half-rate mode (set before Initialize): a frontend frame spans `retraces` VI retraces and the game's
// VIWaitForRetrace waits that many, as when a frame overruns on hardware; 1 is the normal 60 fps.
void SetFrameStep(uint32_t retraces) noexcept;
uint32_t FrameStep() noexcept;
// Frame timing: time spent presenting (GX encode/submit) since the last take, in nanoseconds.
void AddPresentTime(uint64_t nanoseconds) noexcept;
uint64_t TakePresentTime() noexcept;
[[noreturn]] void Fail(const char* message);
}
