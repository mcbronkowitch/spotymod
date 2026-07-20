#include <doctest/doctest.h>
#include "fx/taps.h"
#include <algorithm>
#include <cmath>
#include <vector>

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

namespace {
// A tape with a single impulse at a known distance behind the write head, so
// a tap reading offset N produces a non-zero sample exactly when N matches.
struct FakeTape {
    static constexpr int32_t kLen = 262144;
    std::vector<float> l = std::vector<float>(kLen, 0.f);
    std::vector<float> r = std::vector<float>(kLen, 0.f);
    int32_t wp = 1000;

    TapeTap view() const { return TapeTap{ l.data(), r.data(), wp, kLen - 1 }; }
    void poke(int32_t offset, float v) {
        const int32_t i = (wp + offset) & (kLen - 1);
        l[i] = v;
        r[i] = v;
    }
};

TapBank make_bank(float dust = 1.f, float rot = 0.f) {
    TapBank b;
    b.init(48000.f);
    b.set_rot(rot);
    b.set_dust(dust);
    return b;
}

// Run the bank until its gain slews settle, returning the last output pair.
void settle(TapBank& b, const TapeTap& t, float& l, float& r, int n = 8000) {
    for (int i = 0; i < n; ++i) { l = 0.f; r = 0.f; b.process(t, l, r); }
}

// Advances the tape's write head by n samples (mirroring DeLine::Write's
// per-sample decrement in production, engine/fx/flux.h) while running the
// bank, so a held offset reads genuinely time-varying material instead of
// the same frozen sample forever. Required for any assertion about the read
// signal's FREQUENCY content: a one-pole's steady state for a constant input
// is that input, independent of its cutoff, so a static write head (as
// settle() above uses) makes cutoff invisible by construction -- fine for
// gain/position tests, wrong for spectral ones.
void run_moving(TapBank& b, FakeTape& tape, float& l, float& r, int n) {
    for (int i = 0; i < n; ++i) {
        l = 0.f; r = 0.f;
        b.process(tape.view(), l, r);
        --tape.wp;
    }
}
}  // namespace

TEST_CASE("tap bank: dust 0 from init is silent and performs no reads") {
    FakeTape tape;
    tape.poke(6000, 1.f);
    TapBank b = make_bank(0.f);
    const int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);

    CHECK_FALSE(b.active());
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    CHECK(l == 0.f);
    CHECK(r == 0.f);
    CHECK(b.reads() == 0);
}

TEST_CASE("tap bank: dropping DUST to 0 rides the taps out instead of cutting them") {
    // Flux takes its bit-exact bypass on !active(). If active() went false the
    // instant the knob hit 0, a full-level tap sum would vanish in one sample.
    FakeTape tape;
    for (int32_t o = 0; o < 40000; ++o) tape.poke(o, 0.5f);   // DC everywhere
    TapBank b = make_bank(1.f);
    const int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    REQUIRE(std::fabs(l) > 1e-3f);

    b.set_dust(0.f);
    CHECK(b.active());                  // still riding out, not cut

    // 8000 samples undershoots: kGainSlewS=0.02s @ 48kHz gives a gain_coef of
    // ~1/960, and 0.7*(1-1/960)^8000 ~= 1.68e-4, still above the 1e-4 snap
    // threshold. >8495 samples are needed; 10000 leaves comfortable margin.
    //
    // One CHECK after the loop, not one per sample (10000 assertions for a
    // single property is noise): accumulate the worst step over the decay
    // and report it via INFO so a failure still shows the offending value.
    float prev = l;
    float worst_step = 0.f;
    for (int i = 0; i < 10000; ++i) {
        l = 0.f; r = 0.f;
        b.process(tape.view(), l, r);
        worst_step = std::max(worst_step, std::fabs(l - prev));
        prev = l;
    }
    INFO("worst per-sample step during the ride-out = " << worst_step);
    CHECK(worst_step < 0.01f);          // no step anywhere in the decay
    CHECK_FALSE(b.active());            // settled: the bypass is now safe
    CHECK(b.reads() == 0);
}

