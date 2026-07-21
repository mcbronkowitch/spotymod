#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include <vector>
#include "sampler/sampler_engine.h"
#include "sampler/sampler_config.h"
using namespace spky;

static constexpr size_t kFrames = 48000 * 2;   // 2 s of storage

// An engine with host memory, pre-loaded with a 441 Hz sine as content.
struct Rig {
    std::vector<SampleBuffer::Frame> mem{kFrames};
    std::vector<float> l, r;
    SamplerEngine e;

    explicit Rig(size_t content = 24000, uint32_t seed = 4242) {
        l.resize(content);
        r.resize(content);
        for (size_t i = 0; i < content; ++i) {
            l[i] = std::sin(6.2831853f * 441.f * float(i) / 48000.f);
            r[i] = l[i];
        }
        e.set_seed(seed);
        e.set_memory(mem.data(), kFrames);
        e.init(48000.f);
        e.set_cycle(1.f);
        if (content) e.load_sample(l.data(), r.data(), content);
        feed(0.5f);
    }
    // SOURCE, SIZE, PITCH, MOTION, LEVEL by lane slot.
    void feed(float pitch, float source = 0.f, float size = 0.5f,
              float motion = 0.f, float level = 1.f) {
        float t[LANE_COUNT] = { source, size, pitch, motion, level };
        e.set_targets(t, 0.5f);
    }
    std::vector<float> render(int n) {
        std::vector<float> out(n);
        for (auto& s : out) { float a = 0.f, b = 0.f; e.process(a, b); s = a; }
        return out;
    }
};

static float rms(const std::vector<float>& v, size_t from, size_t n) {
    double acc = 0.0;
    for (size_t i = from; i < from + n && i < v.size(); ++i) acc += double(v[i]) * v[i];
    return float(std::sqrt(acc / double(n)));
}

TEST_CASE("sampler: FLOW is a standing cloud -- RMS never drops out") {
    Rig g;
    g.e.set_flow(true);
    auto v = g.render(48000 * 3);

    // Prime (233) and not a divisor of 2400 (the spawn interval at the rig's
    // default SIZE=0.5 -> _grain_len 9600 -> _spawn_every 2400): a window
    // equal to the spawn interval is phase-locked to the overlap cycle, so
    // any periodic dip INSIDE a spawn period averages out identically in
    // every window and never shows up. Walking the window across the cycle
    // is what actually exposes ripple.
    const size_t win = 233;
    float lowest = 1e9f, highest = 0.f;
    for (size_t i = 4800; i + win < v.size(); i += win) {   // skip the fill-up
        const float e = rms(v, i, win);
        if (e < lowest)  lowest = e;
        if (e > highest) highest = e;
    }
    REQUIRE(highest > 0.02f);              // the cloud actually sounds
    CHECK(lowest > 0.2f * highest);        // ...and never gaps
}

TEST_CASE("sampler: STEP is silent until the gate opens, and tails off after") {
    Rig g;
    g.e.set_flow(false);
    auto pre = g.render(4800);
    for (float s : pre) CHECK(s == doctest::Approx(0.f).epsilon(0.0001));

    g.e.trigger(0.5f);
    g.e.set_gate(true);
    auto on = g.render(9600);
    CHECK(rms(on, 2400, 4800) > 0.02f);

    g.e.set_gate(false);
    auto off = g.render(48000);
    const int rel = int(sampler_cfg::kBurstReleaseS * 48000.f);
    CHECK(rms(off, 0, size_t(rel / 2)) > 0.005f);   // the release still sounds
    CHECK(rms(off, 40000, 4800) < 0.001f);          // a second later, retired
    CHECK(g.e.active_grains() == 0);
}

TEST_CASE("sampler: an empty buffer is silent under FLOW, STEP and gate") {
    Rig g(0);
    REQUIRE(g.e.is_empty());
    g.e.set_flow(true);
    auto v = g.render(9600);
    for (float s : v) CHECK(s == doctest::Approx(0.f).epsilon(0.0001));
    g.e.set_flow(false);
    g.e.trigger(0.5f);
    g.e.set_gate(true);
    auto w = g.render(9600);
    for (float s : w) CHECK(s == doctest::Approx(0.f).epsilon(0.0001));

    // Silence alone proves nothing here -- read_linear returns silence for an
    // empty buffer whatever the scheduler does. The property under test is
    // that no grain is spawned AT ALL, which only active_grains() can see.
    CHECK(g.e.active_grains() == 0);
}

TEST_CASE("sampler: nullptr memory is silent, not a crash") {
    SamplerEngine e;
    e.set_seed(7);
    e.init(48000.f);                       // set_memory never called
    float t[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 1.f };
    e.set_targets(t, 0.5f);
    e.set_flow(true);
    for (int i = 0; i < 9600; ++i) {
        float a = 1.f, b = 1.f;
        e.process_in(0.5f, 0.5f);
        e.process(a, b);
        CHECK(a == 0.f);
        CHECK(b == 0.f);
    }
    CHECK(e.is_empty());
}

TEST_CASE("sampler: SIZE maps 1 ms .. 42 s piecewise-exponentially") {
    // Task 3 opened both ends of the curve (was 20 ms .. 2 s) and removed
    // the content-length clamp entirely (see "grain length may exceed the
    // content length", below) -- so this test no longer needs a full 2 s of
    // content to observe the top of the range; the Rig's content size is
    // irrelevant to the requested length now.
    Rig g(kFrames);
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.f);
    g.render(200);
    CHECK(g.e.grain_len_samples() ==
          doctest::Approx(sampler_cfg::kSizeFloorS * 48000.f).epsilon(0.02));
    g.feed(0.5f, 0.f, 1.f);
    g.render(200);
    CHECK(g.e.grain_len_samples() ==
          doctest::Approx(sampler_cfg::kSizeCeilS * 48000.f).epsilon(0.02));
    g.feed(0.5f, 0.f, 0.5f);
    g.render(200);
    // 0.02 * 100^0.5 = 0.2 s -- the exponential midpoint, NOT the linear one
    // (~1.01 s). This is the assertion that tells the two mappings apart, and
    // it is unmoved by Task 3: 0.5 sits inside the unchanged middle segment.
    CHECK(g.e.grain_len_samples() == doctest::Approx(0.2f * 48000.f).epsilon(0.05));
}

TEST_CASE("sampler: grain length may exceed the content length") {
    // Was "grain length is clamped to the content length" through M5a. Task
    // 3 removes that clamp: read_linear folds modulo the recorded length, so
    // an over-long grain is a loop under one window, not a defect to guard
    // against.
    Rig g(4800);                           // only 100 ms of content
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 1.f);                // ask for 42 s
    g.render(200);
    CHECK(g.e.grain_len_samples() > 4800.f);
}

