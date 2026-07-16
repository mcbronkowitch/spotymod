#include <doctest/doctest.h>
#include "parts/part.h"
#include <vector>
#include "instrument.h"
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

// --- instrument-level CHOKE ---------------------------------------------------

// Both decks firing fast and free-running, so their pulses overlap often.
static void arm_both(Instrument& inst) {
    inst.init(48000.f);
    inst.set_tempo_bpm(120.f);
    for (int p = 0; p < PART_COUNT; ++p) {
        inst.set_rate(p, p == PART_A ? 0.8f : 0.9f);
        inst.set_density(p, 1.f);
        inst.set_range(p, 1.f);
    }
}

// One sample step; returns the yielding part's gate onset (rising edge).
static bool onset(Instrument& inst, int part, bool& prev) {
    const bool g = inst.gate(part);
    const bool rise = g && !prev;
    prev = g;
    return rise;
}

static bool window_open(const Instrument& inst, int pri) {
    if (inst.gate(pri)) return true;
    for (int v = 0; v < 4; ++v)
        if (inst.voice_env(pri, v) > 1e-4f) return true;
    return false;
}

TEST_CASE("choke 0 is bit-identical to an untouched instrument") {
    Instrument a, b;
    a.init(48000.f);
    b.init(48000.f);
    b.set_choke(0.f);
    std::vector<float> al(1), ar(1), bl(1), br(1);
    for (int i = 0; i < 48000; ++i) {
        a.process(nullptr, nullptr, al.data(), ar.data(), 1);
        b.process(nullptr, nullptr, bl.data(), br.data(), 1);
        REQUIRE(al[0] == bl[0]);
        REQUIRE(ar[0] == br[0]);
    }
}

TEST_CASE("choke -1: B never fires while A is audible (gate or decay)") {
    Instrument inst;
    arm_both(inst);
    inst.set_choke(-1.f);
    std::vector<float> l(1), r(1);
    bool prevB = false, sawA = false;
    for (int i = 0; i < 480000; ++i) {          // 10 s
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
        sawA = sawA || inst.gate(PART_A);
        if (onset(inst, PART_B, prevB))
            CHECK_FALSE(window_open(inst, PART_A));
    }
    CHECK(sawA);                                 // sanity: A actually plays
}

TEST_CASE("choke +1 is the mirror: A yields to B") {
    Instrument inst;
    arm_both(inst);
    inst.set_choke(1.f);
    std::vector<float> l(1), r(1);
    bool prevA = false, sawB = false;
    for (int i = 0; i < 480000; ++i) {
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
        sawB = sawB || inst.gate(PART_B);
        if (onset(inst, PART_A, prevA))
            CHECK_FALSE(window_open(inst, PART_B));
    }
    CHECK(sawB);
}

TEST_CASE("choke -0.5 (gate zone): every A gate blocks B onsets, the decay does not") {
    Instrument inst;
    arm_both(inst);
    inst.set_choke(-0.5f);                       // discrete zone: gate window only
    std::vector<float> l(1), r(1);
    bool prevB = false;
    int onsets = 0, in_decay = 0;
    for (int i = 0; i < 480000; ++i) {
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
        if (onset(inst, PART_B, prevB)) {
            ++onsets;
            CHECK_FALSE(inst.gate(PART_A));      // never inside A's gate
            if (window_open(inst, PART_A)) ++in_decay;
        }
    }
    CHECK(onsets > 0);                            // gate zone still lets B play...
    CHECK(in_decay > 0);                          // ...including during A's decay
}
