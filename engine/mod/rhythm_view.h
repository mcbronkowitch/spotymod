#pragma once
#include <cstdint>

namespace spky {

// The rhythm a lane publishes to whoever wants to place events against it:
// the sample distances between its last three gated boundaries, most recent
// first, as latched at the lane's last cycle wrap.
//
// Lives in the mod layer, NOT in fx/taps.h, because the mod layer must not
// include fx headers -- the same layering rule that keeps SynthEngine out of
// ModLane (see the static_assert in parts/part.cpp). fx/taps.h includes this.
//
// `valid` is false until three onsets have been recorded: two gaps need three
// onsets, and the first onset after init/reset measures from an arbitrary
// starting point rather than from a predecessor, so it is not a rhythm.
//
// A gap is never 0: the ring clamps it to 1 sample at the point it is
// recorded, so a consumer can safely divide by or delay on it without a
// zero-length special case.
//
// Ownership (as of the taps-plumbing cost pass, 2026-07-20): the onset-gap
// ring that produces this view lives on `SuperModulator`, not `ModLane` --
// moved up because only the PITCH/master lane's rhythm was ever consumed
// (`SuperModulator::rhythm()` forwards exactly that lane), so keeping a full
// ring on all five lanes of both parts was nine unread copies. `ModLane`
// itself carries none of this state; it only exposes `fired()` (a gated
// boundary) and `wrapped()` (a cycle wrap), which `SuperModulator::process()`
// consumes to drive the ring -- see mod/super_modulator.cpp.
//
// Consequence: the ring is now fed exclusively from `ModLane::process()`,
// the per-sample path the PITCH lane is always driven through
// (`SuperModulator::process()` never calls `tick()` on it). The `tick()`
// path (the texture-lane control-rate path, spec 2026-07-19
// mod-plane-control-rate) no longer feeds any ring at all, so the
// quantisation contract this comment used to document for it no longer
// describes a reachable situation. The zero-gap clamp above is kept as
// insurance for a future consumer that might feed the ring from `tick()`
// again -- if one appears, its onsets will need the same clamp `process()`'s
// path already relies on, for the same reason: `tick()`'s edge walk can call
// the boundary handler several times inside one kTickInterval window, so a
// naive "samples since the last boundary" read can come out as 0 there.
struct RhythmView {
    int32_t gap[2] = { 0, 0 };
    bool    valid  = false;
};

}  // namespace spky
