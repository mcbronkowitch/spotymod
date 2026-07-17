#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include "mod/super_modulator.h"
#include "mod/divisions.h"
using namespace spky;

TEST_CASE("tide: ladder is reciprocal-symmetric with x1 at centre") {
    CHECK(kTideCount == 9);
    CHECK(tide_index(0.5f) == 4);
    CHECK(kTideRatios[4] == 1.f);
    for (int i = 0; i < kTideCount; ++i)
        CHECK(kTideRatios[i] * kTideRatios[kTideCount - 1 - i]
              == doctest::Approx(1.f));
    CHECK(tide_free(0.5f) == 1.f);                    // exakt: 2^0 (IEEE-754)
    CHECK(tide_free(0.f) == doctest::Approx(0.25f));
    CHECK(tide_free(1.f) == doctest::Approx(4.f));
}

TEST_CASE("tide: 0.5 is a bit-identical no-op (free and synced)") {
    for (int synced = 0; synced <= 1; ++synced) {
        SuperModulator a; a.init(48000.f, 42u);
        SuperModulator b; b.init(48000.f, 42u);
        if (synced) {
            a.set_tempo_bpm(120.f); b.set_tempo_bpm(120.f);
            a.set_synced(true);     b.set_synced(true);
        }
        a.set_rate(0.5f); b.set_rate(0.5f);
        b.set_tide(0.5f);
        bool same = true;
        for (int i = 0; i < 48000; ++i) {
            a.process(); b.process();
            for (int s = 0; s < LANE_COUNT; ++s)
                if (a.lane_output(s) != b.lane_output(s)) same = false;
        }
        CHECK(same);
    }
}

TEST_CASE("tide: free scaling drives texture lanes only") {
    SuperModulator m; m.init(48000.f, 42u);
    m.set_rate(0.3f);
    const float base = m.master_hz();
    m.set_tide(1.f);                                  // frei: x4
    CHECK(m.master_hz() == doctest::Approx(base));    // Melodie-Clock steht
    m.process();                                      // 1 Sample: Phase == Inkrement
    float pitch = m.lane_phase(LANE_PITCH);
    CHECK(m.lane_phase(LANE_SOURCE) == doctest::Approx(pitch * 2.00f * 4.f));
    CHECK(m.lane_phase(LANE_SIZE)   == doctest::Approx(pitch * 0.50f * 4.f));
    CHECK(m.lane_phase(LANE_MOTION) == doctest::Approx(pitch * 0.75f * 4.f));
    CHECK(m.lane_phase(LANE_LEVEL)  == doctest::Approx(pitch * 1.50f * 4.f));
}

TEST_CASE("tide: synced snaps to the ratio ladder, free is continuous") {
    SuperModulator m; m.init(48000.f, 1u);
    m.set_tempo_bpm(120.f);
    m.set_tide(0.3f);                                 // frei: 2^(4*(-0.2))
    CHECK(m.tide_mult() == doctest::Approx(std::pow(2.f, -0.8f)));
    m.set_synced(true);                               // rechnet um: Index 2 = x1/2
    CHECK(m.tide_mult() == doctest::Approx(0.5f));
    m.set_tide(0.f);  CHECK(m.tide_mult() == doctest::Approx(0.25f));
    m.set_tide(1.f);  CHECK(m.tide_mult() == doctest::Approx(4.f));
    m.set_tide(0.5f); CHECK(m.tide_mult() == 1.f);
}

TEST_CASE("tide: composes with the center rate_scale") {
    SuperModulator m; m.init(48000.f, 42u);
    m.set_rate(0.3f);
    m.set_rate_scale(1.f, 2.f);                       // COUPLE/DRIFT-Hook
    m.set_tide(1.f);                                  // x4 obendrauf
    m.process();
    float pitch = m.lane_phase(LANE_PITCH);
    CHECK(m.lane_phase(LANE_SOURCE)
          == doctest::Approx(pitch * 2.f * 2.f * 4.f));
}
