#include <doctest/doctest.h>
#include <cmath>
#include "mod/lane.h"
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
    l.set_step(true, 2);      // 2 steps/cycle; boundary at phase 0.5
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
