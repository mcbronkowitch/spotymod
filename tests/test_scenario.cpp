#include <doctest/doctest.h>
#include <cstdio>
#include <fstream>
#include <vector>
#include "render/scenario.h"
using namespace spky;

TEST_CASE("scenario: parses init + timeline and sorts events by time") {
    const char* path = "test_scenario.json";
    {
        std::ofstream o(path);
        o << R"({
          "sample_rate": 48000,
          "bpm": 100,
          "duration_s": 5,
          "init": [
            {"action":"set_sync","ivalue":1},
            {"action":"set_rate","part":0,"value":0.5}
          ],
          "events": [
            {"t":3.0,"action":"set_probability","part":0,"value":0.2},
            {"t":1.0,"action":"set_step","part":0,"flag":true,"ivalue":16}
          ]
        })";
    }
    Scenario s;
    std::string err;
    REQUIRE(load_scenario(path, s, err));
    CHECK(s.sample_rate == 48000);
    CHECK(s.bpm == doctest::Approx(100.f));
    CHECK(s.duration_s == doctest::Approx(5.0));
    CHECK(s.init_events.size() == 2);
    REQUIRE(s.events.size() == 2);
    CHECK(s.events[0].time_s == doctest::Approx(1.0));   // sorted ascending
    CHECK(s.events[0].action == "set_step");
    CHECK(s.events[0].ivalue == 16);
    CHECK(s.events[1].action == "set_probability");

    Instrument inst;
    inst.init(48000.f);
    for (const auto& e : s.init_events) apply_event(inst, e);   // must not crash
    for (const auto& e : s.events)      apply_event(inst, e);
    std::remove(path);
}

TEST_CASE("scenario: quantizer actions reach the instrument") {
    Instrument inst;
    inst.init(48000.f);
    Event depth;  depth.action = "set_target_depth"; depth.part = 0;
    depth.slot = LANE_PITCH; depth.value = 0.f;
    apply_event(inst, depth);
    Event base;   base.action = "set_target_base"; base.part = 0;
    base.slot = LANE_PITCH; base.value = 0.5f;
    apply_event(inst, base);

    float l = 0.f, r = 0.f;
    for (int i = 0; i < 4000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.pitch_cv(0) == doctest::Approx(17.f / 36.f));   // boot dorian

    Event scale;  scale.action = "set_scale"; scale.svalue = "whole";
    apply_event(inst, scale);
    for (int i = 0; i < 4000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.pitch_cv(0) == doctest::Approx(18.f / 36.f));   // whole tone

    Event mode;   mode.action = "set_quant_mode"; mode.part = 0; mode.svalue = "free";
    apply_event(inst, mode);
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.pitch_cv(0) == doctest::Approx(0.5f));          // raw passthrough
}

TEST_CASE("scenario: fx actions reach the instrument") {
    Instrument inst;
    inst.init(48000.f);

    Event base;
    base.action = "set_fx_target_base";
    base.part = 0;
    base.slot = FXT_FLUX_TIME;
    base.value = 0.8f;
    apply_event(inst, base);
    CHECK(inst.fx_target_value(0, FXT_FLUX_TIME) == doctest::Approx(0.8f));

    Event on;      // must not crash even without FX memory
    on.action = "set_fx_on";
    on.part = 0;
    on.svalue = "flux";
    on.flag = true;
    apply_event(inst, on);

    Event mode;
    mode.action = "set_grit_mode";
    mode.part = 1;
    mode.svalue = "reduce";
    apply_event(inst, mode);

    Event dec;     // global reverb action: no part, null-safe
    dec.action = "set_reverb_decay";
    dec.value = 0.5f;
    apply_event(inst, dec);

    Event mix;     // global reverb action: no part, null-safe
    mix.action = "set_reverb_mix";
    mix.value = 0.3f;
    apply_event(inst, mix);

    Event dif;     // global reverb action: no part, null-safe
    dif.action = "set_reverb_diffusion";
    dif.value = 0.7f;
    apply_event(inst, dif);

    float l = 0.f, r = 0.f;
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(l == l);
}

// DENSE 0 leaves only the downbeat/anchor slot able to fire, so after the
// guaranteed first-sample fire (STEP entry: step -1 -> 0) the next natural
// note is a full cycle away. Settle past that single note's decay before
// checking silence, so the manual trigger is the only voice left.
TEST_CASE("scenario: M2 synth actions reach the instrument") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_voice_decay(0, 0.f);            // shortest decay: fast test
    inst.set_step(0, true, 8);
    inst.set_density(0, 0.f);                // anchor-only: next natural fire is a cycle away
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 10000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.active_voices(0) == 0);       // silent before the tap

    Event trig;                              // trigger_manual is observable
    trig.action = "trigger_manual";
    trig.part = 0;
    apply_event(inst, trig);
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.active_voices(0) == 1);

    Event eng;                               // set_engine is observable
    eng.action = "set_engine";
    eng.part = 0;
    eng.svalue = "test_tone";
    apply_event(inst, eng);
    for (int i = 0; i < 1000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.engine_id(0) == ENGINE_TEST_TONE);
    CHECK(inst.active_voices(0) == 0);

    // the five voice-parameter actions dispatch without crashing (their
    // audible effect is pinned by the engine/env unit tests)
    const char* voice_actions[] = { "set_voice_attack", "set_voice_decay",
                                    "set_voice_resonance", "set_voice_sub",
                                    "set_voice_detune" };
    for (const char* a : voice_actions) {
        Event e;
        e.action = a;
        e.part = 0;
        e.value = 0.5f;
        apply_event(inst, e);
    }
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(l == l);                           // not NaN
}

