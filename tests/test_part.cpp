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
    p.set_target_depth(LANE_PITCH, 0.f);       // isolate the base value
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

TEST_CASE("part: DEPTH 0 pins texture targets to base (pitch is exempt)") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_SIZE, true);
    p.set_target_base(LANE_SIZE, 0.5f);
    p.set_depth(0.f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 5000; ++i) {
        p.process(l, r);
        CHECK(p.target_value(LANE_SIZE) == doctest::Approx(0.5f));
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
    p.set_target_depth(LANE_PITCH, 0.f);
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
// (2.0) = 2 Hz; with the step-clock a step is 48000/(8*2) = 3000 samples
// regardless of the 16-step count. Mirrors the ModLane-level timing in "gate
// releases before the next note when the gap is long"
// (tests/test_gate_density.cpp), but observed through Part.
TEST_CASE("part: gate sustains the composed STEP note, releasing before the next downbeat") {
    Part p;
    p.init(48000.f, 5);
    p.set_step(true, 16);
    p.mod().set_density(0.f);      // anchor-only: notes at steps 0 and 8 (L=8)
    p.mod().set_synced(true);
    p.mod().set_tempo_bpm(60.f);
    p.mod().set_rate(0.5f);        // -> PITCH lane 2 Hz: 3000 samples/step
    const int step_samples = 3000;
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

TEST_CASE("part: COLOR never touches pitch CV or gate (chords are engine-side)") {
    Part a, b;
    a.init(48000.f, 123u);
    b.init(48000.f, 123u);
    b.set_color(0.8f);                             // chords on part B's twin only
    a.set_step(true, 8); b.set_step(true, 8);
    for (int i = 0; i < 96000; ++i) {
        float al, ar, bl, br;
        a.process(al, ar);
        b.process(bl, br);
        CHECK(a.pitch_cv() == b.pitch_cv());       // exact — CV contract survives
        CHECK(a.gate() == b.gate());
    }
}

TEST_CASE("part: chord size follows COLOR") {
    Part p;
    p.init(48000.f, 5u);
    CHECK(p.chord_size() == 1);
    p.set_color(0.5f);
    float l, r;
    for (int i = 0; i < 4800; ++i) p.process(l, r);   // apply() refreshes slots
    CHECK(p.chord_size() == 3);
    p.set_color(0.0f);
    for (int i = 0; i < 4800; ++i) p.process(l, r);
    CHECK(p.chord_size() == 1);
}

TEST_CASE("part: COLOR swept and returned to 0 renders like never touched") {
    Part a, b;
    a.init(48000.f, 42u);
    b.init(48000.f, 42u);
    float al, ar, bl, br;
    for (int i = 0; i < 4800; ++i) { a.process(al, ar); b.process(bl, br); }
    b.set_color(0.9f);                             // sweep up...
    for (int i = 0; i < 4800; ++i) { b.process(bl, br); a.process(al, ar); }
    // now both back at COLOR 0 — but b carries surface history; release it
    b.set_color(0.f);
    a.set_step(true, 8); b.set_step(true, 8);      // drop surfaces, STEP world
    for (int i = 0; i < 48000; ++i) {
        a.process(al, ar);
        b.process(bl, br);
    }
    // state cleanliness: identical firing (pitch CV) from here on
    for (int i = 0; i < 48000; ++i) {
        a.process(al, ar);
        b.process(bl, br);
        CHECK(a.pitch_cv() == b.pitch_cv());
    }
}

TEST_CASE("part: COLOR reaches the chord builder through process(), not the setter") {
    Part p;
    p.init(48000.f, 5u);
    p.set_depth(0.f);                 // MOD = 0: today's-behaviour invariant
    p.set_color(0.5f);
    float l, r;
    for (int i = 0; i < 4800; ++i) p.process(l, r);
    CHECK(p.chord_size() == 3);       // triad zone, exactly the knob position
    p.set_color(0.75f);
    for (int i = 0; i < 4800; ++i) p.process(l, r);
    CHECK(p.chord_size() == 4);
    p.set_color(0.f);
    for (int i = 0; i < 4800; ++i) p.process(l, r);
    CHECK(p.chord_size() == 1);
}

// --- COLOR as a MOTION target (spec 2026-07-18 color-motion-target) ---

namespace {
// Run one part for `n` samples, recording the chord size on every PITCH-lane
// fire. Returns the sizes seen, in order.
static std::vector<int> chord_sizes_over(Part& p, int n) {
    std::vector<int> sizes;
    float l, r;
    for (int i = 0; i < n; ++i) {
        p.process(l, r);
        if (p.lane_fired(LANE_PITCH)) sizes.push_back(p.chord_size());
    }
    return sizes;
}
} // namespace

TEST_CASE("color-mod: COLOR 0 stays one note whatever MOD and MOTION do") {
    Part p;
    p.init(48000.f, 5u);
    p.set_color(0.f);
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);
    p.set_step(true, 8);
    auto sizes = chord_sizes_over(p, 480000);      // 10 s, many MOTION cycles
    REQUIRE(!sizes.empty());
    for (int n : sizes) CHECK(n == 1);
}

TEST_CASE("color-mod: MOD 0 hands the ChordBuilder the knob, exactly") {
    Part p;
    p.init(48000.f, 5u);
    p.set_depth(0.f);
    p.set_target_active(LANE_MOTION, true);
    float l, r;
    for (float knob : {0.f, 0.2f, 0.5f, 0.77f, 1.f}) {
        p.set_color(knob);
        for (int i = 0; i < 480; ++i) p.process(l, r);
        CHECK(p.color_eff() == knob);              // exact, not Approx
    }
}

TEST_CASE("color-mod: a barely-open knob reaches up into chords") {
    Part p;
    p.init(48000.f, 5u);
    p.set_color(0.02f);                            // 2% of travel; gate fully open
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);
    p.set_step(true, 8);
    auto sizes = chord_sizes_over(p, 480000);
    REQUIRE(!sizes.empty());
    int maxn = 0;
    for (int n : sizes) if (n > maxn) maxn = n;
    CHECK(maxn >= 2);                              // the swing is additive, not a ceiling
}

