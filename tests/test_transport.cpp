#include <doctest/doctest.h>
#include <cmath>
#include <limits>
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

TEST_CASE("transport: set_bpm rejects non-positive and non-finite values") {
    // Guards the source that feeds nearest_division()/division_hz() (COUPLE's
    // grid gravity) and the transport's own beat_phase()/beats() readers: a
    // scenario-file `bpm: 0` (host/render/scenario.cpp forwards it
    // unvalidated) must not reach a divide and produce a non-finite grid.
    // The last good tempo is kept rather than clamped to an arbitrary floor
    // -- 0/negative/NaN/Inf are bad input, not a real tempo.
    Transport t;
    t.init(500.f);
    t.set_bpm(140.f);
    CHECK(t.bpm() == doctest::Approx(140.f));

    t.set_bpm(0.f);
    CHECK(t.bpm() == doctest::Approx(140.f));

    t.set_bpm(-10.f);
    CHECK(t.bpm() == doctest::Approx(140.f));

    t.set_bpm(std::numeric_limits<float>::quiet_NaN());
    CHECK(t.bpm() == doctest::Approx(140.f));

    t.set_bpm(std::numeric_limits<float>::infinity());
    CHECK(t.bpm() == doctest::Approx(140.f));

    // A subsequent genuinely valid tempo still applies normally.
    t.set_bpm(90.f);
    CHECK(t.bpm() == doctest::Approx(90.f));
}
