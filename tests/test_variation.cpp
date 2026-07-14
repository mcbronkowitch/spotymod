#include <doctest/doctest.h>
#include "mod/lane.h"
#include <cmath>
#include <vector>

using namespace spky;

// Build a melodic (PITCH-style) STEP lane at pure S&H (shape 1.0).
static ModLane make_melodic_step_lane(uint32_t seed, int steps) {
    ModLane l;
    l.set_melodic(true);
    l.set_principle(Principle::TwoMotif);
    l.init(48000.f, seed);
    l.set_shape(1.0f);
    l.set_step(true, steps);
    l.set_rate_hz(8.0f);
    l.set_variation(0.f);
    return l;
}

// Drive the lane and return raw process() outputs.
static std::vector<float> drive(ModLane& l, int samples) {
    std::vector<float> out;
    for (int n = 0; n < samples; ++n) out.push_back(l.process());
    return out;
}

// Collect fired-note target() values grouped by cycle, split on phase wrap
// (phase() decreasing) so grouping is exact regardless of sample timing.
static std::vector<std::vector<float>> collect_cycles(ModLane& l, int cycles) {
    std::vector<std::vector<float>> out(1);
    float prev = l.phase();
    int wraps = 0;
    for (int n = 0; n < 300000 && wraps <= cycles; ++n) {
        l.process();
        float ph = l.phase();
        if (ph < prev) { out.emplace_back(); ++wraps; } // new cycle starts here
        prev = ph;
        if (l.fired()) out.back().push_back(l.target());
    }
    return out;
}

TEST_CASE("melodic init is deterministic per seed") {
    ModLane a = make_melodic_step_lane(0xABCDEF, 32);
    ModLane b = make_melodic_step_lane(0xABCDEF, 32);
    auto oa = drive(a, 12000), ob = drive(b, 12000);
    REQUIRE(oa.size() == ob.size());
    for (size_t i = 0; i < oa.size(); ++i) CHECK(oa[i] == ob[i]); // bit-identical
}

TEST_CASE("distinct seeds give distinct melodies") {
    ModLane a = make_melodic_step_lane(0x111, 32);
    ModLane b = make_melodic_step_lane(0x222, 32);
    auto oa = drive(a, 12000), ob = drive(b, 12000);
    bool differ = false;
    for (size_t i = 0; i < oa.size(); ++i) if (oa[i] != ob[i]) differ = true;
    CHECK(differ);
}

// variation-parameterised builder layered on the file's base helper.
static ModLane melodic_var(uint32_t seed, int steps, float variation) {
    ModLane l = make_melodic_step_lane(seed, steps);
    l.set_variation(variation);
    return l;
}

TEST_CASE("variation 0: consecutive cycles identical (LOOP), pitch and rhythm") {
    ModLane l = melodic_var(0x100, 16, 0.f);
    auto cy = collect_cycles(l, 4);
    REQUIRE(cy.size() >= 4);
    // compare two mid cycles (avoid the init edge); identical at LOOP.
    REQUIRE(cy[1].size() == cy[2].size());
    for (size_t i = 0; i < cy[1].size(); ++i) CHECK(cy[1][i] == doctest::Approx(cy[2][i]));
}

TEST_CASE("GROW varies pitch within a cycle but keeps the same gate rhythm") {
    ModLane loop = melodic_var(0x200, 16, 0.0f);
    ModLane grow = melodic_var(0x200, 16, 0.8f);
    auto cl = collect_cycles(loop, 2);
    auto cg = collect_cycles(grow, 2);
    // First full cycle (index 1): ev_rate is still ~0 so timing is identical =>
    // same number of notes fire (gates untouched by GROW).
    REQUIRE(cl.size() >= 2); REQUIRE(cg.size() >= 2);
    CHECK(cl[1].size() == cg[1].size());
    bool pitch_changed = false;
    for (size_t i = 0; i < cl[1].size(); ++i)
        if (std::fabs(cl[1][i] - cg[1][i]) > 0.01f) pitch_changed = true;
    CHECK(pitch_changed); // pitch drifted where GROW mutated fired slots
}

TEST_CASE("RENEW at -1 replaces units every cycle; still coherent") {
    ModLane l = melodic_var(0x300, 16, -1.0f);
    auto cy = collect_cycles(l, 4);
    REQUIRE(cy.size() >= 4);
    REQUIRE(cy[1].size() >= 4); REQUIRE(cy[2].size() >= 4);
    bool changed = false;
    size_t m = std::min(cy[1].size(), cy[2].size());
    for (size_t i = 0; i < m; ++i) if (std::fabs(cy[1][i] - cy[2][i]) > 0.01f) changed = true;
    CHECK(changed); // a new phrase per cycle at sustained -1
}

TEST_CASE("determinism: identical drive -> identical output across GROW/RENEW/density") {
    auto run = [](uint32_t seed) {
        ModLane l;
        l.set_melodic(true);
        l.set_principle(Principle::TwoMotif);
        l.init(48000.f, seed);
        l.set_shape(1.0f);
        l.set_step(true, 16);
        l.set_rate_hz(8.0f);
        std::vector<float> out;
        for (int n = 0; n < 60000; ++n) {
            if (n == 5000)  l.set_variation(0.7f);
            if (n == 15000) l.set_density(0.3f);
            if (n == 25000) l.set_variation(-0.8f);
            if (n == 35000) { l.set_principle(Principle::CallResponse); l.new_phrase(); }
            if (n == 45000) l.set_density(1.0f);
            out.push_back(l.process());
        }
        return out;
    };
    auto a = run(0xDECAF); auto b = run(0xDECAF);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]); // bit-identical
}
