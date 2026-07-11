#include <doctest/doctest.h>
#include "synth/env.h"
using namespace spky;

TEST_CASE("env: idle until triggered; attack reaches 1.0 in the programmed time") {
    Env e;
    e.init(48000.f);
    e.set_times(0.1f, 0.5f);
    CHECK(!e.active());
    CHECK(e.process() == 0.f);
    e.trigger();
    int n = 0;
    while (e.value() < 1.f && n < 48000) { e.process(); ++n; }
    CHECK(n >= static_cast<int>(0.1f * 48000 * 0.85f));   // ~4800 samples +/- 15%
    CHECK(n <= static_cast<int>(0.1f * 48000 * 1.15f));
}

TEST_CASE("env: AD decays to exact zero and goes idle (-80 dB cutoff)") {
    Env e;
    e.init(48000.f);
    e.set_times(0.002f, 0.5f);          // decay_s = time to -60 dB
    e.trigger();
    int n = 0;
    while (e.active() && n < 48000 * 3) { e.process(); ++n; }
    CHECK(!e.active());
    CHECK(e.process() == 0.f);          // exact zero once idle
    // idle threshold is -80 dB -> ~1.33 x the -60 dB decay time
    CHECK(n > static_cast<int>(0.5f * 48000));
    CHECK(n < static_cast<int>(0.5f * 48000 * 1.7f));
}

TEST_CASE("env: sustain holds near 0.7 (ADS); set_sustain(0) releases to zero") {
    Env e;
    e.init(48000.f);
    e.set_times(0.002f, 0.2f);
    e.set_sustain(0.7f);
    e.trigger();
    for (int i = 0; i < 48000; ++i) e.process();          // well past attack + decay
    CHECK(e.active());
    CHECK(e.value() == doctest::Approx(0.7f).epsilon(0.02));
    e.set_sustain(0.f);                                    // FLOW demotion
    int n = 0;
    while (e.active() && n < 48000 * 2) { e.process(); ++n; }
    CHECK(!e.active());
}

TEST_CASE("env: retrigger restarts the attack from the current level (no jump)") {
    Env e;
    e.init(48000.f);
    e.set_times(0.05f, 0.3f);
    e.trigger();
    for (int i = 0; i < 12000; ++i) e.process();           // into the decay tail, still above the -80 dB idle cutoff (~21.6k samples)
    float before = e.value();
    CHECK(before > 0.f);
    CHECK(before < 1.f);
    e.trigger();
    float after = e.process();
    CHECK(after >= before);                                // rises, never drops
    CHECK(after - before < 0.01f);                         // and never jumps up
}
