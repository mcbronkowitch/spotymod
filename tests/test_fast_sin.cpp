#include <doctest/doctest.h>
#include <cmath>
#include "util/fast_sin.h"
#include "util/math.h"
using namespace spky;

TEST_CASE("fast_sin: exact at the quadrant anchors") {
    CHECK(fast_sin(0.f)   == doctest::Approx(0.f));
    CHECK(fast_sin(0.25f) == doctest::Approx(1.f));
    CHECK(fast_sin(0.5f)  == doctest::Approx(0.f));
    CHECK(fast_sin(0.75f) == doctest::Approx(-1.f));
    CHECK(fast_sin(1.f)   == doctest::Approx(0.f));      // wraps
    CHECK(fast_sin(1.25f) == doctest::Approx(1.f));      // wraps
    CHECK(fast_sin(-0.75f) == doctest::Approx(1.f));     // negative phase wraps
}

TEST_CASE("fast_sin: bounded error vs std::sin across many cycles") {
    float max_err = 0.f;
    for (int i = 0; i <= 100000; ++i) {
        float ph = -3.f + i * (9.f / 100000.f);          // [-3, 6): includes wraps
        float ref = static_cast<float>(std::sin(static_cast<double>(ph) * 6.283185307179586));
        float err = std::fabs(fast_sin(ph) - ref);
        if (err > max_err) max_err = err;
    }
    CHECK(max_err < 2e-3f);
}

TEST_CASE("fast_sin: output never leaves [-1, 1] (beyond float fuzz)") {
    for (int i = 0; i <= 20000; ++i) {
        float v = fast_sin(i / 20000.f);
        CHECK(v >= -1.0001f);
        CHECK(v <=  1.0001f);
    }
}