TEST_CASE("sampler: recording grows the content and the cloud plays while it does") {
    Rig g(0);
    REQUIRE(g.e.is_empty());
    g.e.set_flow(true);
    g.e.set_recording(true);

    std::vector<float> out;
    float heard_early = 0.f;
    for (int i = 0; i < 24000; ++i) {
        const float s = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        g.e.process_in(s, s);
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        out.push_back(a);
        if (i == 12000) heard_early = rms(out, 6000, 6000);
    }
    g.e.set_recording(false);

    CHECK(g.e.buffer_fill() > 0.f);
    CHECK_FALSE(g.e.is_empty());
    // The sound emerges UNDER the gesture. Without fill-follows this is 0.
    CHECK(heard_early > 0.001f);
}

TEST_CASE("sampler: grains never read past the write head while recording") {
    Rig g(0);
    g.e.set_flow(true);
    // SOURCE at the far end: mapped against the CURRENT fill it stays inside
    // the captured region; mapped against capacity it points two seconds past
    // the write head, into memory that has not been recorded yet.
    //
    // This is asserted on the spawn POSITION, not on loudness. read_linear
    // folds against rec_size(), so an out-of-range position still lands on
    // valid audio after folding -- no amplitude assertion can distinguish the
    // two, and with DC content every position sounds the same.
    g.feed(0.5f, 1.f, 0.3f);
    g.e.set_recording(true);

    float worst = 0.f;
    for (int i = 0; i < 24000; ++i) {
        g.e.process_in(1.f, 1.f);
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        const float over = g.e.last_spawn_pos() - float(g.e.rec_size());
        if (over > worst) worst = over;
    }
    g.e.set_recording(false);
    REQUIRE(g.e.rec_size() > 4800);

    // Not one grain was placed past the write head.
    CHECK(worst <= 0.f);
}

TEST_CASE("sampler: monitoring passes the dry input at unity") {
    // A non-empty, FLOW-active rig, so the cloud itself is not silence --
    // on an empty buffer (the previous form of this test) the monitor-off
    // branch asserts output == 0, which is true whether or not the monitor
    // flag is actually honoured: read_linear() on an empty SampleBuffer is
    // 0 regardless, so that assertion could not have failed either way.
    //
    // Two identically-seeded rigs, same FLOW/render call sequence, one with
    // monitor on and one off. process_in()'s dry values feed nothing but the
    // monitor mix here (recording is never armed), so the two clouds are
    // bit-identical (the seeded-determinism property, proven separately
    // above) and any difference between `on` and `off` at a given sample can
    // only be the monitor path's own contribution -- exactly the unity dry
    // term if honoured, exactly nothing if the flag leaked or was ignored.
    Rig on(24000, 909), off(24000, 909);
    on.e.set_flow(true);
    off.e.set_flow(true);
    on.e.set_monitor(true);
    off.e.set_monitor(false);

    on.render(4800);      // let the cloud establish, identically, on both
    off.render(4800);

    bool cloud_heard = false;
    for (int i = 0; i < 2000; ++i) {
        float al = 0.f, ar = 0.f, bl = 0.f, br = 0.f;
        on.e.process_in(0.37f, -0.21f);
        on.e.process(al, ar);
        off.e.process_in(0.37f, -0.21f);
        off.e.process(bl, br);
        if (std::fabs(bl) > 0.01f) cloud_heard = true;
        CHECK(al == doctest::Approx(bl + 0.37f).epsilon(0.001));
        CHECK(ar == doctest::Approx(br - 0.21f).epsilon(0.001));
    }
    // The branch under test (monitor off) actually has cloud content to
    // hide the monitor path behind -- the property the old empty-buffer rig
    // could not exercise.
    CHECK(cloud_heard);
}

TEST_CASE("sampler: CHOKE hold fades the cloud out and re-arms on release") {
    Rig g;
    g.e.set_flow(true);
    auto before = g.render(24000);
    REQUIRE(rms(before, 12000, 4800) > 0.02f);

    g.e.set_hold(true);
    auto during = g.render(9600);
    for (size_t i = 1; i < 500; ++i)       // click-free: no step at release
        CHECK(std::fabs(during[i] - during[i - 1]) < 0.2f);
    CHECK(rms(during, 4800, 4800) < 0.001f);
    CHECK(g.e.active_grains() == 0);

    g.e.set_hold(false);
    auto after = g.render(24000);
    CHECK(rms(after, 12000, 4800) > 0.02f);
}

TEST_CASE("sampler: identical seed and call sequence render bit-identically") {
    Rig a(24000, 31337), b(24000, 31337);
    a.e.set_flow(true);
    b.e.set_flow(true);
    auto va = a.render(48000);
    auto vb = b.render(48000);
    REQUIRE(va.size() == vb.size());
    for (size_t i = 0; i < va.size(); ++i) REQUIRE(va[i] == vb[i]);
}

TEST_CASE("sampler: clear empties the buffer and silences the cloud") {
    Rig g;
    g.e.set_flow(true);
    g.render(4800);
    REQUIRE(g.e.active_grains() > 0);      // the cloud is running...
    g.e.clear();
    CHECK(g.e.is_empty());
    CHECK(g.e.buffer_fill() == doctest::Approx(0.f));
    // ...and clear() must retire the running grains, not merely leave them
    // reading an empty buffer. Downstream silence cannot tell the two apart.
    CHECK(g.e.active_grains() == 0);
    auto v = g.render(24000);
    CHECK(rms(v, 12000, 4800) < 0.001f);
}

TEST_CASE("sampler: loading new content restarts the cloud immediately") {
    Rig g(0);
    std::vector<float> n(48000);
    uint32_t s = 0x13579BDFu;
    for (auto& v : n) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;   // the engine's own xorshift
        v = (s * (1.f / 4294967296.f)) * 2.f - 1.f;
    }
    g.e.set_flow(true);
    // Long grains: at SIZE=0.8 (kneeHi -- still the unchanged M5a curve, so
    // this test isn't about Task 3's new extremes) the spawn interval is a
    // few thousand samples, so a countdown that survives the load would
    // hold the cloud silent for a noticeable stretch. Was SIZE=1.0 before
    // Task 3, when the top of the range was 2 s content-clamped down to
    // this rig's content; now SIZE=1.0 reaches 42 s and the grain's own
    // attack window (a fraction of grain length) would still be ramping in
    // at the 200 ms mark below, so it can no longer stand in for "long".
    g.feed(0.5f, 0.f, 0.8f);
    g.e.load_sample(n.data(), n.data(), n.size());
    g.render(48000);                       // let the cloud settle and the
                                           // counter land mid-interval
    REQUIRE(g.e.active_grains() > 0);

    // Load again -- _kill_all() retires every grain, so nothing masks a
    // stale countdown.
    g.e.load_sample(n.data(), n.data(), n.size());
    CHECK(g.e.active_grains() == 0);       // the load really did clear them

    auto v = g.render(9600);               // 200 ms
    // The cloud must be audible well inside one old spawn interval.
    CHECK(rms(v, 0, 4800) > 0.01f);
}

// --- Task 4: chord distribution and scatter -------------------------------

