#include <doctest/doctest.h>
#include "mod/lane.h"
using namespace spky;

TEST_CASE("lane STEP: fires once per step") {
    ModLane l;
    l.init(48000.f, 7);
    l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
    l.set_step(true, 4);
    l.set_rate_hz(1.f);
    int fires = 0;
    // Count over LESS than one cycle (47000 < ~48000) so the free-running float
    // phasor does not wrap and re-enter step 0 with a spurious 5th fire.
    for (int i = 0; i < 47000; ++i) { l.process(); if (l.fired()) ++fires; }
    CHECK(fires == 4);
}

TEST_CASE("lane STEP: target held constant within a step") {
    ModLane l;
    l.init(48000.f, 7);
    l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
    l.set_step(true, 4);      // step 0 spans samples [0, 12000)
    l.set_rate_hz(1.f);
    for (int i = 0; i < 3000; ++i) l.process();
    float a = l.target();
    for (int i = 0; i < 5000; ++i) l.process();   // still step 0 (~sample 8000)
    float b = l.target();
    CHECK(a == doctest::Approx(b));
}

TEST_CASE("lane STEP: fixed slew ignores the SMOOTH knob") {
    // Panel switch 3 middle = STEP + fixed slew: the glide time must be constant
    // regardless of SMOOTH. Sample the output a fixed offset past a step boundary
    // for two very different SMOOTH settings; with fixed slew they must match.
    auto glide_after_boundary = [](float smooth) {
        ModLane l;
        l.init(48000.f, 7);
        l.set_range(1.f); l.set_shape(0.5f);
        l.set_step(true, 2);
        l.set_fixed_slew(true);        // engage fixed slew BEFORE SMOOTH
        l.set_smooth(smooth);          // must be ignored while fixed slew is on
        l.set_rate_hz(1.f);            // step 0 spans [0,24000); boundary at ~24000
        for (int i = 0; i < 24100; ++i) l.process();
        return l.process();            // ~100 samples past the step-1 boundary
    };
    CHECK(glide_after_boundary(0.0f) == doctest::Approx(glide_after_boundary(1.0f)).epsilon(0.001));
}
