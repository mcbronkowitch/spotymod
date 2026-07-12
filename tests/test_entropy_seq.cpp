#include <doctest/doctest.h>
#include <algorithm>
#include <vector>
#include "mod/lane.h"
using namespace spky;

// Latched per-step values: run the lane, record target() on every fired step.
static std::vector<float> fired_targets(ModLane& l, int n_samples) {
    std::vector<float> v;
    for (int i = 0; i < n_samples; ++i) {
        l.process();
        if (l.fired()) v.push_back(l.target());
    }
    return v;
}

static ModLane make_sh_step_lane(uint32_t seed, float entropy, float prob, int steps) {
    ModLane l;
    l.init(48000.f, seed);
    l.set_range(1.f); l.set_smooth(0.f);
    l.set_shape(1.f);            // pure S&H end of SHAPE
    l.set_step(true, steps);
    l.set_probability(prob);
    l.set_entropy(entropy);
    l.set_rate_hz(1.f);          // ~48000 samples per cycle
    return l;
}

TEST_CASE("ENTROPY 0: STEP + S&H loops its melody exactly, cycle after cycle") {
    auto l = make_sh_step_lane(42, 0.f, 1.f, 8);
    auto v = fired_targets(l, 48000 * 4 + 24000);        // > 4 cycles
    REQUIRE(v.size() >= 32);
    for (size_t i = 0; i + 8 < 32; ++i) CHECK(v[i] == v[i + 8]);   // exact equality
}

TEST_CASE("melody from the first cycle: buffer pre-filled, deterministic per seed") {
    auto a = make_sh_step_lane(7, 0.f, 1.f, 8);
    auto b = make_sh_step_lane(7, 0.f, 1.f, 8);
    auto va = fired_targets(a, 47000);                   // < one cycle: exactly 8 fires
    auto vb = fired_targets(b, 47000);
    REQUIRE(va.size() == 8);
    bool all_equal = true;
    for (float x : va) if (x != va[0]) all_equal = false;
    CHECK(!all_equal);            // a melody, not one repeated note
    CHECK(va == vb);              // identical seeds -> identical melody
}

TEST_CASE("ENTROPY 0: FLOW + S&H holds one loop-stable value across cycles") {
    ModLane l;
    l.init(48000.f, 42);
    l.set_range(1.f); l.set_smooth(0.f);
    l.set_shape(1.f);
    l.set_step(false, 8);
    l.set_probability(1.f);
    l.set_entropy(0.f);
    l.set_rate_hz(4.f);           // several cycles within one second
    l.process();
    float first = l.target();
    bool constant = true;
    for (int i = 0; i < 48000; ++i) {
        l.process();
        if (l.target() != first) constant = false;
    }
    CHECK(constant);              // no per-cycle redraw anymore
}
