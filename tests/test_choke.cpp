#include <doctest/doctest.h>
#include "parts/part.h"
using namespace spky;

// Drives a bare Part fast enough that the pitch lane fires several times a
// second, then checks that inhibit removes every new gate/trigger while the
// lanes keep running.
static void run(Part& p, int samples, bool& saw_gate) {
    float l, r;
    for (int i = 0; i < samples; ++i) {
        p.process(l, r);
        if (p.gate()) saw_gate = true;
    }
}

TEST_CASE("part: inhibit blocks new gates in FLOW") {
    Part part;
    part.init(48000.f, 0xabcd1234u);
    part.mod().set_tempo_bpm(120.f);
    part.mod().set_rate(0.8f);
    part.mod().set_density(1.f);

    bool gated = false;
    run(part, 48000, gated);
    CHECK(gated);                       // sanity: it fires without inhibit

    part.set_inhibit(true);
    float l, r;                         // ride out the current ~5 ms pulse
    for (int i = 0; i < 480 && part.gate(); ++i) part.process(l, r);

    gated = false;
    run(part, 96000, gated);            // 2 s inhibited: silence on the gate
    CHECK_FALSE(gated);

    part.set_inhibit(false);            // and it comes back
    gated = false;
    run(part, 96000, gated);
    CHECK(gated);
}

TEST_CASE("part: inhibit also mutes the STEP sustain of a suppressed note") {
    Part part;
    part.init(48000.f, 0xabcd1234u);
    part.mod().set_tempo_bpm(120.f);
    part.mod().set_rate(0.8f);
    part.mod().set_density(1.f);
    part.set_step(true, 8);             // STEP mode: gate() includes note sustain

    part.set_inhibit(true);
    float l, r;
    for (int i = 0; i < 480 && part.gate(); ++i) part.process(l, r);

    bool gated = false;
    run(part, 96000, gated);
    CHECK_FALSE(gated);                 // sustain of suppressed notes stays low
}

TEST_CASE("part: max_voice_env is 0 idle and >0 after a trigger") {
    Part part;
    part.init(48000.f, 0xabcd1234u);
    part.mod().set_tempo_bpm(120.f);
    CHECK(part.max_voice_env() == 0.f);

    part.trigger_manual();
    float l, r;
    for (int i = 0; i < 480; ++i) part.process(l, r);
    CHECK(part.max_voice_env() > 0.f);
}