TEST_CASE("tap bank: dust morphs tap 0 in over the first half, tap 1 over the second") {
    FakeTape tape;
    TapBank b = make_bank(0.5f);
    const int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    CHECK(b.reads() == 1);              // tap 0 only at DUST 0.5

    b.set_dust(1.f);
    settle(b, tape.view(), l, r);
    CHECK(b.reads() == 2);              // both taps at DUST 1

    // Back down: tap 1's gain must actually reach 0, not just approach it,
    // for its read to stop. The first two phases above never exercise this
    // -- tap 1 starts at exactly 0 (its Tap{} default) and only ever rises,
    // so a missing gain snap would be invisible to them. 10000 samples (not
    // the default settle() n=8000) because 0.7*(1-1/960)^8000 ~= 1.68e-4
    // still sits above the 1e-4 snap threshold; see the parallel note on the
    // "dropping DUST to 0" test above.
    b.set_dust(0.5f);
    settle(b, tape.view(), l, r, 10000);
    CHECK(b.reads() == 1);              // tap 1's gain snapped back to 0
}

TEST_CASE("tap bank: a muted offset costs no read") {
    FakeTape tape;
    TapBank b = make_bank(1.f);
    const int32_t off[2] = { 6000, tap_tuning::kMuted };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    CHECK(b.reads() == 1);
}

TEST_CASE("tap bank: a tap reads its own offset and nothing else") {
    FakeTape tape;
    tape.poke(6000, 1.f);               // material only at offset 6000
    TapBank b = make_bank(1.f, 0.f);    // ROT 0: filters effectively open
    const int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);

    // Settle the gain slew with the write head parked, then read once.
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    CHECK(std::fabs(l) > 1e-3f);        // tap 0 (reads L, panned left) hears it

    // Move the impulse to an offset NEITHER tap targets (off = {6000,
    // 10500}): clear the original 6000 sample and place new material at
    // 7000 instead. A tap that (incorrectly) read some offset other than
    // its own configured one would still catch this signal, unlike an
    // all-zero substitute tape, which removes the property under test
    // (there being a signal at all) rather than probing it.
    tape.poke(6000, 0.f);
    tape.poke(7000, 1.f);
    float l2 = 0.f, r2 = 0.f;
    settle(b, tape.view(), l2, r2, 2000);
    CHECK(std::fabs(l2) < 1e-4f);
}

TEST_CASE("tap bank: a re-latch dips instead of crossfading -- reads never exceed live taps") {
    FakeTape tape;
    tape.poke(6000, 1.f);
    TapBank b = make_bank(1.f);
    int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);

    off[0] = 20000;                     // far more than kRelatchMin
    off[1] = 31000;
    b.set_offsets(off);
    // Through the whole dip, the bank must never read more than two
    // positions. One CHECK after the loop (not 1000): track the worst
    // (highest) reads() seen and report it via INFO.
    int worst_reads = 0;
    for (int i = 0; i < 1000; ++i) {
        l = 0.f; r = 0.f;
        b.process(tape.view(), l, r);
        worst_reads = std::max(worst_reads, b.reads());
    }
    INFO("worst reads() seen during the dip = " << worst_reads);
    CHECK(worst_reads <= 2);
}

TEST_CASE("tap bank: an offset change below kRelatchMin does not dip") {
    // DC everywhere: with a static (unmoving) write head, only an envelope
    // dip can move l from here on -- the tape's own value can't, since tap 0
    // reads the same absolute sample every call regardless of which of the
    // two nearby offsets is selected.
    FakeTape tape;
    for (int32_t o = 0; o < 40000; ++o) tape.poke(o, 1.f);
    TapBank b = make_bank(1.f);
    int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    // Settle well past the gain slew's own convergence tail: at n=8000 a
    // ~1.7e-4 residual remains and would itself drift l over the next few
    // hundred samples, easily mistaken for a dip at this epsilon.
    settle(b, tape.view(), l, r, 20000);
    const float before = l;

    // == 6000 + kRelatchMin(64) - 1, pinned as a literal: deriving this from
    // the live constant would make the test's own INPUT move with a mutated
    // kRelatchMin (e.g. kRelatchMin=1 collapses this to 6000+0, a no-op
    // offset that never reaches the boundary check at all), rather than the
    // code's decision about it -- silently defeating the mutation this test
    // exists to catch.
    off[0] = 6063;
    b.set_offsets(off);
    // A single process() call can't observe a wrongly-triggered dip:
    // Dip::out's envelope is hann_value_at(1.0) == 1.0 on its very first
    // sample regardless of whether a dip just started. Run past a full dip
    // cycle (2*dip_len + 1 = 193 samples) instead.
    //
    // One CHECK after the loop, not 500: keep the sample that deviated most
    // from `before` and Approx-compare only that one, reporting the worst
    // deviation via INFO so a failure is still diagnosable.
    float worst = before;
    float worst_dev = 0.f;
    for (int i = 0; i < 500; ++i) {
        l = 0.f; r = 0.f;
        b.process(tape.view(), l, r);
        const float dev = std::fabs(l - before);
        if (dev > worst_dev) { worst_dev = dev; worst = l; }
    }
    INFO("worst deviation from `before` over the window = " << worst_dev);
    CHECK(worst == doctest::Approx(before).epsilon(1e-4)); // no envelope movement anywhere in the window
}