TEST_CASE("color-mod: density varies per note at a mid knob position") {
    Part p;
    p.init(48000.f, 5u);
    p.set_color(0.35f);                            // near the 2/3-note zone edge (0.375)
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);
    p.set_step(true, 8);
    auto sizes = chord_sizes_over(p, 480000);
    REQUIRE(sizes.size() > 4);
    int mn = sizes[0], mx = sizes[0];
    for (int n : sizes) { if (n < mn) mn = n; if (n > mx) mx = n; }
    CHECK(mn < mx);                                // spread, not specific sizes
}

TEST_CASE("color-mod: an inactive MOTION target modulates nothing") {
    Part p;
    p.init(48000.f, 5u);
    p.set_color(0.5f);
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, false);
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        CHECK(p.color_eff() == 0.5f);              // exact
    }
}

TEST_CASE("color-mod: deterministic — same seed, same density sequence") {
    Part a, b;
    a.init(48000.f, 7u);
    b.init(48000.f, 7u);
    for (Part* p : {&a, &b}) {
        p->set_color(0.35f);
        p->set_depth(1.f);
        p->set_target_active(LANE_MOTION, true);
        p->set_step(true, 8);
    }
    auto sa = chord_sizes_over(a, 240000);
    auto sb = chord_sizes_over(b, 240000);
    CHECK(sa == sb);
    REQUIRE(!sa.empty());
}

TEST_CASE("part: an engine swapped in off the raster gets the commanded target immediately") {
    // Finding 3 regression. Before the fix, the engine-swap block in
    // process() completed the swap but left Part::_ctrl_ctr running on its
    // old schedule, so a freshly active engine got no set_targets()/
    // set_chord() push until the next natural raster tick -- up to
    // kCtrlInterval-1 samples on its power-on defaults. For TestToneEngine
    // that default is _freq = 220.f (test_tone_engine.h), not the commanded
    // pitch.
    //
    // Two warm-up process() calls (still on the boot engine) before
    // set_engine(): the SoftSwitch fade-out is a fixed 191-sample countdown
    // from the sample set_engine() starts it, so this lands the swap at
    // absolute sample 193 -- one sample past a raster tick (192), i.e. the
    // worst-case off-raster phase, which maximizes the stale window a bug
    // would leave (up to kCtrlInterval - 1 samples, here ending at the next
    // tick at 288).
    //
    // The swap sample itself is unusable as a probe: is_idle() -- and so the
    // swap -- fires exactly when SoftSwitch's fade output is at its silent
    // floor, and stays at/near exactly 0 for a few samples after (idle ->
    // rise transition), so amplitude there is dominated by the fade, not by
    // which engine state is behind it. Zero-crossing counting a window later
    // in the stale range sidesteps that: a positive scale factor from the
    // fade never flips the sign of the raw tone.
    // A tone held at the commanded 880 Hz completes ~1.3 cycles in an
    // 80-sample window (80/48000 s), i.e. several sign changes; a tone stuck
    // on TestToneEngine's 220 Hz power-on default cannot complete even half
    // a cycle in that window from a phase-0 start, i.e. zero sign changes.
    // That gap is what this test asserts on -- no exact-value/bit-matching
    // against a reference engine needed.
    Part p;
    p.init(48000.f, 11u);
    p.quant().set_mode(QuantMode::Free);   // passthrough: no slew to settle
    p.mod().set_rate(0.f);                 // park the LFOs so no lane fires in this window
    p.set_target_active(LANE_PITCH, false);
    p.set_target_base(LANE_PITCH, 1.f);    // commanded freq = 110 * 8^1 = 880 Hz

    float l = 0.f, r = 0.f;
    p.process(l, r);
    p.process(l, r);                       // two warm-up calls, see comment above
    p.set_engine(ENGINE_TEST_TONE);

    int swap_sample = -1;
    for (int i = 2; i < 96 * 4 && swap_sample < 0; ++i) {
        p.process(l, r);
        if (p.engine_id() == ENGINE_TEST_TONE) swap_sample = i;
    }
    REQUIRE(swap_sample == 193);   // pins the precondition this test relies on
    REQUIRE((swap_sample % SynthEngine::kCtrlInterval) != 0);   // deliberately off-raster

    int crossings = 0;
    float prev = 0.f;
    bool have_prev = false;
    for (int i = swap_sample + 1; i <= swap_sample + 80; ++i) {
        p.process(l, r);
        if (l == 0.f) continue;            // ambiguous fade-zero sample near the swap; skip
        if (have_prev && ((prev < 0.f) != (l < 0.f))) ++crossings;
        prev = l;
        have_prev = true;
    }
    // >=1 already rules out the 220 Hz power-on default from a phase-0
    // start over 80 samples (it cannot complete a half cycle); >=2 leaves
    // margin.
    CHECK(crossings >= 2);
}

