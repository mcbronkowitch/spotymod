#include <doctest/doctest.h>
#include "fx/fx_util.h"
using namespace spky;

TEST_CASE("xfade: stage 0 is exactly lhs, stage 1 is exactly rhs") {
    XFade x;
    float o0, o1;
    x.SetStage(0.f);
    x.Process(0.25f, -0.5f, 0.9f, 0.9f, o0, o1);
    CHECK(o0 == 0.25f);
    CHECK(o1 == -0.5f);
    x.SetStage(1.f);
    x.Process(0.25f, -0.5f, 0.9f, 0.7f, o0, o1);
    CHECK(o0 == 0.9f);
    CHECK(o1 == 0.7f);
}

TEST_CASE("softswitch: rises to 1 within ~4 ms, falls back to idle") {
    SoftSwitch s;
    s.init(48000.f);
    CHECK(s.process() == 0.f);
    CHECK(s.is_idle());
    s.set_on(true);
    for (int i = 0; i < 300; ++i) s.process();   // 4 ms = 192 samples
    CHECK(s.process() == 1.f);
    CHECK(!s.is_idle());
    s.set_on(false);
    for (int i = 0; i < 300; ++i) s.process();
    CHECK(s.process() == 0.f);
    CHECK(s.is_idle());
}

TEST_CASE("softswitch: immediate flag jumps straight to hold") {
    SoftSwitch s;
    s.init(48000.f);
    s.set_on(true, true);
    CHECK(s.process() == 1.f);
}
