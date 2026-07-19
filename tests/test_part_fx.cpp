#include <doctest/doctest.h>
#include <cmath>
#include "fx/part_fx.h"
using namespace spky;

static float s_pf_l[Flux::kMaxSamples];
static float s_pf_r[Flux::kMaxSamples];

// fxv helper: boot bases with individual overrides
static void fill(float* v, float grit, float time, float mix, float send, float fb) {
    v[FXT_GRIT_INT] = grit;
    v[FXT_FLUX_TIME] = time;
    v[FXT_FX_MIX] = mix;
    v[FXT_REV_SEND] = send;
    v[FXT_FLUX_FB] = fb;
}

TEST_CASE("part_fx: both blocks off is bit-exact dry, send 0 is exact zero") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r, 0xD0571u);
    float v[FXT_COUNT];
    fill(v, 0.3f, 0.4f, 1.f, 0.f, 0.45f);
    for (int i = 0; i < 2000; ++i) {
        float s = 0.4f * std::sin(0.013f * i);
        float l = s, r = s, sl = 1.f, sr = 1.f;
        fx.process(l, r, sl, sr, v);
        CHECK(l == s);
        CHECK(r == s);
        CHECK(sl == 0.f);
        CHECK(sr == 0.f);
    }
}

TEST_CASE("part_fx: FX MIX 0 keeps the dry signal even with grit engaged") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r, 0xD0571u);
    fx.set_fx_on(FxBlock::Grit, true, true);
    float v[FXT_COUNT];
    fill(v, 0.9f, 0.4f, 0.f, 0.f, 0.f);
    for (int i = 0; i < 2000; ++i) {
        float s = 0.4f * std::sin(0.013f * i);
        float l = s, r = s, sl, sr;
        fx.process(l, r, sl, sr, v);
        CHECK(l == doctest::Approx(s).epsilon(1e-6));
    }
}

TEST_CASE("part_fx: FX MIX 1 with grit on changes the signal") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r, 0xD0571u);
    fx.set_fx_on(FxBlock::Grit, true, true);
    float v[FXT_COUNT];
    fill(v, 0.9f, 0.4f, 1.f, 0.f, 0.f);
    int diff = 0;
    for (int i = 0; i < 4800; ++i) {
        float s = 0.4f * std::sin(0.028f * i);
        float l = s, r = s, sl, sr;
        fx.process(l, r, sl, sr, v);
        if (std::fabs(l - s) > 1e-4f) ++diff;
    }
    CHECK(diff > 1000);
}

TEST_CASE("part_fx: send taps post-FX at the equal-power gain") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r, 0xD0571u);
    float v[FXT_COUNT];
    fill(v, 0.3f, 0.4f, 1.f, 1.f, 0.45f);   // send fully open
    // prime the smoothers (first process snaps), then measure
    float l = 0.f, r = 0.f, sl, sr;
    fx.process(l, r, sl, sr, v);
    for (int i = 1; i < 200; ++i) {
        float s = 0.4f * std::sin(0.013f * i);
        l = s; r = s;
        fx.process(l, r, sl, sr, v);
        CHECK(sl == doctest::Approx(l));    // sin(pi/2) = 1: send == post-fx out
    }
}

TEST_CASE("part_fx: comp default 0 leaves chain and send bit-exact") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r, 0xD0571u);
    const float fxv[FXT_COUNT] = {0.f, 0.5f, 1.f, 0.5f, 0.f};
    for (int i = 0; i < 4800; ++i) {
        float s = 0.5f * std::sin(6.2831853f * 220.f * i / 48000.f);
        float l = s, r = s, sl = 0.f, sr = 0.f;
        fx.process(l, r, sl, sr, fxv);
        CHECK(l == s);                                   // FX off + comp 0 = dry bits
        CHECK(sl == doctest::Approx(s * std::sin(0.5f * 1.5707963f)));
    }
}

