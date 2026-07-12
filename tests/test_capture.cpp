#include <doctest/doctest.h>
#include <cmath>
#include "mod/capture.h"
#include "mod/lane.h"
#include "mod/super_modulator.h"
#include "mod/lane_id.h"
#include "instrument.h"
#include "render/scenario.h"
using namespace spky;

TEST_CASE("CaptureLoop: init state is centered, invalid, fired only on slot 0") {
    CaptureLoop loop;
    loop.reset();
    CHECK(loop.valid() == false);
    // capture the (unrecorded) init ring so we can read it back through the loop
    loop.capture_now();
    CHECK(loop.valid() == true);
    CHECK(loop.value(0) == doctest::Approx(0.f));
    CHECK(loop.value(100) == doctest::Approx(0.f));
    CHECK(loop.fired(0) == true);
    CHECK(loop.fired(1) == false);
    CHECK(loop.fired(191) == false);
}

TEST_CASE("CaptureLoop: record then capture_now copies ring -> loop") {
    CaptureLoop loop;
    loop.reset();
    loop.record(5, 0.25f, true);
    loop.record(6, -0.5f, false);
    // before capture, the frozen loop still holds the init state
    CHECK(loop.value(5) == doctest::Approx(0.f));
    loop.capture_now();
    CHECK(loop.value(5) == doctest::Approx(0.25f));
    CHECK(loop.fired(5) == true);
    CHECK(loop.value(6) == doctest::Approx(-0.5f));
    CHECK(loop.fired(6) == false);
}

TEST_CASE("CaptureLoop: freezing captures the current window, incl. stale slots ahead") {
    CaptureLoop loop;
    loop.reset();
    // pass 1: fill every slot with a ramp value + fire on slot 0
    for (int s = 0; s < CaptureLoop::kSlots; ++s)
        loop.record(s, static_cast<float>(s) / CaptureLoop::kSlots, s == 0);
    // pass 2: overwrite only the first half (playhead at slot 96), new value +1 offset
    for (int s = 0; s < 96; ++s)
        loop.record(s, 1.f + static_cast<float>(s) / CaptureLoop::kSlots, s == 0);
    loop.capture_now();   // freeze mid-cycle: [0,96) = pass 2, [96,192) = pass 1
    CHECK(loop.value(10)  == doctest::Approx(1.f + 10.f / CaptureLoop::kSlots)); // fresh
    CHECK(loop.value(150) == doctest::Approx(150.f / CaptureLoop::kSlots));      // stale pass-1
}

TEST_CASE("CaptureLoop: kSlots is 192") {
    CHECK(CaptureLoop::kSlots == 192);
}

// Configure a STEP PITCH-style lane with a capture loop attached.
static void configure_step_capture(ModLane& l, CaptureLoop& loop,
                                   int steps = 8, float prob = 1.f) {
    loop.reset();
    l.init(48000.f, 4242);
    l.set_capture_loop(&loop);
    l.set_range(1.f);
    l.set_shape(0.75f);         // pulse boundary: distinct step values, f=0 => zero S&H weight
    l.set_smooth(0.f);
    l.set_step(true, steps);
    l.set_probability(prob);
    l.set_rate_hz(1.f);         // 1 cycle/sec = 48000 samples/cycle
}

TEST_CASE("ModLane record: fired slots line up with STEP boundaries") {
    ModLane l; CaptureLoop loop;
    configure_step_capture(l, loop, 8, 1.f);
    for (int i = 0; i < 48000 + 500; ++i) l.process();  // > one full cycle
    loop.capture_now();
    // 8 steps over 192 slots => a boundary fires roughly every 24 slots.
    int fired_slots = 0;
    for (int s = 0; s < CaptureLoop::kSlots; ++s) if (loop.fired(s)) ++fired_slots;
    CHECK(fired_slots >= 6);   // ~8, tolerant of phase drift at the seam
    CHECK(fired_slots <= 10);
}

TEST_CASE("ModLane record: recorded value equals the lane target at that slot") {
    ModLane l; CaptureLoop loop;
    configure_step_capture(l, loop, 4, 1.f);
    // run to a known phase inside step 1 (phase ~0.30 => slot ~57), read target
    for (int i = 0; i < 48000; ++i) l.process();   // align to cycle start-ish
    for (int i = 0; i < 14400; ++i) l.process();   // +0.30 cycle
    float tgt = l.target();
    int   slot = static_cast<int>(l.phase() * CaptureLoop::kSlots);
    loop.capture_now();
    CHECK(loop.value(slot) == doctest::Approx(tgt));
}

