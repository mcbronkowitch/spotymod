#include <doctest/doctest.h>
#include <cstdio>
#include <fstream>
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
            {"action":"set_sync_mode","part":0,"value":"free"},
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
    Event depth;  depth.action = "set_depth"; depth.part = 0; depth.value = 0.f;
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

    Event shim;    // global reverb action: no part, null-safe
    shim.action = "set_reverb_shimmer";
    shim.value = 0.5f;
    apply_event(inst, shim);

    float l = 0.f, r = 0.f;
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(l == l);
}
