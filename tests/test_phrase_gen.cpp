#include <doctest/doctest.h>
#include "mod/phrase_gen.h"
#include "mod/rng.h"
#include <cmath>
#include <initializer_list>

using namespace spky;

TEST_CASE("layout invariant holds for awkward step counts") {
    for (int steps : {5, 7, 11, 13, 32, 3, 1}) {
        for (int pi = 0; pi < (int)Principle::kCount; ++pi) {
            int n = steps > 32 ? 32 : steps;
            int k, L, r;
            pg_derive_sizing((Principle)pi, n, k, L, r);
            CHECK(k >= 1);
            CHECK(L >= 1);
            CHECK(r >= 0);
            CHECK(r < (L > 1 ? L : 2));   // tail shorter than a motif
            CHECK(k * L + r == n);        // the core invariant
        }
    }
}

TEST_CASE("metric weight: downbeat strongest, binary subdivision order") {
    CHECK(pg_metric_weight(0) == doctest::Approx(1.0f));
    CHECK(pg_metric_weight(8) > pg_metric_weight(4));
    CHECK(pg_metric_weight(4) > pg_metric_weight(2));
    CHECK(pg_metric_weight(2) > pg_metric_weight(1));
    CHECK(pg_metric_weight(1) > 0.0f);
}

TEST_CASE("contour walk is a line, not independent draws, and deterministic") {
    Rng a; a.seed(0xC0FFEE);
    Rng b; b.seed(0xC0FFEE);
    float wa[16], wb[16];
    pg_contour_walk(a, wa, 16, 0.f, 0.6f, 0.12f);
    pg_contour_walk(b, wb, 16, 0.f, 0.6f, 0.12f);
    float sum_absdiff = 0.f;
    for (int i = 0; i < 16; ++i) {
        CHECK(wa[i] == doctest::Approx(wb[i]));      // determinism
        CHECK(wa[i] >= -1.0f); CHECK(wa[i] <= 1.0f); // bounded
        if (i > 0) sum_absdiff += std::fabs(wa[i] - wa[i-1]);
    }
    CHECK((sum_absdiff / 15.f) < 0.4f);              // small mean step => a line
}

TEST_CASE("arrangement: TwoMotif repeats a motif; OneMotif is one motif") {
    uint8_t moti[32], uniti[32];
    int mc, uc;
    pg_build_arrangement(Principle::TwoMotif, 4, moti, uniti, mc, uc);
    // A A B A  => id 0 appears 3x, id 1 once
    int count0 = 0; for (int j = 0; j < 4; ++j) count0 += (moti[j] == 0);
    CHECK(count0 == 3);
    CHECK(mc == 2);
    pg_build_arrangement(Principle::OneMotif, 4, moti, uniti, mc, uc);
    for (int j = 0; j < 4; ++j) CHECK(moti[j] == 0);
    CHECK(mc == 1);
}
