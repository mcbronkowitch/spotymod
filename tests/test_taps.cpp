#include <doctest/doctest.h>
#include "fx/taps.h"
#include <cmath>

using namespace spky;

namespace {
constexpr int32_t kTapeLen = 262144;    // Flux::kMaxSamples

RhythmView view(int32_t g0, int32_t g1) {
    RhythmView rv;
    rv.gap[0] = g0;
    rv.gap[1] = g1;
    rv.valid = true;
    return rv;
}

// The same +-2% test the guard uses, applied to the SPACINGS the listener
// hears: the dry signal sits at t = 0, so the spacings are out[0] and
// out[1] - out[0].
bool spacings_uniform(const int32_t out[2]) {
    const float a = static_cast<float>(out[0]);
    const float b = static_cast<float>(out[1] - out[0]);
    const float mean = 0.5f * (a + b);
    if (mean <= 0.f) return false;
    const float tol = tap_tuning::kUniformTol * mean;
    return std::fabs(a - mean) <= tol && std::fabs(b - mean) <= tol;
}
}  // namespace

TEST_CASE("derive_offsets: taps sit on the previous two onsets") {
    int32_t out[2];
    derive_offsets(view(6000, 9000), kTapeLen, out);
    CHECK(out[0] == 6000);
    CHECK(out[1] == 15000);
}

TEST_CASE("derive_offsets: an invalid view mutes both taps") {
    RhythmView rv = view(6000, 9000);
    rv.valid = false;
    int32_t out[2];
    derive_offsets(rv, kTapeLen, out);
    CHECK(out[0] == tap_tuning::kMuted);
    CHECK(out[1] == tap_tuning::kMuted);
}

TEST_CASE("derive_offsets: uniform gaps are spread into a limp") {
    int32_t out[2];
    derive_offsets(view(6000, 6000), kTapeLen, out);
    CHECK(out[0] == 6000);
    CHECK(out[1] == 6000 + 4500);     // second gap becomes 0.75 * 6000
    CHECK_FALSE(spacings_uniform(out));
}

TEST_CASE("derive_offsets: gaps within the tolerance still count as uniform") {
    int32_t out[2];
    derive_offsets(view(6000, 6060), kTapeLen, out);   // 1% apart
    CHECK(out[0] == 6000);
    CHECK(out[1] - out[0] == 4500);                    // the guard fired
}

TEST_CASE("derive_offsets: gaps outside the tolerance are left alone") {
    int32_t out[2];
    derive_offsets(view(6000, 6600), kTapeLen, out);   // 10% apart
    CHECK(out[0] == 6000);
    CHECK(out[1] - out[0] == 6600);                    // untouched
}

TEST_CASE("derive_offsets: an offset past the tape mutes that tap, never clamps") {
    int32_t out[2];
    // gap[0] fits, the cumulative sum does not.
    derive_offsets(view(200000, 200000), kTapeLen, out);
    CHECK(out[0] == 200000);
    CHECK(out[1] == tap_tuning::kMuted);
    // Both past the tape: both muted, and they must NOT collapse onto one
    // another (that would double an echo instead of dropping it).
    derive_offsets(view(300000, 300000), kTapeLen, out);
    CHECK(out[0] == tap_tuning::kMuted);
    CHECK(out[1] == tap_tuning::kMuted);
}

TEST_CASE("derive_offsets: a uniform pair too short to spread audibly mutes, "
          "not buzzes") {
    // g0=g1=40: mean=40, tol=0.8, both within tol -> guard fires and
    // recomputes g1 = trunc(0.75 * 40) = 30, which is below kMinGap (32) --
    // the post-spread bail-out at the end of the guard block. Only
    // g0 in [32, 42] can reach this path at all, so nothing else in this
    // file comes near it.
    int32_t out[2];
    derive_offsets(view(40, 40), kTapeLen, out);
    CHECK(out[0] == tap_tuning::kMuted);
    CHECK(out[1] == tap_tuning::kMuted);
}

