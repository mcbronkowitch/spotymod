#include "fx/taps.h"
#include "fx/flux.h"
#include <cmath>

using namespace spky;

// The power-of-two contract TapeTap's AND-mask read depends on, checked once
// here rather than on every construction in the audio path.
static_assert((Flux::kMaxSamples & (Flux::kMaxSamples - 1)) == 0,
              "TapeTap's mask read requires a power-of-two tape");

void spky::derive_offsets(const RhythmView& rv, int32_t tape_len, int32_t out[2]) {
    out[0] = out[1] = tap_tuning::kMuted;
    if (!rv.valid) return;

    int32_t g0 = rv.gap[0];
    int32_t g1 = rv.gap[1];
    if (g0 < tap_tuning::kMinGap || g1 < tap_tuning::kMinGap) return;

    // Uniformity guard: evenly spaced taps ARE a delay (the diagnosis that
    // killed zone S twice). Counting the dry signal at t = 0, the spacings the
    // listener hears are exactly {g0, g1} -- so testing the gaps IS testing
    // the spacings.
    const float mean = 0.5f * (static_cast<float>(g0) + static_cast<float>(g1));
    const float tol  = tap_tuning::kUniformTol * mean;
    if (std::fabs(static_cast<float>(g0) - mean) <= tol &&
        std::fabs(static_cast<float>(g1) - mean) <= tol) {
        g1 = static_cast<int32_t>(tap_tuning::kUniformSpread * static_cast<float>(g0));
        if (g1 < tap_tuning::kMinGap) return;   // too short to spread audibly
    }

    const int32_t limit = tape_len - 2;
    const int32_t o0 = g0;
    // int64_t: g0 and g1 are otherwise-unconstrained int32_t, so g0 + g1 can
    // overflow int32_t (signed overflow is UB) for large inputs. Widening the
    // sum keeps this comparison exact without narrowing before the bound
    // check below, which is what actually decides whether o1 is representable.
    const int64_t o1 = static_cast<int64_t>(g0) + static_cast<int64_t>(g1);
    // Mute, never clamp: clamping would put two taps at the same position,
    // turning a missing echo into a doubled one.
    if (o0 <= limit) out[0] = o0;
    if (o1 <= limit) out[1] = static_cast<int32_t>(o1);
}
