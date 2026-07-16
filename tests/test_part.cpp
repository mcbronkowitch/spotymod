#include <doctest/doctest.h>
#include <cmath>
#include <algorithm>
#include <vector>
#include "parts/part.h"
using namespace spky;

TEST_CASE("part: inactive target contributes only its base value") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_SIZE, false);
    p.set_target_base(LANE_SIZE, 0.3f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 1000; ++i) p.process(l, r);
    CHECK(p.target_value(LANE_SIZE) == doctest::Approx(0.3f));
}

TEST_CASE("part detune: engine pitch shifts but pitch_cv stays quantized") {
    Part p; p.init(48000.f, 1u);
    p.set_depth(0.f);                          // isolate the base value
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_tune(0.5f);
    float l, r;
    for (int i = 0; i < 4000; ++i) p.process(l, r);   // ride out the quantizer slew
    float cv0 = p.pitch_cv();
    p.set_detune_cents(50.f);                  // +50 cents on the engine pitch
    for (int i = 0; i < 4000; ++i) p.process(l, r);
    CHECK(p.pitch_cv() == doctest::Approx(cv0));       // rack CV out unchanged
}

TEST_CASE("part: active target modulates around its base, clamped to [0,1]") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_target_depth(LANE_PITCH, 1.f);
    p.set_depth(1.f);
    p.mod().set_range(1.f);
    p.mod().set_shape(0.5f);
    p.mod().set_rate(0.6f);
    float minv = 1.f, maxv = 0.f, l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        float t = p.target_value(LANE_PITCH);
        if (t < minv) minv = t;
        if (t > maxv) maxv = t;
    }
    CHECK(maxv > minv);
    CHECK(minv >= 0.f);
    CHECK(maxv <= 1.f);
}

TEST_CASE("part: DEPTH 0 pins targets to base") {
    Part p;
    p.init(48000.f, 5);
    p.quant().set_mode(QuantMode::Free);   // this test asserts the raw path
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_depth(0.f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 5000; ++i) {
        p.process(l, r);
        CHECK(p.target_value(LANE_PITCH) == doctest::Approx(0.5f));
    }
}

TEST_CASE("part: a PITCH fire raises the gate") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_PITCH, true);
    p.mod().set_rate(0.7f);
    bool saw_gate = false;
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        if (p.gate()) saw_gate = true;
    }
    CHECK(saw_gate);
}

TEST_CASE("part: SCALE mode lands pitch only on allowed dorian degrees") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.mod().set_range(1.f);
    p.mod().set_rate(0.6f);
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        float semis = p.pitch_cv() * 36.f;
        int k = static_cast<int>(semis + 0.5f);
        CHECK(std::fabs(semis - k) < 1e-4f);                       // on the grid
        CHECK(((SCALE_MASKS[SCALE_DORIAN] >> (k % 12)) & 1) == 1); // in dorian
    }
}

TEST_CASE("part: FREE mode restores the raw continuous pitch path") {
    Part p;
    p.init(48000.f, 5);
    p.quant().set_mode(QuantMode::Free);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_depth(0.f);
    float l, r;
    p.process(l, r);
    CHECK(p.pitch_cv() == doctest::Approx(0.5f));   // off-grid value passes through
}

TEST_CASE("part: inactive fx target contributes only its base value") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_base(FXT_FLUX_TIME, 0.37f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 1000; ++i) p.process(l, r);
    CHECK(p.fx_target_value(FXT_FLUX_TIME) == doctest::Approx(0.37f));
}

TEST_CASE("part: active fx target modulates around its base, clamped") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_active(FXT_FLUX_TIME, true);
    p.set_fx_target_base(FXT_FLUX_TIME, 0.5f);
    p.set_fx_target_depth(FXT_FLUX_TIME, 1.f);
    p.set_depth(1.f);
    p.mod().set_range(1.f);
    p.mod().set_rate(0.6f);
    float minv = 1.f, maxv = 0.f, l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        float t = p.fx_target_value(FXT_FLUX_TIME);
        if (t < minv) minv = t;
        if (t > maxv) maxv = t;
    }
    CHECK(maxv > minv);
    CHECK(minv >= 0.f);
    CHECK(maxv <= 1.f);
}

