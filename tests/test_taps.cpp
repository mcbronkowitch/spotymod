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
    CHECK(out[1] - out[0] == 4500);                    // the guard fired
}

TEST_CASE("derive_offsets: gaps outside the tolerance are left alone") {
    int32_t out[2];
    derive_offsets(view(6000, 6600), kTapeLen, out);   // 10% apart
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
    // Guard against the test vacuously passing because everything was muted.
    CHECK(checked > 500);
}
