#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

// Deterministic ("lockstep") timing, the base of network play. Copies of the game on different
// devices stay in step only if every guest-visible clock advances identically on all of them, so in
// this mode the time base, VI retraces, audio DMA, OS alarms and sleep timers run on one virtual
// timeline instead of the host's steady clock:
//  * while guest code runs, virtual time stands still apart from a small step per mftb read, so a
//    loop polling the time base still sees it move;
//  * when no guest thread can run, the scheduler moves it straight to the next due event, after
//    waiting for real time to reach that instant.
// Real time only paces the timeline; no guest-visible decision depends on it. The wall-clock pumps
// that deliver retraces, alarms and audio blocks from the middle of GX work or host frame waits are
// off in this mode, since where they land in the guest's execution depends on host speed.
namespace DetClock {

inline constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();

namespace detail {
extern bool g_enabled;
} // namespace detail

inline bool Enabled() noexcept { return detail::g_enabled; }
// Turns the mode on. Must happen before the guest starts.
void Enable() noexcept;
void Reset() noexcept;

// Virtual time-base ticks since boot (60.75 MHz).
uint64_t Now() noexcept;
// Now(), then the per-read step: for the guest's own time-base reads.
uint64_t ReadForGuest() noexcept;
// Moves the timeline forward to `ticks` (never back).
void AdvanceTo(uint64_t ticks) noexcept;
// Sleeps until real time reaches the virtual instant `ticks`. A device that falls more than a few
// frames behind (a slow scene, a debugger) re-anchors instead of fast-forwarding to catch up.
void PaceTo(uint64_t ticks);
// Forgets the real-time anchor, so the next PaceTo starts a fresh one (after a long host wait).
void ResetPacing() noexcept;

// Now() as a steady_clock time point on the virtual timeline, for code that keeps its deadlines as
// time points. Never mix these with real Clock::now() values.
std::chrono::steady_clock::time_point NowTimePoint() noexcept;
// The first tick at which NowTimePoint() reaches `point` (the inverse of NowTimePoint, rounded up).
uint64_t TicksAtOrAfter(std::chrono::steady_clock::time_point point) noexcept;

} // namespace DetClock
