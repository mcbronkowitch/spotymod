#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include "oliverb/stmlib_shim.h"
#include "oliverb/random_oscillator.h"
#include "oliverb/fx_engine.h"
#include "mod/rng.h"

TEST_CASE("oliverb shim: SoftLimit is odd and compressive") {
    CHECK(stmlib::SoftLimit(0.f) == 0.f);
    CHECK(stmlib::SoftLimit(1.f) == doctest::Approx(28.f / 36.f));
    CHECK(stmlib::SoftLimit(-1.f) == doctest::Approx(-28.f / 36.f));
    CHECK(stmlib::SoftLimit(0.5f) < 0.5f);   // compressive above 0
    CHECK(stmlib::SoftLimit(0.01f) == doctest::Approx(0.01f).epsilon(0.01));
}

TEST_CASE("oliverb shim: cosine oscillator oscillates in ~0..1") {
    stmlib::CosineOscillator osc;
    osc.Init(0.001f);
    float mn = 1.f, mx = 0.f;
    for (int i = 0; i < 4000; ++i) {
        float v = osc.Next();
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    CHECK(mn >= -0.1f);
    CHECK(mx <= 1.1f);
    CHECK(mx - mn > 0.5f);   // it actually swings
}

TEST_CASE("oliverb shim: random oscillator is deterministic and bounded") {
    spky::Rng ra, rb;
    ra.seed(0xfeed1u);
    rb.seed(0xfeed1u);
    clouds::RandomOscillator la, lb;
    la.Init(&ra);
    lb.Init(&rb);
    la.set_slope(0.001f);
    lb.set_slope(0.001f);
    bool identical = true, bounded = true;
    for (int i = 0; i < 20000; ++i) {
        float va = la.Next(), vb = lb.Next();
        if (va != vb) identical = false;
        if (va < -1.01f || va > 1.01f) bounded = false;
    }
    CHECK(identical);
    CHECK(bounded);
}

TEST_CASE("oliverb fx_engine: float delay line delays an impulse exactly") {
    typedef clouds::FxEngine<1024, clouds::FORMAT_32_BIT> E;
    static float buf[1024];
    static E eng;
    eng.Init(buf);
    typedef E::Reserve<100> Memory;
    E::DelayLine<Memory, 0> del;
    E::Context c;
    int peak_at = -1;
    for (int i = 0; i < 300; ++i) {
        eng.Start(&c);
        c.Read(i == 0 ? 1.f : 0.f, 1.f);
        c.Write(del, 0.f);
        c.Load(0.f);
        c.Read(del, 99, 1.f);
        float out;
        c.Write(out);
        if (out > 0.5f && peak_at < 0) peak_at = i;
    }
    CHECK(peak_at == 99);   // write at offset 0, read at offset 99 -> 99 samples
}

#include "oliverb/oliverb.h"

namespace {
// 128 KB buffer + core: statics, never on the stack.
float s_ob_buf[clouds::Oliverb::kBufferSize];
clouds::Oliverb s_ob;

void ob_defaults(clouds::Oliverb& ob) {
    ob.Init(s_ob_buf, 0x0BE21D5u);
    ob.set_size(0.5f);
    ob.set_decay(0.7f);
    ob.set_lp(0.6f);
    ob.set_hp(0.01f);
    ob.set_mod_amount(0.f);
    ob.set_mod_rate(0.5f);
    ob.Prepare();
}
} // namespace

TEST_CASE("oliverb core: silence in, exact silence out") {
    ob_defaults(s_ob);
    bool clean = true;
    for (int i = 0; i < 4000; ++i) {
        if (i % 96 == 0) s_ob.Prepare();
        float l = 0.f, r = 0.f;
        s_ob.Process(&l, &r);
        if (l != 0.f || r != 0.f) clean = false;
    }
    CHECK(clean);
}

TEST_CASE("oliverb core: impulse rings a decorrelated stereo tail") {
    ob_defaults(s_ob);
    float tail = 0.f, decorr = 0.f;
    for (int i = 0; i < 48000; ++i) {
        if (i % 96 == 0) s_ob.Prepare();
        float l = (i == 0) ? 1.f : 0.f;
        float r = l;
        s_ob.Process(&l, &r);
        if (i >= 12000) tail += l * l;
        float d = l - r;
        if (d < 0) d = -d;
        if (d > decorr) decorr = d;
    }
    CHECK(tail > 1e-6f);     // still ringing after 0.25 s at decay 0.7
    CHECK(decorr > 1e-4f);   // L and R differ
}

TEST_CASE("oliverb core: decay > 1 blooms but stays bounded") {
    ob_defaults(s_ob);
    s_ob.set_decay(1.05f);
    float peak = 0.f, late_energy = 0.f;
    bool finite = true;
    const int N = 48000 * 8;
    for (int i = 0; i < N; ++i) {
        if (i % 96 == 0) s_ob.Prepare();
        // 2 s of drive, then 6 s of nothing
        float in = (i < 96000) ? 0.4f * spky::fast_sin(220.f * i / 48000.f) : 0.f;
        float l = in, r = in;
        s_ob.Process(&l, &r);
        if (!std::isfinite(l) || !std::isfinite(r)) { finite = false; break; }
        float a = l < 0 ? -l : l;
        if (a > peak) peak = a;
        if (i >= N - 48000) late_energy += l * l;
    }
    CHECK(finite);
    CHECK(peak < 4.f);                          // SoftLimit holds the loop
    CHECK(late_energy / 48000.f > 0.0004f);     // RMS > 0.02 six seconds after input stopped
}

TEST_CASE("oliverb core: bit-deterministic across instances") {
    static float bufA[clouds::Oliverb::kBufferSize];
    static float bufB[clouds::Oliverb::kBufferSize];
    static clouds::Oliverb obA, obB;
    obA.Init(bufA, 0x1234u);
    obB.Init(bufB, 0x1234u);
    for (clouds::Oliverb* ob : { &obA, &obB }) {
        ob->set_size(0.6f);
        ob->set_decay(0.8f);
        ob->set_lp(0.5f);
        ob->set_hp(0.01f);
        ob->set_mod_amount(200.f);
        ob->set_mod_rate(0.6f);
        ob->Prepare();
    }
    bool identical = true;
    for (int i = 0; i < 48000; ++i) {
        if (i % 96 == 0) { obA.Prepare(); obB.Prepare(); }
        float in = (i == 0) ? 1.f : 0.2f * spky::fast_sin(110.f * i / 48000.f);
        float la = in, ra = in, lb = in, rb = in;
        obA.Process(&la, &ra);
        obB.Process(&lb, &rb);
        if (la != lb || ra != rb) { identical = false; break; }
    }
    CHECK(identical);
}
