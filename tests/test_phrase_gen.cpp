#include <doctest/doctest.h>
#include "mod/phrase_gen.h"
#include "mod/rng.h"
#include <cmath>
#include <initializer_list>
#include <algorithm>

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

TEST_CASE("generate_phrase: TwoMotif shows motivic repetition, deterministic") {
    Rng a; a.seed(0xBEEF);
    Rng b; b.seed(0xBEEF);
    float pa[32], pb[32]; bool ga[32], gb[32]; uint8_t ma[32], mb[32];
    PhraseLayout la, lb;
    generate_phrase(Principle::TwoMotif, a, 32, pa, ga, ma, la);
    generate_phrase(Principle::TwoMotif, b, 32, pb, gb, mb, lb);
    CHECK(la.inst_count == 4);
    CHECK(la.motif_len == 8);
    // A A B A: slots 0-7 == 8-7 == 24-31 (id 0), 16-23 differ (id 1)
    for (int i = 0; i < 8; ++i) {
        CHECK(pa[i] == doctest::Approx(pa[8 + i]));
        CHECK(pa[i] == doctest::Approx(pa[24 + i]));
    }
    bool any_diff = false;
    for (int i = 0; i < 8; ++i) if (pa[i] != pa[16 + i]) any_diff = true;
    CHECK(any_diff);
    for (int i = 0; i < 32; ++i) CHECK(pa[i] == doctest::Approx(pb[i])); // determinism
    // has at least one rest somewhere (gate layer active)
    bool any_rest = false; for (int i = 0; i < 32; ++i) any_rest |= !ga[i];
    CHECK(any_rest);
}

TEST_CASE("generate_phrase: OneMotif repeats one identical motif") {
    Rng r; r.seed(7);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::OneMotif, r, 32, p, g, m, L);
    for (int i = 0; i < 8; ++i) {
        CHECK(p[i] == doctest::Approx(p[8 + i]));
        CHECK(p[i] == doctest::Approx(p[16 + i]));
        CHECK(p[i] == doctest::Approx(p[24 + i]));
    }
}

TEST_CASE("generate_phrase: Ostinato is near-static pitch, dense gate") {
    Rng r; r.seed(11);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::Ostinato, r, 32, p, g, m, L);
    float mn = 2.f, mx = -2.f;
    int on = 0;
    for (int i = 0; i < 32; ++i) { mn = std::min(mn, p[i]); mx = std::max(mx, p[i]); on += g[i]; }
    CHECK((mx - mn) < 0.35f);   // near-static
    CHECK(on >= 16);            // dense, at least half on
}

TEST_CASE("generate_phrase: CallResponse answer resolves to root") {
    Rng r; r.seed(13);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::CallResponse, r, 16, p, g, m, L); // k=2, L=8
    CHECK(p[15] == doctest::Approx(0.0f)); // answer's last slot lands on root
}

TEST_CASE("generate_phrase: Hierarchical tiles a cell inside each motif") {
    Rng r; r.seed(17);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::Hierarchical, r, 16, p, g, m, L); // k=2, L=8, cell=4
    CHECK(p[0] == doctest::Approx(p[4]));  // cell repeats within instance 0
    CHECK(p[1] == doctest::Approx(p[5]));
}
