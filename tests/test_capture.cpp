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
