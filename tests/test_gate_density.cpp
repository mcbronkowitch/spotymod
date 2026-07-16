#include <doctest/doctest.h>
#include "mod/lane.h"
#include <set>
#include <vector>

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

TEST_CASE("gate releases before the next note when the gap is long") {
    ModLane l = melodic_step(0x11, 16);
    l.set_density(0.f);                    // notes at steps 0 and 8 only (L=8)
    const int step_samples = 1500;         // 24000-sample cycle / 16 steps
    std::vector<char> gate;
    for (int n = 0; n < 24000; ++n) { l.process(); gate.push_back(l.gate_state()); }
    CHECK(gate[10]);                       // note sounding just after the downbeat
    // note_len is capped at 4 < the 8-step gap, so the gate MUST fall in between
    CHECK_FALSE(gate[7 * step_samples + 10]);
    int run = 0;                           // high run from the downbeat: 1..4 steps
    while (run < 24000 && gate[run]) ++run;
    CHECK(run >= 1 * step_samples - 2);
    CHECK(run <= 4 * step_samples + 2);
}

TEST_CASE("adjacent notes tie: gate high across a run of gated steps") {
    ModLane l = melodic_step(0x11, 16);
    l.set_density(1.f);                    // every step fires -> continuous tie
    l.process();                           // enter step 0 (first fire)
    bool always_high = true;
    for (int n = 1; n < 24000; ++n) { l.process(); always_high &= l.gate_state(); }
    CHECK(always_high);
}

TEST_CASE("gate can sustain across a frozen (rest) step") {
    // k = 7 of 8: one rest slot per cell. Whenever the note before it has
    // note_len >= 2 the gate bridges the rest (high while frozen). Statistical
    // over seeds: bridging must be common (P(len>=2) = 0.45 per phrase).
    int bridged = 0;
    for (uint32_t seed = 1; seed <= 40; ++seed) {
        ModLane l = melodic_step(seed * 31u, 16);
        l.set_density(0.9f);               // k=7 of 8
        bool high_while_frozen = false;
        for (int n = 0; n < 24000; ++n) {
            l.process();
            if (l.frozen() && l.gate_state()) high_while_frozen = true;
        }
        if (high_while_frozen) ++bridged;
    }
    CHECK(bridged >= 8);                   // expected ~18/40
}
