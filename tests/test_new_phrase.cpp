#include <doctest/doctest.h>
#include "mod/lane.h"
#include <vector>
#include <cmath>

using namespace spky;

static ModLane melodic(uint32_t seed, int steps) {
    ModLane l;
    l.set_melodic(true);
    l.set_principle(Principle::TwoMotif);
    l.init(48000.f, seed);
    l.set_shape(1.0f);
    l.set_step(true, steps);
    l.set_variation(0.f);
    l.set_rate_hz(8.0f);
    return l;
}

// Fired targets grouped by cycle, split on phase-wrap events.
static std::vector<std::vector<float>> cycles(ModLane& l, int n) {
    std::vector<std::vector<float>> out(1);
    float prev = l.phase();
    int wraps = 0;
    for (int s = 0; s < 300000 && wraps <= n; ++s) {
        l.process();
        float ph = l.phase();
        if (ph < prev) { out.emplace_back(); ++wraps; }
        prev = ph;
        if (l.fired()) out.back().push_back(l.target());
    }
    return out;
}

TEST_CASE("new_phrase regenerates at a cycle wrap, then loops") {
    ModLane l = melodic(0x400, 16);
    auto before = cycles(l, 2);          // establish the pre-regen phrase
    l.new_phrase();                      // pending; applies at the next wrap
    auto after = cycles(l, 3);
    REQUIRE(before.size() >= 2); REQUIRE(after.size() >= 3);
    // once regenerated, consecutive cycles are stable (LOOP)
    REQUIRE(after[1].size() == after[2].size());
    for (size_t i = 0; i < after[1].size(); ++i)
        CHECK(after[1][i] == doctest::Approx(after[2][i]));
    // and the phrase actually changed vs before the gesture
    bool changed = before[1].size() != after[2].size();
    if (!changed) for (size_t i = 0; i < before[1].size(); ++i)
        if (std::fabs(before[1][i] - after[2][i]) > 0.01f) changed = true;
    CHECK(changed);
}

TEST_CASE("set_step with unchanged effective length does not regen") {
    ModLane l = melodic(0x500, 16);
    auto c1 = cycles(l, 2);
    l.set_step(true, 16);                // same count -> no regen
    auto c2 = cycles(l, 2);
    REQUIRE(c1.size() >= 2); REQUIRE(c2.size() >= 2);
    REQUIRE(c1[1].size() == c2[1].size());
    for (size_t i = 0; i < c1[1].size(); ++i) CHECK(c1[1][i] == doctest::Approx(c2[1][i]));
}

TEST_CASE("moves within steps>=32 do not regenerate the phrase (buffer survives)") {
    // A round-trip 33 -> 40 -> 33 keeps effective n=32 the whole time, so no
    // regen ever fires; the 33-step phrase must equal a lane that stayed at 33.
    ModLane ref = melodic(0x600, 33);
    ModLane l   = melodic(0x600, 33);
    auto cref = cycles(ref, 3);
    l.set_step(true, 40);        // n stays 32 -> no regen
    (void)cycles(l, 1);          // let a wrap pass
    l.set_step(true, 33);        // still n=32 -> no regen
    auto cl = cycles(l, 3);
    REQUIRE(cref.size() >= 3); REQUIRE(cl.size() >= 3);
    REQUIRE(cref[2].size() == cl[2].size());
    for (size_t i = 0; i < cref[2].size(); ++i) CHECK(cref[2][i] == doctest::Approx(cl[2][i]));
}

TEST_CASE("melodic step count above 32 wraps into the buffer, not clamped") {
    // Same seed => same 32-slot buffer, but 40 step positions replay slots 0..7,
    // producing MORE onsets per cycle than 32 steps. If _steps were clamped to 32,
    // the two would be identical.
    ModLane a = melodic(0x700, 32);
    ModLane b = melodic(0x700, 40);
    auto ca = cycles(a, 2);
    auto cb = cycles(b, 2);
    REQUIRE(ca.size() >= 2); REQUIRE(cb.size() >= 2);
    CHECK(cb[1].size() > ca[1].size());  // 40 steps => extra onsets from wrapped slots 0..7
}