TEST_CASE("ModLane record: deterministic loop is identical one cycle apart") {
    ModLane l; CaptureLoop loop;
    configure_step_capture(l, loop, 8, 1.f);   // prob 1, evolve 0 => metronomic
    for (int i = 0; i < 48000 * 3; ++i) l.process();   // settle
    loop.capture_now();
    float a[CaptureLoop::kSlots];
    for (int s = 0; s < CaptureLoop::kSlots; ++s) a[s] = loop.value(s);
    for (int i = 0; i < 48000; ++i) l.process();       // one more full cycle
    loop.capture_now();
    for (int s = 0; s < CaptureLoop::kSlots; ++s)
        CHECK(loop.value(s) == doctest::Approx(a[s]));
}

TEST_CASE("ModLane record: a lane with no capture loop is unaffected") {
    ModLane a; a.init(48000.f, 99);
    a.set_step(true, 8); a.set_shape(0.9f); a.set_rate_hz(1.f);
    ModLane b; b.init(48000.f, 99);
    b.set_step(true, 8); b.set_shape(0.9f); b.set_rate_hz(1.f);
    CaptureLoop loop; loop.reset();
    b.set_capture_loop(&loop);
    // recording must not consume RNG => identical output streams
    for (int i = 0; i < 48000 * 2; ++i)
        CHECK(a.process() == doctest::Approx(b.process()));
}

// Capture a metronomic STEP loop, then return the lane in replay mode.
static void capture_and_replay(ModLane& l, CaptureLoop& loop,
                               int steps = 8, float prob = 1.f) {
    configure_step_capture(l, loop, steps, prob);
    for (int i = 0; i < 48000 * 2; ++i) l.process();  // record >= 2 cycles
    loop.capture_now();
    l.set_replay(true);
    CHECK(l.replaying() == true);
}

TEST_CASE("replay: deterministic — two identical replay runs match sample-for-sample") {
    // Determinism is proven exactly (no tolerance) by running two lanes with the
    // same seed and identical capture history: their float phase accumulators
    // drift identically, so the replay streams are bit-for-bit reproducible.
    // (Same technique as the Task 2 "no capture loop is unaffected" test.)
    ModLane a; CaptureLoop la; capture_and_replay(a, la, 8, 1.f);
    ModLane b; CaptureLoop lb; capture_and_replay(b, lb, 8, 1.f);
    for (int i = 0; i < 48000 * 3; ++i) {
        CHECK(a.process() == doctest::Approx(b.process()));
        CHECK(a.target()  == doctest::Approx(b.target()));
    }
}

TEST_CASE("replay: set_replay before a valid capture does nothing") {
    ModLane l; CaptureLoop loop; loop.reset();
    l.init(48000.f, 7); l.set_capture_loop(&loop);
    l.set_replay(true);
    CHECK(l.replaying() == false);   // loop not valid yet -> stays generative
}

TEST_CASE("replay: probability < 1 suppresses triggers and holds the pitch") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 8, 1.f);
    l.set_probability(0.f);          // every recorded trigger now fails
    int fires = 0;
    bool changed = false;
    float prev = l.target();
    for (int i = 0; i < 48000 * 2; ++i) {
        l.process();
        if (l.fired()) ++fires;
        if (l.target() != doctest::Approx(prev)) changed = true;
    }
    CHECK(fires == 0);               // no engine triggers
    CHECK(changed == false);         // pitch frozen at the held value
}

TEST_CASE("replay: EVOLVE has no effect on the replaying lane") {
    // Two identical replay lanes; set EVOLVE hard on one. If EVOLVE leaked onto
    // the replay path it would diverge b from a. Exact match proves it is ignored.
    ModLane a; CaptureLoop la; capture_and_replay(a, la, 8, 1.f);
    ModLane b; CaptureLoop lb; capture_and_replay(b, lb, 8, 1.f);
    b.set_evolve(1.f);
    for (int i = 0; i < 48000 * 3; ++i)
        CHECK(a.process() == doctest::Approx(b.process()));
}

