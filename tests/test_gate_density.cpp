#include <doctest/doctest.h>
#include "mod/lane.h"

using namespace spky;

static ModLane melodic_step(uint32_t seed, int steps) {
    ModLane l;
    l.set_melodic(true);
    l.set_principle(Principle::TwoMotif);
    l.init(48000.f, seed);           // variation defaults to 0 (LOOP)
    l.set_shape(1.0f);
    l.set_step(true, steps);
    l.set_rate_hz(2.0f);
    return l;
}

// Count fired notes over a fixed span (many cycles); at variation 0 the rate is
// constant so full-vs-thin spans are directly comparable.
static int count_fires(ModLane& l, int samples) {
    int fires = 0;
    for (int n = 0; n < samples; ++n) { l.process(); if (l.fired()) ++fires; }
    return fires;
}

TEST_CASE("DENSITY low drops weak-beat gates; downbeat survives") {
    ModLane full = melodic_step(0x11, 16);
    ModLane thin = melodic_step(0x11, 16);
    thin.set_density(0.2f);
    int f_full = count_fires(full, 24000);
    int f_thin = count_fires(thin, 24000);
    CHECK(f_thin < f_full);          // fewer notes fire when thinned
    CHECK(f_thin >= 1);              // strong beats still fire
}

TEST_CASE("DENSITY is reversible: density 1 == the full pattern") {
    ModLane a = melodic_step(0x11, 16);
    ModLane b = melodic_step(0x11, 16);
    b.set_density(0.2f);             // thin...
    b.set_density(1.0f);             // ...then restore (never edits _gate)
    CHECK(count_fires(a, 24000) == count_fires(b, 24000));
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
