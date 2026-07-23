#include <doctest/doctest.h>
#include <cmath>
#include "mod/lane.h"
#include "mod/super_modulator.h"
using namespace spky;

static void configure_flow(ModLane& l, float hz) {
    l.init(48000.f, 1234);
    l.set_range(1.f);
    l.set_shape(0.5f);        // ramp
    l.set_smooth(0.f);
    l.set_rate_hz(hz);
}

TEST_CASE("lane FLOW: rate accuracy — ~2 fires per second") {
    ModLane l; configure_flow(l, 2.f);
    const int seconds = 5;
    int fires = 0;
    for (int i = 0; i < 48000 * seconds; ++i) { l.process(); if (l.fired()) ++fires; }
    // A free-running float phasor never closes a cycle in EXACTLY N samples, so
    // assert the fire RATE over a multi-second window (+/-1), not exact closure.
    CHECK(fires >= 2 * seconds - 1);   // ~10
    CHECK(fires <= 2 * seconds + 1);
}

TEST_CASE("lane FLOW: output stays in range") {
    ModLane l; configure_flow(l, 3.f);
    for (int i = 0; i < 48000; ++i) {
        float v = l.process();
        CHECK(v >= -1.001f);
        CHECK(v <=  1.001f);
    }
}

TEST_CASE("lane: SMOOTH turns a step into a glide") {
    ModLane l;
    l.init(48000.f, 55);
    l.set_range(1.f);
    l.set_shape(0.5f);        // ramp: consecutive step values differ
    l.set_step(true, 2);      // step-clock: step = 6000 samples; boundary at ~6000
    l.set_smooth(0.5f);       // glide ~3 ms: settles well within a step, still gliding ~1 ms past a boundary
    l.set_rate_hz(1.f);       // cycle_hz = 4 -> 12000 samples/cycle

    for (int i = 0; i < 5000; ++i) l.process();    // settle in step 0
    float settled0 = l.process();
    float target0  = l.target();
    for (int i = 5002; i < 6050; ++i) l.process(); // cross into step 1
    float out_after = l.process();                 // ~1 ms past boundary
    float target1   = l.target();

    CHECK(target1 != doctest::Approx(target0));        // new value latched
    CHECK(std::fabs(out_after - target1) > 0.01f);     // output still gliding
    CHECK(std::fabs(settled0  - target0) < 0.01f);     // was settled before
}

TEST_CASE("lane kick: phase jump is permanent, no decay") {
    ModLane lane; lane.init(48000.f, 4u);
    lane.set_rate_hz(0.f);                 // freeze phase advance to isolate the kick
    CHECK(lane.phase() == doctest::Approx(0.f));
    lane.kick(0.25f, 0.f);
    CHECK(lane.phase() == doctest::Approx(0.25f));
    for (int i = 0; i < 1000; ++i) lane.process();
    CHECK(lane.phase() == doctest::Approx(0.25f));   // permanent
}

TEST_CASE("lane shape_offset: shifts the effective shape; offset 0 is bit-identical") {
    ModLane a; a.init(48000.f, 8u); a.set_rate_hz(1.f); a.set_shape(0.3f);
    ModLane b; b.init(48000.f, 8u); b.set_rate_hz(1.f); b.set_shape(0.3f);
    b.set_shape_offset(0.4f);
    bool differ = false;
    for (int i = 0; i < 48000; ++i)
        if (std::fabs(a.process() - b.process()) > 1e-4f) differ = true;
    CHECK(differ);

    ModLane c; c.init(48000.f, 8u); c.set_rate_hz(1.f); c.set_shape(0.3f);
    ModLane d; d.init(48000.f, 8u); d.set_rate_hz(1.f); d.set_shape(0.3f);
    d.set_shape_offset(0.f);
    bool same = true;
    for (int i = 0; i < 48000; ++i) if (c.process() != d.process()) same = false;
    CHECK(same);
}

TEST_CASE("lane: step clock accessors expose slot and step duration") {
    ModLane l;
    l.init(48000.f, 77);
    l.set_melodic(true);
    l.set_rate_hz(1.f);            // 1 Hz cycle
    l.set_step(true, 8);
    // step_samples: phase covers one cycle per 1/(rate*clock_scale) seconds;
    // with 8 steps at clock_scale 8/8 = 1 a step is sr / (rate * 8) = 6000.
    CHECK(l.steps() == 8);
    CHECK(l.step_samples() == doctest::Approx(6000.f).epsilon(0.001));
    CHECK(l.cur_step() == -1);     // no boundary yet
    // run one full step: the slot counter must have advanced into range
    for (int i = 0; i < 6001; ++i) l.process();
    CHECK(l.cur_step() >= 0);
    CHECK(l.cur_step() < 8);
}

