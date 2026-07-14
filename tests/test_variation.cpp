#include <doctest/doctest.h>
#include "mod/lane.h"
#include <cmath>
#include <vector>

using namespace spky;

// Build a melodic (PITCH-style) STEP lane at pure S&H (shape 1.0).
// NOTE: `set_variation` is added in Task 7; until then this helper calls
// `set_entropy(0.f)`. Switch the one call to `set_variation` in Task 7.
static ModLane make_melodic_step_lane(uint32_t seed, int steps) {
    ModLane l;
    l.set_melodic(true);
    l.set_principle(Principle::TwoMotif);
    l.init(48000.f, seed);
    l.set_shape(1.0f);
    l.set_step(true, steps);
    l.set_rate_hz(8.0f);
    l.set_entropy(0.f);        // -> set_variation(0.f) in Task 7
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
