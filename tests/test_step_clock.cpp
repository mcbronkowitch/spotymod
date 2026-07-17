#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "mod/lane.h"
using namespace spky;

// Spec 2026-07-17 step-clock: in STEP mode RATE is a step clock with an
// 8-step reference — cycle_hz = rate_hz * 8 / steps. Step duration depends
// only on RATE, never on the step count; 8 steps is bit-identical to the
// old pattern-clock behavior.

static ModLane step_lane(int steps, float hz) {
    ModLane l;
    l.init(48000.f, 7);
    l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
    l.set_step(true, steps);
    l.set_rate_hz(hz);
    return l;
}

static std::vector<int> fire_samples(ModLane& l, int samples) {
    std::vector<int> out;
    for (int i = 0; i < samples; ++i) { l.process(); if (l.fired()) out.push_back(i); }
    return out;
}

TEST_CASE("step-clock: 8 steps is the reference — step = 6000 samples at 1 Hz") {
    ModLane l = step_lane(8, 1.f);
    // boundaries every 6000 samples: 8 fires in [0, 47000)
    CHECK(fire_samples(l, 47000).size() == 8);
}

TEST_CASE("step-clock: step duration is independent of the step count") {
    // same RATE => same fires-per-second, whatever STEPS says
    for (int steps : {2, 8, 14, 16}) {
        ModLane l = step_lane(steps, 1.f);
        CHECK(fire_samples(l, 47000).size() == 8);   // one fire per 6000 samples
    }
}

TEST_CASE("step-clock: 8 vs 14 steps stay boundary-aligned (polymeter)") {
    ModLane a = step_lane(8, 1.f);
    ModLane b = step_lane(14, 1.f);
    auto fa = fire_samples(a, 47000);
    auto fb = fire_samples(b, 47000);
    REQUIRE(fa.size() == fb.size());
    for (size_t i = 0; i < fa.size(); ++i)
        CHECK(std::abs(fa[i] - fb[i]) <= 256);   // float-phasor drift only
}

TEST_CASE("step-clock: live STEPS grow 8->16 keeps position and timing") {
    ModLane l = step_lane(8, 1.f);
    for (int i = 0; i < 3000; ++i) l.process();   // mid step 0
    float pos = l.phase() * 8.f;                  // step index + fraction
    l.set_step(true, 16);
    CHECK(l.phase() == doctest::Approx(pos / 16.f).epsilon(0.001));  // rescaled, not jumped
    l.process();
    CHECK_FALSE(l.fired());                       // no ghost boundary on the switch
    int to_fire = 1;
    while (to_fire < 20000) { l.process(); ++to_fire; if (l.fired()) break; }
    CHECK(to_fire > 2900); CHECK(to_fire < 3100); // next boundary still ~sample 6000
    CHECK(static_cast<int>(l.phase() * 16.f) == 1);   // ...and it is step 1
}

TEST_CASE("step-clock: live STEPS shrink 16->8 wraps the index, keeps the grid") {
    ModLane l = step_lane(16, 1.f);
    for (int i = 0; i < 61000; ++i) l.process();  // step 10 of 16 (step = 6000 samples)
    float pos = std::fmod(l.phase() * 16.f, 8.f); // 10.x -> 2.x
    l.set_step(true, 8);
    CHECK(l.phase() == doctest::Approx(pos / 8.f).epsilon(0.001));
    l.process();
    CHECK_FALSE(l.fired());
    int to_fire = 1;
    while (to_fire < 20000) { l.process(); ++to_fire; if (l.fired()) break; }
    // grid unbroken: next fire lands where step 11 of 16 would have (~5000 on)
    CHECK(to_fire > 4700); CHECK(to_fire < 5300);
    CHECK(static_cast<int>(l.phase() * 8.f) == 3);
}

TEST_CASE("step-clock: FLOW rate is untouched by the step count") {
    ModLane a; a.init(48000.f, 7); a.set_rate_hz(2.f); a.set_step(false, 3);
    ModLane b; b.init(48000.f, 7); b.set_rate_hz(2.f); b.set_step(false, 16);
    int fa = 0, fb = 0;
    for (int i = 0; i < 240000; ++i) {            // 5 s
        a.process(); if (a.fired()) ++fa;
        b.process(); if (b.fired()) ++fb;
    }
    CHECK(fa == fb);                              // same seed, same inc: exact
    CHECK(fa >= 9); CHECK(fa <= 11);              // ~2 wraps/s
}
