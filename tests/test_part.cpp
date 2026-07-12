#include <doctest/doctest.h>
#include <cmath>
#include <algorithm>
#include "parts/part.h"
using namespace spky;

TEST_CASE("part: inactive target contributes only its base value") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_SIZE, false);
    p.set_target_base(LANE_SIZE, 0.3f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 1000; ++i) p.process(l, r);
    CHECK(p.target_value(LANE_SIZE) == doctest::Approx(0.3f));
}

TEST_CASE("part detune: engine pitch shifts but pitch_cv stays quantized") {
    Part p; p.init(48000.f, 1u);
    p.set_depth(0.f);                          // isolate the base value
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_tune(0.5f);
    float l, r;
    for (int i = 0; i < 4000; ++i) p.process(l, r);   // ride out the quantizer slew
    float cv0 = p.pitch_cv();
    p.set_detune_cents(50.f);                  // +50 cents on the engine pitch
    for (int i = 0; i < 4000; ++i) p.process(l, r);
    CHECK(p.pitch_cv() == doctest::Approx(cv0));       // rack CV out unchanged
}

TEST_CASE("part: active target modulates around its base, clamped to [0,1]") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_target_depth(LANE_PITCH, 1.f);
    p.set_depth(1.f);
    p.mod().set_range(1.f);
    p.mod().set_shape(0.5f);
    p.mod().set_sync_mode(SyncMode::Free);
    p.mod().set_rate(0.6f);
    float minv = 1.f, maxv = 0.f, l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        float t = p.target_value(LANE_PITCH);
        if (t < minv) minv = t;
        if (t > maxv) maxv = t;
    }
    CHECK(maxv > minv);
    CHECK(minv >= 0.f);
    CHECK(maxv <= 1.f);
}

TEST_CASE("part: DEPTH 0 pins targets to base") {
    Part p;
    p.init(48000.f, 5);
    p.quant().set_mode(QuantMode::Free);   // this test asserts the raw path
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_depth(0.f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 5000; ++i) {
        p.process(l, r);
        CHECK(p.target_value(LANE_PITCH) == doctest::Approx(0.5f));
    }
}

TEST_CASE("part: a PITCH fire raises the gate") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_PITCH, true);
    p.mod().set_sync_mode(SyncMode::Free);
    p.mod().set_rate(0.7f);
    p.mod().set_probability(1.f);
    bool saw_gate = false;
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        if (p.gate()) saw_gate = true;
    }
    CHECK(saw_gate);
}

TEST_CASE("part: SCALE mode lands pitch only on allowed dorian degrees") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.mod().set_range(1.f);
    p.mod().set_sync_mode(SyncMode::Free);
    p.mod().set_rate(0.6f);
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        float semis = p.pitch_cv() * 36.f;
        int k = static_cast<int>(semis + 0.5f);
        CHECK(std::fabs(semis - k) < 1e-4f);                       // on the grid
        CHECK(((SCALE_MASKS[SCALE_DORIAN] >> (k % 12)) & 1) == 1); // in dorian
    }
}

TEST_CASE("part: FREE mode restores the raw continuous pitch path") {
    Part p;
    p.init(48000.f, 5);
    p.quant().set_mode(QuantMode::Free);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_depth(0.f);
    float l, r;
    p.process(l, r);
    CHECK(p.pitch_cv() == doctest::Approx(0.5f));   // off-grid value passes through
}

TEST_CASE("part: inactive fx target contributes only its base value") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_base(FXT_FLUX_TIME, 0.37f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 1000; ++i) p.process(l, r);
    CHECK(p.fx_target_value(FXT_FLUX_TIME) == doctest::Approx(0.37f));
}

TEST_CASE("part: active fx target modulates around its base, clamped") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_active(FXT_FLUX_TIME, true);
    p.set_fx_target_base(FXT_FLUX_TIME, 0.5f);
    p.set_fx_target_depth(FXT_FLUX_TIME, 1.f);
    p.set_depth(1.f);
    p.mod().set_range(1.f);
    p.mod().set_sync_mode(SyncMode::Free);
    p.mod().set_rate(0.6f);
    float minv = 1.f, maxv = 0.f, l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        float t = p.fx_target_value(FXT_FLUX_TIME);
        if (t < minv) minv = t;
        if (t > maxv) maxv = t;
    }
    CHECK(maxv > minv);
    CHECK(minv >= 0.f);
    CHECK(maxv <= 1.f);
}

TEST_CASE("part: master DEPTH 0 pins fx targets to base") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_active(FXT_REV_SEND, true);
    p.set_fx_target_base(FXT_REV_SEND, 0.4f);
    p.set_depth(0.f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 5000; ++i) {
        p.process(l, r);
        CHECK(p.fx_target_value(FXT_REV_SEND) == doctest::Approx(0.4f));
    }
}

