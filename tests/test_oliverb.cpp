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
