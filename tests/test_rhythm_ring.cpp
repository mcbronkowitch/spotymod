#include <doctest/doctest.h>
#include "mod/super_modulator.h"
#include <cmath>
#include <cstdint>

using namespace spky;

namespace {
// The onset-gap ring moved from ModLane up to SuperModulator (taps-plumbing
// cost pass, 2026-07-20): only LANE_PITCH's rhythm was ever consumed
// (SuperModulator::rhythm() forwards exactly that lane), so these tests now
// drive a SuperModulator instead of a standalone ModLane. This fixture
// reproduces the old standalone-lane setup (make_lane(), formerly in this
// file) as closely as possible so every sample-count assertion below still
// holds unchanged:
//
// - SuperModulator::init() seeds lane i with `seed_base + i *
//   2654435761u` (super_modulator.cpp); kPitchSeed/seed_base_for_pitch()
//   below picks seed_base so LANE_PITCH lands on exactly the old tests'
//   0xC0FFEE seed (uint32_t wraparound makes the subtraction well-defined).
//   This matters beyond bit-for-bit paranoia: "a rest step lengthens the
//   gap" and "the view only moves at a cycle wrap" both rest on that exact
//   seed producing a specific non-uniform DENSE-0.5 groove pattern.
// - force_pitch_rate_1hz() uses the public set_rate_scale()/base_hz() pair
//   to pin LANE_PITCH's underlying rate to exactly 1 Hz (kLaneRatio[LANE_PITCH]
//   == 1.f in super_modulator.cpp), matching the old direct
//   l.set_rate_hz(1.f) call these tests were tuned against. There is no
//   per-lane Hz setter on SuperModulator's public surface, by design (RATE
//   is a single macro knob) -- this is the closest equivalent.
constexpr uint32_t kPitchSeed = 0xC0FFEEu;

uint32_t seed_base_for_pitch(uint32_t pitch_seed) {
    return pitch_seed - static_cast<uint32_t>(LANE_PITCH) * 2654435761u;
}

void force_pitch_rate_1hz(SuperModulator& m) {
    m.set_rate_scale(1.f / m.base_hz(), 1.f);
}

// 4 steps/cycle, cycle rate chosen so a step is an exact sample count:
// rate_hz * clock_scale = cycles/s. clock_scale = 8/steps = 2, so
// phase_inc = rate_hz/sr * 2. With rate_hz = 1.0 and sr = 48000:
// phase_inc = 1/24000, one cycle = 24000 samples, one step = 6000 samples.
SuperModulator make_modulator(int steps = 4) {
    SuperModulator m;
    m.init(48000.f, seed_base_for_pitch(kPitchSeed));
    force_pitch_rate_1hz(m);
    m.set_step(true, steps);
    m.set_density(1.f);          // every slot gated: uniform onsets
    m.set_variation(0.f);        // LOOP: no mutation, no drift
    return m;
}
}  // namespace

TEST_CASE("rhythm ring: a uniform STEP pattern reports the step length as both gaps") {
    SuperModulator m = make_modulator(4);
    // Run four full cycles so the ring fills and at least one wrap latches it.
    for (int i = 0; i < 4 * 24000; ++i) m.process();

    const RhythmView& rv = m.rhythm();
    REQUIRE(rv.valid);
    CHECK(rv.gap[0] == doctest::Approx(6000).epsilon(0.01));
    CHECK(rv.gap[1] == doctest::Approx(6000).epsilon(0.01));
}

TEST_CASE("rhythm ring: a rest step lengthens the gap instead of writing an onset") {
    // DENSE below 1 masks the lowest-ranked slots -> fewer onsets per cycle,
    // so at least one gap must exceed a single step length.
    SuperModulator m = make_modulator(8);
    m.set_density(0.5f);         // roughly half the cell notes play
    for (int i = 0; i < 8 * 24000; ++i) m.process();

    const RhythmView& rv = m.rhythm();
    REQUIRE(rv.valid);
    // one step at 8 steps/cycle: cycle = 24000*? -- clock_scale = 8/8 = 1, so
    // phase_inc = 1/48000, cycle = 48000 samples, step = 6000 samples.
    CHECK((rv.gap[0] > 6100 || rv.gap[1] > 6100));
    // Shared premise this test and "the view only moves at a cycle wrap"
    // both rest on: seed 0xC0FFEE at density 0.5 produces at least two
    // distinct gap lengths per cycle (not just a uniformly-longer one).
    CHECK(rv.gap[0] != rv.gap[1]);
}