// Collect the spawn parameters of `want` grains under the current settings.
struct SpawnStats {
    std::vector<float> ratio, pan, pos;
};
static SpawnStats collect(Rig& g, int want) {
    SpawnStats s;
    int last = g.e.spawn_count();
    int guard = 0;
    while (int(s.ratio.size()) < want && guard++ < 4000000) {
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        if (g.e.spawn_count() != last) {
            last = g.e.spawn_count();
            s.ratio.push_back(g.e.last_spawn_ratio());
            s.pan.push_back(g.e.last_spawn_pan());
            s.pos.push_back(g.e.last_spawn_pos());
        }
    }
    return s;
}

TEST_CASE("sampler: COLOR 0 puts every grain on the root") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.2f, 0.f);          // MOTION 0 -> no octave scatter
    const float root[1] = { 0.6f };
    g.e.set_chord(root, 1);

    auto s = collect(g, 200);
    REQUIRE(s.ratio.size() == 200);        // the sample size is a precondition
    const float expect = std::pow(8.f, 0.6f - 0.5f);
    for (float rr : s.ratio) CHECK(rr == doctest::Approx(expect).epsilon(0.001));
}

TEST_CASE("sampler: a chord spreads grains round-robin over its notes") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.2f, 0.f);          // MOTION 0: no octave scatter
    const float chord[4] = { 0.40f, 0.50f, 0.60f, 0.70f };
    g.e.set_chord(chord, 4);

    auto s = collect(g, 400);
    REQUIRE(s.ratio.size() == 400);

    // Every ratio must be one of the four chord ratios...
    int hits[4] = { 0, 0, 0, 0 };
    for (float rr : s.ratio) {
        bool matched = false;
        for (int i = 0; i < 4; ++i) {
            const float want = std::pow(8.f, chord[i] - 0.5f);
            if (std::fabs(rr - want) < 0.001f) { ++hits[i]; matched = true; }
        }
        CHECK(matched);
    }
    // ...and all four must be covered roughly evenly (round-robin, not
    // "always the root", which would still satisfy the check above).
    for (int i = 0; i < 4; ++i) {
        CHECK(hits[i] > 60);
        CHECK(hits[i] < 140);
    }
}

TEST_CASE("sampler: MOTION 0 is tight and centred; MOTION 1 spreads") {
    Rig tight;
    tight.e.set_flow(true);
    tight.feed(0.5f, 0.5f, 0.2f, 0.f);     // MOTION 0
    auto a = collect(tight, 300);
    REQUIRE(a.pos.size() == 300);

    // Position: every grain on the same point.
    float pmin = 1e9f, pmax = -1e9f;
    for (float p : a.pos) { if (p < pmin) pmin = p; if (p > pmax) pmax = p; }
    CHECK(pmax - pmin < 1.f);
    // Pan: dead centre.
    for (float p : a.pan) CHECK(std::fabs(p) < 0.001f);

    Rig wide;
    wide.e.set_flow(true);
    wide.feed(0.5f, 0.5f, 0.2f, 1.f);      // MOTION 1
    auto b = collect(wide, 300);
    REQUIRE(b.pos.size() == 300);

    float qmin = 1e9f, qmax = -1e9f;
    for (float p : b.pos) { if (p < qmin) qmin = p; if (p > qmax) qmax = p; }
    const float content = 24000.f;
    // Spread is real...
    CHECK(qmax - qmin > 0.5f * sampler_cfg::kScatterPosFrac * content);
    // ...and bounded by the spec range (plus a little slack for the fold).
    CHECK(qmax - qmin < 2.2f * sampler_cfg::kScatterPosFrac * content);

    float panmin = 1e9f, panmax = -1e9f;
    for (float p : b.pan) { if (p < panmin) panmin = p; if (p > panmax) panmax = p; }
    CHECK(panmin < -0.7f);
    CHECK(panmax > 0.7f);
}

TEST_CASE("sampler: MOTION 1 scatters some chord notes an octave away") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.5f, 0.2f, 1.f);         // MOTION 1
    const float chord[3] = { 0.45f, 0.55f, 0.65f };
    g.e.set_chord(chord, 3);

    auto s = collect(g, 600);
    REQUIRE(s.ratio.size() == 600);

    int octaves = 0, plain = 0;
    for (float rr : s.ratio) {
        for (int i = 0; i < 3; ++i) {
            const float base = std::pow(8.f, chord[i] - 0.5f);
            if (std::fabs(rr - base) < 0.001f) ++plain;
            else if (std::fabs(rr - base * 2.f) < 0.002f ||
                     std::fabs(rr - base * 0.5f) < 0.002f) ++octaves;
        }
    }
    CHECK(plain + octaves == 600);          // nothing lands off the chord
    // The probability is kScatterOctProb; with 600 draws the binomial band is
    // tight enough to assert both that it happens and that it stays a spice.
    CHECK(octaves > 600 * sampler_cfg::kScatterOctProb * 0.5f);
    CHECK(octaves < 600 * sampler_cfg::kScatterOctProb * 1.6f);
}

TEST_CASE("sampler: MOTION 1 jitters the spawn timing, MOTION 0 does not") {
    // Regular spawn timing at MOTION 0 means the gaps between spawns are all
    // equal. This test reads the gaps directly rather than inferring them.
    auto gaps = [](Rig& g, int want) {
        std::vector<int> out;
        int last_count = g.e.spawn_count(), since = 0;
        while (int(out.size()) < want) {
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
            ++since;
            if (g.e.spawn_count() != last_count) {
                last_count = g.e.spawn_count();
                out.push_back(since);
                since = 0;
            }
        }
        return out;
    };

    Rig tight;
    tight.e.set_flow(true);
    tight.feed(0.5f, 0.f, 0.3f, 0.f);
    auto a = gaps(tight, 60);
    a.erase(a.begin());                     // the first gap is the fill-up
    int amin = 1 << 30, amax = 0;
    for (int v : a) { if (v < amin) amin = v; if (v > amax) amax = v; }
    CHECK(amax - amin <= 1);                // regular, within rounding

    Rig wide;
    wide.e.set_flow(true);
    wide.feed(0.5f, 0.f, 0.3f, 1.f);
    auto b = gaps(wide, 60);
    b.erase(b.begin());
    int bmin = 1 << 30, bmax = 0;
    for (int v : b) { if (v < bmin) bmin = v; if (v > bmax) bmax = v; }
    CHECK(bmax - bmin > 3);                 // genuinely jittered
}

TEST_CASE("sampler: a different seed changes the scattered render") {
    Rig a(24000, 31337), b(24000, 999);
    a.e.set_flow(true);
    b.e.set_flow(true);
    a.feed(0.5f, 0.5f, 0.2f, 1.f);
    b.feed(0.5f, 0.5f, 0.2f, 1.f);
    auto va = a.render(48000);
    auto vb = b.render(48000);
    float diff = 0.f;
    for (size_t i = 0; i < va.size(); ++i) diff += std::fabs(va[i] - vb[i]);
    CHECK(diff > 1.f);
}

// --- Task 5: tape/digital, reverse, voice row -----------------------------

