#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "synth/voice.h"
using namespace spky;

static void feed_clean(Voice& v) {          // pure centered sine, wide-open filter
    v.set_env_times(0.002f, 5.f);
    v.set_morph(0.f);
    v.set_detune_cents(0.f);
    v.set_sub_level(0.f);
    v.set_cutoff_hz(14000.f);
    v.set_resonance(0.1f);
    v.set_pan(0.f);
    v.set_drift_amount(0.f);
    v.update_control(0.002f);
}

static int rising_crossings(const std::vector<float>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] <= 0.f && v[i] > 0.f) ++n;
    return n;
}

TEST_CASE("voice: an idle voice adds exactly nothing") {
    Voice v;
    v.init(48000.f, 42);
    feed_clean(v);
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 1000; ++i) v.process(l, r);
    CHECK(l == 0.f);
    CHECK(r == 0.f);
}

TEST_CASE("voice: trigger latches the frequency (zero-crossing count)") {
    Voice v;
    v.init(48000.f, 42);
    feed_clean(v);
    v.set_sustaining(true);                 // hold at 0.7 for a steady tone
    v.trigger(311.13f);                     // = 110 * 8^0.5
    std::vector<float> out(48000);
    for (auto& s : out) {
        float l = 0.f, r = 0.f;
        v.process(l, r);
        s = l;
    }
    int n = rising_crossings(out);
    CHECK(n >= 308);
    CHECK(n <= 314);
}

TEST_CASE("voice: equal-power pan - hard left silences right, center is equal") {
    Voice v;
    v.init(48000.f, 42);
    feed_clean(v);
    v.set_sustaining(true);
    v.trigger(220.f);
    v.set_pan(-1.f);
    v.update_control(0.002f);
    float suml = 0.f, sumr = 0.f;
    for (int i = 0; i < 4800; ++i) {
        float l = 0.f, r = 0.f;
        v.process(l, r);
        suml += l * l;
        sumr += r * r;
    }
    CHECK(sumr < suml * 1e-4f);             // hard left: right ~ 0
    v.set_pan(0.f);
    v.update_control(0.002f);
    suml = sumr = 0.f;
    for (int i = 0; i < 4800; ++i) {
        float l = 0.f, r = 0.f;
        v.process(l, r);
        suml += l * l;
        sumr += r * r;
    }
    CHECK(suml == doctest::Approx(sumr).epsilon(0.01));   // center: equal power
}

TEST_CASE("voice: equal-power total stays constant across the pan range") {
    std::vector<float> powers;
    for (float pan : { -1.f, -0.5f, 0.f, 0.5f, 1.f }) {
        Voice v;
        v.init(48000.f, 42);
        feed_clean(v);
        v.set_sustaining(true);
        v.trigger(220.f);
        v.set_pan(pan);
        v.update_control(0.002f);
        for (int i = 0; i < 48000; ++i) {           // settle at sustain
            float l = 0.f, r = 0.f;
            v.process(l, r);
        }
        float p = 0.f;
        for (int i = 0; i < 4800; ++i) {
            float l = 0.f, r = 0.f;
            v.process(l, r);
            p += l * l + r * r;
        }
        powers.push_back(p);
    }
    float mn = *std::min_element(powers.begin(), powers.end());
    float mx = *std::max_element(powers.begin(), powers.end());
    CHECK(mx / mn < 1.03f);                          // ~constant loudness
}

TEST_CASE("voice: drift is deterministic per seed and differs across seeds") {
    auto run = [](uint32_t seed) {
        Voice v;
        v.init(48000.f, seed);
        feed_clean(v);
        v.set_drift_amount(1.f);
        v.set_sustaining(true);
        v.trigger(220.f);
        std::vector<float> out;
        out.reserve(48000);
        for (int i = 0; i < 48000; ++i) {
            if (i % 96 == 0) v.update_control(96.f / 48000.f);   // control rate
            float l = 0.f, r = 0.f;
            v.process(l, r);
            out.push_back(l);
        }
        return out;
    };
    auto a = run(7);
    auto b = run(7);
    auto c = run(8);
    CHECK(a == b);                                   // bit-identical per seed
    bool differs = false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != c[i]) differs = true;
    CHECK(differs);                                  // seeds decorrelate
}

TEST_CASE("voice: retrigger mid-decay has no output discontinuity (steal)") {
    Voice v;
    v.init(48000.f, 42);
    feed_clean(v);
    v.set_env_times(0.01f, 0.5f);
    v.trigger(220.f);
    float prev = 0.f, max_delta = 0.f;
    for (int i = 0; i < 24000; ++i) {
        if (i == 12000) v.trigger(440.f);            // hard steal to a new pitch
        float l = 0.f, r = 0.f;
        v.process(l, r);
        if (i > 0) max_delta = std::max(max_delta, std::fabs(l - prev));
        prev = l;
    }
    // A 440 Hz sine at full level moves ~0.058/sample; a from-zero restart
    // would jump by the pre-steal level (~0.5). Retrigger-from-level +
    // phase-continuous oscillators keep the delta at waveform scale.
    CHECK(max_delta < 0.12f);
}

TEST_CASE("voice: set_vel scales output, snaps when idle, slews when active") {
    // idle snap: two identical voices, one at vel 0.5 -> half the energy
    Voice a, b;
    a.init(48000.f, 77u);
    b.init(48000.f, 77u);
    b.set_vel(0.5f);
    a.set_env_times(0.002f, 0.2f); b.set_env_times(0.002f, 0.2f);
    a.trigger(220.f); b.trigger(220.f);
    float ea = 0.f, eb = 0.f;
    for (int i = 0; i < 9600; ++i) {
        if (i % 96 == 0) { a.update_control(0.002f); b.update_control(0.002f); }
        float la = 0.f, ra = 0.f, lb = 0.f, rb = 0.f;
        a.process(la, ra); b.process(lb, rb);
        ea += la * la + ra * ra;
        eb += lb * lb + rb * rb;
    }
    CHECK(std::sqrt(eb / ea) == doctest::Approx(0.5f).epsilon(0.05));

    // active slew: changing vel mid-note must not step the output
    Voice c;
    c.init(48000.f, 77u);
    c.set_env_times(0.002f, 2.f);
    c.trigger(220.f);
    for (int i = 0; i < 4800; ++i) {
        if (i % 96 == 0) c.update_control(0.002f);
        float l = 0.f, r = 0.f; c.process(l, r);
    }
    float before_l = 0.f, before_r = 0.f;
    c.process(before_l, before_r);
    c.set_vel(0.3f);                               // no update_control yet
    float after_l = 0.f, after_r = 0.f;
    c.process(after_l, after_r);
    CHECK(std::fabs(after_l - before_l) < 0.05f);  // no instant jump
}

TEST_CASE("voice: vel 1 is bit-identical to the pre-vel path") {
    // vel defaults to exactly 1; update_control's slew term is exactly 0
    Voice a, b;
    a.init(48000.f, 5u);
    b.init(48000.f, 5u);
    b.set_vel(1.f);                                // explicit 1 == untouched
    a.trigger(330.f); b.trigger(330.f);
    for (int i = 0; i < 4800; ++i) {
        if (i % 96 == 0) { a.update_control(0.002f); b.update_control(0.002f); }
        float la = 0.f, ra = 0.f, lb = 0.f, rb = 0.f;
        a.process(la, ra); b.process(lb, rb);
        CHECK(la == lb);                           // exact
        CHECK(ra == rb);
    }
}