TEST_CASE("tap bank: a re-latch produces no discontinuity") {
    // Two plateaus, not a DC-everywhere tape: a DC tape would make the old
    // and new offsets read the identical value, so an unmitigated instant
    // jump (the mutation this test exists to catch) would be silent and the
    // test would pass for the wrong reason. Both offsets used below start in
    // the +1 plateau; the re-latch below lands in the -1 plateau.
    FakeTape tape;
    for (int32_t o = 0; o < 20000; ++o) tape.poke(o, 1.f);
    for (int32_t o = 20000; o < 40000; ++o) tape.poke(o, -1.f);
    TapBank b = make_bank(1.f);
    int32_t off[2] = { 6000, 10500 };     // both inside the +1 plateau
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);

    off[0] = 30000;                        // the -1 plateau
    b.set_offsets(off);
    // One CHECK after the loop, not 1000: accumulate the worst per-sample
    // step and report it via INFO.
    float prev = l;
    float worst_step = 0.f;
    for (int i = 0; i < 1000; ++i) {
        l = 0.f; r = 0.f;
        b.process(tape.view(), l, r);
        worst_step = std::max(worst_step, std::fabs(l - prev));
        prev = l;
    }
    INFO("worst per-sample step across the re-latch = " << worst_step);
    CHECK(worst_step < 0.05f);              // no step, only the dip's ramp
}

TEST_CASE("tap bank: a repeated unchanged offset during a running dip does not restart it") {
    // Two plateaus (same trick as "a re-latch produces no discontinuity"
    // above): old and new offsets read genuinely different content, so both
    // "did the dip finish" and "did a no-op push re-trigger it" are
    // observable in l, not masked by a DC tape reading the same value
    // regardless of position.
    FakeTape tape;
    for (int32_t o = 0; o < 20000; ++o) tape.poke(o, 1.f);
    for (int32_t o = 20000; o < 40000; ++o) tape.poke(o, -1.f);
    TapBank b = make_bank(1.f);
    // Tap 1 muted: isolate the signal to tap 0 alone, so l unambiguously
    // reports what tap 0 is reading.
    int32_t off[2] = { 6000, tap_tuning::kMuted };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r, 20000);
    REQUIRE(l == doctest::Approx(0.7f * tap_tuning::kPanNear).epsilon(0.05));

    off[0] = 25000;   // far into the -1 plateau, well past kRelatchMin
    b.set_offsets(off);

    // Model the real caller (Instrument's control tick, every 96 samples --
    // exactly _dip_len at 48 kHz, per the brief): re-push the SAME target on
    // every tick while the dip is running. At this moment, what would the
    // un-fixed code do? Its early-out guards require `t.dip == Dip::run`,
    // so a no-op push mid-dip falls through and unconditionally resets
    // dip_ctr to _dip_len -- the envelope jumps back to hann_value_at(1.0)
    // == 1.0 from wherever it was, every single tick, forever. That is a
    // world apart from a single smooth dip settling once, so this is
    // observable both as a per-sample step far above a single dip's ramp
    // and as the tap never actually converging on the new plateau's value.
    float worst_delta = 0.f;
    float prev = l;
    for (int tick = 0; tick < 30; ++tick) {
        for (int i = 0; i < 96; ++i) {
            l = 0.f; r = 0.f;
            b.process(tape.view(), l, r);
            worst_delta = std::max(worst_delta, std::fabs(l - prev));
            prev = l;
        }
        b.set_offsets(off);   // unchanged target -- must be a no-op
    }
    INFO("worst per-sample delta over 30 ticks (2880 samples) = " << worst_delta);
    CHECK(worst_delta < 0.05f);

    // And the tap must actually have arrived at the new offset -- not just
    // "not stepping", but genuinely reading the -1 plateau now.
    CHECK(l == doctest::Approx(-0.7f * tap_tuning::kPanNear).epsilon(0.05));
}