TEST_CASE("part: master DEPTH 0 pins fx targets to base") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_active(FXT_REV_SEND, true);
    p.set_fx_target_base(FXT_REV_SEND, 0.4f);
    p.set_depth(0.f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 5000; ++i) {
        p.process(l, r);
        CHECK(p.fx_target_value(FXT_REV_SEND) == doctest::Approx(0.4f));
    }
}

TEST_CASE("part: fx targets are never quantized (unlike the PITCH lane)") {
    Part p;
    p.init(48000.f, 5);   // boots in SCALE mode
    p.set_fx_target_base(FXT_FX_MIX, 0.437f);   // off any scale grid
    float l, r;
    p.process(l, r);
    CHECK(p.fx_target_value(FXT_FX_MIX) == doctest::Approx(0.437f));
}

TEST_CASE("part: 4-output process yields sends that follow REV SEND") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_base(FXT_REV_SEND, 1.f);    // send fully open
    float l, r, sl, sr;
    for (int i = 0; i < 2000; ++i) p.process(l, r, sl, sr);
    CHECK(sl == doctest::Approx(l));            // sin(pi/2) = 1
    p.set_fx_target_base(FXT_REV_SEND, 0.f);
    for (int i = 0; i < 2000; ++i) p.process(l, r, sl, sr);   // ride out smoother
    CHECK(sl == doctest::Approx(0.f));
}

TEST_CASE("part: boots on the synth engine and hums in FLOW (drone promise)") {
    Part p;
    p.init(48000.f, 5);
    CHECK(p.engine_id() == ENGINE_SYNTH);
    float energy = 0.f, l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        energy += l * l;
    }
    CHECK(p.active_voices() >= 1);
    CHECK(energy > 1e-3f);
}

// DENSE 0 leaves only the downbeat/anchor slot able to fire, so after the
// guaranteed first-sample fire (STEP entry: step -1 -> 0) the next natural
// note is a full cycle away. Settle past that single note's decay before
// checking silence, so the manual trigger is the only voice left.
TEST_CASE("part: manual trigger fires at the current pitch and raises the gate") {
    Part p;
    p.init(48000.f, 5);
    p.set_voice_decay(0.f);   // shortest decay ratio: settle window stays short
    p.set_step(true, 8);
    p.mod().set_density(0.f);     // anchor-only: next natural fire is a cycle away
    float l, r;
    for (int i = 0; i < 10000; ++i) p.process(l, r);   // past the boot note's decay
    CHECK(p.active_voices() == 0);          // silent before the tap
    p.trigger_manual();
    CHECK(p.gate());
    p.process(l, r);
    CHECK(p.active_voices() == 1);
}

// Restores the coverage lost when PROBABILITY was removed (the old test used
// set_probability(0) to freeze all natural firing, isolating a manual trigger
// so a full decay tail could be measured over several seconds of silence).
// DENSITY can no longer freeze the downbeat slot (it is unmaskable by design,
// see test_gate_density.cpp), so instead of waiting for silence this measures
// the decay tail WITHIN the first cycle, before any second trigger can occur:
//   - DENSITY 0 leaves only the downbeat slot able to fire, so after the
//     guaranteed first-sample fire (STEP entry: step -1 -> 0) the NEXT
//     natural fire cannot happen before a full master cycle elapses.
//   - The fixed 50 ms measurement offset is well inside both cycles under
//     test (144 ms @ rate 0.8, ~622 ms @ rate 0.6), so no second trigger can
//     land before voice_env(0) is sampled, in either case.
// voice_env(0) is read (not active_voices()/silence) because SynthEngine's
// decay is 1.5x the cycle length (by spec) - a full cycle is NOT long enough
// for the tail to reach idle, so envelope LEVEL at a shared time offset is
// the observable that distinguishes a fast decay from a slow one.
//
// This only passes if Part is actually forwarding mod().set_rate/set_cycle
// to the engine: if that wiring broke, both engines would keep the default
// 1.0 s cycle (1.5 s decay) regardless of rate_norm, so voice_env(0) would be
// (near) IDENTICAL at the fixed offset instead of clearly separated.
TEST_CASE("part: decay length follows the master cycle (set_cycle forwarding)") {
    auto env_at_offset = [](float rate_norm, int offset_samples) {
        Part p;
        p.init(48000.f, 5);
        p.set_step(true, 8);          // cancels the boot FLOW auto-trigger
        p.mod().set_density(0.f);     // only the unmaskable downbeat slot fires
        p.mod().set_rate(rate_norm);
        float l, r;
        p.process(l, r);              // downbeat fires here (step -1 -> 0)
        REQUIRE(p.lane_fired(LANE_PITCH));
        REQUIRE(p.active_voices() == 1);   // exactly one voice: the downbeat
        for (int i = 0; i < offset_samples; ++i) p.process(l, r);
        return p.voice_env(0);
    };
    const int offset = 2400;   // 50 ms @ 48 kHz
    float fast = env_at_offset(0.8f, offset);   // ~6.9 Hz -> cycle ~0.144 s
    float slow = env_at_offset(0.6f, offset);   // ~1.6 Hz -> cycle ~0.622 s
    CHECK(slow > fast);
    CHECK(slow - fast > 0.3f);   // clearly separated, not just noise
}

