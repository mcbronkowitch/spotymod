#include <doctest/doctest.h>
#include <cmath>
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

TEST_CASE("sampler: SIZE maps 20 ms .. 2 s exponentially") {
    // Needs a full 2 s of content: at SIZE=1 the requested grain length is
    // exactly 96000 samples (2 s), and grain length is deliberately clamped
    // to the recorded content length (see "grain length is clamped to the
    // content length", below). The default Rig only preloads 24000 samples
    // (0.5 s), which would clamp the top of this curve and defeat the very
    // property under test -- so this test alone needs the full kFrames of
    // content to observe the unclamped exponential mapping.
    Rig g(kFrames);
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.f);
    g.render(200);
    CHECK(g.e.grain_len_samples() == doctest::Approx(0.02f * 48000.f).epsilon(0.02));
    g.feed(0.5f, 0.f, 1.f);
    g.render(200);
    CHECK(g.e.grain_len_samples() == doctest::Approx(2.0f * 48000.f).epsilon(0.02));
    g.feed(0.5f, 0.f, 0.5f);
    g.render(200);
    // 0.02 * 100^0.5 = 0.2 s -- the exponential midpoint, NOT the linear one
    // (~1.01 s). This is the assertion that tells the two mappings apart.
    CHECK(g.e.grain_len_samples() == doctest::Approx(0.2f * 48000.f).epsilon(0.05));
}

TEST_CASE("sampler: grain length is clamped to the content length") {
    Rig g(4800);                           // only 100 ms of content
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 1.f);                // ask for 2 s
    g.render(200);
    CHECK(g.e.grain_len_samples() <= 4800);
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
    Rig g(0);
    g.e.set_monitor(true);
    float a = 0.f, b = 0.f;
    g.e.process_in(0.37f, -0.21f);
    g.e.process(a, b);
    CHECK(a == doctest::Approx(0.37f).epsilon(0.001));
    CHECK(b == doctest::Approx(-0.21f).epsilon(0.001));

    g.e.set_monitor(false);
    g.e.process_in(0.37f, -0.21f);
    g.e.process(a, b);
    CHECK(a == doctest::Approx(0.f).epsilon(0.001));
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
    // Long grains: the spawn interval is ~24000 samples, so a countdown that
    // survives the load would hold the cloud silent for half a second.
    g.feed(0.5f, 0.f, 1.f);
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
    CHECK(down > 400 * 0.5f * 0.6f);
    CHECK(down < 400 * 0.5f * 1.4f);
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
