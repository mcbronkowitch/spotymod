#include <doctest/doctest.h>
#include <cmath>
#include "mod/lane.h"
using namespace spky;

static void configure_flow(ModLane& l, float hz, float prob = 1.f) {
    l.init(48000.f, 1234);
    l.set_range(1.f);
    l.set_shape(0.5f);        // ramp
    l.set_smooth(0.f);
    l.set_probability(prob);
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

TEST_CASE("lane FLOW: probability 0 freezes after the first cycle") {
    ModLane l; configure_flow(l, 4.f, 0.f);
    for (int i = 0; i < 48000; ++i) l.process();   // >= one full cycle
    CHECK(l.frozen());
}

TEST_CASE("lane: SMOOTH turns a step into a glide") {
    ModLane l;
    l.init(48000.f, 55);
    l.set_range(1.f);
    l.set_shape(0.5f);        // ramp: consecutive step values differ
    l.set_step(true, 2);      // 2 steps/cycle; boundary at phase 0.5
    l.set_probability(1.f);
    l.set_smooth(0.5f);       // glide ~3 ms: settles well within a step, still gliding ~1 ms past a boundary
    l.set_rate_hz(1.f);       // 48000 samples/cycle -> step is 24000 samples

    for (int i = 0; i < 20000; ++i) l.process();   // settle in step 0
    float settled0 = l.process();
    float target0  = l.target();
    for (int i = 20002; i < 24050; ++i) l.process(); // cross into step 1
    float out_after = l.process();                   // ~1 ms past boundary
    float target1   = l.target();

    CHECK(target1 != doctest::Approx(target0));        // new value latched
    CHECK(std::fabs(out_after - target1) > 0.01f);     // output still gliding
    CHECK(std::fabs(settled0  - target0) < 0.01f);     // was settled before
}
