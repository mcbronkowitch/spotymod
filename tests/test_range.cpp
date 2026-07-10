#include <doctest/doctest.h>
#include "mod/range.h"
using namespace spky;

TEST_CASE("range: minimum is off") {
    CHECK(apply_range( 1.f, 0.f) == doctest::Approx(0.f));
    CHECK(apply_range(-1.f, 0.f) == doctest::Approx(0.f));
    CHECK(apply_range( 0.f, 0.f) == doctest::Approx(0.f));
}

TEST_CASE("range: full unipolar at mid travel") {
    CHECK(apply_range( 1.f, 0.5f) == doctest::Approx(1.f));
    CHECK(apply_range(-1.f, 0.5f) == doctest::Approx(0.f));
    CHECK(apply_range( 0.f, 0.5f) == doctest::Approx(0.5f));
}

TEST_CASE("range: full bipolar at maximum") {
    CHECK(apply_range( 1.f, 1.f) == doctest::Approx( 1.f));
    CHECK(apply_range(-1.f, 1.f) == doctest::Approx(-1.f));
    CHECK(apply_range( 0.f, 1.f) == doctest::Approx( 0.f));
}

TEST_CASE("range: monotonic in v for every fixed r") {
    for (int ri = 0; ri <= 10; ++ri) {
        float r = ri / 10.f;
        float prev = apply_range(-1.f, r);
        for (int vi = -9; vi <= 10; ++vi) {
            float cur = apply_range(vi / 10.f, r);
            CHECK(cur >= prev - 1e-5f);
            prev = cur;
        }
    }
}