TEST_CASE("part_fx: comp sits BEFORE the send tap — the send gets louder too") {
    const float fxv[FXT_COUNT] = {0.f, 0.5f, 1.f, 0.8f, 0.f};
    auto send_rms = [&](float amount) {
        PartFx fx;
        fx.init(48000.f, s_pf_l, s_pf_r, 0xD0571u);
        fx.set_comp(amount);
        double acc = 0.0;
        int n = 0;
        for (int i = 0; i < 96000; ++i) {
            float s = 0.05f * std::sin(6.2831853f * 220.f * i / 48000.f);  // quiet!
            float l = s, r = s, sl = 0.f, sr = 0.f;
            fx.process(l, r, sl, sr, fxv);
            if (i >= 24000) { acc += sl * sl; ++n; }
        }
        return std::sqrt((float)(acc / n));
    };
    CHECK(send_rms(1.f) > send_rms(0.f) * 1.5f);   // full-wet motivation, verified
}

TEST_CASE("part_fx: synced rate + BPM place the echo, not FXT_FLUX_TIME") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r, 0xD0571u);
    fx.set_fx_on(FxBlock::Flux, true, true);
    fx.set_flux_mix(1.f);              // 0 dB wet
    fx.set_bpm(120.f);
    fx.set_flux_rate(3);              // "1/4" @120 -> 0.5 s
    float v[FXT_COUNT];
    fill(v, 0.f, 0.99f, 1.f, 0.f, 0.f);   // FXT_FLUX_TIME = 0.99 must NOT move the echo
    int idx = -1;
    for (int i = 0; i < 30000; ++i) {
        float l = (i == 0) ? 1.f : 0.f;
        float r = l, sl, sr;
        fx.process(l, r, sl, sr, v);
        if (i > 100 && std::fabs(l) > 1e-3f) { idx = i; break; }
    }
    CHECK(idx >= 23900);
    CHECK(idx <= 24200);             // ~24000 (0.5 s), independent of v[FXT_FLUX_TIME]
}

TEST_CASE("part_fx: set_dust reaches the flux block") {
    static float s_pf_dust_l[Flux::kMaxSamples], s_pf_dust_r[Flux::kMaxSamples];
    PartFx fx;
    fx.init(48000.f, s_pf_dust_l, s_pf_dust_r, 0xD0571u);
    fx.set_fx_on(FxBlock::Flux, true, true);
    fx.set_flux_mix(1.f);

    CHECK(!fx.flux().dust_active());
    fx.set_dust(0.5f);
    CHECK(fx.flux().dust_active());     // the forwarded value actually landed
    fx.set_dust(0.f);
    CHECK(!fx.flux().dust_active());
}

TEST_CASE("part_fx: set_rot reaches the flux block") {
    // dust_active()/head_gain() don't reflect ROT, so prove the forward the
    // same way test_flux.cpp does for Flux::set_rot itself: run two otherwise
    // identical instances (same seed, same echo/dust settings) that differ
    // ONLY in the ROT value PartFx::set_rot was given, and require the grain
    // streams to diverge. A dropped/no-op forwarder leaves both at Flux's
    // default ROT = 0, so the streams would stay identical and this fails.
    static float s_pf_rot_l0[Flux::kMaxSamples], s_pf_rot_r0[Flux::kMaxSamples];
    static float s_pf_rot_l1[Flux::kMaxSamples], s_pf_rot_r1[Flux::kMaxSamples];
    PartFx fx0, fx1;
    fx0.init(48000.f, s_pf_rot_l0, s_pf_rot_r0, 0xABCDu);
    fx1.init(48000.f, s_pf_rot_l1, s_pf_rot_r1, 0xABCDu);
    for (PartFx* fx : {&fx0, &fx1}) {
        fx->set_fx_on(FxBlock::Flux, true, true);
        fx->flux().set_bpm(120.f);
        fx->flux().set_rate(3);
        fx->flux().set_feedback(0.6f);
        fx->flux().set_mix(1.f);
        fx->set_dust(0.9f);   // dust must be active for ROT to have anything to act on
    }
    fx0.set_rot(0.f);
    fx1.set_rot(0.9f);
    REQUIRE(fx0.flux().dust_active());
    REQUIRE(fx1.flux().dust_active());

    bool any_diff = false;
    for (int i = 0; i < 48000; ++i) {
        const float s = std::sin(0.01f * i) * 0.4f;
        float l0 = s, r0 = s;
        float l1 = s, r1 = s;
        fx0.flux().process(l0, r0);
        fx1.flux().process(l1, r1);
        if (l0 != l1) { any_diff = true; break; }
    }
    CHECK(any_diff);   // ROT actually reached the flux block and changed its output
}