TEST_CASE("part: fx targets are never quantized (unlike the PITCH lane)") {
    Part p;
    p.init(48000.f, 5);   // boots in SCALE mode
    p.set_fx_target_base(FXT_FX_MIX, 0.437f);   // off any scale grid
    float l, r;
    p.process(l, r);
    CHECK(p.fx_target_value(FXT_FX_MIX) == doctest::Approx(0.437f));
}

TEST_CASE("part: 4-output process yields sends that follow REV SEND") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_base(FXT_REV_SEND, 1.f);    // send fully open
    float l, r, sl, sr;
    for (int i = 0; i < 2000; ++i) p.process(l, r, sl, sr);
    CHECK(sl == doctest::Approx(l));            // sin(pi/2) = 1
    p.set_fx_target_base(FXT_REV_SEND, 0.f);
    for (int i = 0; i < 2000; ++i) p.process(l, r, sl, sr);   // ride out smoother
    CHECK(sl == doctest::Approx(0.f));
}

TEST_CASE("part: boots on the synth engine and hums in FLOW (drone promise)") {
    Part p;
    p.init(48000.f, 5);
    CHECK(p.engine_id() == ENGINE_SYNTH);
    float energy = 0.f, l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        energy += l * l;
    }
    CHECK(p.active_voices() >= 1);
    CHECK(energy > 1e-3f);
}

TEST_CASE("part: FLOW at probability 0 never goes silent; STEP decays to silence") {
    Part p;
    p.init(48000.f, 5);
    p.mod().set_probability(0.f);
    float l, r;
    for (int i = 0; i < 48000 * 4; ++i) p.process(l, r);
    float energy = 0.f;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        energy += l * l;
    }
    CHECK(energy > 1e-4f);                  // the drone holds at probability 0

    Part q;
    q.init(48000.f, 5);
    q.mod().set_probability(0.f);
    q.set_step(true, 8);                    // STEP: the boot drone is released
    for (int i = 0; i < 48000 * 10; ++i) q.process(l, r);
    float tail = 0.f;
    for (int i = 0; i < 48000; ++i) {
        q.process(l, r);
        tail += l * l;
    }
    CHECK(tail == 0.f);                     // decays to EXACT silence and stays
}

TEST_CASE("part: manual trigger fires at the current pitch and raises the gate") {
    Part p;
    p.init(48000.f, 5);
    p.set_step(true, 8);
    p.mod().set_probability(0.f);
    float l, r;
    for (int i = 0; i < 48000 * 8; ++i) p.process(l, r);
    CHECK(p.active_voices() == 0);          // silent before the tap
    p.trigger_manual();
    CHECK(p.gate());
    p.process(l, r);
    CHECK(p.active_voices() == 1);
}

TEST_CASE("part: decay length follows the master cycle (set_cycle forwarding)") {
    auto tail_samples = [](float rate_norm) {
        Part p;
        p.init(48000.f, 5);
        p.set_step(true, 8);
        p.mod().set_probability(0.f);
        p.mod().set_sync_mode(SyncMode::Free);
        p.mod().set_rate(rate_norm);
        float l, r;
        // settle (no boot drone here: set_step ran before the first process()
        // call, which cancels the pending FLOW auto-trigger)
        for (int i = 0; i < 48000 * 3; ++i) p.process(l, r);
        p.trigger_manual();
        int n = 0;
        while (p.active_voices() > 0 && n < 48000 * 10) {
            p.process(l, r);
            ++n;
        }
        return n;
    };
    int slow = tail_samples(0.6f);   // ~1.61 Hz -> cycle 0.62 s -> decay ~0.93 s
    int fast = tail_samples(0.8f);   // ~6.9 Hz  -> cycle 0.14 s -> decay ~0.22 s
    CHECK(slow > fast * 2);          // longer cycle => audibly longer notes
}

TEST_CASE("part: engine switch test tone <-> synth is click-free") {
    Part p;
    p.init(48000.f, 5);
    float prev_l = 0.f, max_delta = 0.f;
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        if (i == 12000) p.set_engine(ENGINE_TEST_TONE);
        if (i == 30000) p.set_engine(ENGINE_SYNTH);
        p.process(l, r);
        if (i > 0) max_delta = std::max(max_delta, std::fabs(l - prev_l));
        prev_l = l;
    }
    // a hard swap would step by the level difference (~0.3-0.5); the 4 ms
    // Hann fade keeps sample-to-sample deltas at waveform scale.
    CHECK(max_delta < 0.15f);
    CHECK(p.engine_id() == ENGINE_SYNTH);   // second switch completed
}

TEST_CASE("part: the test tone engine reports zero active voices") {
    Part p;
    p.init(48000.f, 5);
    p.set_engine(ENGINE_TEST_TONE);
    float l, r;
    for (int i = 0; i < 1000; ++i) p.process(l, r);   // ride out the 4 ms fades
    CHECK(p.engine_id() == ENGINE_TEST_TONE);
    CHECK(p.active_voices() == 0);
}
