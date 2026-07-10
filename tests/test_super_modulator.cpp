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
