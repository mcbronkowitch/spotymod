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
// describes a reachable situation. The zero-gap clamp above is kept, but
// `SuperModulator::process()`'s path does not rely on it -- it cannot reach
// it: that function increments its accumulator (`_since_onset`) once,
// unconditionally, before the point where it reads that accumulator to
// record a gap, and the record point is the only place the accumulator is
// reset to 0. So the accumulator is always >= 1 by the time it is read
// there, and the clamp's `: 1` branch is dead code on this path -- provably
// so from the ordering of those three lines alone, not just because nothing
// currently drives the ring from `tick()`. A future `tick()`-fed consumer
// would need its own accumulator (`tick()`'s edge walk can call the boundary
// handler several times inside one kTickInterval window, so a naive "samples
// since the last boundary" counter there really can read 0) and, with it,
// its own clamp -- it could not inherit this one's, since this one belongs
// to an accumulator that structurally never hits zero at its own read.
struct RhythmView {
    int32_t gap[2] = { 0, 0 };
    bool    valid  = false;
};

}  // namespace spky
