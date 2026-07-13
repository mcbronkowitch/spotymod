#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "fx/comp.h"
using namespace spky;

static std::vector<float> sine(int n, float amp) {
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = amp * std::sin(6.2831853f * 220.f * i / 48000.f);
    return v;
}

static float rms(const std::vector<float>& v, size_t from = 0) {
    double acc = 0.0;
    for (size_t i = from; i < v.size(); ++i) acc += v[i] * v[i];
    return std::sqrt((float)(acc / (v.size() - from)));
}

// Run a mono-as-stereo signal through a fresh Comp at a given amount,
// skipping the first half second so envelopes/smoothers settle.
static std::vector<float> run(const std::vector<float>& in, float amount) {
    Comp c;
    c.init(48000.f);
    c.set_amount(amount);
    std::vector<float> out;
    out.reserve(in.size());
    for (float s : in) {
        float l = s, r = s;
        c.process(l, r);
        out.push_back(l);
    }
    return out;
}

TEST_CASE("comp: amount 0 is a bit-exact bypass") {
    Comp c;
    c.init(48000.f);
    auto in = sine(4800, 0.5f);
    for (float s : in) {
        float l = s, r = -s;
        c.process(l, r);
        CHECK(l == s);
        CHECK(r == -s);
    }
    CHECK(!c.engaged());
}

TEST_CASE("comp: loudness rises monotonically with the knob on quiet material") {
    // The spec's design intent, verified: quiet (-24 dBFS) must come UP.
    auto in = sine(96000, 0.063f);
    const float amounts[4] = {0.f, 0.33f, 0.66f, 1.f};
    float prev = -1.f;
    for (float a : amounts) {
        float level = rms(run(in, a), 24000);
        CHECK(level > prev);
        prev = level;
    }
}

TEST_CASE("comp: hot material is gain-reduced at full amount") {
    // 0.9-amp sine at amount 1: threshold -32 dB, ratio 10:1 -> heavy GR.
    // Net gain (GR + makeup) must be BELOW the makeup-only quiet case,
    // i.e. peaks come down relative to quiet material = compression.
    auto hot = sine(96000, 0.9f);
    auto quiet = sine(96000, 0.063f);
    float hot_gain = rms(run(hot, 1.f), 24000) / rms(hot, 24000);
    float quiet_gain = rms(run(quiet, 1.f), 24000) / rms(quiet, 24000);
    CHECK(quiet_gain > hot_gain);   // dynamics squeezed toward each other
    CHECK(hot_gain < 2.f);          // hot must not explode under makeup
}

TEST_CASE("comp: stereo link keeps the image") {
    // R at half of L: the SAME gain must hit both, so the ratio holds.
    Comp c;
    c.init(48000.f);
    c.set_amount(0.8f);
    auto in = sine(48000, 0.8f);
    for (size_t i = 0; i < in.size(); ++i) {
        float l = in[i], r = 0.5f * in[i];
        c.process(l, r);
        if (i > 24000 && std::fabs(in[i]) > 0.1f)
            CHECK(r == doctest::Approx(0.5f * l).epsilon(1e-5));
    }
}

TEST_CASE("comp: release slows as the knob rises (pump zone)") {
    // Step from loud to quiet; measure how long the envelope keeps the
    // gain depressed. At amount 1 (release 350 ms) recovery must take
    // longer than at amount 0.4 (release ~106 ms).
    auto recovery_gain = [](float amount) {
        Comp c;
        c.init(48000.f);
        c.set_amount(amount);
        // 1 s loud to charge the envelope
        for (int i = 0; i < 48000; ++i) {
            float l = 0.9f * std::sin(6.2831853f * 220.f * i / 48000.f);
            float r = l;
            c.process(l, r);
        }
        // 120 ms of near-silence, then read the gain
        for (int i = 0; i < 5760; ++i) { float l = 1e-4f, r = 1e-4f; c.process(l, r); }
        return c.gain_db();
    };
    // Deeper knob = slower recovery = gain still lower after 120 ms
    CHECK(recovery_gain(1.f) < recovery_gain(0.4f));
}

TEST_CASE("comp: attack bites within tens of milliseconds") {
    // Hot DC-ish input at full amount: after 50 ms the applied gain must be
    // BELOW unity (heavy GR beats the +20 dB makeup). Without a working
    // attack the envelope stays empty and the gain would sit at +20 dB.
    Comp c;
    c.init(48000.f);
    c.set_amount(1.f);
    for (int i = 0; i < 2400; ++i) { float l = 0.9f, r = 0.9f; c.process(l, r); }
    CHECK(c.gain_db() < 0.f);
}

TEST_CASE("comp: deterministic") {
    auto in = sine(48000, 0.7f);
    auto a = run(in, 0.75f);
    auto b = run(in, 0.75f);
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

TEST_CASE("comp: turning the knob back to 0 re-arms the bit-exact bypass") {
    Comp c;
    c.init(48000.f);
    c.set_amount(1.f);
    auto in = sine(24000, 0.5f);
    for (float s : in) { float l = s, r = s; c.process(l, r); }
    c.set_amount(0.f);
    // let the 2 ms amount smoother settle
    for (int i = 0; i < 4800; ++i) { float l = 0.f, r = 0.f; c.process(l, r); }
    CHECK(!c.engaged());
    for (float s : in) {
        float l = s, r = s;
        c.process(l, r);
        CHECK(l == s);
        CHECK(r == s);
    }
}

TEST_CASE("comp: makeup cannot push the envelope past the post-comp ceiling") {
    // Steady hot-ish material at dense settings: without a cap the +16 dB
    // auto-makeup lifts the post-comp envelope past full scale and the
    // master limiter downstream grinds audibly (M4.6 by-ear finding: the
    // 0:26 clip in comp_pump). The gain computer must never command a
    // gain that lifts its own envelope above the post-comp ceiling.
    Comp c;
    c.init(48000.f);
    c.set_amount(0.7f);
    float peak_out = 0.f;
    for (int i = 0; i < 96000; ++i) {
        float s = 0.5f * std::sin(6.2831853f * 220.f * i / 48000.f);
        float l = s, r = s;
        c.process(l, r);
        if (i >= 48000) peak_out = std::max(peak_out, std::fabs(l));
    }
    CHECK(peak_out <= 0.5f);
}