TEST_CASE("tap bank: a re-latch arriving mid-dip (during Dip::out and during "
          "Dip::in) never steps the envelope or exceeds live-tap reads") {
    FakeTape tape;
    for (int32_t o = 0; o < 20000; ++o) tape.poke(o, 1.f);
    for (int32_t o = 20000; o < 40000; ++o) tape.poke(o, -1.f);
    for (int32_t o = 40000; o < 60000; ++o) tape.poke(o, 0.5f);
    TapBank b = make_bank(1.f);
    int32_t off[2] = { 6000, tap_tuning::kMuted };   // isolate to tap 0
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r, 20000);

    float worst_delta = 0.f;
    int worst_reads = 0;
    float prev = l;
    auto run = [&](int n) {
        for (int i = 0; i < n; ++i) {
            l = 0.f; r = 0.f;
            b.process(tape.view(), l, r);
            worst_delta = std::max(worst_delta, std::fabs(l - prev));
            worst_reads = std::max(worst_reads, b.reads());
            prev = l;
        }
    };

    // _dip_len is 96 at 48 kHz (kDipSeconds=0.002s). First retarget starts
    // a fresh Dip::out (dip_ctr=96).
    off[0] = 25000;
    b.set_offsets(off);
    run(30);   // dip_ctr now ~66: comfortably mid Dip::out, not near an edge.

    // Interrupt AGAIN while still mid Dip::out with a third target.
    off[0] = 45000;
    b.set_offsets(off);
    // 66 more samples finish the fade-out and flip to Dip::in (t.off jumps
    // to 45000); 40 more climb partway up -- dip_ctr now ~40, comfortably
    // mid Dip::in, not near either edge.
    run(106);

    // Interrupt a SECOND time, now mid Dip::in.
    off[0] = 25000;
    b.set_offsets(off);
    // Past a full dip cycle (2*96) with margin: enough for the reversed
    // fade-out to finish, the jump to 25000, and a fresh fade-in to settle.
    run(300);

    INFO("worst per-sample |delta| = " << worst_delta
         << ", worst reads() = " << worst_reads);
    // No envelope step anywhere, at either interruption. What would the
    // un-fixed code produce here? Both mid-dip pushes fall through its
    // Dip::run-only guards and unconditionally reset dip_ctr to _dip_len,
    // so the envelope snaps back to 1.0 from whatever it was at each
    // interruption -- a jump far above a single dip's smooth ramp. Differs.
    CHECK(worst_delta < 0.05f);
    // Standing invariant, not specific to this bug: with tap 1 muted, only
    // one tap is ever live, so reads() must never exceed 1 regardless of
    // how many re-latches land mid-dip. A crossfade-style regression (not
    // what Important 1 is about) is what this would actually catch.
    CHECK(worst_reads <= 1);

    // And the tap must genuinely have settled on the final target's value.
    CHECK(l == doctest::Approx(-0.7f * tap_tuning::kPanNear).epsilon(0.05));
}