TEST_CASE("rhythm ring: invalid until three onsets have been seen") {
    // A 4-step lane can't isolate this threshold: by the time its first wrap
    // fires, all 4 steps of that cycle have already onset (density 1), so
    // _onsets is already saturated past 3. Use 2 steps/cycle instead -- still
    // a 6000-sample step (clock_scale = 8/2 = 4, phase_inc = 4/48000 =
    // 1/12000, cycle = 12000) -- so the first wrap sees exactly two onsets:
    // one real gap, not two, and not yet a rhythm.
    SuperModulator m = make_modulator(2);
    CHECK_FALSE(m.rhythm().valid);          // fresh modulator

    // First wrap: onsets at step0 (~n=1) and step1 (~n=6000) have fired;
    // the wrap-triggered onset that opens cycle 2 fires an instant later in
    // the same process() call, but the ring's latch reads it before the
    // record (see SuperModulator::process()).
    // +500 samples covers float32's rounding of phase_inc landing the wrap a
    // couple of samples past the ideal 12000, well short of the next step
    // boundary at ~18000.
    for (int i = 0; i < 12000 + 500; ++i) m.process();
    // Pin the precondition itself: we are just past the first real wrap, not
    // just short of one. Without this, if phase arithmetic ever shifted the
    // wrap outside this window, CHECK_FALSE below would pass against a
    // never-latched default _rhythm -- proving nothing.
    REQUIRE(m.pitch_phase() < 0.1f);
    CHECK_FALSE(m.rhythm().valid);          // two onsets seen -- one gap, not a rhythm

    // Second wrap: that cycle-2-opening onset is now the ring's third onset,
    // in time for this wrap's latch.
    for (int i = 0; i < 12000; ++i) m.process();
    CHECK(m.rhythm().valid);
}

TEST_CASE("rhythm ring: the view only moves at a cycle wrap") {
    // A uniform (density 1) pattern can't isolate this: every onset's gap is
    // ~6000 regardless of when it's read, so "latch at every onset" and
    // "latch only at the wrap" are indistinguishable by value. DENSE < 1
    // (as in the "rest step" case) makes onsets fall at genuinely different
    // intervals within a cycle, so reading mid-cycle only agrees with the
    // wrap-latched snapshot if the latch really did wait for the wrap.
    SuperModulator m = make_modulator(8);
    m.set_density(0.5f);
    // clock_scale = 8/8 = 1, so phase_inc = 1/48000 -- float32 rounds that a
    // hair low, so the real cycle is ~47969 samples, not the ideal 48000.
    // Run comfortably past the 2nd real wrap (~95939).
    for (int i = 0; i < 2 * 48000 + 500; ++i) m.process();
    REQUIRE(m.rhythm().valid);
    const RhythmView before = m.rhythm();

    // Advance most of the next cycle (3rd real wrap is ~47969 samples later,
    // ~143908) -- several onsets of varying gap length fire, but no wrap.
    for (int i = 0; i < 40000; ++i) m.process();
    // Pin the precondition itself: comfortably short of the next wrap, not
    // past it. Without this, if phase arithmetic ever shifted the 3rd real
    // wrap into this window, the CHECKs below would pass vacuously -- the
    // ring would have re-latched to the same values by coincidence rather
    // than the test having caught "latch only moves at a wrap".
    REQUIRE(m.pitch_phase() > 0.5f);
    CHECK(m.rhythm().gap[0] == before.gap[0]);
    CHECK(m.rhythm().gap[1] == before.gap[1]);
}

