#include <doctest/doctest.h>
#include "mod/waveforms.h"
using namespace spky;

TEST_CASE("waveforms: canonical shapes at bank anchor points") {
    CHECK(shape_value(0.25f, 0.00f, 0.f) == doctest::Approx(1.0f));   // sine peak
    CHECK(shape_value(0.50f, 0.25f, 0.f) == doctest::Approx(1.0f));   // triangle peak
    CHECK(shape_value(0.50f, 0.50f, 0.f) == doctest::Approx(0.0f));   // ramp mid
    CHECK(shape_value(0.25f, 0.75f, 0.f) == doctest::Approx(1.0f));   // pulse high
    CHECK(shape_value(0.75f, 0.75f, 0.f) == doctest::Approx(-1.0f));  // pulse low
}

TEST_CASE("waveforms: S&H end returns the held random value") {
    CHECK(shape_value(0.1f, 1.0f,  0.42f) == doctest::Approx( 0.42f).epsilon(0.01));
    CHECK(shape_value(0.9f, 1.0f, -0.31f) == doctest::Approx(-0.31f).epsilon(0.01));
}

TEST_CASE("waveforms: output stays bipolar for all shapes") {
    for (int i = 0; i <= 20; ++i) {
        float ph = i / 20.f;
        for (int s = 0; s <= 10; ++s) {
            float v = shape_value(ph, s / 10.f, 0.5f);
            CHECK(v >= -1.001f);
            CHECK(v <=  1.001f);
        }
    }
}
