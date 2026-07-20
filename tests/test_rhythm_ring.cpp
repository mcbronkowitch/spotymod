#include <doctest/doctest.h>
#include "mod/lane.h"
#include "mod/super_modulator.h"
#include <cmath>

using namespace spky;

namespace {
// A melodic STEP lane at a known rate. 4 steps/cycle, cycle rate chosen so a
// step is an exact sample count: rate_hz * clock_scale = cycles/s.
// clock_scale = 8/steps = 2, so phase_inc = rate_hz/sr * 2.
// With rate_hz = 1.0 and sr = 48000: phase_inc = 1/24000, one cycle = 24000
// samples, one step = 6000 samples.
ModLane make_lane(int steps = 4) {
    ModLane l;
    // set_melodic() must precede init(): init() only builds the groove
    // ranking (pg_gen_groove) when _melodic is already true, matching every
    // other lane-builder helper in this test suite (test_gate_density.cpp,
    // test_new_phrase.cpp, test_variation.cpp). Calling it after init() -- as
    // an earlier draft of this file did -- leaves the groove at its default
    // (len=1), so _groove_k() is always 1 and DENSITY has no effect.
    l.set_melodic(true);
    l.init(48000.f, 0xC0FFEEu);
    l.set_step(true, steps);
    l.set_rate_hz(1.f);
    l.set_density(1.f);          // every slot gated: uniform onsets
    l.set_variation(0.f);        // LOOP: no mutation, no drift
    return l;
}
}  // namespace

TEST_CASE("rhythm ring: a uniform STEP pattern reports the step length as both gaps") {
    ModLane l = make_lane(4);
    // Run four full cycles so the ring fills and at least one wrap latches it.
    for (int i = 0; i < 4 * 24000; ++i) l.process();

    const RhythmView& rv = l.rhythm();
    REQUIRE(rv.valid);
    CHECK(rv.gap[0] == doctest::Approx(6000).epsilon(0.01));
    CHECK(rv.gap[1] == doctest::Approx(6000).epsilon(0.01));
}

TEST_CASE("rhythm ring: a rest step lengthens the gap instead of writing an onset") {
    // DENSE below 1 masks the lowest-ranked slots -> fewer onsets per cycle,
    // so at least one gap must exceed a single step length.
    ModLane l = make_lane(8);
    l.set_density(0.5f);         // roughly half the cell notes play
    for (int i = 0; i < 8 * 24000; ++i) l.process();

    const RhythmView& rv = l.rhythm();
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
    ModLane l = make_lane(2);
    CHECK_FALSE(l.rhythm().valid);          // fresh lane

    // First wrap: onsets at step0 (~n=1) and step1 (~n=6000) have fired;
    // the wrap-triggered onset that opens cycle 2 fires an instant later in
    // the same process() call, but _wrap_events() reads the ring before it.
    // +500 samples covers float32's rounding of phase_inc landing the wrap a
    // couple of samples past the ideal 12000, well short of the next step
    // boundary at ~18000.
    for (int i = 0; i < 12000 + 500; ++i) l.process();
    // Pin the precondition itself: we are just past the first real wrap, not
    // just short of one. Without this, if phase arithmetic ever shifted the
    // wrap outside this window, CHECK_FALSE below would pass against a
    // never-latched default _rhythm -- proving nothing.
    REQUIRE(l.phase() < 0.1f);
    CHECK_FALSE(l.rhythm().valid);          // two onsets seen -- one gap, not a rhythm

    // Second wrap: that cycle-2-opening onset is now the ring's third onset,
    // in time for this wrap's latch.
    for (int i = 0; i < 12000; ++i) l.process();
    CHECK(l.rhythm().valid);
}

