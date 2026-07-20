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
struct RhythmView {
    int32_t gap[2] = { 0, 0 };
    bool    valid  = false;
};

}  // namespace spky
