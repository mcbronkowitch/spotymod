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
// A gap is never 0: `ModLane` clamps it to 1 sample at the point it is
// recorded, so a consumer can safely divide by or delay on it without a
// zero-length special case.
//
// Contract for lanes driven through `ModLane::tick()` (the texture-lane
// control-rate path, spec 2026-07-19 mod-plane-control-rate): `tick()`
// advances `_since_onset` once per kTickInterval-sample (96-sample) window,
// but its edge walk can call the boundary handler several times inside that
// same window. Gaps recorded on that path are therefore quantised to
// multiples of kTickInterval, and any onsets after the first one in a
// window collapse to the 1-sample clamp above rather than their true
// intra-window distance. `rhythm()` is only fully sample-accurate on a
// lane driven by `process()`; on a `tick()`-driven lane it is a coarse,
// still-monotonic-with-tempo approximation, not a sample-accurate one.
struct RhythmView {
    int32_t gap[2] = { 0, 0 };
    bool    valid  = false;
};

}  // namespace spky