TEST_CASE("rhythm ring: the view only moves at a cycle wrap") {
    // A uniform (density 1) pattern can't isolate this: every onset's gap is
    // ~6000 regardless of when it's read, so "latch at every onset" and
    // "latch only at the wrap" are indistinguishable by value. DENSE < 1
    // (as in the "rest step" case) makes onsets fall at genuinely different
    // intervals within a cycle, so reading mid-cycle only agrees with the
    // wrap-latched snapshot if the latch really did wait for the wrap.
    ModLane l = make_lane(8);
    l.set_density(0.5f);
    // clock_scale = 8/8 = 1, so phase_inc = 1/48000 -- float32 rounds that a
    // hair low, so the real cycle is ~47969 samples, not the ideal 48000.
    // Run comfortably past the 2nd real wrap (~95939).
    for (int i = 0; i < 2 * 48000 + 500; ++i) l.process();
    REQUIRE(l.rhythm().valid);
    const RhythmView before = l.rhythm();

    // Advance most of the next cycle (3rd real wrap is ~47969 samples later,
    // ~143908) -- several onsets of varying gap length fire, but no wrap.
    for (int i = 0; i < 40000; ++i) l.process();
    // Pin the precondition itself: comfortably short of the next wrap, not
    // past it. Without this, if phase arithmetic ever shifted the 3rd real
    // wrap into this window, the CHECKs below would pass vacuously -- the
    // ring would have re-latched to the same values by coincidence rather
    // than the test having caught "latch only moves at a wrap".
    REQUIRE(l.phase() > 0.5f);
    CHECK(l.rhythm().gap[0] == before.gap[0]);
    CHECK(l.rhythm().gap[1] == before.gap[1]);
}

TEST_CASE("rhythm ring: FLOW lanes fill the ring from cycle wraps") {
    ModLane l;
    // set_melodic() before init(): see make_lane()'s comment above. Harmless
    // here today -- FLOW's `gated` is unconditionally true (lane.cpp) so the
    // groove ranking this ordering protects is never consulted -- but
    // reordering it anyway keeps this file free of the anti-pattern its own
    // fix targets, so a later edit that adds set_density() here doesn't
    // silently get no DENSE effect.
    l.set_melodic(true);
    l.init(48000.f, 0xC0FFEEu);
    l.set_step(false, 4);        // FLOW: clock_scale = 1, cycle = 48000 samples
    l.set_rate_hz(1.f);
    l.set_variation(0.f);
    for (int i = 0; i < 4 * 48000; ++i) l.process();

    const RhythmView& rv = l.rhythm();
    REQUIRE(rv.valid);
    CHECK(rv.gap[0] == doctest::Approx(48000).epsilon(0.01));
}

TEST_CASE("rhythm ring: reset invalidates and the ring re-fills from scratch") {
    // 2 steps/cycle, as in "invalid until three onsets" above: only checking
    // CHECK_FALSE right after reset() doesn't exercise the onset counter --
    // _rhythm is reset to RhythmView{} directly, so that assertion holds
    // even if _onsets survives the reset. A 4-step lane's first post-reset
    // wrap can't catch a stale counter either (4 onsets is already enough on
    // its own). With 2 steps/cycle, only two onsets fire by the first
    // post-reset wrap -- not a rhythm yet, unless a stale, already-saturated
    // counter from before the reset makes it look like one.
    ModLane l = make_lane(2);
    for (int i = 0; i < 3 * 12000; ++i) l.process();
    REQUIRE(l.rhythm().valid);

    l.reset(0.f);
    CHECK_FALSE(l.rhythm().valid);          // no gap straddles the reset

    // First post-reset wrap: two onsets since reset -- one real gap.
    for (int i = 0; i < 12000 + 500; ++i) l.process();
    // Pin the precondition: just past the first post-reset wrap, not short
    // of it -- see the identical margin-loop note in "invalid until three
    // onsets have been seen" above.
    REQUIRE(l.phase() < 0.1f);
    CHECK_FALSE(l.rhythm().valid);

    // Second post-reset wrap: the ring has genuinely re-filled from scratch.
    for (int i = 0; i < 12000; ++i) l.process();
    CHECK(l.rhythm().valid);
    CHECK(l.rhythm().gap[0] == doctest::Approx(6000).epsilon(0.01));
}