TEST_CASE("sampler: Digital holds the grain length; Tape scales it by 1/ratio") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.3f, 0.f);          // MOTION 0: no octave scatter
    const float up[1] = { 0.5f + 1.f / 6.f };   // +6 semitones -> ratio ~1.414
    g.e.set_chord(up, 1);

    g.e.set_tape_mode(false);
    auto d = collect(g, 40);
    REQUIRE(d.ratio.size() == 40);
    const int digital_len = g.e.last_spawn_len();

    g.e.set_tape_mode(true);
    auto t = collect(g, 40);
    REQUIRE(t.ratio.size() == 40);
    const int tape_len = g.e.last_spawn_len();

    const float ratio = g.e.last_spawn_ratio();
    REQUIRE(ratio > 1.3f);                  // the precondition the test rests on
    CHECK(tape_len == doctest::Approx(float(digital_len) / ratio).epsilon(0.05));

    // And at unity ratio the two modes agree (else "scales by 1/ratio" could
    // be any constant scaling).
    const float unity[1] = { 0.5f };
    g.e.set_chord(unity, 1);
    g.e.set_tape_mode(false);
    collect(g, 20);
    const int d2 = g.e.last_spawn_len();
    g.e.set_tape_mode(true);
    collect(g, 20);
    CHECK(g.e.last_spawn_len() == doctest::Approx(float(d2)).epsilon(0.05));
}

TEST_CASE("sampler: ATK and DEC shape the window from soft to percussive") {
    auto peak_at = [](Rig& g, float atk, float dec, int probe) {
        g.e.set_window_attack(atk);
        g.e.set_window_decay(dec);
        g.e.set_flow(true);
        g.feed(0.5f, 0.f, 0.5f, 0.f);
        return g.render(probe);
    };
    // A long attack must reach a lower level early in the grain than a short
    // one. Measured on the first 20 ms of a fresh cloud.
    Rig soft, hard;
    auto s = peak_at(soft, 1.f, 1.f, 960);
    auto h = peak_at(hard, 0.f, 0.f, 960);
    float se = 0.f, he = 0.f;
    for (int i = 0; i < 480; ++i) { se += std::fabs(s[i]); he += std::fabs(h[i]); }
    CHECK(he > se * 1.3f);
}

TEST_CASE("sampler: SUB sends a share of grains an octave down") {
    Rig none;
    none.e.set_flow(true);
    none.feed(0.5f, 0.f, 0.2f, 0.f);
    const float root[1] = { 0.5f };
    none.e.set_chord(root, 1);
    none.e.set_sub(0.f);
    auto a = collect(none, 300);
    REQUIRE(a.ratio.size() == 300);
    for (float rr : a.ratio) CHECK(rr == doctest::Approx(1.f).epsilon(0.001));

    Rig half;
    half.e.set_flow(true);
    half.feed(0.5f, 0.f, 0.2f, 0.f);
    half.e.set_chord(root, 1);
    half.e.set_sub(0.5f);
    auto b = collect(half, 400);
    REQUIRE(b.ratio.size() == 400);
    int down = 0;
    for (float rr : b.ratio) if (rr < 0.75f) ++down;
    // Expected share is sub_n (0.5 here) * kSubMaxShare, not a bare 0.5: tie
    // the bounds to the constant so they track it if it is ever retuned.
    const float expect = 400 * 0.5f * sampler_cfg::kSubMaxShare;
    CHECK(down > expect * 0.6f);
    CHECK(down < expect * 1.4f);
}

TEST_CASE("sampler: DTUN spreads grain ratios in cents, 0 is exact") {
    Rig none;
    none.e.set_flow(true);
    none.feed(0.5f, 0.f, 0.2f, 0.f);
    const float root[1] = { 0.5f };
    none.e.set_chord(root, 1);
    none.e.set_detune(0.f);
    auto a = collect(none, 200);
    REQUIRE(a.ratio.size() == 200);
    for (float rr : a.ratio) CHECK(rr == doctest::Approx(1.f).epsilon(0.0005));

    Rig wide;
    wide.e.set_flow(true);
    wide.feed(0.5f, 0.f, 0.2f, 0.f);
    wide.e.set_chord(root, 1);
    wide.e.set_detune(1.f);
    auto b = collect(wide, 300);
    REQUIRE(b.ratio.size() == 300);
    float lo = 1e9f, hi = -1e9f;
    for (float rr : b.ratio) { if (rr < lo) lo = rr; if (rr > hi) hi = rr; }
    // +-35 cents -> ratio band of 2^(70/1200) ~= 1.041 wide.
    const float band = std::pow(2.f, 2.f * sampler_cfg::kDetuneCeilCt / 1200.f);
    // band is only ~4% above unity (2^(70/1200)), so a lower bound expressed
    // as a fraction OF band (as first drafted -- band * 0.7) stays below 1.0
    // and is satisfied by hi/lo == 1.0, the exact value produced when the
    // detune multiply is a no-op. That let a dropped-multiply mutation
    // survive. Bounding the reach relative to unity instead -- at least
    // halfway from 1.0 to the full band -- makes the spread itself the
    // observable, so a no-op detune fails this assertion.
    CHECK(hi / lo > 1.f + (band - 1.f) * 0.5f);
    CHECK(hi / lo < band * 1.15f);
}

TEST_CASE("sampler: FILT full left fades to silence at ANY lane position") {
    // The FILT invariant, mirrored from the synth (tests/test_filt.cpp).
    for (float lane : { 0.f, 0.25f, 0.5f, 0.75f, 1.f }) {
        Rig g;
        g.e.set_flow(true);
        g.feed(0.5f, 0.f, lane, 0.f);       // lane == the FILTER/SIZE slot
        g.e.set_filt(-1.f);
        auto v = g.render(24000);
        CHECK(rms(v, 12000, 4800) < 0.001f);
    }
    // ...and full right is emphatically not silent, at the same lane values.
    Rig open;
    open.e.set_flow(true);
    open.feed(0.5f, 0.f, 0.5f, 0.f);
    open.e.set_filt(1.f);
    auto v = open.render(24000);
    CHECK(rms(v, 12000, 4800) > 0.01f);
}