// F4 (whole-branch review): step_samples() must account for the EVOLVE rate
// walk. The lane advances its phase by _phase_inc * (1 + _ev_rate) -- both in
// process() and in tick()'s dp1 -- and _ev_rate is clamped to +-0.2, so a
// step_samples() built on _phase_inc alone is wrong by up to 20% under
// EVOLVE/GROW. Part pushes this number to the sampler as its step clock
// (set_step_clock), which the STEP grid fallback slices on directly; a 20%
// error there is audible drift against the phrase.
//
// The measurement is the definition: count samples between the lane's own step
// boundaries and require step_samples() to agree. It is taken inside a single
// cycle, because _ev_rate only moves at a wrap (_wrap_events), so the step
// duration is constant across the seven boundaries measured.
TEST_CASE("lane: step_samples() accounts for the EVOLVE rate walk") {
    ModLane l;
    l.init(48000.f, 4242);
    l.set_melodic(true);
    l.set_rate_hz(1.f);                 // nominal step = 48000 / (1 * 8) = 6000
    l.set_step(true, 8);
    l.set_variation(1.f);               // GROW: _ev_rate random-walks at each wrap
    for (int i = 0; i < 48000 * 200; ++i) l.process();   // 200 cycles: let it leave 0

    while (!l.wrapped()) l.process();   // land on a wrap: _ev_rate is now fixed
                                        // for the whole cycle below
    const float claimed = l.step_samples();
    int prev = l.cur_step(), boundaries = 0, samples = 0;
    while (boundaries < 7) {            // steps 1..7 of this cycle
        l.process();
        ++samples;
        if (l.cur_step() != prev) { prev = l.cur_step(); ++boundaries; }
    }
    const float measured = static_cast<float>(samples) / 7.f;
    INFO("claimed=" << claimed << " measured=" << measured);
    // Non-vacuous: the rate walk really did move off zero, so the uncorrected
    // 1/(_phase_inc * steps) == 6000 is a materially different answer.
    REQUIRE(std::fabs(measured - 6000.f) > 60.f);
    CHECK(claimed == doctest::Approx(measured).epsilon(0.002));
}

// --- step_index: die Rundungsregel, die process() und der Snap teilen -------
//
// Der Snap (spec 2026-07-23 sampler-performance-fixes) muss den Slot aus einer
// Phase berechnen, BEVOR die Lane sie verarbeitet hat -- zurueckgelesen waere
// cur_step() noch -1, weil ModLane::reset() es genau darauf setzt. Diese
// Funktion ist die eine Stelle, an der die Regel steht.
TEST_CASE("lane: step_index folds a phase onto its slot") {
    CHECK(ModLane::step_index(0.f,     8) == 0);
    CHECK(ModLane::step_index(0.124f,  8) == 0);
    CHECK(ModLane::step_index(0.125f,  8) == 1);
    CHECK(ModLane::step_index(0.5f,    8) == 4);
    CHECK(ModLane::step_index(0.999f,  8) == 7);

    // Die obere Klemme ist der Grund, warum das eine Funktion ist und kein
    // int-Cast: ohne sie liefert eine Phase von exakt 1.0 den Slot 8 und
    // greift einen Schritt hinter das Ende der Phrase.
    CHECK(ModLane::step_index(1.f,     8) == 7);
    CHECK(ModLane::step_index(1.5f,    8) == 7);

    // Ein einzelner Schritt hat nur den Slot 0, bei jeder Phase.
    CHECK(ModLane::step_index(0.f,     1) == 0);
    CHECK(ModLane::step_index(0.99f,   1) == 0);
}

// snap_pitch_phase setzt die PITCH-Lane und NUR sie -- die vier Texturlanes
// laufen weiter, sonst waere es die RST-Geste (reset_phases) unter anderem
// Namen. Der Onset-Gap-Ring wird mitgenullt: nach einem Phasensprung waere
// der naechste gemessene Abstand einer, den es nie gab, und dieser
// Rhythmus-Blick steuert die FX-Abgriffe des ANDEREN Decks.
TEST_CASE("mod: snap_pitch_phase moves the pitch lane alone") {
    SuperModulator m;
    m.init(48000.f, 7u);
    for (int i = 0; i < 5000; ++i) m.process();

    const float tex_before = m.lane_phase(LANE_MOTION);
    REQUIRE(m.pitch_phase() != doctest::Approx(0.25f).epsilon(1e-3));

    m.snap_pitch_phase(0.25f);

    CHECK(m.pitch_phase() == doctest::Approx(0.25f).epsilon(1e-6));
    CHECK(m.lane_phase(LANE_MOTION) == doctest::Approx(tex_before).epsilon(1e-6));
    CHECK(m.rhythm().gap[0] == 0);
    CHECK(m.rhythm().gap[1] == 0);
}