TEST_CASE("replay: SMOOTH still glides between loop steps") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 4, 1.f);
    l.set_smooth(0.6f);              // audible glide
    bool gliding = false;
    for (int i = 0; i < 48000 * 2; ++i) {
        float out = l.process();
        // right after a boundary the smoothed output lags the loop target
        if (std::fabs(out - l.target()) > 0.02f) gliding = true;
    }
    CHECK(gliding == true);
}

TEST_CASE("replay: RANGE scales the loop down to off") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 8, 1.f);
    l.set_range(0.f);                // off
    for (int i = 0; i < 48000; ++i) {
        float out = l.process();
        CHECK(out == doctest::Approx(0.f));
    }
}

TEST_CASE("replay: the loop persists across replay-off (two-buffer promise)") {
    // The frozen loop buffer must survive replay-off: generative recording writes
    // the ring, never the frozen loop, and capture_now is not called again — so the
    // loop is byte-for-byte identical when replay re-engages. Drift-immune.
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 8, 1.f);
    float snap[CaptureLoop::kSlots];
    for (int s = 0; s < CaptureLoop::kSlots; ++s) snap[s] = loop.value(s);

    l.set_replay(false);
    for (int i = 0; i < 48000 * 2; ++i) l.process();   // generative runs on

    l.set_replay(true);
    for (int s = 0; s < CaptureLoop::kSlots; ++s)
        CHECK(loop.value(s) == doctest::Approx(snap[s]));   // same loop returned
    CHECK(l.replaying() == true);

    int fires = 0;                                          // and it is live again
    for (int i = 0; i < 48000; ++i) { l.process(); if (l.fired()) ++fires; }
    CHECK(fires >= 4);
}

TEST_CASE("replay: toggling replay does not jump beyond one boundary step") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 8, 1.f);
    float before = l.phase();
    l.set_replay(false);
    float after = l.process();  (void)after;
    // phase advances by one sample only; no phase reset on toggle
    CHECK(std::fabs(l.phase() - before) < 0.001f);
}

static void configure_sm_step(SuperModulator& sm, float prob = 1.f) {
    sm.init(48000.f, 1000);
    sm.set_range(1.f);
    sm.set_shape(0.9f);
    sm.set_smooth(0.f);
    sm.set_step(true, 8);
    sm.set_probability(prob);
    sm.set_rate(0.5f);           // some audible rate
    sm.set_sync_mode(SyncMode::Free);
}

TEST_CASE("SuperModulator: loop_valid false until capture") {
    SuperModulator sm; configure_sm_step(sm);
    CHECK(sm.loop_valid() == false);
    for (int i = 0; i < 48000 * 2; ++i) sm.process();
    sm.capture_now();
    CHECK(sm.loop_valid() == true);
}

TEST_CASE("SuperModulator: replay swaps only the PITCH lane (EVOLVE isolated to live lanes)") {
    // Two identical modulators drift identically. Replay both; turn EVOLVE on for
    // a, off for b. The replayed PITCH lane ignores EVOLVE -> a and b match exactly
    // (drift-immune). The live MOTION lane is driven by EVOLVE -> a diverges from b.
    SuperModulator a; configure_sm_step(a);
    SuperModulator b; configure_sm_step(b);
    for (int i = 0; i < 48000 * 2; ++i) { a.process(); b.process(); }
    a.capture_now(); b.capture_now();
    a.set_replay(true);  b.set_replay(true);
    a.set_evolve(1.f);   b.set_evolve(0.f);
    CHECK(a.replaying() == true);
    CHECK(a.loop_valid() == true);

    bool motion_diverged = false;
    for (int i = 0; i < 48000 * 3; ++i) {
        a.process(); b.process();
        CHECK(a.lane_output(LANE_PITCH) == doctest::Approx(b.lane_output(LANE_PITCH)));
        if (a.lane_output(LANE_MOTION) != doctest::Approx(b.lane_output(LANE_MOTION)))
            motion_diverged = true;
    }
    CHECK(motion_diverged == true);
}