TEST_CASE("sampler: RES boosts content parked at the cutoff") {
    // A resonant SVF lowpass peaks near its own cutoff: content sitting right
    // at that frequency comes out louder than through the same filter flat.
    // The cutoff is FILT's to move (sampler_config.h: kFiltNeutral is the
    // resting position, LANE_SIZE plays no part -- lane 1 is grain length,
    // not filter, in the sampler). Solve cutoff_hz(n) = kCutoffMinHz *
    // (kCutoffMaxHz/kCutoffMinHz)^n for the normalized rail position n that
    // puts the cutoff right on the Rig's 441 Hz content, then invert
    // _update_control's `n_raw = kFiltNeutral + off` / FILT-knob mapping to
    // find the set_filt() value that produces that n.
    const float target_n =
        std::log(441.f / sampler_cfg::kCutoffMinHz) /
        std::log(sampler_cfg::kCutoffMaxHz / sampler_cfg::kCutoffMinHz);
    const float off = target_n - sampler_cfg::kFiltNeutral;
    // off = filt_amt directly on the right half; off = kFiltLeftScale *
    // filt_amt on the left half (sampler_engine.cpp's _update_control), so
    // dividing back out on that side recovers the knob value.
    const float filt_amt = off < 0.f ? off / sampler_cfg::kFiltLeftScale : off;

    Rig flat;
    flat.e.set_flow(true);
    flat.feed(0.5f, 0.5f, 0.5f, 0.f, 1.f);     // PITCH unity, MOTION 0: steady tone
    flat.e.set_filt(filt_amt);
    flat.e.set_resonance(0.f);
    auto vf = flat.render(96000);
    const float rms_flat = rms(vf, 48000, 48000);

    Rig res(24000, 4242);                      // same content, seed and calls as `flat`
    res.e.set_flow(true);
    res.feed(0.5f, 0.5f, 0.5f, 0.f, 1.f);
    res.e.set_filt(filt_amt);
    res.e.set_resonance(0.9f);
    auto vr = res.render(96000);
    const float rms_res = rms(vr, 48000, 48000);

    // Measured ~0.32 (flat) vs ~11.7 (resonant) -- a ~36x lift, comfortably
    // clear of a 2x bar, so 2x leaves ample margin while still failing a
    // no-op SetRes.
    CHECK(rms_flat > 0.02f);           // the tone is actually there to boost
    CHECK(rms_res > rms_flat * 2.f);   // ...and resonance measurably lifts it
}

TEST_CASE("sampler: Reverse plays the material backwards") {
    Rig fwd(24000, 555), rev(24000, 555);
    // SOURCE must be off zero here. The rig's content is a stationary 441 Hz
    // sine over exactly 220.5 cycles in 24000 samples, so content[k] ==
    // content[N-k] for every k (sin(w(N-k)) = sin(wN - wk) = sin(pi - wk) =
    // sin(wk) when wN == pi mod 2pi, which holds exactly for this N/frequency
    // pair). At SOURCE 0 a reverse grain folds its negative position to
    // content[N-k], which by that identity equals forward's content[k] --
    // sample for sample -- so the two renders coincide almost exactly and
    // the property under test is unobservable, whatever the implementation
    // does. A nonzero SOURCE reads content[p-k] vs content[p+k], which only
    // coincide for the measure-zero set of p with cos(w*p) == 0; 0.3 is not
    // one of them.
    fwd.feed(0.5f, 0.3f);
    rev.feed(0.5f, 0.3f);
    fwd.e.set_flow(true);
    rev.e.set_flow(true);
    rev.e.set_reverse(true);
    auto a = fwd.render(24000);
    auto b = rev.render(24000);
    float diff = 0.f, energy = 0.f;
    for (size_t i = 0; i < a.size(); ++i) {
        diff   += std::fabs(a[i] - b[i]);
        energy += std::fabs(b[i]);
    }
    CHECK(diff > 10.f);                     // genuinely different...
    CHECK(energy > 10.f);                   // ...and not just silent
}

// --- std::pow off the grain-spawn path (Task 1, sampler-deck) -------------

TEST_CASE("sampler: detune_factor matches std::pow over the full DTUN range") {
    // DTUN is bounded to +-kDetuneCeilCt cents by _next_ratio, so the
    // approximation only ever has to be right on that interval. Assert it
    // there and one cent beyond each end, so a future widening of
    // kDetuneCeilCt fails here loudly instead of drifting silently.
    for (int i = -36; i <= 36; ++i) {
        const float cents = static_cast<float>(i);
        const float want  = std::pow(2.f, cents / 1200.f);
        const float got   = spky::test_detune_factor(cents);
        // Brief's original bound was 1e-7f * want -- tighter than one float
        // ULP near 1.0 (FLT_EPSILON == 1.1920929e-7). At cents == 2 the true
        // value sits almost exactly on a rounding boundary, so std::pow and
        // the polynomial (independently rounded, different code paths) land
        // on adjacent representable floats: a 1-ULP double-rounding
        // artifact, not an approximation error (confirmed against the exact
        // value by hand: both floats are correctly rounded to their nearest
        // representable neighbour). Widened to 4 ULP for headroom across
        // compilers/optimization levels; still four orders of magnitude
        // tighter than would be needed to catch a wrong coefficient.
        CHECK(std::fabs(got - want) < 4.f * std::numeric_limits<float>::epsilon() * want);
    }
    // Exactly zero must be exactly one: _next_ratio skips the multiply at
    // cents == 0, and a factor of 1.0 - epsilon there would make DTUN = 0
    // renders differ from today's.
    CHECK(spky::test_detune_factor(0.f) == 1.f);
}

TEST_CASE("sampler: hoisting the chord ratios leaves spawned ratios unchanged") {
    // The property: precomputing ratio_for(_chord[i]) once per control tick
    // must not change any spawned grain's ratio in the round-robin (COLOR >
    // 0) branch -- this refactor is behaviour-preserving, which is the only
    // reason it is allowed to touch a path the Rng draws on.
    //
    // (The latched single-note branch reads a separate cache, _burst_ratio,
    // set at trigger time rather than _chord_ratio[0] -- see the next test
    // for why, and _burst_ratio's declaration in sampler_engine.h.)
    Rig g(24000, 0x1234u);
    g.feed(/*pitch*/0.5f, /*source*/0.5f, /*size*/0.5f, /*motion*/0.f);
    const float chord[3] = { 0.40f, 0.55f, 0.70f };
    g.e.set_chord(chord, 3);
    g.e.set_flow(true);

    std::vector<float> ratios;
    int last = g.e.spawn_count();
    int guard = 0;
    while (int(ratios.size()) < 16 && guard++ < 4000000) {
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        if (g.e.spawn_count() != last) {
            last = g.e.spawn_count();
            ratios.push_back(g.e.last_spawn_ratio());
        }
    }
    REQUIRE(ratios.size() == 16);

    // Every ratio must be finite and positive -- what the hoist preserves.
    // Checking the exact sequence would also catch a draw-order change, so
    // do that separately: the golden-vector test below already owns draw
    // order, and duplicating it here would give two tests one reason to
    // fail.
    for (float r : ratios) {
        CHECK(std::isfinite(r));
        CHECK(r > 0.f);
    }
}

