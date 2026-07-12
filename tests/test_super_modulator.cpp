#include <doctest/doctest.h>
#include <cmath>
#include "mod/super_modulator.h"
using namespace spky;

TEST_CASE("super: lane rate ratios (x2, x1/2, x1, x3/4, x3/2)") {
    SuperModulator m;
    m.init(48000.f, 42);
    m.set_sync_mode(SyncMode::Free);
    m.set_rate(0.3f);
    m.process();                         // one sample -> each lane phase == its inc
    float pitch = m.lane_phase(LANE_PITCH);
    CHECK(m.lane_phase(LANE_SOURCE) == doctest::Approx(pitch * 2.00f));
    CHECK(m.lane_phase(LANE_SIZE)   == doctest::Approx(pitch * 0.50f));
    CHECK(m.lane_phase(LANE_MOTION) == doctest::Approx(pitch * 0.75f));
    CHECK(m.lane_phase(LANE_LEVEL)  == doctest::Approx(pitch * 1.50f));
}

TEST_CASE("super: SYNC rate follows tempo") {
    SuperModulator m;
    m.init(48000.f, 42);
    m.set_tempo_bpm(120.f);              // 2 beats/sec
    m.set_sync_mode(SyncMode::Sync);
    m.set_rate(0.625f);                  // index 5 -> 1 cycle per beat
    CHECK(m.master_hz() == doctest::Approx(2.0f));
}

TEST_CASE("super: triplet mode is 1.5x the straight sync rate") {
    SuperModulator m;
    m.init(48000.f, 42);
    m.set_tempo_bpm(120.f);
    m.set_sync_mode(SyncMode::SyncTriplet);
    m.set_rate(0.625f);
    CHECK(m.master_hz() == doctest::Approx(3.0f));
}

TEST_CASE("super: lanes are decorrelated (independent random streams)") {
    SuperModulator m;
    m.init(48000.f, 42);
    m.set_sync_mode(SyncMode::Free);
    m.set_rate(0.5f);
    m.set_shape(1.f);                    // S&H exercises each lane's own rng
    m.set_probability(1.f);
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
    m.set_sync_mode(SyncMode::Free); m.set_rate(0.5f);
    float base = m.base_hz();
    m.set_rate_scale(2.f);
    CHECK(m.base_hz()   == doctest::Approx(base));
    CHECK(m.master_hz() == doctest::Approx(base * 2.f));
}

TEST_CASE("super: rate_scale 1 is a bit-identical no-op") {
    SuperModulator a; a.init(48000.f, 42u); a.set_rate(0.5f);
    SuperModulator b; b.init(48000.f, 42u); b.set_rate(0.5f);
    b.set_rate_scale(1.f);
    bool same = true;
    for (int i = 0; i < 48000; ++i) {
        a.process(); b.process();
        for (int s = 0; s < LANE_COUNT; ++s)
            if (a.lane_output(s) != b.lane_output(s)) same = false;
    }
    CHECK(same);
}

TEST_CASE("super: sync_mode getter reflects the set mode") {
    SuperModulator m; m.init(48000.f, 42u);
    m.set_sync_mode(SyncMode::Sync);
    CHECK(m.sync_mode() == SyncMode::Sync);
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
