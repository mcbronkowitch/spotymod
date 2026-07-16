#include <doctest/doctest.h>
#include "parts/part.h"
#include <vector>
#include <algorithm>
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

static float max_env(const Instrument& inst, int p) {
    float m = 0.f;
    for (int v = 0; v < 4; ++v) m = std::max(m, inst.voice_env(p, v));
    return m;
}

TEST_CASE("choke -0.5 (note zone): B starts only while A's note is not held; the tail is free") {
    // STEP mode with rests in A's groove (arm_both's FLOW drone counts as a
    // note that is always held and would block the stage-1 window forever).
    Instrument inst;
    inst.init(48000.f);
    inst.set_tempo_bpm(120.f);
    inst.set_sync(true);
    for (int p = 0; p < PART_COUNT; ++p) {
        inst.set_step(p, true, 8);
        inst.set_range(p, 1.f);
        inst.set_voice_decay(p, 0.7f);           // long tails: ring past the gate
    }
    inst.set_density(PART_A, 0.5f);              // A has rests (DENSE 1 = legato)
    inst.set_density(PART_B, 1.f);
    inst.set_rate(PART_A, 0.0625f);              // A slow, B fast: rates differ
    inst.set_rate(PART_B, 0.5f);
    inst.set_choke(-0.5f);
    std::vector<float> l(1), r(1);
    bool prevB = false;
    int onsets = 0, in_loud_tail = 0;
    for (int i = 0; i < 960000; ++i) {           // 20 s
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
        if (onset(inst, PART_B, prevB)) {
            ++onsets;
            CHECK_FALSE(inst.gate(PART_A));      // never while A's note is held
            if (max_env(inst, PART_A) > 0.1f)
                ++in_loud_tail;                  // ...but A's ringing tail is free
        }
    }
    CHECK(onsets > 0);                            // note zone still lets B play
    CHECK(in_loud_tail > 0);                      // even while A's tail is loud
}

TEST_CASE("choke -0.5 with a FLOW priority: the drone counts as always held") {
    Instrument inst;
    arm_both(inst);                               // FLOW both: A holds a drone
    inst.set_choke(-0.5f);                        // stage 1 already blocks
    std::vector<float> l(1), r(1);
    bool prevB = false;
    int b_onsets = 0;
    for (int i = 0; i < 480000; ++i) {            // 10 s
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
        if (onset(inst, PART_B, prevB)) ++b_onsets;
    }
    CHECK(b_onsets == 0);                         // no B retrigs under the drone
    CHECK(max_env(inst, PART_B) <= 1e-4f);        // and B's own drone ducked out
}

TEST_CASE("choke never touches the clocks: lanes and pitch CV stay bit-identical") {
    // Skip-not-delay contract: choking mutes notes but must not shift any
    // lane phase, groove position or pitch CV — no "running out of sync".
    auto arm = [](Instrument& inst) {
        inst.init(48000.f);
        inst.set_tempo_bpm(120.f);
        inst.set_sync(true);
        inst.set_couple(1.f);
        for (int p = 0; p < PART_COUNT; ++p) {
            inst.set_step(p, true, 8);
            inst.set_density(p, 0.8f);
            inst.set_range(p, 1.f);
        }
        inst.set_rate(PART_A, 0.25f);
        inst.set_rate(PART_B, 0.5f);             // different rates on purpose
    };
    Instrument ref, choked;
    arm(ref);
    arm(choked);
    choked.set_choke(-1.f);
    std::vector<float> l(1), r(1);
    for (int i = 0; i < 480000; ++i) {           // 10 s
        ref.process(nullptr, nullptr, l.data(), r.data(), 1);
        choked.process(nullptr, nullptr, l.data(), r.data(), 1);
        for (int p = 0; p < PART_COUNT; ++p) {
            REQUIRE(ref.lane_output(p, LANE_PITCH) == choked.lane_output(p, LANE_PITCH));
            REQUIRE(ref.pitch_cv(p) == choked.pitch_cv(p));
            REQUIRE(ref.lane_fired(p, LANE_PITCH) == choked.lane_fired(p, LANE_PITCH));
        }
    }
}

TEST_CASE("choke -1: the yielding FLOW drone ducks out and comes back") {
    Instrument inst;
    arm_both(inst);                               // FLOW: B holds a drone voice
    std::vector<float> l(1), r(1);
    for (int i = 0; i < 96000; ++i)               // let both drones establish
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
    CHECK(max_env(inst, PART_B) > 0.1f);          // B's drone is up

    inst.set_choke(-1.f);                         // A takes the floor
    for (int i = 0; i < 480000; ++i)              // 10 s to decay out
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
    CHECK(max_env(inst, PART_B) <= 1e-4f);        // drone gone, not just gated

    inst.set_choke(0.f);                          // floor is free again
    for (int i = 0; i < 480000; ++i)
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
    CHECK(max_env(inst, PART_B) > 0.1f);          // drone re-armed and audible
}
