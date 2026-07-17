#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "synth/synth_engine.h"
using namespace spky;

// SynthEngine-Ebene: Kennlinien-Tests brauchen gepinnte Lane-Werte (auf
// Instrument-Ebene wandern die Lanes generativ). Muster: test_synth_engine.cpp.

static void feed(SynthEngine& e, float pitch, float filter = 1.f, float level = 1.f) {
    float t[LANE_COUNT] = { 0.f, filter, pitch, 0.f, level };
    e.set_targets(t, 0.5f);
}

// Fresh engine in "measurement" trim: pure sine, no sub, no detune.
static void fresh(SynthEngine& e, uint32_t seed = 99) {
    e.set_seed(seed);
    e.init(48000.f);
    e.set_sub(0.f);
    e.set_detune(0.f);
    e.set_cycle(1.f);
    feed(e, 0.5f);
}

static std::vector<float> render_l(SynthEngine& e, int n) {
    std::vector<float> out(n);
    for (auto& s : out) {
        float l = 0.f, r = 0.f;
        e.process(l, r);
        s = l;
    }
    return out;
}

static float rms(const std::vector<float>& v, size_t from) {
    double acc = 0.0;
    for (size_t i = from; i < v.size(); ++i) acc += (double)v[i] * v[i];
    return std::sqrt((float)(acc / (double)(v.size() - from)));
}

TEST_CASE("filt: 0 is bit-identical on the engine") {
    SynthEngine a, b;
    fresh(a);
    fresh(b);
    b.set_filt(0.f);
    a.trigger(0.5f);
    b.trigger(0.5f);
    auto va = render_l(a, 48000);
    auto vb = render_l(b, 48000);
    for (int i = 0; i < 48000; ++i) REQUIRE(va[i] == vb[i]);
}

TEST_CASE("filt: full left is silent for any lane position") {
    for (float lane : { 0.f, 0.5f, 1.f }) {
        CAPTURE(lane);
        SynthEngine e;
        fresh(e);
        feed(e, 0.5f, lane);
        e.set_filt(-1.f);
        e.trigger(0.5f);
        auto v = render_l(e, 48000);
        // 10-ms-OnePole: nach 0,2 s ist der Rest < e^-20; -80 dBFS ist grosszuegig
        CHECK(rms(v, 9600) < 1e-4f);
    }
}

TEST_CASE("filt: full right pins the cutoff fully open") {
    SynthEngine a, b;                 // A: dunkle Lane + FILT +1 ...
    fresh(a);
    feed(a, 0.5f, 0.2f);
    a.set_filt(1.f);
    fresh(b);                          // ... B: Lane offen, FILT neutral
    feed(b, 0.5f, 1.f);
    b.set_filt(0.f);
    a.trigger(0.5f);
    b.trigger(0.5f);
    auto va = render_l(a, 48000);
    auto vb = render_l(b, 48000);
    for (int i = 0; i < 48000; ++i) REQUIRE(va[i] == vb[i]);   // beide: filter_hz(1)
}

TEST_CASE("filt: bites from the first movement (no dead zone)") {
    SynthEngine a, b;
    fresh(a);
    feed(a, 0.5f, 0.5f);              // Lane mittig
    fresh(b);
    feed(b, 0.5f, 0.5f);
    b.set_filt(-0.1f);                // kleine Auslenkung muss schon wirken
    a.trigger(0.5f);
    b.trigger(0.5f);
    auto va = render_l(a, 48000);
    auto vb = render_l(b, 48000);
    float maxdiff = 0.f;
    for (int i = 0; i < 48000; ++i)
        maxdiff = std::max(maxdiff, std::abs(va[i] - vb[i]));
    CHECK(maxdiff > 1e-3f);
}

TEST_CASE("filt: sweep through the whole range is click-free") {
    SynthEngine e;
    fresh(e);
    feed(e, 0.f, 0.5f);               // 110 Hz: kleines natuerliches Sample-Delta
    e.trigger(0.f);
    render_l(e, 4800);                // Attack ausklingen lassen
    float maxstep = 0.f, prev = 0.f;
    const int blocks = 1000;          // 2 s Sweep in Control-Block-Schritten
    for (int bi = 0; bi <= blocks; ++bi) {
        e.set_filt(-1.f + 2.f * bi / blocks);
        for (int i = 0; i < SynthEngine::kCtrlInterval; ++i) {
            float l = 0.f, r = 0.f;
            e.process(l, r);
            if (bi > 0 || i > 0) maxstep = std::max(maxstep, std::abs(l - prev));
            prev = l;
        }
    }
    CHECK(maxstep < 0.05f);           // Klicks waeren Spruenge >> Signal-Delta
}