TEST_CASE("part: targets reach the engine on the 96-sample raster") {
    // The quantized pitch is recomputed at the control tick and held between
    // ticks. pitch_cv() reads _pitch_q, so it must be a staircase with 96-
    // sample treads -- not a fresh value every sample.
    Part p;
    p.init(48000.f, 3u);
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_target_depth(LANE_PITCH, 1.f);
    p.set_depth(1.f);
    p.quant().set_mode(QuantMode::Free);    // no grid, so any change shows
    p.mod().set_range(1.f);
    p.mod().set_smooth(0.f);
    p.mod().set_rate(0.9f);

    // Pin both the interval and the phase: every sample where pitch_cv()
    // changes must be either a raster tick (index % kCtrlInterval == 0) or a
    // sample where the PITCH lane fired (the event refresh legitimately
    // updates off-raster). This is what actually ties Part's counter to
    // SynthEngine's -- a bare change-count bound would pass under a
    // regressed raster interval too.
    float l, r;
    int changes = 0;
    bool any_change = false;
    float prev = p.pitch_cv();
    for (int i = 0; i < 96 * 20; ++i) {
        p.process(l, r);
        const float now = p.pitch_cv();
        if (now != prev) {
            ++changes;
            any_change = true;
            const bool on_raster = (i % SynthEngine::kCtrlInterval) == 0;
            const bool on_fire = p.lane_fired(LANE_PITCH);
            CHECK((on_raster || on_fire));
        }
        prev = now;
    }
    CHECK(any_change);
    // Finding 6: the phase check above (on_raster || on_fire) passes just as
    // well under a raster that fires on every 2nd or 3rd tick (192- or
    // 288-sample interval -- still a multiple of kCtrlInterval), i.e. a
    // regression that makes the raster coarser stays green with only that
    // check. Pin a lower bound too. This scenario observes 9 changes (all
    // of them on-raster; PITCH never fires here) over the 20 ticks this loop
    // renders; 6 leaves comfortable margin below that while still well
    // above what a 192- or 288-sample raster would produce over the same
    // window (10 and ~7 tick opportunities respectively, each hit at
    // roughly the same rate as here).
    CHECK(changes >= 6);
}