TEST_CASE("rhythm ring: init() clears a stale ring") {
    // SuperModulator::init() re-inits every lane on a host sample-rate
    // change (super_modulator.cpp). A lane carrying a saturated _onsets and
    // gaps measured in the old sample rate's units must not republish that
    // stale rhythm as valid at its first post-init wrap.
    //
    // Checking CHECK_FALSE immediately after init() alone doesn't exercise
    // the onset counter: init() sets _rhythm = RhythmView{} directly, so
    // that assertion holds even if a stale, saturated _onsets survived the
    // rest of init(). This is the same gap "reset invalidates and the ring
    // re-fills from scratch" (above) closes for reset() -- drive the lane
    // to just past the first post-init wrap and check there instead, where
    // a fresh ring (2 onsets) reads invalid but a stale, carried-over
    // _onsets (>=3 from before re-init) would read valid.
    ModLane l = make_lane(4);
    for (int i = 0; i < 4 * 24000; ++i) l.process();
    REQUIRE(l.rhythm().valid);          // ring is full and latched before re-init

    l.init(48000.f, 0xC0FFEEu);
    CHECK_FALSE(l.rhythm().valid);      // immediate post-init: _rhythm was reset directly

    // set_step() must follow init() -- init() only re-seeds the phrase/groove
    // generator, it doesn't touch the step configuration set on the lane
    // instance. Reconfigure to 2 steps/cycle so the first post-init wrap
    // sees exactly two onsets, as in "invalid until three onsets have been
    // seen" and "reset invalidates" above.
    l.set_step(true, 2);
    l.set_rate_hz(1.f);
    l.set_density(1.f);
    l.set_variation(0.f);

    // First post-init wrap: two onsets since init() -- one real gap, not a
    // rhythm, unless a stale _onsets carried across init() makes it look
    // like one. clock_scale = 8/2 = 4, phase_inc = 4/48000 = 1/12000,
    // cycle = 12000 samples; +500 covers float32's rounding of phase_inc
    // landing the wrap a couple of samples past the ideal 12000.
    for (int i = 0; i < 12000 + 500; ++i) l.process();
    // Pin the precondition itself: just past the first post-init wrap, not
    // short of it -- see the identical margin-loop note in "invalid until
    // three onsets have been seen" above. Without this, if phase arithmetic
    // ever shifted the wrap outside this window, the CHECK_FALSE below
    // would pass vacuously.
    REQUIRE(l.phase() < 0.1f);
    CHECK_FALSE(l.rhythm().valid);      // must not republish the stale ring at the first post-init wrap

    // Second post-init wrap: the ring has genuinely re-filled from scratch.
    for (int i = 0; i < 12000; ++i) l.process();
    CHECK(l.rhythm().valid);
    CHECK(l.rhythm().gap[0] == doctest::Approx(6000).epsilon(0.01));
}

TEST_CASE("rhythm ring: the tick() path never publishes a zero gap") {
    // _since_onset only advances once per 96-sample tick() window, but the
    // edge walk inside tick() can call _on_boundary() several times in that
    // same window (lane.cpp: "Panel-reachable worst case is ~8 edges"). A
    // fast STEP rate with every slot gated forces multiple onsets per
    // window; the second and later onsets must never record/publish a gap
    // of exactly 0 samples.
    ModLane l = make_lane(8);
    l.set_rate_hz(300.f);   // step length ~20 samples: several onsets land
                             // inside one 96-sample tick() window.
    bool saw_valid = false;
    for (int i = 0; i < 2000; ++i) {
        l.tick();
        if (l.rhythm().valid) {
            saw_valid = true;
            CHECK(l.rhythm().gap[0] != 0);
            CHECK(l.rhythm().gap[1] != 0);
        }
    }
    REQUIRE(saw_valid);   // the loop must actually have exercised the ring
}

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
    // _wrap_events() latches _rhythm from the PREVIOUS cycle's onset count
    // before _on_boundary() records the current wrap's onset, so validity
    // (_onsets >= 3) needs a 4th wrap, not a 3rd; +500 covers float32's
    // rounding of phase_inc landing real cycles a hair past the ideal 24000.
    for (int i = 0; i < 4 * 24000 + 500; ++i) m.process();
    REQUIRE(m.pitch_phase() < 0.1f);   // just past the 4th real wrap, not short of it

    const RhythmView& rv = m.rhythm();
    REQUIRE(rv.valid);
    CHECK(rv.gap[0] == doctest::Approx(24000).epsilon(0.01));
    CHECK(rv.gap[1] == doctest::Approx(24000).epsilon(0.01));
}
