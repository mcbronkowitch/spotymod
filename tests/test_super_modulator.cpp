#include <doctest/doctest.h>
#include <cmath>
#include <string>
#include "mod/super_modulator.h"
#include "mod/divisions.h"
using namespace spky;

TEST_CASE("super: lane rate ratios (x2, x1/2, x1, x3/4, x3/2)") {
    SuperModulator m;
    m.init(48000.f, 42);
    m.set_rate(0.3f);
    // Texture lanes advance on the 96-sample raster (Task 4, spec
    // 2026-07-19 mod-plane-control-rate): read the ratio on that grid, not
    // after one sample -- 96 process() calls give the pitch lane the same
    // elapsed time as the texture lanes' single tick().
    for (int i = 0; i < ModLane::kTickInterval; ++i) m.process();
    float pitch = m.lane_phase(LANE_PITCH);
    CHECK(m.lane_phase(LANE_SOURCE) == doctest::Approx(pitch * 2.00f));
    CHECK(m.lane_phase(LANE_SIZE)   == doctest::Approx(pitch * 0.50f));
    CHECK(m.lane_phase(LANE_MOTION) == doctest::Approx(pitch * 0.75f));
    CHECK(m.lane_phase(LANE_LEVEL)  == doctest::Approx(pitch * 1.50f));
}

TEST_CASE("super: synced rate snaps to the division ladder") {
    SuperModulator m;
    m.init(48000.f, 1u);
    m.set_tempo_bpm(120.f);
    m.set_synced(true);
    m.set_rate(0.5f);                                  // center detent = 1/4
    CHECK(m.base_hz() == doctest::Approx(2.f));        // 1 cpb at 120 bpm
    m.set_rate(0.52f);                                 // still inside the same detent
    CHECK(m.base_hz() == doctest::Approx(2.f));
    m.set_rate(0.f);
    CHECK(m.base_hz() == doctest::Approx(0.0625f));    // 8 bars
    m.set_rate(1.f);
    CHECK(m.base_hz() == doctest::Approx(16.f));       // 1/32 = 8 cpb
}

TEST_CASE("super: triplet rungs live on the ladder") {
    SuperModulator m;
    m.init(48000.f, 1u);
    m.set_tempo_bpm(120.f);
    m.set_synced(true);
    m.set_rate(13.f / 16.f);                           // index 13 = 1/8T
    CHECK(m.base_hz() == doctest::Approx(6.f));
    CHECK(std::string(kDivisions[m.division()].name) == "1/8T");
}

TEST_CASE("super: synced getter and free default") {
    SuperModulator m;
    m.init(48000.f, 1u);
    CHECK_FALSE(m.synced());
    m.set_synced(true);
    CHECK(m.synced());
}

TEST_CASE("super: split rate scale drives pitch and mod lanes separately") {
    SuperModulator m;
    m.init(48000.f, 1u);
    m.set_rate(0.5f);
    const float base = m.base_hz();
    m.set_rate_scale(1.f, 2.f);
    CHECK(m.pitch_scale() == doctest::Approx(1.f));
    CHECK(m.mod_scale()   == doctest::Approx(2.f));
    CHECK(m.master_hz()   == doctest::Approx(base));   // master follows the pitch lane
    m.set_rate_scale(0.5f, 1.f);
    CHECK(m.master_hz()   == doctest::Approx(base * 0.5f));
}

TEST_CASE("super: lanes are decorrelated (independent random streams)") {
    SuperModulator m;
    m.init(48000.f, 42);
    m.set_rate(0.5f);
    m.set_shape(1.f);                    // S&H exercises each lane's own rng
    m.set_range(1.f);
    bool differ = false;
    for (int i = 0; i < 48000; ++i) {
        m.process();
        if (std::fabs(m.lane_output(LANE_PITCH) - m.lane_output(LANE_SOURCE)) > 0.05f)
            differ = true;
    }
    CHECK(differ);
}

TEST_CASE("super: rate_scale multiplies the master rate; base_hz stays put") {
    SuperModulator m; m.init(48000.f, 42u);
    m.set_rate(0.5f);
    float base = m.base_hz();
    m.set_rate_scale(2.f, 2.f);
    CHECK(m.base_hz()   == doctest::Approx(base));
    CHECK(m.master_hz() == doctest::Approx(base * 2.f));
}

TEST_CASE("super: rate_scale 1 is a bit-identical no-op") {
    SuperModulator a; a.init(48000.f, 42u); a.set_rate(0.5f);
    SuperModulator b; b.init(48000.f, 42u); b.set_rate(0.5f);
    b.set_rate_scale(1.f, 1.f);
    bool same = true;
    for (int i = 0; i < 48000; ++i) {
        a.process(); b.process();
        for (int s = 0; s < LANE_COUNT; ++s)
            if (a.lane_output(s) != b.lane_output(s)) same = false;
    }
    CHECK(same);
}

TEST_CASE("super: spot stumbles every lane except the PITCH master lane") {
    SuperModulator a; a.init(48000.f, 1u);
    float before[LANE_COUNT];
    for (int i = 0; i < LANE_COUNT; ++i) before[i] = a.lane_phase(i);
    Rng rng; rng.seed(77u);
    a.spot(rng);
    // PITCH is the anchor everything else stumbles around — SPOT leaves it alone.
    CHECK(a.lane_phase(LANE_PITCH) == doctest::Approx(before[LANE_PITCH]));
    int moved = 0;
    for (int i = 0; i < LANE_COUNT; ++i)
        if (i != LANE_PITCH && std::fabs(a.lane_phase(i) - before[i]) > 1e-6f) ++moved;
    CHECK(moved >= 3);   // the other four lanes stumble

    // determinism: same seed -> same kicks
    SuperModulator x; x.init(48000.f, 1u);
    SuperModulator y; y.init(48000.f, 1u);
    Rng rx; rx.seed(5u); Rng ry; ry.seed(5u);
    x.spot(rx); y.spot(ry);
    CHECK(x.lane_phase(LANE_SOURCE) == y.lane_phase(LANE_SOURCE));
}

TEST_CASE("super: texture lanes hold between control ticks, pitch stays per-sample") {
    SuperModulator m;
    m.init(48000.f, 42u);
    m.set_rate(0.6f);
    m.set_shape(0.3f);          // continuous FLOW: per-sample path would move
    m.set_smooth(0.f);
    m.process();                // counter boots at 0: the first call ticks
    float held[LANE_COUNT];
    for (int s = 0; s < LANE_COUNT; ++s) held[s] = m.lane_output(s);
    bool stair_ok = true;
    for (int i = 1; i < 96 * 20; ++i) {
        m.process();
        for (int s = 0; s < LANE_COUNT; ++s) {
            if (s == LANE_PITCH) continue;
            if (i % 96 == 0) held[s] = m.lane_output(s);
            else if (m.lane_output(s) != held[s]) stair_ok = false;
        }
    }
    CHECK(stair_ok);            // texture = 96-sample staircase by construction
}