TEST_CASE("part: LEVEL reaches the engine (set_targets push) only on the raster") {
    // No public accessor exposes the raster-held _tg cache directly:
    // target_value(LANE_LEVEL) and fx_target_value() both recompute live and
    // bypass it entirely (part.cpp:55-65), and SynthEngine/PartFx keep the
    // pushed value and its own smoother private. TestToneEngine is the one
    // place the push is externally visible at all: set_targets() there
    // writes _amp = t[LANE_LEVEL] with no smoothing (test_tone_engine.h),
    // unlike SynthEngine's 10 ms level smoother or PartFx's 2 ms smoothers,
    // so its audio output is a direct, undistorted readout of whatever was
    // last pushed.
    //
    // Rather than reverse-engineering that amplitude out of the sine (fragile
    // -- tried it, the algebra is right but nails down bit-exactness only
    // asymptotically near the peak), run a second, independent TestToneEngine
    // ("shadow") fed the LIVE value every sample instead of Part's
    // raster-held cache, phase-locked to Part's real engine by starting both
    // from the same first active sample. If Part's push were live too, the
    // two outputs would always agree; because it is raster-held, they must
    // agree exactly AT a tick (both just received the identical fresh value)
    // and are free to disagree between ticks (the live target has moved on
    // but Part's cache has not).
    Part p;
    p.init(48000.f, 7u);
    p.fx().set_comp(0.f);   // 0 = bit-exact bypass (comp.h); the only always-on FX block

    // Land the engine-switch activation exactly on a raster tick. This is
    // STILL needed after Finding 3's fix, just for a different reason than
    // before: that fix re-arms Part::_ctrl_ctr to 0 in the swap block, so
    // TestToneEngine gets the commanded target on its very first active
    // sample regardless of where the swap lands (the cold-start staleness
    // this comment used to describe is gone). But re-arming the counter
    // also re-phases Part's raster from then on to (swap sample, swap
    // sample + kCtrlInterval, ...) -- harmless in general, but this test's
    // `i % kCtrlInterval == 0` checks below assume the raster is still on
    // Part's original 0, kCtrlInterval, 2*kCtrlInterval, ... grid from
    // construction. Landing the swap ON that grid keeps the re-phase a
    // no-op. The SoftSwitch fade-out is a fixed 191-sample countdown from
    // the sample set_engine() effectively starts it; one warm-up
    // process() call first (still on the boot engine, otherwise inert)
    // shifts that countdown by one sample, from absolute sample 191 to
    // 192 -- a multiple of kCtrlInterval.
    float warm_l, warm_r;
    p.process(warm_l, warm_r);
    p.set_engine(ENGINE_TEST_TONE);

    p.set_target_active(LANE_PITCH, false);
    p.set_target_base(LANE_PITCH, 1.f);       // freq = 110 * 8^1 = 880 Hz, fixed
    p.quant().set_mode(QuantMode::Free);       // passthrough: no slew to settle

    p.set_target_active(LANE_LEVEL, true);
    p.set_target_base(LANE_LEVEL, 0.8f);
    p.set_target_depth(LANE_LEVEL, 1.f);
    p.set_depth(1.f);
    p.mod().set_range(1.f);
    p.mod().set_smooth(0.f);
    p.mod().set_rate(1.f);                     // fast LFO: visible movement inside 96 samples

    TestToneEngine shadow;
    shadow.init(48000.f);
    float shadow_tg[LANE_COUNT] = { 0.f, 0.f, 0.f, 0.f, 0.f };
    shadow_tg[LANE_PITCH] = 1.f;                // matches Part's fixed PITCH target exactly

    float l, r;
    int  since_switch = 0;
    int  first_active_abs_i = -1;
    bool tick_mismatch = false;
    bool off_tick_divergence = false;

    for (int i = 0; i < 96 * 30; ++i) {
        const int abs_i = i + 1;                // +1 for the warm-up sample above
        p.process(l, r);
        const bool tone_active = (p.engine_id() == ENGINE_TEST_TONE);
        if (!tone_active) continue;             // shadow starts the instant Part's real tone does
        if (first_active_abs_i < 0) first_active_abs_i = abs_i;

        const float live = p.target_raw(LANE_LEVEL);   // same call _control_tick() makes internally
        shadow_tg[LANE_LEVEL] = live;
        float sl, sr;
        shadow.set_targets(shadow_tg, 0.5f);
        shadow.process(sl, sr);

        ++since_switch;
        if (since_switch <= 300) continue;      // let the 4 ms engine-switch fade settle

        const bool at_tick = (abs_i % SynthEngine::kCtrlInterval) == 0;
        const float d = std::fabs(l - sl);
        if (at_tick) {
            if (d > 1e-4f) tick_mismatch = true;
        } else if (d > 0.02f) {
            off_tick_divergence = true;
        }
    }

    // Finding 5: assert the precondition the warm-up above exists to
    // establish, rather than leaving it an unstated coincidence of the fade
    // length and the warm-up count. If SoftSwitch's fade length or the Hann
    // table size ever changes, this fails here, by name, instead of showing
    // up downstream as an opaque tick_mismatch that points at the raster.
    REQUIRE(first_active_abs_i >= 0);
    REQUIRE((first_active_abs_i % SynthEngine::kCtrlInterval) == 0);

    // At every raster tick Part just latched the same live value the shadow
    // is fed every sample, so the two engines must agree there.
    CHECK_FALSE(tick_mismatch);
    // Task 4 (spec 2026-07-19 mod-plane-control-rate) moved LANE_LEVEL --
    // a texture lane -- onto the same 96-sample tick() raster as Part's own
    // push cache, and the two rasters are phase-locked. So target_raw()
    // itself no longer moves between ticks for a texture lane: the "live"
    // shadow input now changes in lockstep with Part's cache, not every
    // sample. The old sanity check (the streams must pull apart somewhere
    // off-tick, or the tick-agreement check above would be vacuous) no
    // longer holds for a texture lane and is expected to invert -- there is
    // no off-tick divergence left to find.
    CHECK_FALSE(off_tick_divergence);
}