// Drive part 0 as a Dorian STEP melody with a capture loop, return it replaying.
static void inst_capture_replay(Instrument& inst) {
    inst.init(48000.f);                 // engine-only init (no FX chain)
    inst.set_tempo_bpm(120.f);
    inst.set_engine(0, ENGINE_SYNTH);
    inst.set_step(0, true, 8);
    inst.set_shape(0, 0.9f);
    inst.set_smooth(0, 0.f);
    inst.set_range(0, 1.f);
    inst.set_probability(0, 1.f);
    inst.set_depth(0, 1.f);
    inst.set_rate(0, 0.5f);
    inst.set_target_active(0, LANE_PITCH, true);
    inst.set_target_base(0, LANE_PITCH, 0.5f);
    inst.set_scale(SCALE_DORIAN);
    inst.set_tune(0, 0.5f);             // neutral
    float l, r;
    for (int i = 0; i < 48000 * 2; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    inst.capture_now(0);
    CHECK(inst.loop_valid(0) == true);
    inst.set_replay(0, true);
    CHECK(inst.replaying(0) == true);
}

TEST_CASE("Instrument: TUNE transposes the replayed loop") {
    // Two identical replaying instruments; transpose only b. They stay phase-
    // aligned (same seed/config), so any pitch_cv difference is caused by TUNE.
    Instrument a; inst_capture_replay(a);
    Instrument b; inst_capture_replay(b);
    b.set_tune(0, 1.0f);                 // +max transpose on b only
    bool differs = false;
    float l, r;
    for (int i = 0; i < 9600; ++i) {
        a.process(nullptr, nullptr, &l, &r, 1);
        b.process(nullptr, nullptr, &l, &r, 1);
        if (a.pitch_cv(0) != doctest::Approx(b.pitch_cv(0))) differs = true;
    }
    CHECK(differs == true);
}

TEST_CASE("Instrument: scale change requantizes the replayed loop") {
    // Same technique: change only b's scale; a stays Dorian (set in the helper).
    Instrument a; inst_capture_replay(a);
    Instrument b; inst_capture_replay(b);
    b.set_scale(SCALE_WHOLE);            // different scale masks -> different pitches
    bool differs = false;
    float l, r;
    for (int i = 0; i < 9600; ++i) {
        a.process(nullptr, nullptr, &l, &r, 1);
        b.process(nullptr, nullptr, &l, &r, 1);
        if (a.pitch_cv(0) != doctest::Approx(b.pitch_cv(0))) differs = true;
    }
    CHECK(differs == true);
}

TEST_CASE("Instrument: recorded fired pattern drives triggers via lane_fired") {
    Instrument inst; inst_capture_replay(inst);
    float l, r;
    int fires = 0;
    for (int i = 0; i < 48000; ++i) {
        inst.process(nullptr, nullptr, &l, &r, 1);
        if (inst.lane_fired(0, LANE_PITCH)) ++fires;
    }
    CHECK(fires >= 4);                   // ~8 triggers/cycle at prob 1
}

TEST_CASE("Instrument: probability thinning on the loop holds notes (fewer triggers)") {
    Instrument inst; inst_capture_replay(inst);
    inst.set_probability(0, 0.f);        // all recorded triggers fail -> hold
    float l, r;
    int fires = 0;
    for (int i = 0; i < 48000 * 2; ++i) {
        inst.process(nullptr, nullptr, &l, &r, 1);
        if (inst.lane_fired(0, LANE_PITCH)) ++fires;
    }
    CHECK(fires == 0);
}

TEST_CASE("scenario: capture_now + set_replay dispatch through apply_event") {
    Instrument inst; inst.init(48000.f);
    inst.set_step(0, true, 8);
    inst.set_shape(0, 0.9f);
    inst.set_target_active(0, LANE_PITCH, true);
    inst.set_rate(0, 0.5f);
    float l, r;
    for (int i = 0; i < 48000 * 2; ++i) inst.process(nullptr, nullptr, &l, &r, 1);

    Event cap;   cap.action = "capture_now"; cap.part = 0;
    Event play;  play.action = "set_replay";  play.part = 0; play.flag = true;
    apply_event(inst, cap);
    apply_event(inst, play);
    CHECK(inst.loop_valid(0) == true);
    CHECK(inst.replaying(0) == true);

    Event stop; stop.action = "set_replay"; stop.part = 0; stop.flag = false;
    apply_event(inst, stop);
    CHECK(inst.replaying(0) == false);
}
