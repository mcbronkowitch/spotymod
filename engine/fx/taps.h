#pragma once
#include <cstdint>
#include "mod/rhythm_view.h"

namespace spky {

// Read-only view over the FLUX tape. Moved verbatim from fx/dust.h, which is
// deleted in task 4. There is only a `mask` -- no size/mask pair that could
// disagree; the power-of-two contract is checked once at compile time in
// taps.cpp against Flux::kMaxSamples, not per construction.
struct TapeTap {
    const float* l = nullptr;
    const float* r = nullptr;
    int32_t write_ptr = 0;
    int32_t mask = 0;

    int32_t size() const { return mask + 1; }

    // `offset` is samples BEHIND the write head; the head decrements, so a
    // constant offset is exactly 1x forward playback of material that old.
    float read(bool right, int32_t offset) const {
        int32_t i = (write_ptr + offset) & mask;
        return (right ? r : l)[i];
    }
};

namespace tap_tuning {

// A muted tap. 0 is safe as the sentinel because a sounding offset is always
// >= kMinGap; an offset of 0 would read at the write head and is never wanted.
constexpr int32_t kMuted = 0;

// Below this a "gap" is not a rhythm, it is a buzz -- and 0.75 * g would round
// toward a second gap equal to the first, defeating the uniformity guard.
constexpr int32_t kMinGap = 32;

// Gaps count as uniform when both lie within this fraction of their mean. A
// fraction, not an absolute count: at 240 samples a 2-sample jitter must not
// read as non-uniform, at 30000 a 50-sample drift must not read as uniform.
constexpr float kUniformTol = 0.02f;

// The spread applied when the guard fires: the MOTION lane's x3/4 ratio, a
// polyrhythm the instrument already runs. Cumulative offsets become g*{1,1.75}
// -- a limp, not a grid.
constexpr float kUniformSpread = 0.75f;

}  // namespace tap_tuning

// Turn a lane's published rhythm into two tape offsets, in samples behind the
// write head. Pure: no state, no sample rate, no tape. This is where the rule
// that Zone S lacked lives, and it is unit-testable on its own.
//
// out[i] == tap_tuning::kMuted means "this tap does not sound".
void derive_offsets(const RhythmView& rv, int32_t tape_len, int32_t out[2]);

}  // namespace spky
