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

TEST_CASE("regenerate_unit: regenerates its unit, keeps siblings identical, other units untouched") {
    Rng r; r.seed(0x1234);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::TwoMotif, r, 32, p, g, m, L); // A A B A, units {0,1}
    float u0_before[8]; for (int i = 0; i < 8; ++i) u0_before[i] = p[i];     // unit 0 (motif A)
    float b_id1[8];     for (int i = 0; i < 8; ++i) b_id1[i] = p[16 + i];    // unit 1 (motif B)
    Rng r2; r2.seed(0x55);
    regenerate_unit(Principle::TwoMotif, r2, L, m, /*unit=*/0, p, g);
    bool u0_changed = false;                                  // regeneration actually ran
    for (int i = 0; i < 8; ++i) if (std::fabs(p[i] - u0_before[i]) > 1e-6f) u0_changed = true;
    CHECK(u0_changed);
    for (int i = 0; i < 8; ++i) {                             // all id-0 siblings still identical
        CHECK(p[i] == doctest::Approx(p[8 + i]));
        CHECK(p[i] == doctest::Approx(p[24 + i]));
    }
    for (int i = 0; i < 8; ++i) CHECK(p[16 + i] == doctest::Approx(b_id1[i])); // unit 1 untouched
}

TEST_CASE("regenerate_unit: CallResponse pair regenerates, answer still resolves") {
    Rng r; r.seed(0x99);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::CallResponse, r, 16, p, g, m, L); // 1 unit (Q&A pair)
    float before[16]; for (int i = 0; i < 16; ++i) before[i] = p[i];
    Rng r2; r2.seed(0x2);
    regenerate_unit(Principle::CallResponse, r2, L, m, 0, p, g);
    bool changed = false;                                       // pair body actually changed
    for (int i = 0; i < 15; ++i) if (std::fabs(p[i] - before[i]) > 1e-6f) changed = true;
    CHECK(changed);
    CHECK(p[15] == doctest::Approx(0.0f));                      // answer still resolves to root
}

TEST_CASE("regenerate_unit gates match generate_phrase at the same slots") {
    Rng r; r.seed(0x321);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::TwoMotif, r, 32, p, g, m, L);
    float u1_before[8]; for (int i = 0; i < 8; ++i) u1_before[i] = p[16 + i];
    bool  gbefore[8];   for (int i = 0; i < 8; ++i) gbefore[i] = g[16 + i];
    Rng r2; r2.seed(0x321 ^ 0xABC);
    regenerate_unit(Principle::TwoMotif, r2, L, m, 1, p, g);    // motif B (unit 1)
    bool changed = false;                                       // regeneration actually ran
    for (int i = 0; i < 8; ++i) if (std::fabs(p[16 + i] - u1_before[i]) > 1e-6f) changed = true;
    CHECK(changed);
    for (int i = 0; i < 8; ++i) CHECK(g[16 + i] == gbefore[i]); // gate pattern still position-stable
}

TEST_CASE("groove: deterministic per seed, valid permutation, anchor rank 0") {
    for (uint32_t seed : {1u, 0xBEEFu, 0xC0FFEEu}) {
        Rng a; a.seed(seed);
        Rng b; b.seed(seed);
        GrooveCell ga, gb;
        pg_gen_groove(a, 8, ga);
        pg_gen_groove(b, 8, gb);
        bool seen[8] = {};
        for (int i = 0; i < 8; ++i) {
            CHECK(ga.rank_of_slot[i] == gb.rank_of_slot[i]);   // determinism
            CHECK(ga.note_len[i] == gb.note_len[i]);
            REQUIRE(ga.rank_of_slot[i] < 8);
            seen[ga.rank_of_slot[i]] = true;
        }
        for (int i = 0; i < 8; ++i) CHECK(seen[i]);            // permutation
        CHECK(ga.rank_of_slot[0] == 0);                        // anchor is always first
        CHECK(ga.len == 8);
    }
}

TEST_CASE("groove: note lengths in [1,4], biased short") {
    float sum = 0.f; int count = 0;
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        Rng r; r.seed(seed * 2654435761u);
        GrooveCell g;
        pg_gen_groove(r, 8, g);
        for (int i = 0; i < 8; ++i) {
            REQUIRE(g.note_len[i] >= 1);
            REQUIRE(g.note_len[i] <= 4);
            sum += g.note_len[i]; ++count;
        }
    }
    CHECK(sum / count < 2.2f);   // mean ~1.7: short notes common, sustains rare
}

TEST_CASE("groove: syncopation occurs — off-beats reach the top ranks") {
    int synced = 0;
    const int kSeeds = 400;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng r; r.seed(seed * 0x9E3779B9u);
        GrooveCell g;
        pg_gen_groove(r, 8, g);
        for (int i = 1; i < 8; i += 2)                 // any odd slot in the top half?
            if (g.rank_of_slot[i] < 4) { ++synced; break; }
    }
    CHECK(synced > kSeeds / 4);   // pushes are a real, common feature
}

TEST_CASE("groove: L=1 degenerates cleanly") {
    Rng r; r.seed(3);
    GrooveCell g;
    pg_gen_groove(r, 1, g);
    CHECK(g.len == 1);
    CHECK(g.rank_of_slot[0] == 0);
}