TEST_CASE("tap bank: a push mid-dip back to the pre-dip offset is not swallowed "
          "by a stale pending target") {
    // set_offsets' fix has two halves: the pending-target computation
    // (`cur = dip==run ? t.off : t.next_off`, taps.cpp:101) and the
    // retarget-without-restart body below it. Reverting ONLY the body is
    // caught by the "re-latch arriving mid-dip" test above. Reverting ONLY
    // the guard -- back to the old unconditional `cur = t.off` -- is caught
    // by nothing else in this file: "a repeated unchanged offset" above
    // re-pushes the value the dip is already headed to (next_off), which a
    // reverted `cur = t.off` guard also treats as a no-op, just for the
    // wrong reason, so that test still passes against the revert.
    //
    // This test instead pushes a value mid-dip that equals the tap's
    // PRE-dip position (t.off, which a Dip::out hasn't overwritten yet) but
    // NOT where the tap is actually headed (t.next_off). A reverted guard
    // reads `cur` as t.off, sees `want == cur`, and silently drops the
    // push -- next_off is never updated, so the running dip-out completes
    // on its ORIGINAL, now-abandoned target instead.
    FakeTape tape;
    for (int32_t o = 0; o < 20000; ++o) tape.poke(o, 1.f);
    for (int32_t o = 20000; o < 40000; ++o) tape.poke(o, -1.f);
    TapBank b = make_bank(1.f);
    int32_t off[2] = { 6000, tap_tuning::kMuted };   // isolate to tap 0
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r, 20000);
    REQUIRE(l == doctest::Approx(0.7f * tap_tuning::kPanNear).epsilon(0.05));

    off[0] = 25000;             // start a dip toward the -1 plateau
    b.set_offsets(off);
    for (int i = 0; i < 30; ++i) { l = 0.f; r = 0.f; b.process(tape.view(), l, r); }
    // ~30 of _dip_len=96 samples in: comfortably mid Dip::out. t.off is
    // STILL 6000 here -- a Dip::out only writes t.off when it completes.

    off[0] = 6000;               // back to the PRE-dip position -- NOT next_off (25000)
    b.set_offsets(off);

    // Past a full dip cycle (fade-out finishes, t.off <- next_off, a fresh
    // fade-in climbs and settles), with margin.
    for (int i = 0; i < 300; ++i) { l = 0.f; r = 0.f; b.process(tape.view(), l, r); }

    // Fixed code: mid Dip::out, `cur` tracked next_off (25000), so
    // want(6000) != cur triggered a real retarget -- the tap lands back on
    // the 6000 plateau's value (+1). Reverted guard: `cur` read t.off
    // (still 6000 at push time), want == cur looked like a no-op, the push
    // was dropped, and the abandoned 25000 target -- the -1 plateau --
    // wins instead. Differs.
    INFO("l = " << l);
    CHECK(l == doctest::Approx(0.7f * tap_tuning::kPanNear).epsilon(0.05));
}

TEST_CASE("tap bank: un-muting to an offset inside [kMinGap, kRelatchMin) "
          "still dips and sounds") {
    FakeTape tape;
    tape.poke(40, 1.f);   // 40 sits in [kMinGap(32), kRelatchMin(64)) -- a
                          // value derive_offsets can legitimately emit.
    TapBank b = make_bank(1.f);
    int32_t off[2] = { tap_tuning::kMuted, tap_tuning::kMuted };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);   // gain slew converges; tap stays muted
    CHECK(b.reads() == 0);

    off[0] = 40;
    b.set_offsets(off);
    // At this moment, what would the un-fixed code produce? want=40,
    // t.off=kMuted(0): d=40 < kRelatchMin(64) and t.dip==Dip::run, so its
    // distance guard swallows the request outright -- reads() stays 0 and
    // l stays 0 forever, regardless of how long this runs. Differs from a
    // fixed implementation, which dips and settles at 40 within one dip
    // cycle (2*96 samples). 300 is comfortably past that.
    settle(b, tape.view(), l, r, 300);
    CHECK(b.reads() == 1);
    CHECK(std::fabs(l) > 1e-3f);
}

TEST_CASE("tap bank: muting from an offset inside [kMinGap, kRelatchMin) "
          "still dips and silences") {
    FakeTape tape;
    tape.poke(40, 1.f);
    TapBank b = make_bank(1.f);
    // Reach offset 40 via an ordinary large jump first: kMuted(0) -> 1000
    // (d=1000), then 1000 -> 40 (d=960). Both distances are comfortably
    // clear of kRelatchMin on their own, so this setup cannot itself
    // exercise the bug under test -- it exists only to get the tap sitting
    // at 40 so the MUTE direction can be tested in isolation from the
    // UN-MUTE direction (covered by the test above).
    int32_t off[2] = { 1000, tap_tuning::kMuted };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r, 300);

    off[0] = 40;
    b.set_offsets(off);
    settle(b, tape.view(), l, r, 300);
    REQUIRE(b.reads() == 1);
    REQUIRE(std::fabs(l) > 1e-3f);          // confirm the tap really is at 40

    off[0] = tap_tuning::kMuted;            // mute FROM inside [32,63]
    b.set_offsets(off);
    // At this moment, what would the un-fixed code produce? want=kMuted(0),
    // t.off=40: d=40 < kRelatchMin(64) and t.dip==Dip::run, so the distance
    // guard blocks the mute -- reads() stays 1 and l stays nonzero forever.
    // Differs from a fixed implementation, which dips to silence within one
    // dip cycle.
    settle(b, tape.view(), l, r, 300);
    CHECK(b.reads() == 0);
    CHECK(l == 0.f);
}