TEST_CASE("sampler: a latched single-note STEP burst holds its ratio while PITCH drifts underneath it") {
    // Property under test: _next_ratio's latched branch (latched &&
    // _chord_n <= 1) must read _burst_ratio, frozen at the trigger() that
    // set _burst_pitch -- NOT _chord_ratio[0], which tracks _chord[0] live.
    // Part::_control_tick calls SamplerEngine::set_chord() unconditionally
    // every 96 samples regardless of whether a burst is currently latched
    // (part.cpp:138), so a single-note STEP burst held under PITCH
    // modulation (vibrato, or any MOD lane targeting PITCH) hits exactly
    // this every control tick. Nothing else in this file drives PITCH that
    // way -- every other test sets it once via feed()/set_chord() and holds
    // it steady -- so this is the one case that would have silently caught
    // (or missed) the Task 1 hoist reading the wrong cache.
    Rig g;
    g.e.set_flow(false);            // STEP mode
    g.e.trigger(0.3f);              // single note: _chord_n == 1, now latched
    g.e.set_gate(true);

    std::vector<float> ratios;
    int last = g.e.spawn_count();
    int sample = 0;
    int guard = 0;
    while (int(ratios.size()) < 12 && guard++ < 400000) {
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        ++sample;
        if (sample % sampler_cfg::kCtrlInterval == 0) {
            // Simulate Part::_control_tick refreshing the live chord surface
            // underneath the still-latched burst -- what a PITCH-lane LFO
            // would do, without ever calling trigger()/trigger_chord() again.
            const float drifted = 0.3f + 0.02f * float(sample % 20);
            g.e.set_chord(&drifted, 1);
        }
        if (g.e.spawn_count() != last) {
            last = g.e.spawn_count();
            ratios.push_back(g.e.last_spawn_ratio());
        }
    }
    REQUIRE(ratios.size() == 12);
    // Every grain in the burst spawns at the SAME ratio: the one latched at
    // trigger() time, unmoved by the live PITCH drift injected above.
    for (float r : ratios) CHECK(r == ratios[0]);
}

// --- golden vector: the Rng draw order is a hard contract -----------------
//
// Every other test in this suite (and in test_sampler_part.cpp) checks a
// MARGINAL distribution: position spread, octave share, timing jitter, sub
// share, detune band. Successive xorshift32 outputs are all uniform, so
// swapping the order of any two draws in _spawn_one()/_next_ratio() leaves
// every one of those marginal distributions unchanged -- the whole suite
// stays green under a reorder. The determinism test ("identical seed and
// call sequence render bit-identically") compares two instances of the SAME
// build against each other, so it cannot see a reorder either: both
// instances would still agree with each other, just not with a differently
// ordered build.
//
// This test pins down the actual SEQUENCE of drawn values -- position, pan,
// ratio and length together, per spawn, in order -- so a reordering of the
// draws (position, pan, octave, timing, sub, detune -- the contract stated
// in _spawn_one()'s comment), a change to the seed decorrelation constant,
// or a change to the SOURCE-to-position mapping all show up as a changed
// tuple somewhere in the first 20 spawns, even though none of them would
// move any marginal statistic.
//
// The numbers below are a GOLDEN VECTOR: captured once from a known-good
// build, not derived from first principles. If you are reading this because
// the test just failed after a change that looks unrelated to the sampler
// (e.g. a draw got reordered, added, or removed anywhere in _spawn_one() or
// _next_ratio(), the seed-decorrelation constant in SamplerEngine::init
// changed, or the `- 1.f` in the SOURCE/span mapping moved) -- STOP. Ask
// *why* the numbers changed before updating them. Updating them to make the
// test pass again silently re-opens exactly the hole this test exists to
// close.
TEST_CASE("sampler: golden vector -- Rng draw order and SOURCE mapping are locked") {
    Rig g(24000, 20260718u);
    g.e.set_flow(true);
    // MOTION > 0 so position jitter, pan spread and octave scatter are all
    // live; a multi-note chord so the round-robin draw is exercised too;
    // SUB and DTUN nonzero so their draws leave a mark on the ratio as well
    // (both draw from the Rng every spawn regardless of knob value, but a
    // zero knob makes the draw's RESULT invisible in the ratio -- nonzero
    // makes the whole draw order observable in one number).
    g.feed(/*pitch*/0.5f, /*source*/0.5f, /*size*/0.3f, /*motion*/1.0f);
    const float chord[3] = { 0.40f, 0.55f, 0.70f };
    g.e.set_chord(chord, 3);
    g.e.set_sub(0.4f);
    g.e.set_detune(0.5f);

    struct Spawn { float pos, pan, ratio; int len; };
    std::vector<Spawn> got;
    int last = g.e.spawn_count();
    int guard = 0;
    while (int(got.size()) < 20 && guard++ < 4000000) {
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        if (g.e.spawn_count() != last) {
            last = g.e.spawn_count();
            got.push_back({ g.e.last_spawn_pos(), g.e.last_spawn_pan(),
                             g.e.last_spawn_ratio(), g.e.last_spawn_len() });
        }
    }
    REQUIRE(got.size() == 20);

    // Golden vector: seed 20260718, content 24000 (441 Hz sine, Rig
    // default), pitch 0.5 / source 0.5 / size 0.3 / motion 1.0, chord
    // {0.40, 0.55, 0.70}, sub 0.4, detune 0.5. (last_spawn_pos, last_spawn_pan,
    // last_spawn_ratio, last_spawn_len) for the first 20 spawns under FLOW.
    // Captured once from a known-good build -- see the file-level comment
    // above this test before changing any of these numbers.
    static const Spawn golden[20] = {
        // pos,            pan,          ratio,        len
        // Updated for kScatterPosFrac 0.25 -> 1.0
        { 12803.53027344f, -0.69252360f, 0.20196824f, 3821 },
        {  2237.63378906f,  0.98490131f, 0.55502838f, 3821 },
        { 13720.91796875f, -0.30681819f, 1.53079367f, 3821 },
        {  1708.38085938f, -0.88041586f, 0.40453154f, 3821 },
        { 19170.82031250f, -0.11837190f, 0.55053788f, 3821 },
        { 14489.21093750f,  0.86559057f, 0.75218982f, 3821 },
        { 15170.44531250f,  0.43894804f, 0.40997744f, 3821 },
        {  4912.32128906f,  0.79144990f, 0.55878091f, 3821 },
        {   870.30957031f,  0.07455552f, 1.52497661f, 3821 },
        {  2560.60449219f,  0.83863580f, 0.40733621f, 3821 },
        {  7252.95117188f, -0.27693748f, 0.55035543f, 3821 },
        { 17490.03710938f, -0.75745511f, 1.52152061f, 3821 },
        {  2490.95703125f,  0.96780789f, 0.40774995f, 3821 },
        {  2534.66406250f,  0.42561364f, 0.55428278f, 3821 },
        { 12696.35742188f, -0.26600885f, 1.51086748f, 3821 },
        { 14777.22656250f, -0.27030134f, 0.81130999f, 3821 },
        { 17399.78125000f, -0.78998744f, 1.10466695f, 3821 },
        { 14736.56640625f, -0.03883266f, 0.76514953f, 3821 },
        { 17097.07421875f, -0.89893818f, 0.81913930f, 3821 },
        { 22963.72070312f, -0.89280307f, 2.21407151f, 3821 },
    };

    // Absolute, not relative, tolerance: the regression this test exists to
    // catch (the `- 1.f` dropped from the SOURCE/span mapping) shifts pos by
    // only ~0.5 samples out of a value in the thousands -- a RELATIVE
    // epsilon like doctest::Approx's default (eps * max(|a|,|b|,1)) would
    // swallow that shift completely at pos's magnitude and defeat the whole
    // point of this test. Bounds are tight enough to fail on a half-sample
    // pos shift, a detectable pan/ratio change from a swapped draw, or a
    // rescaled draw from a changed seed constant, while leaving room for
    // ordinary cross-build float rounding in the last couple of ULPs.
    for (size_t i = 0; i < 20; ++i) {
        INFO("spawn #", i);
        CHECK(std::fabs(got[i].pos   - golden[i].pos)   < 0.01f);
        CHECK(std::fabs(got[i].pan   - golden[i].pan)   < 0.00002f);
        CHECK(std::fabs(got[i].ratio - golden[i].ratio) < 0.00002f);
        CHECK(got[i].len == golden[i].len);
    }
}