TEST_CASE("rhythm ring: FLOW lanes fill the ring from cycle wraps") {
    // LANE_PITCH is always melodic under SuperModulator (set unconditionally
    // in SuperModulator::init()), matching the old standalone test's
    // explicit l.set_melodic(true) -- see this file's namespace comment.
    SuperModulator m;
    m.init(48000.f, seed_base_for_pitch(kPitchSeed));
    force_pitch_rate_1hz(m);
    m.set_step(false, 4);        // FLOW: clock_scale = 1, cycle = 48000 samples
    m.set_variation(0.f);
    for (int i = 0; i < 4 * 48000; ++i) m.process();

    const RhythmView& rv = m.rhythm();
    REQUIRE(rv.valid);
    CHECK(rv.gap[0] == doctest::Approx(48000).epsilon(0.01));
}

TEST_CASE("rhythm ring: reset invalidates and the ring re-fills from scratch") {
    // 2 steps/cycle, as in "invalid until three onsets" above: only checking
    // CHECK_FALSE right after reset doesn't exercise the onset counter --
    // _rhythm is reset to RhythmView{} directly, so that assertion holds
    // even if the onset count survives the reset. A 4-step lane's first
    // post-reset wrap can't catch a stale counter either (4 onsets is
    // already enough on its own). With 2 steps/cycle, only two onsets fire
    // by the first post-reset wrap -- not a rhythm yet, unless a stale,
    // already-saturated counter from before the reset makes it look like one.
    SuperModulator m = make_modulator(2);
    for (int i = 0; i < 3 * 12000; ++i) m.process();
    REQUIRE(m.rhythm().valid);

    // reset_phases() resets every lane's phase, not just PITCH's -- the RST
    // bar-resync entry point (super_modulator.h) -- but only PITCH's ring is
    // read here, so that breadth doesn't affect the assertions below.
    m.reset_phases();
    CHECK_FALSE(m.rhythm().valid);          // no gap straddles the reset

    // First post-reset wrap: two onsets since reset -- one real gap.
    for (int i = 0; i < 12000 + 500; ++i) m.process();
    // Pin the precondition: just past the first post-reset wrap, not short
    // of it -- see the identical margin-loop note in "invalid until three
    // onsets have been seen" above.
    REQUIRE(m.pitch_phase() < 0.1f);
    CHECK_FALSE(m.rhythm().valid);

    // Second post-reset wrap: the ring has genuinely re-filled from scratch.
    for (int i = 0; i < 12000; ++i) m.process();
    CHECK(m.rhythm().valid);
    CHECK(m.rhythm().gap[0] == doctest::Approx(6000).epsilon(0.01));
}

TEST_CASE("rhythm ring: init() clears a stale ring") {
    // Instrument::init() (via Part) re-inits every lane's SuperModulator on
    // a host sample-rate change. A modulator carrying a saturated onset
    // count and gaps measured in the old sample rate's units must not
    // republish that stale rhythm as valid at its first post-init wrap.
    //
    // Checking CHECK_FALSE immediately after init() alone doesn't exercise
    // the onset counter: init() sets the published view to RhythmView{}
    // directly, so that assertion holds even if a stale, saturated onset
    // count survived the rest of init(). This is the same gap "reset
    // invalidates and the ring re-fills from scratch" (above) closes for
    // reset_phases() -- drive the modulator to just past the first post-init
    // wrap and check there instead, where a fresh ring (2 onsets) reads
    // invalid but a stale, carried-over onset count (>=3 from before
    // re-init) would read valid.
    SuperModulator m = make_modulator(4);
    for (int i = 0; i < 4 * 24000; ++i) m.process();
    REQUIRE(m.rhythm().valid);          // ring is full and latched before re-init

    m.init(48000.f, seed_base_for_pitch(kPitchSeed));
    CHECK_FALSE(m.rhythm().valid);      // immediate post-init: the view was reset directly

    // Reconfigure to 2 steps/cycle so the first post-init wrap sees exactly
    // two onsets, as in "invalid until three onsets have been seen" and
    // "reset invalidates" above. init() re-seeds the phrase/groove
    // generator and the rate scale (both reset to identity) but does not
    // touch step config or DENSE/VARIATION -- these must be redriven here,
    // same as the old standalone-ModLane version of this test had to redo
    // set_step()/set_rate_hz()/set_density()/set_variation() after init().
    m.set_step(true, 2);
    force_pitch_rate_1hz(m);
    m.set_density(1.f);
    m.set_variation(0.f);

    // First post-init wrap: two onsets since init() -- one real gap, not a
    // rhythm, unless a stale onset count carried across init() makes it look
    // like one. clock_scale = 8/2 = 4, phase_inc = 4/48000 = 1/12000,
    // cycle = 12000 samples; +500 covers float32's rounding of phase_inc
    // landing the wrap a couple of samples past the ideal 12000.
    for (int i = 0; i < 12000 + 500; ++i) m.process();
    // Pin the precondition itself: just past the first post-init wrap, not
    // short of it -- see the identical margin-loop note in "invalid until
    // three onsets have been seen" above. Without this, if phase arithmetic
    // ever shifted the wrap outside this window, the CHECK_FALSE below
    // would pass vacuously.
    REQUIRE(m.pitch_phase() < 0.1f);
    CHECK_FALSE(m.rhythm().valid);      // must not republish the stale ring at the first post-init wrap

    // Second post-init wrap: the ring has genuinely re-filled from scratch.
    for (int i = 0; i < 12000; ++i) m.process();
    CHECK(m.rhythm().valid);
    CHECK(m.rhythm().gap[0] == doctest::Approx(6000).epsilon(0.01));
}