TEST_CASE("tap bank: ROT separates the two taps spectrally") {
    // Tap 0 is low-passed, tap 1 high-passed. Feed alternating (Nyquist)
    // content and let the write head actually advance (run_moving) so the
    // taps see time-varying material instead of one frozen sample: at ROT 1
    // tap 0's contribution (dominant in L, panned near) must shrink hard and
    // tap 1's (dominant in R, panned near) must largely survive.
    FakeTape open_tape, split_tape;
    for (int32_t o = 0; o < 40000; ++o) {
        const float v = (o & 1) ? 1.f : -1.f;
        open_tape.poke(o, v);
        split_tape.poke(o, v);
    }

    TapBank open = make_bank(1.f, 0.f);
    TapBank split = make_bank(1.f, 1.f);
    const int32_t off[2] = { 6000, 10500 };
    open.set_offsets(off);
    split.set_offsets(off);

    float ol = 0.f, orr = 0.f, sl = 0.f, sr = 0.f;
    settle(open, open_tape.view(), ol, orr);    // gain slew: content doesn't matter
    settle(split, split_tape.view(), sl, sr);
    // Let each filter reach its periodic steady state against genuinely
    // alternating input. 4000 samples is far past 5 time constants of even
    // the slowest filter here (tap 1's open-side one-pole at 20 Hz, tau ~=
    // 382 samples).
    run_moving(open, open_tape, ol, orr, 4000);
    run_moving(split, split_tape, sl, sr, 4000);

    // Steady-state one-pole gain at Nyquist is a/(2-a), and the complementary
    // high-pass (x - lp(x)) gain is (2-2a)/(2-a). Working those out for the
    // tuned cutoffs (kLpOpenHz/kLpSplitHz for tap 0, kHpOpenHz/kHpSplitHz for
    // tap 1) and combining through the pan weights (kPanNear/kPanFar) gives a
    // computed split/open ratio of L ~= 0.322 and R ~= 0.681 -- confirmed
    // against this test's own measured output (0.3223 / 0.6809) to 3 decimal
    // places.
    //
    // The old thresholds (ratio < 0.5 / > 0.5) were so loose that kLpSplitHz
    // had to rise ~10x (400 -> ~3850 Hz) before the L check even noticed, and
    // kHpOpenHz could move anywhere without the R check reacting at all.
    // These four bounds are pinned as LITERALS -- not recomputed from
    // kLpOpenHz/kLpSplitHz/kHpOpenHz/kHpSplitHz, or mutating a cutoff would
    // silently drag the expectation along with it -- each placed at the
    // midpoint between the correct ratio and the nearest value produced by
    // halving or doubling one of the three OBSERVABLE cutoffs (see the
    // mutation matrix in the task-3 report's Fix wave section): the tightest
    // gap is kLpSplitHz halved, landing L at 0.3118, only ~0.0052 below the
    // L-lo bound -- still >>1e-4, comfortably clear of float noise in this
    // fully deterministic computation.
    //
    // kHpOpenHz is NOT covered by any bound: at Nyquist a high-pass is
    // already at (or extremely near) unity gain regardless of a 20 Hz vs.
    // 2000 Hz cutoff, so halving/doubling it moves neither ratio measurably
    // (confirmed by mutation: L 0.3223 -> 0.3223/0.3225, R 0.6807 ->
    // 0.6804/0.6814, both static to 3 decimals). It is unobservable through
    // this test by construction, not by an oversight in the bounds below.
    const float ratio_l = std::fabs(sl) / std::fabs(ol);
    const float ratio_r = std::fabs(sr) / std::fabs(orr);
    INFO("ratio_l=" << ratio_l << " ratio_r=" << ratio_r);
    CHECK(ratio_l > 0.317f);   // catches kLpOpenHz x2, kLpSplitHz /2
    CHECK(ratio_l < 0.330f);   // catches kLpOpenHz /2, kLpSplitHz x2, kHpSplitHz /2
    CHECK(ratio_r > 0.665f);   // catches kLpOpenHz x2, kHpSplitHz x2
    CHECK(ratio_r < 0.699f);   // catches kLpOpenHz /2, kHpSplitHz /2
}
