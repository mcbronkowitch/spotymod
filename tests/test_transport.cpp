#include <doctest/doctest.h>
#include <cmath>
#include "center/transport.h"
using namespace spky;

TEST_CASE("transport: beats advance at bpm/60 per second of control ticks") {
    Transport t;
    t.init(500.f);            // Center's control rate at 48 kHz
    t.set_bpm(120.f);         // 2 beats per second
    for (int i = 0; i < 500; ++i) t.tick();
    CHECK(t.beats() == doctest::Approx(2.0).epsilon(1e-6));
    CHECK(t.beat_phase() == doctest::Approx(0.f).epsilon(1e-4));
}

TEST_CASE("transport: clock_pulse snaps the phase to the nearest beat") {
    Transport t;
    t.init(500.f);
    t.set_bpm(120.f);
    for (int i = 0; i < 540; ++i) t.tick();   // 2.16 beats
    t.clock_pulse();
    CHECK(t.beats() == doctest::Approx(2.0));
    for (int i = 0; i < 210; ++i) t.tick();   // 2.84 beats
    t.clock_pulse();
    CHECK(t.beats() == doctest::Approx(3.0)); // rounds up too
}

TEST_CASE("transport: reset zeroes the downbeat") {
    Transport t;
    t.init(500.f);
    t.set_bpm(97.f);
    for (int i = 0; i < 1234; ++i) t.tick();
    t.reset();
    CHECK(t.beats() == doctest::Approx(0.0));
    CHECK(t.beat_phase() == doctest::Approx(0.f));
}
