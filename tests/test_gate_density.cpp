#include <doctest/doctest.h>
#include "mod/lane.h"
#include <set>

using namespace spky;

static ModLane melodic_step(uint32_t seed, int steps) {
    ModLane l;
    l.set_melodic(true);
    l.set_principle(Principle::TwoMotif);
    l.init(48000.f, seed);           // variation defaults to 0 (LOOP)
    l.set_shape(1.0f);
    l.set_step(true, steps);
    l.set_rate_hz(2.0f);             // one cycle = 24000 samples
    return l;
}

// Which step indices fire over one full cycle. At rate 2 Hz / 16 steps a step
// is 1500 samples; l.phase() just after a fire identifies the entered step.
static std::set<int> fired_step_set(ModLane& l, int steps, int samples) {
    std::set<int> out;
    for (int n = 0; n < samples; ++n) {
        l.process();
        if (l.fired()) out.insert(static_cast<int>(l.phase() * steps) % steps);
    }
    return out;
}

TEST_CASE("DENSE is monotonic: raising density only adds notes to the groove") {
    const float densities[] = {0.05f, 0.3f, 0.6f, 1.0f};
    std::set<int> prev;
    for (int d = 0; d < 4; ++d) {
        ModLane l = melodic_step(0x11, 16);
        l.set_density(densities[d]);
        auto s = fired_step_set(l, 16, 24000);
        for (int step : prev) CHECK(s.count(step) == 1);  // superset of the sparser set
        CHECK(s.size() >= prev.size());
        prev = s;
    }
    CHECK(prev.size() == 16);        // density 1 -> every step fires
}

TEST_CASE("DENSE 0 leaves exactly the cell anchors") {
    ModLane l = melodic_step(0x11, 16);   // n=16 -> 2 instances of L=8
    l.set_density(0.f);
    auto s = fired_step_set(l, 16, 24000);
    CHECK(s == std::set<int>{0, 8});      // slot 0 of each instance (rank 0)
}

TEST_CASE("DENSE is reversible: density 1 == the full pattern") {
    ModLane a = melodic_step(0x11, 16);
    ModLane b = melodic_step(0x11, 16);
    b.set_density(0.2f);             // thin...
    b.set_density(1.0f);             // ...then restore (never edits the groove)
    CHECK(fired_step_set(a, 16, 24000) == fired_step_set(b, 16, 24000));
}

TEST_CASE("FLOW never freezes after PROBABILITY removal") {
    ModLane l;
    l.set_melodic(true);
    l.init(48000.f, 0x22);
    l.set_shape(0.5f);
    l.set_step(false, 8);            // FLOW: no per-step gate => no freeze source
    l.set_rate_hz(3.0f);
    bool ever_frozen = false;
    for (int n = 0; n < 48000; ++n) { l.process(); ever_frozen |= l.frozen(); }
    CHECK_FALSE(ever_frozen);
}