// --- Task 2: MOTION scatter range ----

TEST_CASE("sampler: MOTION at full scatters across the whole buffer") {
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0x77u);
    eng.init(48000.f);

    // Generate a 220 Hz sine wave and load it
    std::vector<float> l(48000), r(48000);
    for (size_t i = 0; i < 48000; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    eng.load_sample(l.data(), r.data(), 48000);

    // SOURCE=0.5, MOTION=1.0, SIZE=0.5
    float targets[LANE_COUNT] = { 0.5f, 0.5f, 0.5f, 1.f, 0.8f };
    eng.set_targets(targets, 0.5f);
    eng.set_flow(true);

    float lo = 1e9f, hi = -1e9f;
    int seen = 0;
    for (int i = 0; i < 48000 * 20 && seen < 400; ++i) {
        float l = 0.f, r = 0.f;
        const unsigned before = eng.spawn_count();
        eng.process(l, r);
        if (eng.spawn_count() != before) {
            const float p = eng.last_spawn_pos();
            lo = p < lo ? p : lo;
            hi = p > hi ? p : hi;
            ++seen;
        }
    }
    REQUIRE(seen >= 400);

    // SOURCE is pinned mid-buffer, so the reachable set is a window of width
    // 2 * kScatterPosFrac * content centred there (the wrap only matters at
    // the ends). At the old 0.25 that window spans half the buffer and this
    // fails; at 1.0 it spans all of it.
    const float content = 48000.f;
    CHECK(lo < 0.10f * content);
    CHECK(hi > 0.90f * content);
}

TEST_CASE("sampler: MOTION at zero does not scatter position at all") {
    // The companion property. Without this, a test that only checks the
    // spread passes just as well against a mapping that ignores MOTION and
    // scatters everything all the time.
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0x77u);
    eng.init(48000.f);

    // Generate a 220 Hz sine wave and load it
    std::vector<float> l(48000), r(48000);
    for (size_t i = 0; i < 48000; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    eng.load_sample(l.data(), r.data(), 48000);

    // SOURCE=0.5, MOTION=0.0, SIZE=0.5
    float targets[LANE_COUNT] = { 0.5f, 0.5f, 0.5f, 0.f, 0.8f };
    eng.set_targets(targets, 0.5f);
    eng.set_flow(true);

    float first = -1.f;
    int seen = 0;
    for (int i = 0; i < 48000 * 50 && seen < 50; ++i) {
        float l = 0.f, r = 0.f;
        const unsigned before = eng.spawn_count();
        eng.process(l, r);
        if (eng.spawn_count() != before) {
            if (first < 0.f) first = eng.last_spawn_pos();
            CHECK(eng.last_spawn_pos() == doctest::Approx(first));
            ++seen;
        }
    }
    REQUIRE(seen >= 50);
}

// --- Task 3: SIZE opens at both ends; the CPU floor moves to the spawn interval ---

TEST_CASE("sampler: SIZE is unchanged over the middle of its travel") {
    // The "on top, not instead" contract. Anything in [0.2, 0.8] must give
    // exactly the M5a value, so that everything already listened to stays
    // where it was.
    using namespace spky::sampler_cfg;
    for (float n : {0.20f, 0.35f, 0.50f, 0.65f, 0.80f}) {
        const float want = kSizeMinS * std::pow(kSizeRange, n);
        CHECK(spky::test_size_seconds(n) == doctest::Approx(want).epsilon(1e-6));
    }
}

TEST_CASE("sampler: SIZE reaches both new extremes") {
    using namespace spky::sampler_cfg;
    CHECK(spky::test_size_seconds(0.f) == doctest::Approx(kSizeFloorS).epsilon(1e-5));
    CHECK(spky::test_size_seconds(1.f) == doctest::Approx(kSizeCeilS).epsilon(1e-5));
}

TEST_CASE("sampler: SIZE is continuous at both knees and monotonic throughout") {
    using namespace spky::sampler_cfg;
    // Continuity: a jump at a knee would be an audible click when SIZE is
    // swept, which is the failure this piecewise curve most invites.
    //
    // Deviation from the brief's literal offset here: the brief's Step 2
    // used a +-1e-4 offset, which this curve genuinely fails at kneeHi
    // (measured: |above-below| = 0.001947, tolerance 1e-3*above = 0.000798)
    // -- NOT because the curve is discontinuous (it is exactly continuous
    // in value at both knees analytically: the top/bottom branches are
    // defined to equal the middle branch's value at the knee itself), but
    // because the curve is deliberately kinked in SLOPE there (per the
    // sampler_config.h comment, "the kinks are audible ... which is the
    // point"). At kneeHi the outer segment's slope is ~4.3x the middle
    // segment's, so a symmetric +-1e-4 window straddling the knee picks up
    // that slope jump, not a value jump, and the picked-up amount already
    // exceeds a 1e-3 relative tolerance on its own. A 1e-5 offset shrinks
    // that same slope-driven term by ~10x (verified numerically) while
    // still failing loudly on an actual C0 discontinuity, which would be
    // orders of magnitude larger (~kSizeMinS or more) and independent of
    // the offset's size.
    for (float knee : {kSizeKneeLo, kSizeKneeHi}) {
        const float below = spky::test_size_seconds(knee - 1e-5f);
        const float above = spky::test_size_seconds(knee + 1e-5f);
        CHECK(std::fabs(above - below) < 1e-3f * above);
    }
    // Monotonic: a non-monotonic segment would make part of the knob travel
    // backwards, which no amount of listening would forgive.
    float prev = spky::test_size_seconds(0.f);
    for (int i = 1; i <= 1000; ++i) {
        const float cur = spky::test_size_seconds(static_cast<float>(i) / 1000.f);
        CHECK(cur >= prev);
        prev = cur;
    }
}

TEST_CASE("sampler: a grain may be longer than the material it reads") {
    // The content clamp is gone. read_linear folds, so an over-long grain is
    // a loop with a window over it -- assert the LENGTH, not the sound,
    // because the sound is exactly what the fold makes indistinguishable.
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000 * 2);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0x99u);
    eng.init(48000.f);

    std::vector<float> l(24000), r(24000);   // half a second of content
    for (size_t i = 0; i < 24000; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    eng.load_sample(l.data(), r.data(), 24000);

    // SOURCE=0.5, SIZE=1.0 (ask for the full 42 s), PITCH=0.5, MOTION=0
    float targets[LANE_COUNT] = { 0.5f, 1.f, 0.5f, 0.f, 0.8f };
    eng.set_targets(targets, 0.5f);
    eng.set_flow(true);

    int seen = 0;
    for (int i = 0; i < 48000 && seen < 1; ++i) {
        float l2 = 0.f, r2 = 0.f;
        const unsigned before = eng.spawn_count();
        eng.process(l2, r2);
        if (eng.spawn_count() != before) ++seen;
    }
    REQUIRE(seen == 1);
    // Under M5a this was clamped to 24000. Now it is the full 42 s.
    CHECK(eng.last_spawn_len() > 24000);
}