TEST_CASE("derive_offsets: sub-musical gaps mute rather than produce a buzz") {
    int32_t out[2];
    // NON-uniform on purpose. A uniform sub-musical pair like (4, 4) would be
    // caught by the guard's own `g1 < kMinGap` bail-out even with the entry
    // guard deleted, so the test would survive its own mutation and prove
    // nothing. This pair reaches the tail only if the entry guard is gone.
    derive_offsets(view(4, 100), kTapeLen, out);
    CHECK(out[0] == tap_tuning::kMuted);
    CHECK(out[1] == tap_tuning::kMuted);
    derive_offsets(view(100, 4), kTapeLen, out);
    CHECK(out[0] == tap_tuning::kMuted);
    CHECK(out[1] == tap_tuning::kMuted);
}

// THE property test: the reason this spec exists. Zone S was evenly spaced by
// construction and therefore was a delay. Over every gap pair on a musically
// reachable grid -- 240 to 96000 samples (5 ms to 2 s at 48 kHz), 32 geometric
// steps, both gaps independently -- the resulting spacings must never be
// uniform.
TEST_CASE("derive_offsets: no gap pair ever yields evenly spaced taps") {
    constexpr int kSteps = 32;
    constexpr float lo = 240.f, hi = 96000.f;
    const float ratio = std::pow(hi / lo, 1.f / static_cast<float>(kSteps - 1));

    int checked = 0;
    for (int i = 0; i < kSteps; ++i) {
        const int32_t g0 = static_cast<int32_t>(lo * std::pow(ratio, static_cast<float>(i)));
        for (int j = 0; j < kSteps; ++j) {
            const int32_t g1 = static_cast<int32_t>(lo * std::pow(ratio, static_cast<float>(j)));
            int32_t out[2];
            derive_offsets(view(g0, g1), kTapeLen, out);
            if (out[0] == tap_tuning::kMuted || out[1] == tap_tuning::kMuted) continue;
            INFO("g0=" << g0 << " g1=" << g1 << " out={" << out[0] << "," << out[1] << "}");
            CHECK_FALSE(spacings_uniform(out));
            ++checked;
        }
    }
    // This grid's minimum gap (240) is far above kMinGap (32) and its
    // largest reachable cumulative sum (~175130) is far under `limit`
    // (~262142), so nothing on it can ever be muted: `checked` is always
    // every pair examined. This assertion does NOT show the mute paths are
    // exercised -- they are not, anywhere on this grid -- it only catches a
    // silent change to the grid's geometry (e.g. widening `hi`) that pushes
    // pairs into the mute region without anyone noticing the coverage shift.
    CHECK(checked == 1024);   // 32x32, all reachable -- see comment above
}