TEST_CASE("part: engine switch test tone <-> synth is click-free") {
    Part p;
    p.init(48000.f, 5);
    float prev_l = 0.f, max_delta = 0.f;
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        if (i == 12000) p.set_engine(ENGINE_TEST_TONE);
        if (i == 30000) p.set_engine(ENGINE_SYNTH);
        p.process(l, r);
        if (i > 0) max_delta = std::max(max_delta, std::fabs(l - prev_l));
        prev_l = l;
    }
    // a hard swap would step by the level difference (~0.3-0.5); the 4 ms
    // Hann fade keeps sample-to-sample deltas at waveform scale.
    CHECK(max_delta < 0.15f);
    CHECK(p.engine_id() == ENGINE_SYNTH);   // second switch completed
}

TEST_CASE("part: the test tone engine reports zero active voices") {
    Part p;
    p.init(48000.f, 5);
    p.set_engine(ENGINE_TEST_TONE);
    float l, r;
    for (int i = 0; i < 1000; ++i) p.process(l, r);   // ride out the 4 ms fades
    CHECK(p.engine_id() == ENGINE_TEST_TONE);
    CHECK(p.active_voices() == 0);
}

// Composed-note sustain routed to the GATE output (rhythm-groove-design.md
// section 3). SYNCED + tempo 60 + rate norm 0.5 lands on division index 8
// ("1/4", cpb 1) -> base_hz 1 Hz -> PITCH lane rate = base_hz * kLaneRatio
// (2.0) = 2 Hz, a 24000-sample cycle / 16 steps = 1500 samples/step. Mirrors
// the ModLane-level timing in "gate releases before the next note when the
// gap is long" (tests/test_gate_density.cpp), but observed through Part.
TEST_CASE("part: gate sustains the composed STEP note, releasing before the next downbeat") {
    Part p;
    p.init(48000.f, 5);
    p.set_step(true, 16);
    p.mod().set_density(0.f);      // anchor-only: notes at steps 0 and 8 (L=8)
    p.mod().set_synced(true);
    p.mod().set_tempo_bpm(60.f);
    p.mod().set_rate(0.5f);        // -> PITCH lane 2 Hz: 1500 samples/step
    const int step_samples = 1500;
    std::vector<char> gate;
    float l, r;
    for (int n = 0; n < 24000; ++n) { p.process(l, r); gate.push_back(p.gate()); }
    CHECK(gate[10]);                          // note sounding just after the downbeat
    // note_len is capped at 4 < the 8-step gap, so the gate MUST fall in between
    CHECK_FALSE(gate[7 * step_samples + 10]);
    int run = 0;                              // high run from the downbeat: 1..4 steps
    while (run < 24000 && gate[run]) ++run;
    CHECK(run >= 1 * step_samples - 2);
    CHECK(run <= 4 * step_samples + 2);
}

// FLOW must be unaffected by the composed-sustain wiring: ModLane::gate_state()
// returns true unconditionally in FLOW, so Part::gate() must NOT OR that in —
// only the retrigger pulse (_gate_ctr) should ever raise the gate here.
TEST_CASE("part: gate in FLOW stays pulse-only (never permanently high)") {
    Part p;
    p.init(48000.f, 5);   // boots in FLOW (drone); no set_step() call
    CHECK_FALSE(p.gate());   // nothing has fired yet: low at the very first sample
    bool saw_low = false;
    float l, r;
    for (int n = 0; n < 24000; ++n) {   // 0.5 s
        p.process(l, r);
        if (!p.gate()) saw_low = true;
    }
    CHECK(saw_low);
}