// There used to be a case here -- "the tick() path never publishes a zero
// gap" -- driving ModLane::tick() directly and reading ModLane::rhythm().
// It is deliberately NOT relocated: the ring now lives on SuperModulator and
// is fed exclusively by LANE_PITCH's process() (SuperModulator::process()
// never calls tick() on LANE_PITCH), so the scenario that test exercised --
// several onsets landing inside one tick() call, one of them recording a
// same-call zero-sample gap -- can no longer happen to any ring anywhere:
// process() advances one edge at a time, so consecutive onsets it records
// are always at least 1 sample apart. See rhythm_view.h's updated contract
// comment. The zero-gap clamp itself (SuperModulator::process()) is kept as
// insurance for a future tick()-fed consumer, but there is currently nothing
// that can exercise it, so pinning it with a test here would be testing
// dead code, not a live guarantee.
TEST_CASE("rhythm ring: SuperModulator::rhythm() forwards the PITCH lane") {
    // The texture lanes run at different ratios of the same base rate
    // (kLaneRatio in super_modulator.cpp: SOURCE x2, SIZE x0.5, PITCH x1,
    // MOTION x0.75, LEVEL x1.5), so their FLOW cycle periods differ from
    // PITCH's. If rhythm() ever forwarded a different lane, this gap would
    // come out at 12000 (SOURCE), 48000 (SIZE), 32000 (MOTION) or 16000
    // (LEVEL) instead of PITCH's 24000, and the CHECKs below would fail.
    SuperModulator m;
    m.init(48000.f, 1u);
    m.set_synced(true);
    m.set_tempo_bpm(120.f);
    m.set_rate(0.5f);   // division idx 8 ("1/4", cpb=1) -> base_hz = 2 Hz ->
                         // PITCH (ratio x1) cycles every 48000/2 = 24000 samples.
    // The ring's latch (on wrapped()) reads the ring from BEFORE the current
    // wrap's own record (on fired()) -- see SuperModulator::process() -- so
    // validity (>= 3 onsets) needs a 4th wrap, not a 3rd; +500 covers
    // float32's rounding of phase_inc landing real cycles a hair past the
    // ideal 24000.
    for (int i = 0; i < 4 * 24000 + 500; ++i) m.process();
    REQUIRE(m.pitch_phase() < 0.1f);   // just past the 4th real wrap, not short of it

    const RhythmView& rv = m.rhythm();
    REQUIRE(rv.valid);
    CHECK(rv.gap[0] == doctest::Approx(24000).epsilon(0.01));
    CHECK(rv.gap[1] == doctest::Approx(24000).epsilon(0.01));
}