TEST_CASE("scenario: center actions dispatch to the instrument") {
    Instrument inst; inst.init(48000.f);

    Event ec; ec.action = "set_couple"; ec.value = 0.7f;
    apply_event(inst, ec);
    CHECK(inst.couple() == doctest::Approx(0.7f));     // couple is not smoothed

    Event ed; ed.action = "set_drift"; ed.value = 0.4f;
    apply_event(inst, ed);
    std::vector<float> l(1), r(1);
    for (int i = 0; i < 48000; ++i) inst.process(nullptr, nullptr, l.data(), r.data(), 1);
    CHECK(inst.drift() == doctest::Approx(0.4f).epsilon(0.05));   // smoothed toward target

    Event es; es.action = "spot";   apply_event(inst, es);   // must not crash
    Event et; et.action = "settle"; apply_event(inst, et);
    CHECK(true);
}

TEST_CASE("scenario: set_comp and set_master_drive dispatch without throwing") {
    Instrument inst;
    inst.init(48000.f);
    Event e;
    e.action = "set_comp";
    e.part = 1;
    e.value = 0.8f;
    apply_event(inst, e);
    e.action = "set_master_drive";
    e.value = 0.5f;
    apply_event(inst, e);
    // No getters exist by design (matches set_reverb_*); reaching here alive
    // plus the Task 3/4 integration tests covers the wiring.
    CHECK(true);
}

TEST_CASE("scenario: set_color dispatches to the chord layer") {
    Instrument inst;
    inst.init(48000.f);
    Event e;
    e.action = "set_color";
    e.part = 0;
    e.value = 0.5f;
    apply_event(inst, e);          // dispatch smoke test: the voice-count check does NOT discriminate the
    // wiring (density-0 anchor fires alone reach 3 voices); the real evidence
    // is the chord_bloom render with-vs-without set_color (task-6 report)
    inst.set_density(0, 0.f);
    float outL[64], outR[64];
    for (int i = 0; i < 3000; ++i) inst.process(nullptr, nullptr, outL, outR, 64);
    CHECK(inst.active_voices(0) >= 3);
}

TEST_CASE("scenario: the sampler control actions reach the engine") {
    // Setup: the task-5 brief sketches an `Instrument::Config` rig that does
    // not exist in this repo. Follow the InstRig pattern already proven in
    // tests/test_sampler_part.cpp instead -- FxMem with injected per-part
    // sampler memory, then Instrument::init(sample_rate, mem).
    std::vector<float> echo[PART_COUNT][2];
    std::vector<SampleBuffer::Frame> sbuf[PART_COUNT];
    AmbientReverb reverb;
    FxMem mem;
    for (int p = 0; p < PART_COUNT; ++p) {
        for (int c = 0; c < 2; ++c) {
            echo[p][c].assign(Flux::kMaxSamples, 0.f);
            mem.echo[p][c] = echo[p][c].data();
        }
        sbuf[p].assign(48000, SampleBuffer::Frame{ 0.f, 0.f });
        mem.sampler_buf[p] = sbuf[p].data();
    }
    mem.sampler_frames = 48000;
    mem.reverb = &reverb;

    Instrument inst;
    inst.init(48000.f, mem);
    inst.set_engine(0, ENGINE_SAMPLER);
    // MOTION boots active (Part::_active) and adds its own swing on top of
    // the overlap knob (Part::_control_tick's omod, +/-kOverlapMod = 0.2).
    // Switch it off so the knob-only value is what lands on overlap_eff --
    // otherwise even a knob of exactly 0.0 rests wherever MOTION's LFO
    // happens to be, which is not what this test is checking.
    inst.set_target_active(0, LANE_MOTION, false);
    float in = 0.f, l = 0.f, r = 0.f;
    for (int i = 0; i < 960; ++i) inst.process(&in, &in, &l, &r, 1);   // click-free swap settle

    Event e;
    e.part = 0;

    e.action = "sampler_overlap"; e.value = 0.f;
    apply_event(inst, e);
    // Part stores the knob; the effective value reaches the engine on the
    // next control tick (Part::_control_tick, 96-sample raster), so drive
    // samples past that before reading it back. Default overlap_eff is 1.0
    // (Part::_overlap_eff init), so landing on 0.0 proves the action arrived.
    for (int i = 0; i < 200; ++i) inst.process(&in, &in, &l, &r, 1);
    CHECK(inst.sampler_overlap_eff(0) == doctest::Approx(0.f));

    // sampler_scan needs recorded content: SamplerEngine::_update_control
    // forces scan_pos() to 0 whenever rec_size() == 0, so proving the
    // playhead actually moves requires audio in the buffer first.
    in = 0.5f;
    inst.sampler_record(0, true);
    for (int i = 0; i < 24000; ++i) inst.process(&in, &in, &l, &r, 1);
    inst.sampler_record(0, false);
    in = 0.f;
    for (int i = 0; i < 960; ++i) inst.process(&in, &in, &l, &r, 1);
    REQUIRE(inst.sampler_fill(0) > 0.4f);

    e.action = "sampler_scan"; e.value = -1.f;
    apply_event(inst, e);
    for (int i = 0; i < 4800; ++i) inst.process(&in, &in, &l, &r, 1);
    // Reverse from a home position folds upward, so any motion at all
    // proves the action landed.
    const float pos_before_punch = inst.sampler_scan_pos(0);
    CHECK(pos_before_punch != 0.f);

    e.action = "sampler_punch";
    apply_event(inst, e);
    // punch() writes _scan_pos = 0.f immediately (not on the next control
    // tick), so the value it left behind was demonstrably non-zero above --
    // this isn't just reading a head that was already parked at home.
    CHECK(inst.sampler_scan_pos(0) == 0.f);
}