// The grid above draws g0 and g1 independently from the same 32-step
// geometric sequence; adjacent steps are ~21% apart, an order of magnitude
// outside kUniformTol's +-2% window. g0 == g1 only on the diagonal (32 of
// 1024 pairs), so that is the only place the guard ever fires, and off the
// diagonal the assertion above restates derive_offsets' own arithmetic
// rather than testing it: it cannot fail for any change to the guard.
//
// This grid instead perturbs each g0 by a fixed set of ratios straddling the
// guard's real decision boundary, so it can probe the region an independent
// draw structurally cannot reach: pairs a few percent apart, near the edge
// of the tolerance band. The boundary, derived from the guard's own
// formula -- fires iff |g0 - mean| <= tol and |g1 - mean| <= tol, which with
// g1 = r*g0 reduces algebraically to |1 - r| <= kUniformTol * (1 + r) --
// is r in [0.9608, 1.0408] for kUniformTol = 0.02.
//
// Two things are checked per pair: the property (spacings are never
// uniform -- the feature's actual guarantee), and the guard's decision
// itself (whether it fired) -- which is what gives the assertion teeth:
// it is not satisfiable by construction, so it breaks the moment
// kUniformTol or kUniformSpread moves the boundary it pins.
TEST_CASE("derive_offsets: the uniformity guard fires exactly inside its "
          "documented tolerance band") {
    // Pinned to the DOCUMENTED values (taps.h's own comments on kUniformTol
    // and kUniformSpread: 2% tolerance, 0.75 spread), deliberately NOT read
    // from tap_tuning::* --
    // if either constant drifts from what's written here, the guard's
    // actual behaviour and this test's fixed expectation diverge and the
    // per-pair CHECK below fails. Reading the live constants instead would
    // make this self-consistent with any value and unable to catch either
    // mutation, the same hollowness this test exists to fix.
    constexpr float kDocumentedTol = 0.02f;
    constexpr float kDocumentedSpread = 0.75f;

    constexpr int kSteps = 32;
    constexpr float lo = 240.f, hi = 96000.f;
    const float ratio = std::pow(hi / lo, 1.f / static_cast<float>(kSteps - 1));

    // Straddles r in [0.9608, 1.0408] on both sides, plus values far outside
    // it so the negative case isn't only ever "just barely outside".
    constexpr float kRatios[] = {0.5f,  0.9f,  0.95f, 0.96f, 0.97f, 0.99f,
                                  1.0f,  1.01f, 1.03f, 1.04f, 1.05f, 1.1f,
                                  1.5f,  2.0f,  3.0f};

    int checked = 0;
    int inside_band_checked = 0;
    for (int i = 0; i < kSteps; ++i) {
        const int32_t g0 = static_cast<int32_t>(lo * std::pow(ratio, static_cast<float>(i)));
        for (float r : kRatios) {
            const int32_t g1 = static_cast<int32_t>(static_cast<float>(g0) * r);
            if (g1 < tap_tuning::kMinGap) continue;   // not a reachable pair

            int32_t out[2];
            derive_offsets(view(g0, g1), kTapeLen, out);
            if (out[0] == tap_tuning::kMuted || out[1] == tap_tuning::kMuted) continue;
            ++checked;

            INFO("g0=" << g0 << " g1=" << g1 << " r=" << r
                       << " out={" << out[0] << "," << out[1] << "}");

            // 1. The property: never evenly spaced.
            CHECK_FALSE(spacings_uniform(out));

            // 2. The guard's decision, against the documented boundary
            // (not the live constants -- see above). Computed from the
            // actual integer g0/g1, not the nominal r, so truncation in
            // g1 = static_cast<int32_t>(r * g0) can't put this on the
            // wrong side of the boundary it's meant to pin.
            const float mean = 0.5f * (static_cast<float>(g0) + static_cast<float>(g1));
            const float tol = kDocumentedTol * mean;
            const bool inside_band =
                std::fabs(static_cast<float>(g0) - mean) <= tol &&
                std::fabs(static_cast<float>(g1) - mean) <= tol;

            if (inside_band) {
                ++inside_band_checked;
                const int32_t spread = static_cast<int32_t>(
                    kDocumentedSpread * static_cast<float>(g0));
                CHECK(out[1] - out[0] == spread);
            } else {
                CHECK(out[1] - out[0] == g1);
            }
        }
    }
    // Exact counts, not a floor: a silent change to the grid or the ratio
    // list that stops reaching the boundary on either side should fail this
    // loudly rather than continue passing on a shrunken sample. 480 nominal
    // combinations (32 g0 x 15 ratios); 3 are muted at the largest g0/r
    // combinations (cumulative offset exceeds the tape), leaving 477. Of
    // those, exactly the 6 ratios strictly inside [0.9608, 1.0408] --
    // 0.97, 0.99, 1.0, 1.01, 1.03, 1.04 -- land inside the band for every
    // g0 (192 = 6 x 32); 0.96 and 1.05 sit just outside it and never do.
    CHECK(checked == 477);
    CHECK(inside_band_checked == 192);
}
