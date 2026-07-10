#include <doctest/doctest.h>
#include <vector>
#include <cmath>
#include "mod/lane.h"
using namespace spky;

static std::vector<float> run_lane(float evolve, uint32_t seed, int n) {
    ModLane l;
    l.init(48000.f, seed);
    l.set_range(1.f);
    l.set_shape(0.25f);       // triangle: shape-sensitive, S&H unused
    l.set_smooth(0.f);
    l.set_probability(1.f);
    l.set_evolve(evolve);
    l.set_rate_hz(1.f);       // ~48000 samples per cycle
    std::vector<float> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) out.push_back(l.process());
    return out;
}

// LOOP determinism is bit-exact reproducibility: same seed -> identical stream
// every run. (A cycle-vs-cycle comparison would drift ~2.6e-3 because a
// free-running float phasor does not close a cycle in exactly 48000 samples.)
TEST_CASE("lane LOOP: deterministic and reproducible (evolve = 0)") {
    auto a = run_lane(0.f, 2024, 96000);
    auto b = run_lane(0.f, 2024, 96000);
    bool identical = true;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) identical = false;
    CHECK(identical);
}

TEST_CASE("lane EVOLVE: still deterministic, but wanders away from the loop") {
    auto loop = run_lane(0.f, 2024, 96000);
    auto ev1  = run_lane(1.f, 2024, 96000);
    auto ev2  = run_lane(1.f, 2024, 96000);

    bool ev_reproducible = true;
    for (size_t i = 0; i < ev1.size(); ++i) if (ev1[i] != ev2[i]) ev_reproducible = false;
    CHECK(ev_reproducible);                          // deterministic even while evolving

    float drift = 0.f;                               // but departs from the non-evolving loop
    for (size_t i = 48000; i < ev1.size(); ++i) drift += std::fabs(ev1[i] - loop[i]);
    CHECK(drift > 1.f);
}

// EVOLVE wanders shape, phase AND rate (spec). Pin the rate axis: under EVOLVE
// the samples between fires (the cycle length) must vary noticeably.
TEST_CASE("lane EVOLVE: cycle length wanders (rate wander)") {
    ModLane l;
    l.init(48000.f, 2024);
    l.set_range(1.f); l.set_shape(0.25f); l.set_smooth(0.f);
    l.set_probability(1.f); l.set_evolve(1.f); l.set_rate_hz(1.f);
    int last_fire = -1;
    std::vector<int> gaps;
    for (int i = 0; i < 48000 * 30; ++i) {
        l.process();
        if (l.fired()) { if (last_fire >= 0) gaps.push_back(i - last_fire); last_fire = i; }
    }
    REQUIRE(gaps.size() >= 5);
    int mn = gaps[0], mx = gaps[0];
    for (int g : gaps) { if (g < mn) mn = g; if (g > mx) mx = g; }
    CHECK(mx - mn > 100);                             // cycle length varies (rate wander)
}
