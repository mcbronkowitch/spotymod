#include <doctest/doctest.h>
#include <cmath>
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