TEST_CASE("sampler: spawn_interval clamps to its floor only when it must") {
    // test_spawn_interval is the extracted helper _update_control calls to
    // derive _spawn_every. Unlike the old version of this test, this one
    // pins overlaps that actually engage the floor -- at today's kOverlap=4
    // the shortest grain the SIZE curve can produce (48 samples at 48 kHz)
    // already gives 12, above the floor, so a test confined to kOverlap=4
    // could never see the clamp fire. These cases pick overlaps by hand
    // instead, so the floor is exercised regardless of what kOverlap is
    // today.
    using namespace spky::sampler_cfg;

    // Engages: 48 / 16 = 3, below kSpawnMinSamples (8) -- must clamp to
    // exactly the floor. THIS is the assertion that dies if the floor line
    // (`raw < kSpawnMinSamples ? kSpawnMinSamples : raw`) is deleted: without
    // it the function would return the unclamped 3.
    CHECK(spky::test_spawn_interval(48.f, 16) == doctest::Approx(kSpawnMinSamples));

    // Does not engage: 48 / 4 = 12, already above the floor -- must pass
    // through unclamped. Proves the floor doesn't stomp values that are fine.
    CHECK(spky::test_spawn_interval(48.f, 4) == doctest::Approx(12.f));

    // Boundary: raw exactly at the floor must return the floor unchanged...
    CHECK(spky::test_spawn_interval(kSpawnMinSamples * 2.f, 2) == doctest::Approx(kSpawnMinSamples));
    // ...and raw just below it must still be lifted up to the floor.
    const float just_below = kSpawnMinSamples - 0.5f;
    CHECK(spky::test_spawn_interval(just_below * 2.f, 2) == doctest::Approx(kSpawnMinSamples));
}

TEST_CASE("sampler: the spawn interval never falls below its floor") {
    // This is the CPU guard Task 3 relocates. At today's kOverlap (4), the
    // shortest grain the SIZE curve can produce (SIZE=0 -> kSizeFloorS ->
    // 48 samples at 48 kHz) already yields a 12-sample interval, above
    // kSpawnMinSamples (8) -- so this test is DORMANT: the bound below holds
    // no matter what spawn_interval's floor does, because the floor never
    // fires along this path. It becomes a real check of the observed spawn
    // rate once a later task raises kOverlap to 8 or 16, at which point the
    // same SIZE=0 grain drives the interval under the floor and this test
    // starts actually constraining behaviour. Kept (rather than deleted) so
    // it is already in place and already correct when that happens. The
    // helper-level test above, not this one, is what proves the floor works
    // today.
    using namespace spky::sampler_cfg;
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0xABu);
    eng.init(48000.f);

    std::vector<float> l(48000), r(48000);
    for (size_t i = 0; i < 48000; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    eng.load_sample(l.data(), r.data(), 48000);

    // SOURCE=0.5, SIZE=0 (shortest grains), PITCH=0.5, MOTION=0 (no timing jitter)
    float targets[LANE_COUNT] = { 0.5f, 0.f, 0.5f, 0.f, 0.8f };
    eng.set_targets(targets, 0.5f);
    eng.set_flow(true);

    const int kSamples = 48000;
    for (int i = 0; i < kSamples; ++i) { float l2 = 0.f, r2 = 0.f; eng.process(l2, r2); }

    const double max_spawns = kSamples / static_cast<double>(kSpawnMinSamples) + 1.0;
    CHECK(eng.spawn_count() <= max_spawns);
}

// --- Task 5: pitch to +-4 octaves, resonance into self-oscillation --------

TEST_CASE("sampler: pitch is unchanged over the middle half of its travel") {
    using namespace spky::sampler_cfg;
    for (float n : {0.25f, 0.375f, 0.5f, 0.625f, 0.75f}) {
        const float want = std::pow(8.f, n - 0.5f);
        CHECK(spky::test_ratio_for(n) == doctest::Approx(want).epsilon(1e-6));
    }
    CHECK(spky::test_ratio_for(0.5f) == doctest::Approx(1.f).epsilon(1e-6));
}

TEST_CASE("sampler: pitch reaches four octaves either way") {
    using namespace spky::sampler_cfg;
    const float top = std::pow(2.f, kPitchOctaves);
    CHECK(spky::test_ratio_for(1.f) == doctest::Approx(top).epsilon(1e-5));
    CHECK(spky::test_ratio_for(0.f) == doctest::Approx(1.f / top).epsilon(1e-5));
}

TEST_CASE("sampler: pitch is continuous at both knees and monotonic throughout") {
    using namespace spky::sampler_cfg;
    // Offset 1e-5, for the same reason Task 3's SIZE continuity test uses it:
    // the slope kink at a knee is intended, so this comparison picks up
    // (slope difference x offset) on top of any genuine discontinuity.
    // Shrinking the offset shrinks the legitimate part and leaves a real
    // value jump untouched. At 1e-4 the honest kink alone can exceed the
    // tolerance and fail a correct curve.
    for (float knee : {kPitchKneeLo, kPitchKneeHi}) {
        const float below = spky::test_ratio_for(knee - 1e-5f);
        const float above = spky::test_ratio_for(knee + 1e-5f);
        CHECK(std::fabs(above - below) < 1e-3f * above);
    }
    float prev = spky::test_ratio_for(0.f);
    for (int i = 1; i <= 1000; ++i) {
        const float cur = spky::test_ratio_for(static_cast<float>(i) / 1000.f);
        CHECK(cur >= prev);
        prev = cur;
    }
}

TEST_CASE("sampler: resonance at maximum stays finite") {
    // The ceiling at 0.95 had no documented reason. This is the test that
    // decides whether it needs one.
    Rig g(48000, 0x5Eu);
    g.e.set_resonance(1.f);
    // SOURCE=0, SIZE=0.5, PITCH=0.5, MOTION=0, LEVEL=1 (Rig's feed defaults).
    g.feed(0.5f);
    g.e.set_flow(true);

    float peak = 0.f;
    // Sweep FILT across its whole range while resonating: a self-oscillating
    // SVF is likeliest to diverge while its cutoff is moving, not while it
    // sits still, so a fixed-cutoff test would miss the failure it is for.
    for (int i = 0; i < 48000 * 10; ++i) {
        g.e.set_filt(-1.f + 2.f * (static_cast<float>(i) / (48000.f * 10.f)));
        float l = 0.f, r = 0.f;
        g.e.process(l, r);
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
        peak = std::fabs(l) > peak ? std::fabs(l) : peak;
        peak = std::fabs(r) > peak ? std::fabs(r) : peak;
    }
    CHECK(peak < 8.f);
}
