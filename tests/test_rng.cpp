#include <doctest/doctest.h>
#include "mod/rng.h"
using namespace spky;

TEST_CASE("rng: deterministic for a given seed") {
    Rng a, b;
    a.seed(12345);
    b.seed(12345);
    for (int i = 0; i < 1000; ++i) CHECK(a.next_u32() == b.next_u32());
}

TEST_CASE("rng: different seeds diverge") {
    Rng a, b;
    a.seed(1); b.seed(2);
    bool differ = false;
    for (int i = 0; i < 10; ++i) if (a.next_u32() != b.next_u32()) differ = true;
    CHECK(differ);
}

TEST_CASE("rng: unipolar in [0,1) and roughly uniform") {
    Rng r; r.seed(99);
    for (int i = 0; i < 100000; ++i) {
        float u = r.next_unipolar();
        CHECK(u >= 0.f);
        CHECK(u < 1.f);
    }
    Rng r2; r2.seed(7);
    double sum = 0; int n = 200000;
    for (int i = 0; i < n; ++i) sum += r2.next_unipolar();
    CHECK(sum / n == doctest::Approx(0.5).epsilon(0.02));
}
