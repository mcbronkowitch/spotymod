#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include <utility>
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

// Defined with the slice tests further down; forward-declared because two of
// the STEP cases up here need real transient material too.
static void feed_clicks(Rig& g, size_t content, int n);

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

// Task 9 rewrite of "sampler: STEP is silent until the gate opens, and tails
// off after". Two of its three halves survived the slice-groove rewrite and
// are kept verbatim in spirit: STEP makes no sound before a note, and a note
// is fully retired a beat after it ends. The third half asserted a release of
// a FIXED length (kBurstReleaseS, hard-coded as 0.06f once the constant was
// deleted) -- that length is gone with the burst scheduler, and the literal is
// deliberately NOT resurrected. What replaces it is the property that actually
// holds now: the release runs over the GRAIN'S OWN decay window, so DEC --
// which used to have no say at all over how a STEP note ended -- now sets the
// tail length. A build that went back to a fixed release, or that killed
// grains outright on the gate fall, collapses the two measurements below onto
// each other and fails.
//
// The gate is opened BEFORE the trigger on purpose: it separates "the gate
// alone spawns nothing" (the F-02 STEP direction, see near the end of this
// file) from "the fire spawns", inside the silence half.
TEST_CASE("sampler: STEP is silent until a note fires, and releases over its own DEC") {
    // Returns how many samples the note takes to retire after the gate falls.
    auto tail_samples = [](float dec_n) {
        Rig g;
        g.e.set_flow(false);
        g.e.set_window_decay(dec_n);
        // SIZE well above slice unity so the grain would still have seconds of
        // window left when the gate falls -- otherwise a long DEC would be
        // masked by the grain simply reaching its own end first, and the two
        // measurements would converge for a reason unrelated to the release.
        g.feed(/*pitch*/0.5f, /*source*/0.f, /*size*/0.8f);
        g.render(96);                       // let the control tick see SIZE

        g.e.set_gate(true);                 // gate up, nothing composed yet
        auto pre = g.render(4800);
        for (float s : pre) CHECK(s == doctest::Approx(0.f).epsilon(0.0001));
        REQUIRE(g.e.active_grains() == 0);  // the gate armed nothing

        g.e.trigger(0.5f);                  // the fire itself spawns
        auto on = g.render(4800);
        REQUIRE(g.e.active_grains() == 1);
        CHECK(rms(on, 0, 4800) > 0.02f);    // it sounds under the note

        g.e.set_gate(false);
        int n = 0;
        while (g.e.active_grains() > 0 && n < 48000 * 4) {
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
            ++n;
        }
        CHECK(g.e.active_grains() == 0);    // retired, not sustained
        auto after = g.render(48000);
        CHECK(rms(after, 0, 48000) < 0.001f);   // and nothing respawns behind it
        return n;
    };

    const int shortest = tail_samples(0.f);
    const int longest  = tail_samples(1.f);
    INFO("release tail: DEC 0 -> " << shortest << " samples, DEC 1 -> "
         << longest);
    CHECK(shortest > 0);                    // a release, not a hard cut
    CHECK(longest > 4 * shortest);          // ...whose length is DEC's, not fixed
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

TEST_CASE("sampler: MOTION never moves a grain off its chord note") {
    // This test used to assert the OPPOSITE -- that MOTION scattered roughly
    // kScatterOctProb of grains an octave away. That scatter was removed
    // (sampler_engine.cpp, _next_ratio): it defeated the morphagene-controls
    // requirement that a sampler deck hold its pitch against a synth deck in
    // the same key, and it did so through LANE_MOTION, which survives the
    // PITCH lane being switched off. MOTION's other scatters stay; this pins
    // that pitch is not among them.
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.5f, 0.2f, 1.f);         // MOTION 1 -- the worst case
    const float chord[3] = { 0.45f, 0.55f, 0.65f };
    g.e.set_chord(chord, 3);

    auto s = collect(g, 600);
    REQUIRE(s.ratio.size() == 600);

    int plain = 0, octaves = 0;
    for (float rr : s.ratio) {
        for (int i = 0; i < 3; ++i) {
            const float base = std::pow(8.f, chord[i] - 0.5f);
            if (std::fabs(rr - base) < 0.001f) ++plain;
            else if (std::fabs(rr - base * 2.f) < 0.002f ||
                     std::fabs(rr - base * 0.5f) < 0.002f) ++octaves;
        }
    }
    CHECK(plain == 600);        // every grain on a chord note, at full MOTION
    CHECK(octaves == 0);        // and not one octave away
}

TEST_CASE("sampler: a single note holds one exact ratio at every MOTION") {
    // The single-note case is the one the author actually plays (COLOR 0), and
    // the one the octave scatter broke on his panel: LANE_MOTION sits on
    // Part's default base of 0.5 even with MOD at zero, so 12.5% of grains
    // jumped. Sweeping MOTION across its range must not produce a second
    // ratio.
    for (float motion : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
        Rig g;
        g.e.set_flow(true);
        g.feed(0.5f, 0.5f, 0.2f, motion);
        auto s = collect(g, 400);
        REQUIRE(s.ratio.size() == 400);
        const float first = s.ratio.front();
        bool all_same = true;
        for (float rr : s.ratio) if (rr != first) all_same = false;
        CHECK(all_same);
    }
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

// REMOVED: "sampler: hoisting the chord ratios leaves spawned ratios
// unchanged". It collected 16 ratios and asserted only isfinite(r) and
// r > 0.f, which a build that read the wrong cache entry, always used
// _chord_ratio[0], or returned a constant 1.0 would pass identically -- at
// exactly the site where the plan's original instruction was wrong. A test
// that cannot fail reads as coverage without being any.
//
// The property is genuinely owned elsewhere: "sampler: golden vector -- Rng
// draw order and SOURCE mapping are locked" pins the exact ratio of each of
// the first 20 spawns over a 3-note chord (0.20 / 0.55 / 1.53 ... -- visibly
// per-note, so a constant or a wrong cache entry fails it), and "a latched
// single-note STEP burst holds its ratio while PITCH drifts underneath it"
// covers the separate _burst_ratio cache.

// Task 9 rewrite of "...a latched single-note STEP burst holds its ratio while
// PITCH drifts underneath it". The property is unchanged and still real; only
// its VEHICLE moved. The old form collected 12 grains out of one trigger by
// letting the free-running STEP burst spawn under a held gate, and that
// scheduler is gone. Since Task 7 a single fire produces more than one grain
// again -- the roll retriggers -- and those go through the same _next_ratio(),
// so the test now rides a roll instead of a burst. Note the retrigger path
// (sampler_engine.cpp, process()) never re-latches: it must keep reading the
// _burst_ratio frozen at the fire, not the live _chord[0] this test drifts.
TEST_CASE("sampler: a latched single-note STEP roll holds its ratio while PITCH drifts underneath it") {
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
    //
    // StepRig is declared further down this file, so this rig is built by hand
    // -- clicky material for real slices, STEP, a 6000-sample step clock, DENS
    // at maximum (subdivision cap 8) and a phrase slot whose metric weight is
    // 0, which makes the roll probability exactly 1: the fire below ALWAYS
    // arms a roll, no dice involved.
    Rig g(0);
    feed_clicks(g, 48000, 8);
    g.e.set_flow(false);            // STEP mode
    g.e.set_step_clock(6000.f);
    g.feed(/*pitch*/0.5f, /*source*/0.f, /*size*/0.2f);   // short grains: the
    g.e.set_overlap(1.f);                                  // pool never fills
    g.render(96);                   // let the control tick see SIZE/DENS
    g.e.set_phrase_pos(/*slot*/1, /*steps*/8, /*weight*/0.f);
    g.e.trigger(0.3f);              // single note: _chord_n == 1, now latched
    g.e.set_gate(true);             // rolls only retrigger under the gate
    REQUIRE(g.e.retrig_period() > 0);   // the roll really armed

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
            // underneath the still-latched note -- what a PITCH-lane LFO
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
    // Every grain of the roll spawns at the SAME ratio: the one latched at
    // trigger() time, unmoved by the live PITCH drift injected above.
    for (float r : ratios) CHECK(r == ratios[0]);
    // ...and that one ratio is the TRIGGER'S, not the drift's. Without this
    // the loop above would pass just as happily on a build that read
    // _chord_ratio[0] but happened to see a constant chord.
    CHECK(ratios[0] == doctest::Approx(spky::test_ratio_for(0.3f)).epsilon(1e-6));
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
    //
    // RECAPTURED 2026-07-21, and the file-level comment's "STOP, ask why"
    // applies -- so here is why. _next_ratio() lost BOTH of its Rng draws when
    // MOTION's octave scatter was removed (the roll, and the up/down direction
    // draw). That is one of the causes that comment enumerates, and every
    // following draw in _spawn_one shifts two positions up the stream, so all
    // 20 tuples move. The numbers were not adjusted until the test passed;
    // they were re-captured wholesale and then checked against what the change
    // predicts:
    //
    //   The chord's three ratios are 8^(p-0.5) = 0.794, 1.109, 1.516. Every
    //   ratio below is one of those three, at most halved once by SUB and
    //   detuned by a few cents -- there is no factor of 2 anywhere. The old
    //   vector spanned 0.020..2.214 (0.794 halved by both the octave roll and
    //   SUB, up to 1.107 doubled); this one spans 0.406..1.529, narrowed by
    //   exactly one octave at each end. pos, pan and len are unchanged in
    //   character, only re-drawn.
    static const Spawn golden[20] = {
        // pos,            pan,          ratio,        len
        { 12803.53027344f, -0.69252360f, 0.40792397f, 3821 },
        { 12347.84765625f, -0.53483331f, 1.11854935f, 3821 },
        { 14493.06835938f, -0.98459131f, 1.51102293f, 3821 },
        { 21022.68359375f,  0.71411252f, 0.80873936f, 3821 },
        { 14869.51953125f, -0.82430881f, 1.11897147f, 3821 },
        {  2658.53613281f,  0.29880500f, 1.51594043f, 3821 },
        { 17732.84179688f, -0.76020461f, 0.81971961f, 3821 },
        {   366.85937500f, -0.92472601f, 0.55725181f, 3821 },
        { 17776.35937500f,  0.88420212f, 1.51119888f, 3821 },
        {  6994.29687500f, -0.24982208f, 0.40905151f, 3821 },
        {   870.30957031f,  0.07455552f, 0.55567920f, 3821 },
        {  2460.46289062f, -0.39328730f, 0.75581837f, 3821 },
        { 15710.42968750f,  0.25942016f, 0.80998176f, 3821 },
        { 19427.55859375f, -0.08393264f, 1.10071087f, 3821 },
        { 17490.03710938f, -0.75745511f, 1.52877951f, 3821 },
        { 21073.81835938f,  0.60381067f, 0.40589118f, 3821 },
        {  4529.81250000f, -0.02573544f, 1.11435354f, 3821 },
        { 20963.06445312f, -0.77458274f, 1.51434517f, 3821 },
        { 12696.35742188f, -0.26600885f, 0.81264138f, 3821 },
        { 15659.34960938f, -0.31699651f, 0.55088216f, 3821 },
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
    // pins overlaps that actually engage the floor. That was written when
    // kOverlap was 4, where the shortest grain the SIZE curve can produce
    // (48 samples at 48 kHz) gives 12, above the floor, so a test confined
    // to the engine's own overlap could never see the clamp fire. At today's
    // kOverlap = 8 it would (48 / 8 = 6), but these cases still pick
    // overlaps by hand so the floor stays exercised regardless of what
    // kOverlap becomes next.
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
    // This is the CPU guard Task 3 relocates, checked end to end on the
    // observed spawn rate rather than on the helper.
    //
    // DO NOT DELETE THIS AS DORMANT -- it no longer is, and an older version
    // of this comment said it was. That was true at kOverlap = 4: the
    // shortest grain the SIZE curve can produce (SIZE=0 -> kSizeFloorS -> 48
    // samples at 48 kHz) gave 48 / 4 = 12, already above kSpawnMinSamples
    // (8), so the floor never fired along this path and the bound below held
    // regardless of what spawn_interval did. Raising density to kOverlap = 8
    // changed that: 48 / 8 = 6 is BELOW the floor, so the floor is now the
    // thing setting the spawn rate here, and the assertion sits exactly on
    // its bound -- 48000 / 8 + 1 = 6001 permitted against 6001 observed, with
    // no slack at all. Delete or weaken the floor and this fails on the next
    // spawn. It is the live, razor-thin end-to-end check; the helper-level
    // test above covers the arithmetic in isolation.
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

// Sweeps FILT across its whole range while resonating at `res`, over
// `seconds` of real time, and returns the peak sample magnitude reached.
// A self-oscillating SVF is likeliest to diverge while its cutoff is
// moving, not while it sits still, so a fixed-cutoff sweep would miss the
// failure this is for.
static float sweep_peak(float res, double seconds) {
    Rig g(48000, 0x5Eu);
    g.e.set_resonance(res);
    // SOURCE=0, SIZE=0.5, PITCH=0.5, MOTION=0, LEVEL=1 (Rig's feed defaults).
    g.feed(0.5f);
    g.e.set_flow(true);

    const long n = static_cast<long>(48000.0 * seconds);
    float peak = 0.f;
    for (long i = 0; i < n; ++i) {
        g.e.set_filt(-1.f + 2.f * (static_cast<float>(i) / static_cast<float>(n)));
        float l = 0.f, r = 0.f;
        g.e.process(l, r);
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
        peak = std::fabs(l) > peak ? std::fabs(l) : peak;
        peak = std::fabs(r) > peak ? std::fabs(r) : peak;
    }
    return peak;
}

TEST_CASE("sampler: resonance ceiling is duration-stable, not just finite") {
    // Peak-vs-resonance turned out to be a smooth accelerating curve with no
    // knee (fixed 10 s sweep: 0.7 -> 7.22, 0.8 -> 11.11, 0.9 -> 21.20,
    // 0.95 -> 34.59, 1.0 -> 72.77) -- no peak threshold marks a regime
    // change on that curve, so one was never testing a real property.
    //
    // The real property is duration-dependence. Below the clamp, the peak
    // barely moves as the same sweep runs 10x longer -- measured growth from
    // a 10 s sweep to a 100 s one: 0.85 -> 2.3%, 0.90 -> 7.0%, 0.92 -> 11.4%,
    // 0.95 -> 26.6%, 1.0 -> 210%. The clamp is set to 0.90: comfortably
    // inside the stable side against this test's 10% tolerance, with real
    // distance to 0.95, which fails it outright.
    //
    // What this is NOT (see set_resonance's comment for the long form): it is
    // not a divergence boundary. At 300-3000 s horizons 0.95 and 0.98 also
    // plateau, just higher; only 1.0 is genuinely unbounded. 0.90 is a
    // headroom choice. And the 10% tolerance is as much a line through a
    // continuum as the peak threshold this replaced -- 0.92's 11.4% fails
    // only because the number is written 10 and not 12. The improvement is
    // in the QUANTITY measured, not in the line having stopped being taste:
    // duration-dependence separates ringing from running away, and a peak at
    // one fixed sweep length does not.
    const float res = 1.f;  // set_resonance clamps this to the real ceiling
    const float short_peak = sweep_peak(res, 10.0);
    const float long_peak  = sweep_peak(res, 100.0);
    CHECK(std::fabs(long_peak - short_peak) < 0.10f * short_peak);
}

// --- Task 6: density, measured rather than guessed -------------------------

TEST_CASE("sampler: density telemetry at worst case") {
    // Not a pass/fail threshold -- a measurement. Worst case means every
    // slot contended: maximum MOTION (timing jitter bunches spawns), short
    // SIZE (frequent spawns), FLOW running continuously. Mirrors the setup
    // in "the spawn interval never falls below its floor" above, but with
    // MOTION=1 instead of 0 and full run length, so this is deliberately the
    // busiest the engine can be asked to get.
    using namespace spky::sampler_cfg;
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0xD1u);
    eng.init(48000.f);

    std::vector<float> l(48000), r(48000);
    for (size_t i = 0; i < 48000; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    eng.load_sample(l.data(), r.data(), 48000);

    // SOURCE=0.5 (default), SIZE=0.1 (short, frequent spawns), PITCH=0.5
    // (default), MOTION=1 (max timing jitter), LEVEL=1.
    float targets[LANE_COUNT] = { 0.5f, 0.1f, 0.5f, 1.f, 1.f };
    eng.set_targets(targets, 0.5f);
    eng.set_flow(true);

    long long total = 0;
    int peak = 0;
    const int kSamples = 48000 * 10;
    for (int i = 0; i < kSamples; ++i) {
        float l2 = 0.f, r2 = 0.f;
        eng.process(l2, r2);
        const int a = eng.active_grains();
        total += a;
        peak = a > peak ? a : peak;
    }
    const double mean = static_cast<double>(total) / kSamples;
    const int attempts = eng.spawn_count() + eng.dropped_spawns();
    const double drop_frac =
        attempts > 0 ? static_cast<double>(eng.dropped_spawns()) / attempts : 0.0;
    MESSAGE("kOverlap=" << SamplerEngine::kOverlap << " kGrains=" << kGrains
            << " mean=" << mean << " peak=" << peak
            << " spawned=" << eng.spawn_count()
            << " dropped=" << eng.dropped_spawns()
            << " drop_frac=" << drop_frac);

    // Was dieser Test bis 2026-07-22 behauptete: drop_frac < 1 %, denn "der
    // Pool ist gross genug, um mit Worst-Case-MOTION/SIZE mitzuhalten"
    // (gemessen 0 von 11514 Versuchen bei (8, 16)). Das stimmte und war
    // genau das Problem: mitzuhalten hiess, die Grainzahl auf bis zu 11
    // stapeln zu lassen, wo DENS 8 bestellt hatte, und die CPU-Kosten eines
    // Blocks sind linear in dieser Zahl. Auf der Hardware waren das 117 %
    // des Blockbudgets im schlimmsten Block gegen 92 % im mittleren
    // (docs/bench/2026-07-22, inst_sampler_worst).
    //
    // Seit kSpawnHeadroom ist das Fallenlassen die ABSICHT, nicht der
    // Defekt: der Deckel kauft die Spitze mit ein paar Prozent der Spawns.
    // Die beiden Zusagen unten sind deshalb neu, und die zweite ist die
    // wichtigere -- ein Deckel, der im gewoehnlichen Betrieb zuschlaegt,
    // waere ein Dichteverlust und kein CPU-Schutz.
    const int ceiling =
        int(std::ceil(double(SamplerEngine::kOverlap))) + spky::sampler_cfg::kSpawnHeadroom;
    CHECK(peak <= ceiling);      // der Deckel haelt, nicht bloss kGrains
    CHECK(drop_frac < 0.10);     // und er kostet eine Minderheit der Spawns

    // Der Deckel greift NUR im Extrem. Dieselbe Wolke bei musikalischer
    // Dichte (DENS ~4, halbsekundige Grains, MOTION halb) darf keinen
    // einzigen Spawn verlieren -- gemessen 0.0 %.
    SamplerEngine calm;
    calm.set_memory(mem.data(), mem.size());
    calm.set_seed(0x51A9u);
    calm.init(48000.f);
    calm.load_sample(l.data(), r.data(), 48000);
    float calm_targets[LANE_COUNT] = { 0.5f, 0.35f, 0.5f, 0.5f, 1.f };
    calm.set_targets(calm_targets, 0.5f);
    calm.set_overlap(0.45f);
    calm.set_flow(true);
    for (int i = 0; i < 48000 * 10; ++i) {
        float a = 0.f, b = 0.f;
        calm.process(a, b);
    }
    MESSAGE("musical setting: spawned=" << calm.spawn_count()
            << " dropped=" << calm.dropped_spawns());
    CHECK(calm.dropped_spawns() == 0);
}

// --- Final review, Critical: tape mode must not starve the grain pool -------

TEST_CASE("sampler: tape mode at max SIZE and min pitch keeps the cloud spawning") {
    // The bug this pins: in tape mode a grain lasts SIZE / ratio, and nothing
    // bounded that product. At SIZE 1.0 (_grain_len = 42 s = 2,016,000
    // samples) with PITCH at minimum (ratio 2^-4 = 0.0625) an unclamped grain
    // runs 32,256,000 samples -- 11 minutes -- and the reachable minimum
    // ratio is lower still (octave scatter x SUB x detune -> ~0.0154, giving
    // ~45 minutes). _spawn_every is 2,016,000 / kOverlap = 252,000, so all
    // kGrains slots fill after kGrains * _spawn_every = 4,032,000 samples
    // (84 s) and EVERY spawn after that is dropped until the first grain
    // finally retires. No knob recovers it: length is latched at spawn and
    // only CHOKE / set_hold frees a slot early.
    //
    // The bound is `lenf <= _spawn_every * kGrains` -- a grain must not
    // outlive the time it takes to fill every slot, because spawns past that
    // point are dropped anyway. Self-adjusting to any kOverlap/kGrains.
    //
    // PITCH at minimum alone is enough to reach the failure and keeps the
    // test deterministic: MOTION = 0 means no octave scatter and no timing
    // jitter, SUB and DTUN are at their defaults (0).
    using namespace spky::sampler_cfg;
    const size_t kCap = static_cast<size_t>(kSizeCeilS * 48000.f);   // 2,016,000
    std::vector<SampleBuffer::Frame> mem(kCap);
    SamplerEngine eng;
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0x7A9Eu);
    eng.init(48000.f);

    std::vector<float> l(kCap), r(kCap);
    for (size_t i = 0; i < kCap; ++i) {
        l[i] = std::sin(6.2831853f * 110.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    eng.load_sample(l.data(), r.data(), kCap);

    eng.set_tape_mode(true);
    // SOURCE=0, SIZE=1 (42 s), MOTION=0, LEVEL=1. The PITCH LANE does not
    // reach the grain ratio -- Part composes pitch and hands it over through
    // set_chord, which is what _next_ratio reads -- so minimum pitch is
    // set_chord(0.f), i.e. ratio_for(0) = 2^-kPitchOctaves = 0.0625.
    float targets[LANE_COUNT] = { 0.f, 1.f, 0.5f, 0.f, 1.f };
    eng.set_targets(targets, 0.5f);
    const float lowest[1] = { 0.f };
    eng.set_chord(lowest, 1);
    eng.set_flow(true);

    // Run well past the point where kGrains slots would have filled:
    // kGrains * _spawn_every = 4,032,000 samples. 6,000,000 is 125 s, half
    // again as long, so a starved pool has had ~8 further spawn intervals in
    // which to drop every one of them.
    const int kSamples = 6000000;
    int at_fill = 0;
    for (int i = 0; i < kSamples; ++i) {
        float a = 0.f, b = 0.f;
        eng.process(a, b);
        if (i == 4032000) at_fill = eng.spawn_count();
    }

    MESSAGE("tape max-SIZE min-pitch: spawned=" << eng.spawn_count()
            << " dropped=" << eng.dropped_spawns()
            << " at_fill=" << at_fill
            << " last_len=" << eng.last_spawn_len());

    // The cloud kept spawning past the point a starved pool would have
    // frozen at exactly kGrains spawns. Unclamped, spawn_count() sticks at
    // kGrains (16) forever and every later attempt is a drop.
    CHECK(at_fill >= SamplerEngine::kGrains);
    CHECK(eng.spawn_count() > at_fill);
    CHECK(eng.spawn_count() >= 20);

    // ...and it did so without dropping: the bound is exactly the pool's
    // throughput, so at MOTION = 0 (no timing jitter) nothing contends. A
    // couple of drops would be float-rounding at the boundary, not failure;
    // an unclamped build drops many tens.
    CHECK(eng.dropped_spawns() <= 2);

    // The grain length is bounded by the pool-fill time, not by SIZE / ratio.
    CHECK(eng.last_spawn_len() <= SamplerEngine::kGrains * 252000 + 1);
}

// --- Final review: the two maxima that had only ever been measured apart ----

TEST_CASE("sampler: maximum record bloom into maximum resonance stays bounded") {
    // The gap this closes: the record-feedback bloom (fixed point ~2.3 of
    // full scale) and the resonance sweep (peak ~21x on unit-amplitude
    // content) had each been measured on its own, never together, so the
    // headroom into the SVF was unknown. This runs both at maximum at once.
    //
    // Stage 1 blooms the buffer: feedback at the top of its travel is above
    // unity, so overdubbing hot material into the loop drives it up to the
    // saturator's fixed point. Stage 2 granulates that bloomed content with
    // resonance at maximum while sweeping FILT across its whole range --
    // the condition a self-oscillating SVF is likeliest to ring under, and
    // the same sweep the resonance ceiling was chosen against.
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0xB1005u);
    eng.init(48000.f);

    // --- stage 1: bloom the buffer ---
    eng.set_feedback(1.f);                 // above unity: the bloom
    eng.set_recording(true);
    for (int i = 0; i < 48000 * 20; ++i)   // 20 passes over a 1 s loop
        eng.process_in(0.9f * std::sin(6.2831853f * 220.f * float(i) / 48000.f),
                       0.9f * std::sin(6.2831853f * 220.f * float(i) / 48000.f));
    eng.set_recording(false);
    for (int i = 0; i < 1024; ++i) eng.process_in(0.f, 0.f);   // let the fade finish

    float content_peak = 0.f;
    for (size_t i = 0; i < mem.size(); ++i) {
        content_peak = std::fabs(mem[i].l) > content_peak ? std::fabs(mem[i].l) : content_peak;
        content_peak = std::fabs(mem[i].r) > content_peak ? std::fabs(mem[i].r) : content_peak;
    }
    // The precondition the whole test rests on: the bloom actually happened,
    // so the SVF below is being fed hot content and not a quiet loop.
    REQUIRE(content_peak > 1.f);

    // --- stage 2: granulate it at maximum resonance under a full FILT sweep ---
    eng.set_resonance(1.f);                // clamps to the real ceiling (0.90)
    float targets[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 1.f };
    eng.set_targets(targets, 0.5f);
    eng.set_flow(true);

    const long n = 48000 * 10;
    float peak = 0.f;
    for (long i = 0; i < n; ++i) {
        eng.set_filt(-1.f + 2.f * (static_cast<float>(i) / static_cast<float>(n)));
        float l = 0.f, r = 0.f;
        eng.process(l, r);
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
        peak = std::fabs(l) > peak ? std::fabs(l) : peak;
        peak = std::fabs(r) > peak ? std::fabs(r) : peak;
    }
    MESSAGE("bloom x resonance: content_peak=" << content_peak << " out_peak=" << peak);

    // The outcome, recorded because it was genuinely unknown: the two maxima
    // together do NOT multiply. Measured content_peak 2.20 (the bloom's
    // saturator fixed point, matching the ~2.3 measured on its own) and
    // out_peak 4.48 -- about 2x the content, nowhere near the ~48 that
    // naively composing the bloom's 2.3 with the resonance sweep's 21x on
    // unit-amplitude content would predict. Two things absorb it: the
    // overlap normalization (1/sqrt(active)) divides the summed cloud down,
    // and the 21x figure came from a sine parked at the cutoff, which a
    // sweep passing through only touches briefly.
    //
    // 8.0 is the ceiling: ~1.8x headroom over the measurement for seed and
    // build noise, and still tight enough to fail loudly on a real
    // regression. Verified sensitive -- lifting set_resonance's clamp from
    // 0.90 to 1.0 takes out_peak to 33.7 and fails this outright.
    CHECK(peak < 8.f);
}

TEST_CASE("sample_data exposes the loaded content at rec_size length") {
    std::vector<SampleBuffer::Frame> mem(4096);
    SamplerEngine eng;
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(1);
    eng.init(48000.f);
    CHECK(eng.sample_data() == mem.data());   // the host's own pointer back
    CHECK(eng.rec_size() == 0);

    std::vector<float> l(1000), r(1000);
    for (size_t i = 0; i < 1000; ++i) {       // a ramp, so an offset shows up
        l[i] = (float)i / 1000.f;
        r[i] = -(float)i / 1000.f;
    }
    eng.load_sample(l.data(), r.data(), 1000);
    CHECK(eng.rec_size() == 1000);
    CHECK(eng.sample_data()[0].l   == doctest::Approx(0.f));
    CHECK(eng.sample_data()[500].l == doctest::Approx(0.5f));
    CHECK(eng.sample_data()[500].r == doctest::Approx(-0.5f));
    CHECK(eng.sample_data()[999].l == doctest::Approx(0.999f));
}

TEST_CASE("a host can carry content across a re-init by copying it out") {
    // This is exactly what the VCV host does on a sample-rate change, and the
    // reason sample_data() exists. init() memsets the injected buffer
    // (SampleBuffer::clear), so the snapshot MUST be taken into separate
    // storage first -- copying out of the buffer after init() would read
    // zeroes, and that mistake is what this case is here to catch.
    std::vector<SampleBuffer::Frame> mem(4096);
    SamplerEngine eng;
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(1);
    eng.init(48000.f);

    std::vector<float> l(800, 0.25f), r(800, -0.75f);
    eng.load_sample(l.data(), r.data(), 800);

    const size_t n = eng.rec_size();
    std::vector<float> sl(n), sr(n);
    for (size_t i = 0; i < n; ++i) { sl[i] = eng.sample_data()[i].l;
                                     sr[i] = eng.sample_data()[i].r; }

    eng.init(44100.f);
    CHECK(eng.rec_size() == 0);
    CHECK(eng.sample_data()[10].l == doctest::Approx(0.f));  // init DID wipe it

    eng.load_sample(sl.data(), sr.data(), n);
    CHECK(eng.rec_size() == 800);
    CHECK(!eng.is_empty());
    CHECK(eng.sample_data()[10].l  == doctest::Approx(0.25f));
    CHECK(eng.sample_data()[799].r == doctest::Approx(-0.75f));
}

TEST_CASE("clear() erases stale content, not just rec_size") {
    // I-3 fix: SampleBuffer::clear() now skips its memset when _size == 0
    // going in, to avoid an unconditional ~16 MB stall on a buffer that is
    // already empty (see the factory-drone autoload in host/vcv, which hits
    // exactly that case on the audio thread). This pins the property that
    // must NOT regress: when the buffer HAD content (_size != 0 going into
    // clear()), the memset still runs and the stale frames are actually
    // erased -- not merely hidden behind rec_size() reporting 0.
    // sample_data() returns the raw buffer pointer regardless of rec_size(),
    // so it can see past the "valid" length exactly the way a naive host bug
    // (reading past rec_size after a load) would. A clear() that skipped the
    // memset unconditionally (not just when _size == 0) would leave the old
    // 0.5f/-0.75f content sitting here and fail this loop.
    std::vector<SampleBuffer::Frame> mem(4096);
    SamplerEngine eng;
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(1);
    eng.init(48000.f);

    std::vector<float> l(800, 0.5f), r(800, -0.75f);
    eng.load_sample(l.data(), r.data(), 800);
    REQUIRE(eng.rec_size() == 800);
    REQUIRE(eng.sample_data()[400].l == doctest::Approx(0.5f));
    REQUIRE(eng.sample_data()[400].r == doctest::Approx(-0.75f));

    eng.clear();
    CHECK(eng.rec_size() == 0);
    for (size_t i = 0; i < 800; ++i) {
        CHECK(eng.sample_data()[i].l == doctest::Approx(0.f));
        CHECK(eng.sample_data()[i].r == doctest::Approx(0.f));
    }
}

TEST_CASE("sample_data is null without injected memory") {
    SamplerEngine bare;
    bare.set_seed(1);
    bare.init(48000.f);
    CHECK(bare.sample_data() == nullptr);
    CHECK(bare.rec_size() == 0);
    CHECK(bare.is_empty());
}

TEST_CASE("sampler: DENS sets the grain overlap, and the spawn interval follows") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.5f);          // SIZE 0.5 -> 0.2 s -> 9600 samples
    g.render(200);
    const float len = g.e.grain_len_samples();
    REQUIRE(len == doctest::Approx(9600.f).epsilon(0.05));

    // Knob 1.0 -> overlap 8: the shipped density, unchanged from M5b.
    g.e.set_overlap(1.f);
    g.render(200);
    CHECK(g.e.overlap() == doctest::Approx(8.f));
    CHECK(g.e.spawn_interval_samples() == doctest::Approx(len / 8.f).epsilon(0.01));

    // Knob 0.0 -> overlap 1: one grain at a time, back to back. This is the
    // sparse regime where the ATK/DEC window shape becomes audible at all
    // (measured: 23% crest-factor swing isolated vs 1-16% in the dense cloud).
    g.e.set_overlap(0.f);
    g.render(200);
    CHECK(g.e.overlap() == doctest::Approx(1.f));
    CHECK(g.e.spawn_interval_samples() == doctest::Approx(len).epsilon(0.01));

    // Knob 0.5 -> overlap 4.5: pin the midpoint too. The two extremes above
    // are consistent with either a linear or a symmetric-curve mapping; only
    // a value strictly between them can catch a mis-mapped (e.g.
    // exponential) curve, and getting this shape right is exactly the open
    // question flagged for the listening pass (final-fixes report, Befund F).
    g.e.set_overlap(0.5f);
    g.render(200);
    CHECK(g.e.overlap() == doctest::Approx(4.5f));
}

TEST_CASE("sampler: the CPU floor on the spawn interval survives at every overlap") {
    // kSpawnMinSamples caps the spawn RATE, so it must bind hardest at the
    // shortest grain and the highest overlap. 1 ms at 48 kHz is 48 samples;
    // 48 / 8 = 6, which is below the floor of 8.
    //
    // Swept over every integer overlap 1..8 (final-fixes report, Befund F) --
    // the previous version of this test pinned only the two edges (1 and 8)
    // despite its name; a floor regression that only misfired at, say,
    // overlap 5 would have passed silently.
    for (int overlap = 1; overlap <= 8; ++overlap) {
        const float raw = 48.f / static_cast<float>(overlap);
        const float expect = raw < sampler_cfg::kSpawnMinSamples
                                  ? sampler_cfg::kSpawnMinSamples : raw;
        CHECK(test_spawn_interval(48.f, overlap) == doctest::Approx(expect));
    }
    // A grain long enough that the floor never binds within the overlap
    // range at all (9600 / 8 = 1200, far above the floor of 8).
    CHECK(test_spawn_interval(9600.f, 8) == doctest::Approx(1200.f));
    CHECK(test_spawn_interval(9600.f, 1) == doctest::Approx(9600.f));
}

TEST_CASE("sampler: lowering overlap only loosens the pool ceiling, never tightens it") {
    // len_ceil = _spawn_every * kGrains, and _spawn_every grows as overlap
    // falls -- so a lower overlap can never trim a grain that a higher one
    // would have allowed. This is the invariant that makes DENS safe to turn
    // down under any SIZE.
    const float len = 9600.f;
    float prev = 0.f;
    for (int ov = 8; ov >= 1; --ov) {
        const float ceil_v = test_spawn_interval(len, ov) * float(SamplerEngine::kGrains);
        CHECK(ceil_v >= prev);
        prev = ceil_v;
    }
}


TEST_CASE("sampler: the SCAN curve has a dead centre, hits 1.0x at the knee, tops at 8x") {
    using namespace sampler_cfg;
    // Dead zone: not "small", exactly zero. A creeping playhead at knob
    // centre would make the frozen state unreachable.
    CHECK(test_scan_rate(0.f)      == 0.f);
    CHECK(test_scan_rate(0.019f)   == 0.f);
    CHECK(test_scan_rate(-0.019f)  == 0.f);

    // Knee: realtime, on both sides, exactly.
    CHECK(test_scan_rate(kScanKnee)  == doctest::Approx(1.f).epsilon(0.001));
    CHECK(test_scan_rate(-kScanKnee) == doctest::Approx(-1.f).epsilon(0.001));

    // Ends.
    CHECK(test_scan_rate(1.f)  == doctest::Approx(kScanMaxRate).epsilon(0.001));
    CHECK(test_scan_rate(-1.f) == doctest::Approx(-kScanMaxRate).epsilon(0.001));
    CHECK(test_scan_rate(kScanDead) == doctest::Approx(kScanMinRate).epsilon(0.01));

    // Continuity ACROSS the knee, which the monotonicity loop below cannot
    // see: it only catches falling steps, so a jump UP at the knee would pass
    // it unnoticed. This is the assertion that pins the two branches together.
    CHECK(test_scan_rate(kScanKnee + 1e-4f) == doctest::Approx(1.f).epsilon(0.002));
    CHECK(test_scan_rate(-kScanKnee - 1e-4f) == doctest::Approx(-1.f).epsilon(0.002));

    // Monotone over the whole travel, including the sign change at centre --
    // the property that makes the knob playable. A sampled step like this
    // can only ever catch a FALLING step; it cannot detect a rising kink at
    // either knee (see the continuity checks above for that).
    float prev = -1e9f;
    for (int i = 0; i <= 400; ++i) {
        const float n = -1.f + 2.f * float(i) / 400.f;
        const float v = test_scan_rate(n);
        CHECK(v >= prev - 1e-6f);
        prev = v;
    }
}

TEST_CASE("sampler: SCAN advances the playhead, folds at the content edge, and reverses") {
    Rig g(24000);                     // 0.5 s of content
    g.e.set_flow(true);
    CHECK(g.e.scan_pos() == 0.f);

    // Realtime forward: after 24000 samples the head has travelled one full
    // content length and folded back to ~0.
    g.e.set_scan(sampler_cfg::kScanKnee);
    g.render(12000);
    const float half = g.e.scan_pos();
    CHECK(half == doctest::Approx(12000.f).epsilon(0.02));
    g.render(12000);
    CHECK(g.e.scan_pos() < 600.f);    // folded, not run past the end

    // Reverse walks it back down.
    g.e.set_scan(-sampler_cfg::kScanKnee);
    g.render(6000);
    const float back = g.e.scan_pos();
    CHECK(back > 12000.f);            // folded downward through zero
}

TEST_CASE("sampler: the playhead honours its [0, content) contract in reverse") {
    // Regression pin for the fold guard: a reverse run repeatedly crosses
    // zero, and that is exactly where a value just below zero folds to
    // EXACTLY content in float32.
    Rig g(24000);
    g.e.set_flow(true);
    g.e.set_scan(-1.f);
    for (int i = 0; i < 400; ++i) {
        g.render(480);
        CHECK(g.e.scan_pos() >= 0.f);
        CHECK(g.e.scan_pos() < 24000.f);   // strictly below, never equal
    }
}

TEST_CASE("sampler: a frozen SCAN leaves the playhead exactly where it was") {
    Rig g(24000);
    g.e.set_flow(true);
    g.e.set_scan(sampler_cfg::kScanKnee);
    g.render(4800);
    const float parked = g.e.scan_pos();
    REQUIRE(parked > 0.f);

    g.e.set_scan(0.f);
    g.render(48000);                  // a full second of nothing happening
    CHECK(g.e.scan_pos() == parked);  // exactly, not approximately
}

TEST_CASE("sampler: an empty buffer parks the playhead instead of drifting") {
    Rig g(0);
    REQUIRE(g.e.is_empty());
    g.e.set_flow(true);
    g.e.set_scan(1.f);
    g.render(48000);
    CHECK(g.e.scan_pos() == 0.f);
}

TEST_CASE("sampler: clear() and load_sample() send the playhead home") {
    Rig g(24000);
    g.e.set_flow(true);
    g.e.set_scan(sampler_cfg::kScanKnee);
    g.render(4800);
    REQUIRE(g.e.scan_pos() > 0.f);

    g.e.clear();
    CHECK(g.e.scan_pos() == 0.f);

    g.e.load_sample(g.l.data(), g.r.data(), 24000);
    g.render(4800);
    REQUIRE(g.e.scan_pos() > 0.f);
    g.e.load_sample(g.l.data(), g.r.data(), 24000);
    CHECK(g.e.scan_pos() == 0.f);
}

TEST_CASE("sampler: SCAN moves what the grains actually read") {
    // The point of the whole feature: not that a counter advances, but that
    // the spawn position follows it. ORGANIZE stays put; only SCAN moves.
    Rig g(24000);
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.5f);          // SOURCE 0 -- the head sits at the start
    g.e.set_scan(0.f);
    g.render(4800);
    const float parked = g.e.last_spawn_pos();

    g.e.set_scan(sampler_cfg::kScanKnee);
    g.render(9600);
    CHECK(g.e.last_spawn_pos() > parked + 1000.f);
}

TEST_CASE("sampler: punch() forces a grain now, even where the next one is seconds away") {
    // This is the test that carries the feature's reason for existing. At
    // overlap 1 and a long SIZE the scheduler is idle for the whole grain
    // length, so every knob is dead until the next spawn. punch() is the
    // gesture that says "read again now".
    Rig g(kFrames);
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 1.f);           // SIZE 1.0 -> 42 s
    g.e.set_overlap(0.f);             // overlap 1 -> _spawn_every == grain length
    g.render(4800);

    const int before = g.e.spawn_count();
    g.render(4800);                   // 100 ms: nowhere near the next spawn
    REQUIRE(g.e.spawn_count() == before);

    g.e.punch();
    g.render(200);                    // a couple of control ticks
    CHECK(g.e.spawn_count() > before);
}

TEST_CASE("sampler: punch() rewinds the playhead without killing what is sounding") {
    Rig g(24000);
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.5f);
    g.e.set_scan(sampler_cfg::kScanKnee);
    g.render(9600);
    REQUIRE(g.e.scan_pos() > 0.f);
    const int sounding = g.e.active_grains();
    REQUIRE(sounding > 0);

    g.e.punch();
    CHECK(g.e.scan_pos() == 0.f);
    // The distinction from clear()/load_sample(): those go through _kill_all
    // and silence the deck. punch() is a musical gesture on a running cloud.
    CHECK(g.e.active_grains() == sounding);
}

TEST_CASE("sampler: SCAN keeps a constant lag behind the write head while recording") {
    // The bug this guards: record and playback heads are supposed to be
    // independent (spec 2026-07-21 morphagene-controls), but both _scan_pos
    // and rec_size() start at zero and, at SCAN realtime forward, grow by the
    // identical amount every control tick. Folding _scan_pos modulo rec_size()
    // -- the ordinary (non-recording) behaviour -- then resets it to 0 on
    // every single tick, so the head is effectively nailed at the start the
    // whole time a recording runs. A test that only asserted "scan_pos() > 0"
    // would not tell that broken state apart from a real running head, since
    // both can show a small positive number at any single instant. What
    // actually distinguishes them is measuring the GAP between the two heads
    // (rec_size() - scan_pos()) at several points in time and requiring it to
    // settle on a constant: under the fold it is pinned near 0 forever; under
    // the fix it climbs to kScanRecordLagS worth of frames and then stops
    // moving, because ceiling = content - lag grows by exactly the amount
    // _scan_pos does each tick once both are advancing at realtime.
    Rig g(0);
    REQUIRE(g.e.is_empty());
    g.e.set_flow(true);
    g.e.set_scan(sampler_cfg::kScanKnee);   // realtime forward, exactly 1.0x
    g.e.set_recording(true);

    const float lag_frames = sampler_cfg::kScanRecordLagS * 48000.f;   // 12000
    std::vector<float> gaps;
    for (int i = 0; i < 48000; ++i) {       // 1 s: well past the lag, short of
        g.e.process_in(0.f, 0.f);           // the Rig's 2 s capacity
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        if (i % 2400 == 2399) gaps.push_back(float(g.e.rec_size()) - g.e.scan_pos());
    }
    g.e.set_recording(false);

    REQUIRE(gaps.size() >= 10);
    // Settled at the lag, not just "some positive gap" -- the number that
    // tells this apart from a coincidental fold remainder.
    for (size_t i = gaps.size() - 5; i < gaps.size(); ++i)
        CHECK(gaps[i] == doctest::Approx(lag_frames).epsilon(0.02));
    // ...and CONSTANT across those later points, not merely close to the
    // target independently at each one -- the property a still-drifting
    // value could not have.
    const float last = gaps.back();
    for (size_t i = gaps.size() - 5; i < gaps.size(); ++i)
        CHECK(gaps[i] == doctest::Approx(last).epsilon(0.002));
}

TEST_CASE("sampler: SCAN slower than realtime falls behind on its own during recording") {
    // The clamp must not turn into a second fold: when the head is naturally
    // slower than the write head, it should fall further back with time
    // (reading progressively older material), not get held at a fixed
    // distance. The lag ceiling only matters when SCAN would otherwise catch
    // up to (or reach) the write head; below realtime it never gets that
    // close, so the clamp stays slack throughout.
    Rig g(0);
    g.e.set_flow(true);
    // Comfortably inside the sub-knee exponential segment, so scan_rate() is
    // small and positive -- confirmed indirectly by the test itself finding a
    // growing gap; test_scan_rate() pins the curve shape separately.
    g.e.set_scan(sampler_cfg::kScanDead + 0.05f);
    g.e.set_recording(true);

    auto gap_after = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            g.e.process_in(0.f, 0.f);
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
        }
        return float(g.e.rec_size()) - g.e.scan_pos();
    };

    const float gap_early = gap_after(24000);   // 0.5 s
    const float gap_late  = gap_after(24000);    // another 0.5 s, cumulative 1 s

    // Growing without bound is exactly what distinguishes "falling behind" (no
    // clamp engaged) from "held at the lag" (Test above) -- both give a
    // positive gap, but only one keeps climbing.
    CHECK(gap_late > gap_early + 1000.f);
    g.e.set_recording(false);
}

TEST_CASE("sampler: the lag clamp never sends the playhead negative early in a recording") {
    // content - lag is negative for the first kScanRecordLagS seconds of any
    // recording into empty material (content grows from 0, lag is fixed at
    // 12000 frames @ 48 kHz) -- the exact case the spec calls out as needing
    // its own floor at 0, separate from the ceiling clamp.
    Rig g(0);
    g.e.set_flow(true);
    g.e.set_scan(sampler_cfg::kScanKnee);   // realtime forward
    g.e.set_recording(true);

    const float lag_frames = sampler_cfg::kScanRecordLagS * 48000.f;
    for (int i = 0; i < 4800; ++i) {         // 0.1 s: content stays far below lag
        g.e.process_in(0.f, 0.f);
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        REQUIRE(float(g.e.rec_size()) < lag_frames);   // the condition under test
        CHECK(g.e.scan_pos() == 0.f);                  // clamped to the floor, not negative
    }
    g.e.set_recording(false);
}

// --- Review 2026-07-22: Spawn-Rate und CPU-Boden ---

TEST_CASE("F-01: the spawn rate matches nominal even with timing jitter") {
    // Der Timing-Jitter ist symmetrisch gezogen (Rng::next_bipolar), also
    // muss er die MITTLERE Rate unangetastet lassen. Vor dem Fix kappte der
    // Clamp in _update_control nur die zu LANGEN Intervalle, womit die Rate
    // bei MOTION = 1 um rund 20 % zu hoch lag.
    //
    // MOTION wird hier ausdruecklich gesetzt statt auf dem Lane-Default zu
    // ruhen: F-04 aendert diesen Default, und dieser Test misst den
    // Scheduler, nicht die Lane.
    for (float size : {0.2f, 0.5f, 0.8f}) {
        Rig g;
        g.feed(0.5f, 0.f, size, 1.f, 1.f);      // MOTION = 1: maximaler Jitter
        g.e.set_overlap(1.f);
        g.e.set_flow(true);

        const int kSamples = 48000 * 10;
        g.render(kSamples);

        const float every   = g.e.spawn_interval_samples();
        const float nominal = float(kSamples) / every;
        // Verlorene Spawns zaehlen mit: gemessen wird die RATE des
        // Schedulers, nicht wie viele Slots gerade frei waren.
        const float actual  = float(g.e.spawn_count() + g.e.dropped_spawns());

        INFO("SIZE=" << size << " spawn_every=" << every
             << " nominal=" << nominal << " actual=" << actual);
        CHECK(actual == doctest::Approx(nominal).epsilon(0.03));
    }
}

TEST_CASE("F-03: the CPU floor bounds the spawn rate WITH jitter applied") {
    // sampler_config.h:73 sagt zu, kSpawnMinSamples deckle die Spawn-Rate
    // bei 6 kHz pro Part. Vor dem Fix multiplizierte der Jitter erst NACH
    // dem Boden, sodass real 2-Sample-Intervalle auftraten (24 kHz).
    using namespace spky::sampler_cfg;
    for (float motion : {0.5f, 1.f}) {
        Rig g;
        g.feed(0.5f, 0.f, 0.f, motion, 1.f);    // SIZE 0 -> kuerzestes Grain
        g.e.set_overlap(1.f);                   // overlap 8 -> 48/8 = 6 < Boden
        g.e.set_flow(true);

        int prev = g.e.spawn_count() + g.e.dropped_spawns();
        int last_i = 0, shortest = 1 << 30;
        for (int i = 0; i < 48000 * 5; ++i) {
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
            const int now = g.e.spawn_count() + g.e.dropped_spawns();
            if (now != prev) {
                if (last_i != 0 && i - last_i < shortest) shortest = i - last_i;
                last_i = i;
                prev = now;
            }
        }
        INFO("MOTION=" << motion << " shortest interval=" << shortest);
        CHECK(float(shortest) >= kSpawnMinSamples);
    }
}

TEST_CASE("F-01: a shrinking SIZE still cancels a long pending countdown") {
    // Das ist der Zweck, den der Clamp urspruenglich hatte, und er muss den
    // Fix ueberleben: faehrt SIZE herunter, darf kein Countdown des ALTEN,
    // langen Intervalls stehenbleiben und die Wolke verstummen lassen.
    Rig g;
    g.feed(0.5f, 0.f, 0.9f, 0.f, 1.f);          // langes Grain, kein Jitter
    g.e.set_overlap(0.f);                       // overlap 1 -> Intervall = Grain
    g.e.set_flow(true);
    g.render(96);                               // ein Control-Tick: Spawn laeuft
    const float long_every = g.e.spawn_interval_samples();
    REQUIRE(long_every > 100000.f);

    g.feed(0.5f, 0.f, 0.3f, 0.f, 1.f);          // SIZE faellt drastisch
    g.render(96);
    const float short_every = g.e.spawn_interval_samples();
    REQUIRE(short_every < long_every / 10.f);

    // Innerhalb zweier neuer Intervalle muss wieder gespawnt werden.
    const int before = g.e.spawn_count();
    g.render(int(short_every) * 2 + 96);
    CHECK(g.e.spawn_count() > before);
}

TEST_CASE("F-02: a gate edge does not re-phase the FLOW scheduler") {
    // Im FLOW laeuft der Scheduler ohnehin. _spawn_ctr = 0 auf der Flanke
    // erzwingt dort einen Sofort-Spawn, und Part liefert eine Flanke pro
    // PITCH-Zyklus auch im FLOW -- bei langem SIZE und kleinem DENS spawnt
    // die Wolke dadurch im Phrasenrhythmus statt nach DENS.
    Rig g;
    g.feed(0.5f, 0.f, 1.0f, 0.f, 1.f);          // SIZE max
    g.e.set_overlap(0.f);                       // DENS min -> overlap 1
    g.e.set_flow(true);
    g.render(96);
    const float every = g.e.spawn_interval_samples();
    REQUIRE(every > 48000.f * 20.f);            // ~42 s Grundintervall

    // Fuenf Gate-Flanken, eine alle 9600 Samples (0.2 s), wie ein PITCH-Zyklus
    // sie liefert -- zusammen 1 s, also ein Vierzigstel des 42-s-Intervalls.
    // Jede Flanke, die durchkaeme, waere ein Spawn, den DENS nicht bestellt hat.
    for (int k = 0; k < 5; ++k) {
        g.e.set_gate(true);  g.render(240);
        g.e.set_gate(false); g.render(9360);
    }
    const int total = g.e.spawn_count() + g.e.dropped_spawns();
    INFO("spawn_every=" << every << " total spawns in 1 s=" << total);
    CHECK(total <= 2);                          // der Anfangsspawn, sonst nichts
}

// DELETED (Task 9): "F-02: a gate edge still starts a STEP burst
// immediately". It pinned `_spawn_ctr = 0` on the rising gate edge -- the arm
// that made the old free-running STEP burst fire on the beat instead of up to
// one spawn interval late. Task 5 removed both the arm and the burst: a STEP
// grain now comes from trigger()/trigger_chord() calling _fire_slice directly,
// so it is on the composed beat by construction, with no scheduler left to
// re-phase. Nothing remains for that test to observe.
//
// What replaces it, in two halves:
//   - the timing requirement it stood for -- "STEP fires on the composed
//     rhythm, not late" -- is now carried by "sampler STEP: a fire spawns
//     exactly one grain at a slice start", which sees the fire land within 64
//     samples of trigger().
//   - the INVERSE, which only became a live risk once the arm was gone, is
//     the case below.
TEST_CASE("F-02: in STEP the gate arms nothing -- only a fire spawns") {
    // The STEP counterpart to "a gate edge does not re-phase the FLOW
    // scheduler" above. In STEP the gate is now purely a note-length signal:
    // it releases what is sounding on its falling edge and it lets rolls
    // retrigger, but it must never put a grain into the pool by itself.
    // Re-introducing any spawn-on-edge arm -- the obvious "fix" for anyone who
    // reads the deleted test's title without its history -- would double the
    // first grain of every composed note.
    Rig g;
    g.feed(0.5f, 0.f, 0.5f, 0.f, 1.f);
    g.e.set_flow(false);
    g.render(4800);
    const int before = g.e.spawn_count() + g.e.dropped_spawns();

    for (int k = 0; k < 5; ++k) {               // five clean gate edges
        g.e.set_gate(true);  g.render(2400);
        g.e.set_gate(false); g.render(2400);
    }
    const int after = g.e.spawn_count() + g.e.dropped_spawns();
    INFO("before=" << before << " after=" << after);
    CHECK(after == before);                     // no edge ever spawned anything

    // ...and the rig is not simply dead: the same engine fires on a trigger.
    g.e.trigger(0.5f);
    g.render(64);
    CHECK(g.e.spawn_count() + g.e.dropped_spawns() == after + 1);
}

TEST_CASE("F-09: grain length stays under the _off stall bound at any DENS") {
    // grain.h begruendet die Stall-Freiheit mit der Pool-Decke bei kOverlap
    // = 8 (4 032 000). Seit DENS zur Laufzeit auf overlap 1 gehen kann, ist
    // die Decke 32 256 000 -- weit ueber 2^23, wo _off in float32 einfriert
    // und das Grain fuer den Rest seiner Lebensdauer DC ausgibt.
    using namespace spky::sampler_cfg;
    Rig g;
    g.feed(0.f, 0.f, 1.0f, 0.f, 1.f);       // SIZE max (PITCH lane does not
                                             // reach the sampler ratio --
                                             // set_chord below does)
    g.e.set_tape_mode(true);                // Tape: lenf = _grain_len / ratio
    g.e.set_overlap(0.f);                   // DENS min -> overlap 1
    const float lowest = 0.f;               // TUNE 0 -> ratio 2^-4 = 0.0625
    g.e.set_chord(&lowest, 1);
    g.e.set_flow(true);
    g.render(48000);

    REQUIRE(g.e.spawn_count() > 0);
    INFO("last_spawn_len=" << g.e.last_spawn_len()
         << " ceil=" << kGrainLenCeil);
    CHECK(float(g.e.last_spawn_len()) <= kGrainLenCeil);
}

TEST_CASE("F-09: a stalled grain would emit DC -- the guard keeps it moving") {
    // Der Verhaltenstest hinter der Zahl. Ein isoliertes Grain statt der
    // FLOW-Wolke: bei FLOW ueberlappen bis zu kGrains phasenversetzte Kopien
    // desselben Ratios, und ein frisch gespawnter Nachbar maskiert im
    // Summensignal jedes einzelne eingefrorene Grain -- probeweise blieb die
    // FLOW-Wolke bei diesem Setup ueber 40e6 Samples hoerbar in Bewegung,
    // obwohl das aelteste Grain laengst haette einfrieren muessen. Isoliert
    // (FLOW aus, ein Gate-Puls) zeigt sich der Stall unmaskiert.
    //
    // Die Pruefung selbst ist eine Disjunktion, nicht "bewegt sich noch":
    // ein Grain, das laengst uebliche zuende gegangen ist, ist gesund, egal
    // ob das Fenster in dem Moment gerade zufaellig ausklingt. Der Bug ist
    // ausschliesslich "noch aktiv UND eingefroren". Das macht die Pruefung
    // robust gegen den Mechanismus des jeweiligen Fixes: der hier gewaehlte
    // (eine harte Laengendecke) beendet das Grain, bevor es je in die
    // Stall-Zone kaeme, und beweist damit nicht per se, dass die
    // Leseposition dort noch fein genug schreitet -- nur, dass niemand mehr
    // zuhoert, wenn sie es nicht mehr taete.
    Rig g;
    g.feed(0.f, 0.f, 1.0f, 0.f, 1.f);        // SIZE max (see set_chord below
                                              // for why PITCH is set there)
    g.e.set_tape_mode(true);
    g.e.set_overlap(0.f);                    // DENS min -> overlap 1
    const float lowest = 0.f;                // TUNE 0 -> ratio 2^-4 = 0.0625
    g.e.set_chord(&lowest, 1);
    g.e.set_flow(false);
    // Seit Task 5 (slice groove) feuert nicht mehr die Gate-Flanke, sondern
    // die komponierte Note selbst: ein trigger() == ein Slice-Grain. Das Gate
    // wird hier gar nicht mehr gebraucht -- ohne freilaufende Spawns unter
    // dem Gate gibt es nichts mehr abzuschalten, und ein set_gate(false)
    // wuerde das eine Grain sofort releasen und den Stall-Test entwerten.
    // trigger(0.f) latcht dasselbe Ratio 2^-4, das set_chord oben stellt.
    g.e.trigger(0.f);                        // ein Fire: ein Spawn
    g.render(96);
    REQUIRE(g.e.spawn_count() == 1);

    // 19 Mio. Samples vorspulen -- weit jenseits der empirisch gemessenen
    // Einfrier-Schwelle dieses Setups (~17-18e6, siehe grain.h) und weit
    // jenseits von kGrainLenCeil (4 194 304 = 87 s) -- und dann ein enges,
    // 1-Sekunden-Fenster (statt "die zweite Haelfte" eines riesigen Renders)
    // messen: ein grobes Fenster ueber Millionen Samples faengt sowohl die
    // noch bewegte Fruehphase als auch eine spaeter eingefrorene Spaetphase
    // ein und mittelt den Stall damit weg.
    g.render(19000000);
    auto v = g.render(48000);
    float lo = 1e9f, hi = -1e9f;
    for (float x : v) { if (x < lo) lo = x; if (x > hi) hi = x; }
    const bool retired = g.e.active_grains() == 0;
    INFO("active_grains=" << g.e.active_grains()
         << " signal range at ~19e6 samples in: " << lo << " .. " << hi);
    CHECK((retired || (hi - lo > 1e-4f)));
}

TEST_CASE("F-10: the tape ceiling binds at one octave down, not at an extreme") {
    // Kein Verhaltensfix, sondern die Zahl, die der Kommentar am Cap nennen
    // muss. Bei DENS max ist len_ceil = (overlap + kSpawnHeadroom) /
    // overlap * _grain_len, und Tape gibt lenf = _grain_len / ratio -- die
    // Decke greift also schon knapp unter Unity, bei ganz normalem SIZE.
    // Deshalb hier bewusst KEIN feed()-Aufruf, der SIZE aendert -- der
    // Rig-Default (0.5) ist Teil des Befunds, nicht ein Extremwert wie
    // SIZE 1.0 in F-09.
    //
    // Der Titel sagt "eine Oktave", und das war die Zahl, solange len_ceil
    // gegen kGrains rechnete (Faktor 2 bei DENS max, also ratio 0.5). Seit
    // dem Dichtedeckel rechnet sie gegen die ERREICHBARE Poolgroesse
    // (sampler_engine.cpp, len_ceil), und die ist bei DENS max 9 statt 16 --
    // Faktor 1.125, also bindet die Decke schon ab ratio 8/9 = 0.889, rund
    // zwei Halbtoenen abwaerts. Der Befund ist damit nicht widerlegt,
    // sondern verschaerft: die Decke sitzt noch weiter im gewoehnlichen
    // Bereich als bei ihrer Aufnahme. Alle Zahlen unten haengen an
    // kSpawnHeadroom, damit sie der Konstanten folgen statt ihr
    // hinterherzulaufen.
    //
    // Die PITCH-Spur von feed() erreicht das Sampler-Ratio nicht (siehe
    // ratio_for's Kommentar und F-09's set_chord-Aufrufe): _next_ratio()
    // liest _chord_ratio, gefuettert ueber set_chord(). Die pitch_norm-Werte
    // unten sind also fuer set_chord, nicht die feed()-PITCH-Spur, und
    // test_ratio_for() macht das resultierende Ratio direkt nachpruefbar,
    // statt es aus 8^(p-0.5) zu erraten -- unterhalb von kPitchKneeLo gilt
    // diese Formel ohnehin nicht mehr.
    Rig g;
    g.e.set_tape_mode(true);
    g.e.set_overlap(1.f);                   // DENS max -> overlap 8
    g.e.set_flow(true);

    // Der Deckenfaktor bei DENS max, aus denselben Groessen gerechnet, die
    // _spawn_one benutzt: erreichbare Slots geteilt durch Overlap.
    const float kOvlMax = spky::sampler_cfg::kOverlapMax;
    const float ceil_fac =
        (std::ceil(kOvlMax) + float(spky::sampler_cfg::kSpawnHeadroom)) / kOvlMax;

    // Diesseits der Decke: knapp unter Unity, wo Tape noch streckt. Bei
    // ceil_fac = 1.125 muss ratio ueber 0.889 liegen; 0.95 laesst zu beiden
    // Seiten Luft.
    const float p_shy = 0.4753f;
    const float ratio_shy = spky::test_ratio_for(p_shy);
    REQUIRE(ratio_shy > 1.f / ceil_fac);
    REQUIRE(ratio_shy < 1.f);
    g.e.set_chord(&p_shy, 1);
    g.render(48000);
    const float len_shy = float(g.e.last_spawn_len());
    const float base    = g.e.grain_len_samples();
    INFO("ratio=" << ratio_shy << " len=" << len_shy << " base=" << base);
    // Zwei Aussagen, und die zweite ist die, die den Test von "streckt
    // ueberhaupt" zu "streckt UNGEKAPPT" macht: die Laenge ist genau
    // base / ratio, und sie liegt unter der Decke. Ohne die zweite wuerde
    // eine Decke, die auf einen Wert zwischen base und base/ratio kappt,
    // hier noch durchgehen.
    CHECK(len_shy == doctest::Approx(base / ratio_shy).epsilon(0.01));
    CHECK(len_shy < ceil_fac * base);

    // Jenseits der Decke -- nicht die alten vier Oktaven / 45 Minuten aus
    // dem urspruenglichen Kommentar, sondern tief genug, dass die Decke
    // sicher bindet.
    Rig g2;
    g2.e.set_tape_mode(true);
    g2.e.set_overlap(1.f);
    g2.e.set_flow(true);
    const float p_deep = 0.2f;
    const float ratio_deep = spky::test_ratio_for(p_deep);
    REQUIRE(ratio_deep < 0.5f);
    g2.e.set_chord(&p_deep, 1);
    g2.render(48000);
    const float len_deep = float(g2.e.last_spawn_len());
    const float base2    = g2.e.grain_len_samples();
    INFO("ratio=" << ratio_deep << " len=" << len_deep << " base=" << base2);
    CHECK(len_deep == doctest::Approx(ceil_fac * base2).epsilon(0.01));
}

// --- SIZE is asymmetrically live -------------------------------------------
//
// Bastian, 2026-07-22: "wenn ich len aufdrehe dann laeuft die Geschichte ja
// sehr lange, wenn ich len dann wieder runter drehe aktualisiert er nicht die
// Laenge der laufenden Grains. Unter Umstaenden laeuft da was minutenlang und
// ich kann es nicht abbrechen."
//
// He was right, and it was not a small window: at SIZE 1.0 a grain is 42 s
// (kSizeCeilS), in tape mode the pool ceiling allows 84 s and kGrainLenCeil
// 87 s. Nothing on the deck could stop it -- _release_all has exactly one
// caller, set_hold, and that is driven only by the OTHER part's CHOKE window
// (instrument.cpp:110). Leaving FLOW deliberately does not release either
// (grain.h's release() comment).
//
// The fix is deliberately one-directional. Turning SIZE DOWN re-scales every
// running grain as if it had been spawned at the new size; turning it UP
// changes nothing, so the "cloud drags behind a moving lane" character the
// Grain class was built around survives on the way up.
//
// These tests switch FLOW off after priming so that no new grains muddy the
// measurement -- FLOW off stops spawning but, by design, does not touch what
// is already running, which is exactly the isolation wanted here.

// Samples until the render goes and stays quiet, or -1 if it never does.
static int silence_after(const std::vector<float>& v, float thresh = 1e-4f) {
    for (size_t i = v.size(); i-- > 0; )
        if (std::fabs(v[i]) > thresh) return static_cast<int>(i) + 1;
    return 0;
}

TEST_CASE("sampler: SIZE turned down cuts the cloud that is already sounding") {
    // SIZE 0.8 sits on the exponential segment: 0.02 * 100^0.8 = 0.796 s,
    // 38,218 samples. SIZE 0.2 is 0.05 s, 2,411 -- a factor of ~16 down.
    // 3,000 and not more: the spawn interval at SIZE 0.8 is 38,218 / 8 =
    // 4,777, so exactly ONE grain is running and the tail below measures it
    // alone. Priming past the interval leaves a second, younger grain whose
    // own rescaled length is what the render then shows.
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.8f);
    g.render(3000);                       // a long grain is running
    g.e.set_flow(false);                  // no new spawns from here on

    // Control: left alone, it keeps sounding for the rest of its 38,218.
    {
        Rig h;
        h.e.set_flow(true);
        h.feed(0.5f, 0.f, 0.8f);
        h.render(3000);
        h.e.set_flow(false);
        auto tail = h.render(20000);
        REQUIRE(silence_after(tail) > 19000);
    }

    g.feed(0.5f, 0.f, 0.2f);              // SIZE down
    auto tail = g.render(20000);
    // 38,218 * (2,411 / 38,218) = 2,411 total, and 3,000 are already played,
    // so the cap is in the past: the grain gets the click-free floor and is
    // gone within a few hundred samples, not twenty thousand.
    CHECK(silence_after(tail) < 500);
}

TEST_CASE("sampler: SIZE turned up does not stretch what is already sounding") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.2f);              // short grains, 2,411 samples
    g.render(500);
    g.e.set_flow(false);

    g.feed(0.5f, 0.f, 0.8f);              // SIZE up -- must change nothing
    auto tail = g.render(8000);
    // The grain still ends on its own 2,411 (minus the 500 already rendered),
    // not stretched to 38,218.
    const int end = silence_after(tail);
    CHECK(end > 1500);
    CHECK(end < 2500);
}

TEST_CASE("sampler: a SIZE wobble that returns does not compound") {
    // The rejected design multiplied the remaining life by (new / previous)
    // on every control tick where SIZE fell. That telescopes: a lane
    // modulating SIZE would shorten the cloud by the full ratio once per LFO
    // cycle and the cloud would collapse over a few seconds. Scaling against
    // the size the grain was SPAWNED at instead is idempotent -- the result
    // depends only on where SIZE is now, not on how it got there.
    // Both variants render the SAME number of samples and differ only in how
    // many of the eight blocks dip SIZE. Comparing two runs of unequal length
    // would just measure the extra rendering (the first draft did, and read
    // as a 2,800-sample "compounding" that was nothing but elapsed time).
    auto lifetime = [](int dips) {
        Rig g;
        g.e.set_flow(true);
        g.feed(0.5f, 0.f, 0.8f);
        g.render(500);
        g.e.set_flow(false);

        std::vector<float> all;
        auto add = [&all](std::vector<float> v) {
            all.insert(all.end(), v.begin(), v.end());
        };
        for (int i = 0; i < 8; ++i) {
            g.feed(0.5f, 0.f, i < dips ? 0.5f : 0.8f);   // down to 0.2 s
            add(g.render(200));           // > kCtrlInterval, so it lands
            g.feed(0.5f, 0.f, 0.8f);      // and back up
            add(g.render(200));
        }
        add(g.render(30000));
        return silence_after(all);
    };
    const int none = lifetime(0);
    const int once = lifetime(1);
    const int many = lifetime(8);
    REQUIRE(none > once + 1000);          // the trim did something...
    CHECK(many > once - 200);             // ...and doing it 8x does no more
    CHECK(many < once + 200);
}

TEST_CASE("sampler: tape keeps its smear when SIZE is trimmed") {
    // Tape length is _grain_len / ratio, so a tape grain is legitimately
    // LONGER than the current SIZE. Capping at SIZE itself would silently
    // end the "low notes smear long" promise the moment the knob moved; the
    // scaling rule keeps the proportion.
    auto tape_life = [](float size_now) {
        Rig g;
        g.e.set_tape_mode(true);
        g.e.set_flow(true);
        g.feed(0.25f, 0.f, 0.5f);         // pitched down -> a long tape grain
        g.render(500);
        g.e.set_flow(false);
        g.feed(0.25f, 0.f, size_now);
        return silence_after(g.render(120000));
    };
    const int untouched = tape_life(0.5f);
    const int halved    = tape_life(0.35f);   // 0.02*100^0.35 / 0.2 = 0.502
    REQUIRE(untouched > 5000);
    // Still a long grain, just proportionally shorter -- not clipped to the
    // 0.1 s that SIZE 0.35 alone would give.
    CHECK(halved < untouched);
    CHECK(halved > 0.25f * static_cast<float>(untouched));
}

// Click-train content for the slice tests: n clicks evenly spaced, 5 ms decay.
static void feed_clicks(Rig& g, size_t content, int n) {
    std::vector<float> l(content, 0.f), r;
    const size_t gap = content / size_t(n);
    for (int c = 0; c < n; ++c)
        for (size_t i = 0; i < 240 && size_t(c) * gap + i < content; ++i)
            l[size_t(c) * gap + i] = std::exp(-float(i) / 60.f);
    r = l;
    g.e.load_sample(l.data(), r.data(), content);
}

TEST_CASE("sampler: load_sample scans the material into slices") {
    Rig g(0);                        // no preloaded content
    feed_clicks(g, 48000, 8);
    CHECK(g.e.slice_count() == 8);
}

TEST_CASE("sampler: recording detects slices as it writes") {
    Rig g(0);
    g.e.set_recording(true);
    // 1 s of input with 4 clicks, fed through process_in like a host would
    for (int i = 0; i < 48000; ++i) {
        float x = 0.f;
        for (int c = 0; c < 4; ++c) {
            const int at = c * 12000;
            if (i >= at && i < at + 240) x = std::exp(-float(i - at) / 60.f);
        }
        g.e.process_in(x, x);
        float a, b; g.e.process(a, b);
    }
    g.e.set_recording(false);
    REQUIRE(g.e.slice_count() == 4);
    // Not just the count -- the markers must land at the RIGHT frames, or a
    // systematic off-by-one in the `head` snapshot (process_in) would pass
    // silently. Same tolerance test_slice_map.cpp uses for its own scan
    // tests: within [click - preroll, click + a few ms detector lag].
    const int pre = int(sampler_cfg::kOnsetPreRollS * 48000.f);
    for (int c = 0; c < 4; ++c) {
        const uint32_t at = static_cast<uint32_t>(c * 12000);
        const uint32_t lo = at > static_cast<uint32_t>(pre) ? at - static_cast<uint32_t>(pre) : 0;
        CHECK(g.e.slice_start(c) >= lo);
        CHECK(g.e.slice_start(c) <= at + 144);
    }
}

// Proves Finding 1's guard is load-bearing: set_recording(true) immediately
// followed by set_recording(false), with no write in between, leaves
// SampleBuffer in State::fadeout with _fade_ctr == 0 -- idle->fadein just set
// it to 0, and the immediate fadein->fadeout transition (set_recording's
// default branch) never touches _fade_ctr. The next write() then hits the
// early return documented at sample_buffer.cpp:131-150 ("two REC toggles
// inside one audio block") and flips to idle WITHOUT writing or moving the
// head. A flag-based is_recording()-before-the-call check cannot see that:
// is_recording() is still true entering the call, so it would still call
// SliceMap::on_write(0, ...) even though frame 0 was never touched. Measured
// effect with the flag-based guard reverted: on_write's own "new take" path
// (_last_frame == SIZE_MAX on this SliceMap's very first live call) re-aims
// its sweep at frame 0 and wipes the marker load_sample legitimately placed
// there -- slice_count() drops from 1 to 0, not up. Either direction is the
// same defect: a write() call that never happened must not reach SliceMap
// at all. The head-advance guard (process_in) must suppress it.
TEST_CASE("sampler: a same-block REC on/off does not plant a phantom marker") {
    Rig g(0);
    // Loud pre-existing content at frame 0, so the spurious on_write() (if
    // the guard fails) has real content to corrupt, not silence.
    std::vector<float> l(4800), r;
    for (size_t i = 0; i < l.size(); ++i) l[i] = std::sin(6.2831853f * 441.f * float(i) / 48000.f);
    r = l;
    g.e.load_sample(l.data(), r.data(), l.size());
    const int before = g.e.slice_count();

    g.e.set_recording(true);
    g.e.set_recording(false);           // fadein -> fadeout, _fade_ctr == 0, nothing written yet
    float a, b;
    g.e.process_in(0.f, 0.f);           // drives write()'s _fade_ctr == 0 early return
    g.e.process(a, b);

    CHECK(g.e.slice_count() == before); // untouched frame 0 must not reach SliceMap at all
}

TEST_CASE("sampler: clear drops the slices with the content") {
    Rig g(0);
    feed_clicks(g, 48000, 8);
    REQUIRE(g.e.slice_count() == 8);
    g.e.clear();
    CHECK(g.e.slice_count() == 0);
}

// --- Task 5: the STEP slice core -----------------------------------------

// STEP slice rig: clicky content, STEP mode, phrase context pushed by hand
// the way Part will in Task 8. 8 clicks over 1 s -> 8 slices; step clock
// 6000 samples (an 8th at 120-ish bpm).
struct StepRig : Rig {
    StepRig() : Rig(0) {
        feed_clicks(*this, 48000, 8);
        e.set_flow(false);
        e.set_step_clock(6000.f);
        feed(0.5f);                     // MOTION 0, SIZE 0.5 (slice unity)
    }
    // one composed note: phrase position, latch, gate on
    void fire(int slot, int steps = 8, float weight = 1.f) {
        e.set_phrase_pos(slot, steps, weight);
        e.trigger(0.5f);
        e.set_gate(true);
    }
    void note_off() { e.set_gate(false); }
};

TEST_CASE("sampler STEP: a fire spawns exactly one grain at a slice start") {
    StepRig g;
    const int before = g.e.spawn_count();
    g.fire(0);
    g.render(64);
    CHECK(g.e.spawn_count() == before + 1);
    // spawn position sits on a marker
    bool on_marker = false;
    for (int i = 0; i < g.e.slice_count(); ++i)
        if (std::fabs(g.e.last_spawn_pos() - float(i * 6000)) < 200.f)
            on_marker = true;
    CHECK(on_marker);
}

TEST_CASE("sampler STEP: MOTION 0 walks the slices in order and wraps home") {
    StepRig g;
    std::vector<int> seq;
    for (int cycle = 0; cycle < 2; ++cycle)
        for (int slot = 0; slot < 4; ++slot) {
            g.fire(slot);
            g.render(64);
            seq.push_back(g.e.last_slice());
            g.note_off();
            g.render(64);
        }
    // ascending within a cycle, identical across cycles
    for (int i = 0; i < 3; ++i) CHECK(seq[i + 1] == (seq[i] + 1) % 8);
    for (int i = 0; i < 4; ++i) CHECK(seq[i] == seq[i + 4]);
}

TEST_CASE("sampler STEP: SIZE centre is slice unity, the ends trim and overrun") {
    StepRig g;
    g.fire(0);
    g.render(64);
    const int at_unity = g.e.last_spawn_len();
    CHECK(at_unity == doctest::Approx(6000).epsilon(0.05));
    g.note_off(); g.render(6000);
    g.feed(0.5f, 0.f, 0.f);            // SIZE 0: attack tip
    g.render(96);                       // let the control tick see it
    g.fire(1);
    g.render(64);
    CHECK(g.e.last_spawn_len() < at_unity / 8);
    g.note_off(); g.render(6000);
    g.feed(0.5f, 0.f, 1.f);            // SIZE 1: overrun
    g.render(96);
    g.fire(2);
    g.render(64);
    CHECK(g.e.last_spawn_len() > at_unity * 8);
}

TEST_CASE("sampler STEP: the gate falling releases the grain, no burst tail") {
    StepRig g;
    g.feed(0.5f, 0.f, 1.f);            // long window so it outlives the note
    g.render(96);
    g.fire(0);
    // The measurement window has to land on MATERIAL, and this rig's material
    // is a click train: the grain starts on the marker at frame 0 at ratio 1,
    // so output sample n reads frame n, and everything between the clicks
    // (6000 apart) is genuine silence. Measured at the brief's original
    // window (1000..1500): rms exactly 0 with a live 94464-sample grain
    // sitting at pos 0 -- the window, not the engine. Sample 6000 is the
    // second click, still far inside the grain, and reads 0.058.
    // 6000..6500 was the original window and it passed only by straddling
    // exactly one click; the whole 0..8000 span holds two clicks either way,
    // so it survives a change to the ratio, the marker pre-roll or the attack.
    auto v = g.render(8000);
    CHECK(rms(v, 0, 8000) > 0.001f);   // sounding under the gate
    g.note_off();
    g.render(48000);                    // far past any release fade
    CHECK(g.e.active_grains() == 0);   // released, not sustained
    const int n = g.e.spawn_count();
    g.render(12000);
    CHECK(g.e.spawn_count() == n);     // and nothing respawns after the gate
}

// The Rng draw order inside a STEP fire is a CONTRACT: walk, roll, pan, all
// three drawn every fire, in that order. Walk only starts being applied in
// Task 6 and roll in Task 7, so nothing else in the suite would notice if
// those lines were reordered or made conditional -- and the sharpest way to
// make one conditional is to skip the draws when the fire is going to be
// dropped at the grain density ceiling. The drop happens INSIDE _spawn_slice,
// strictly after the three draws, and it has to stay that way.
//
// The proof: run the same fire sequence twice, once where the middle fires
// hit the ceiling and drop, once where they all spawn. If the dropped fires
// consumed their draws, the Rng is at the identical position by the last
// fire, and that last fire's pan -- the only draw-derived value the engine
// exposes -- is bit-identical between the runs.
TEST_CASE("sampler STEP: a dropped fire still consumes its Rng draws") {
    // MOTION well off 0 is load-bearing: pan is `draw * motion`, so at the
    // rig's default MOTION 0 every pan is 0.f and this test would pass
    // vacuously no matter which draw (or none) fed it.
    const float kMotion = 0.8f;
    const int   kFires  = 6;

    // `drop`: hold every note, so grains pile up and the later fires hit the
    // ceiling. Otherwise: release after each fire so every one of them spawns.
    auto last_pan_after = [&](bool drop) {
        StepRig g;
        g.e.set_overlap(0.f);              // DENS min -> ceiling 1 + headroom
        g.feed(0.5f, 0.f, 1.f, kMotion);   // SIZE 1: grains outlive the fires
        g.render(96);                       // let the control tick see it
        for (int i = 0; i < kFires; ++i) {
            g.fire(i);
            g.render(64);
            if (!drop) { g.note_off(); g.render(48000); }
        }
        const int dropped = g.e.dropped_spawns();
        // Clear the pool, then one final fire that succeeds in BOTH runs.
        g.note_off();
        g.render(48000);
        REQUIRE(g.e.active_grains() == 0);
        const int before = g.e.spawn_count();
        g.fire(kFires);
        g.render(64);
        REQUIRE(g.e.spawn_count() == before + 1);   // it really landed
        // Report the drop count alongside so the caller can assert the two
        // runs actually differed in outcome.
        return std::make_pair(g.e.last_spawn_pan(), dropped);
    };

    const auto with_drops = last_pan_after(true);
    const auto no_drops   = last_pan_after(false);

    REQUIRE(with_drops.second > 0);          // the ceiling really bit
    REQUIRE(no_drops.second == 0);           // ...and only in the first run
    REQUIRE(std::fabs(no_drops.first) > 0.01f);   // pan is not vacuously 0
    // Same number of fires -> same number of draws -> same pan, regardless of
    // how many of those fires survived the ceiling.
    CHECK(with_drops.first == doctest::Approx(no_drops.first).epsilon(1e-6));

    // The pair above pins the draw COUNT, not the ORDER: swapping walk and pan
    // shifts both runs identically and slips straight through it. So pin the
    // POSITION too, with a golden value -- the first fire of a fresh rig at
    // seed 4242, where pan is the THIRD draw of the stream. Reorder the three
    // and pan reads a different draw and this literal moves.
    StepRig g;
    g.feed(0.5f, 0.f, 0.5f, kMotion);
    g.render(96);
    g.fire(0);
    g.render(64);
    CHECK(g.e.last_spawn_pan() == doctest::Approx(-0.2129190f).epsilon(1e-4));
}

// --- Task 6: the MOTION walk ---------------------------------------------

TEST_CASE("sampler STEP: MOTION 0 is structurally still -- no walk, centered pan") {
    StepRig g;
    for (int slot = 0; slot < 8; ++slot) {
        g.fire(slot);
        g.render(64);
        CHECK(g.e.last_spawn_pan() == 0.f);
        CHECK(g.e.last_slice() == slot);
        g.note_off();
        g.render(64);
    }
}

TEST_CASE("sampler STEP: MOTION 1 leaves the ordered path") {
    StepRig g;
    g.feed(0.5f, 0.f, 0.5f, 1.f);      // MOTION 1
    g.render(96);
    int deviations = 0;
    int prev = -1;
    for (int cycle = 0; cycle < 4; ++cycle)
        for (int slot = 0; slot < 8; ++slot) {
            g.fire(slot);
            g.render(64);
            const int s = g.e.last_slice();
            if (prev >= 0 && s != (prev + 1) % 8) ++deviations;
            prev = s;
            g.note_off();
            g.render(64);
        }
    // The cubed walk leaves small steps common: some fires still land in
    // order, but across 32 fires a fully-ordered run is out of the question.
    CHECK(deviations > 4);
}

TEST_CASE("sampler STEP: transientless material falls back to the tempo grid") {
    Rig g;                              // default rig: 441 Hz sine, no clicks
    g.e.set_flow(false);
    g.e.set_step_clock(6000.f);
    REQUIRE(g.e.slice_count() < sampler_cfg::kMinSlices);
    std::vector<float> pos;
    for (int slot = 0; slot < 3; ++slot) {
        g.e.set_phrase_pos(slot, 8, 1.f);
        g.e.trigger(0.5f);
        g.e.set_gate(true);
        g.render(64);
        pos.push_back(g.e.last_spawn_pos());
        g.e.set_gate(false);
        g.render(64);
    }
    // consecutive fires step exactly one step-clock through the material
    CHECK(std::fabs(pos[1] - pos[0] - 6000.f) < 1.f);
    CHECK(std::fabs(pos[2] - pos[1] - 6000.f) < 1.f);
}

// --- Task 7: rolls --------------------------------------------------------

TEST_CASE("sampler STEP: an off-beat at DENS max rolls at exactly step/subdiv") {
    StepRig g;
    g.e.set_overlap(1.f);              // DENS max -> subdiv cap 8
    g.render(96);
    // weight 0 = deepest off-beat -> roll probability 1 at DENS max
    g.fire(1, 8, 0.f);
    const int period = int(6000.f / 8.f);
    REQUIRE(g.e.retrig_period() == period);
    const int start = g.e.spawn_count();
    g.render(period * 4 + 8);
    CHECK(g.e.spawn_count() == start + 4);   // 4 retriggers, sample-exact
    g.note_off();
    g.render(period * 2);
    CHECK(g.e.spawn_count() == start + 4);   // gate fall stops the roll dead
}

TEST_CASE("sampler STEP: DENS min never rolls, whatever the dice say") {
    StepRig g;
    g.e.set_overlap(0.f);              // DENS min -> subdiv cap 1
    g.render(96);
    for (int i = 0; i < 16; ++i) {
        g.fire(i % 8, 8, 0.f);
        CHECK(g.e.retrig_period() == 0);
        g.note_off();
        g.render(128);
    }
}

TEST_CASE("sampler STEP: downbeats mostly hit once, offs roll -- the bias is real") {
    StepRig g;
    g.e.set_overlap(1.f);
    g.render(96);
    int down_rolls = 0, off_rolls = 0;
    for (int i = 0; i < 64; ++i) {
        g.fire(0, 8, 1.f);             // weight 1: downbeat
        if (g.e.retrig_period() > 0) ++down_rolls;
        g.note_off(); g.render(128);
        g.fire(1, 8, 0.2f);            // weight 0.2: off
        if (g.e.retrig_period() > 0) ++off_rolls;
        g.note_off(); g.render(128);
    }
    CHECK(down_rolls == 0);            // p = (1 - 1.0) * dens = 0, structural
    CHECK(off_rolls > 30);             // p = 0.8 at DENS max
}

// --- Task 9: the STEP golden vector ---------------------------------------
//
// --- STEP golden vector: the slice-groove Rng draw order is a hard contract:
// per fire: walk, roll, pan. Per roll retrigger: walk, pan. A fire that hits
// a full pool still draws all three (the drop happens in _spawn_slice, after
// the draws). Any change to this order re-pins this table AND the spec's
// Determinism section -- never regenerate silently.
//
// The FLOW golden vector further up this file ("sampler: golden vector -- Rng
// draw order and SOURCE mapping are locked") makes the same argument for the
// FLOW cloud, and everything its file-level comment says about marginal
// distributions applies here word for word: every other STEP test in this
// suite checks a MARGIN (the walk deviates, pan is centred at MOTION 0,
// downbeats do not roll, the roll period is step/subdiv). Successive xorshift32
// outputs are all uniform, so swapping walk and pan, or moving the roll draw
// to either side of them, leaves every one of those margins intact and the
// suite green. Only the SEQUENCE of drawn values catches it.
//
// This table is a GOLDEN VECTOR: captured once from a known-good build, not
// derived from first principles. If it fails after a change that looks
// unrelated -- a draw reordered, added or removed in _fire_slice() or the roll
// branch of process(), the seed decorrelation constant moved, the walk cubing
// or its pool scaling changed, SliceMap's marker placement shifted -- STOP.
// Ask WHY the numbers changed before touching them. Updating them to get back
// to green silently re-opens exactly the hole this test exists to close.
TEST_CASE("sampler STEP: golden vector -- the slice-groove draw order is locked") {
    StepRig g;                         // seed 4242, 8 clicks -> 8 slices,
                                       // step clock 6000
    // MOTION 0.5 makes all three draws OBSERVABLE at once: the walk scales by
    // MOTION (0 would multiply it away and freeze the slice order), pan scales
    // by MOTION (0 would make every pan literally 0.f and pin nothing), and
    // half-scale keeps the walk from saturating so the slice column stays
    // informative. SIZE 0.35 keeps the grains short enough that a rolling note
    // does not sit on the density ceiling for the whole table.
    g.feed(/*pitch*/0.5f, /*source*/0.f, /*size*/0.35f, /*motion*/0.5f);
    g.e.set_overlap(1.f);              // DENS max: subdiv cap 8, rolls live
    g.render(96);                       // let the control tick see SIZE/DENS

    // pg_metric_weight's shape over an 8-step phrase, spelled out rather than
    // included: this test pins the SAMPLER's draw order, and hard-wiring the
    // weights keeps a future edit to the phrase generator's accent profile
    // from re-pinning a table that has nothing to do with it.
    static const float weight[8] = { 1.f, 0.2f, 0.5f, 0.2f,
                                     0.35f, 0.2f, 0.5f, 0.2f };

    struct Sp { int slice; float pos, pan; int len; };
    std::vector<Sp> got;
    int last = g.e.spawn_count();
    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i) {
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
            if (g.e.spawn_count() != last) {
                last = g.e.spawn_count();
                got.push_back({ g.e.last_slice(), g.e.last_spawn_pos(),
                                g.e.last_spawn_pan(), g.e.last_spawn_len() });
            }
        }
    };
    // 12 fires across two phrase cycles: slots 0..7 then 0..3. The wrap at
    // fire 8 (slot 3 -> slot 0) is deliberate -- it drives _fire_slice's
    // "phrase went backwards, cursor goes home" branch inside the table.
    for (int fire = 0; fire < 12; ++fire) {
        const int slot = fire % 8;
        g.e.set_phrase_pos(slot, 8, weight[slot]);
        g.e.trigger(0.5f);
        g.e.set_gate(true);
        pump(6000);                     // one step of held note (rolls run)
        g.e.set_gate(false);
        pump(600);                      // release gap (rolls stop dead)
    }

    // Golden vector: seed 4242, StepRig (8 clicks over 48000 frames -> 8
    // slices, step clock 6000), SOURCE 0 / SIZE 0.35 / MOTION 0.5, DENS max,
    // the 12 fires above. One row per SPAWN, so fires and their roll
    // retriggers are interleaved: 12 fires produced 44 grains here, and that
    // COUNT is pinned too -- a roll that stopped arming, or one that ran at a
    // different subdivision, changes the LENGTH of this table before it
    // changes any single row.
    //
    // Reading the columns: `slice` is the walk's output (marker index), `pos`
    // the marker start minus SliceMap's pre-roll, `pan` the third draw of a
    // fire and the second of a retrigger, `len` the slice length scaled by
    // SIZE. `len` varies between 2569/2611/2653 because the click train's
    // detected markers are not perfectly equidistant -- that is the SliceMap
    // talking, and it is what makes `len` worth pinning at all.
    static const Sp golden[] = {
        { 1, 5904.f, -0.133075f, 2611 },
        { 2, 11904.f, -0.220712f, 2611 },
        { 5, 29904.f, -0.317578f, 2611 },
        { 5, 29904.f, -0.171948f, 2611 },
        { 5, 29904.f, -0.198368f, 2611 },
        { 7, 41904.f, 0.0372394f, 2653 },
        { 7, 41904.f, 0.139564f, 2653 },
        { 6, 35904.f, -0.475667f, 2611 },
        { 6, 35904.f, 0.483503f, 2611 },
        { 4, 23904.f, 0.216215f, 2611 },
        { 0, 0.f, 0.323182f, 2569 },
        { 1, 5904.f, 0.0905479f, 2611 },
        { 0, 0.f, 0.307441f, 2569 },
        { 1, 5904.f, -0.411203f, 2611 },
        { 1, 5904.f, 0.468652f, 2611 },
        { 6, 35904.f, -0.401812f, 2611 },
        { 5, 29904.f, -0.35603f, 2611 },
        { 2, 11904.f, 0.492735f, 2611 },
        { 1, 5904.f, 0.379274f, 2611 },
        { 1, 5904.f, 0.359537f, 2611 },
        { 1, 5904.f, -0.025275f, 2611 },
        { 4, 23904.f, 0.255384f, 2611 },
        { 4, 23904.f, -0.297428f, 2611 },
        { 6, 35904.f, -0.440616f, 2611 },
        { 6, 35904.f, -0.454998f, 2611 },
        { 6, 35904.f, -0.353762f, 2611 },
        { 6, 35904.f, -0.478825f, 2611 },
        { 3, 17904.f, 0.437026f, 2611 },
        { 3, 17904.f, 0.463213f, 2611 },
        { 0, 0.f, 0.457436f, 2569 },
        { 5, 29904.f, 0.337979f, 2611 },
        { 5, 29904.f, -0.421374f, 2611 },
        { 1, 5904.f, -0.291568f, 2611 },
        { 3, 17904.f, -0.0496832f, 2611 },
        { 3, 17904.f, 0.155703f, 2611 },
        { 3, 17904.f, 0.347724f, 2611 },
        { 4, 23904.f, 0.2745f, 2611 },
        { 4, 23904.f, -0.306532f, 2611 },
        { 4, 23904.f, 0.183447f, 2611 },
        { 3, 17904.f, 0.173856f, 2611 },
        { 3, 17904.f, -0.168819f, 2611 },
        { 3, 17904.f, -0.163568f, 2611 },
        { 2, 11904.f, 0.251479f, 2611 },
        { 3, 17904.f, -0.347649f, 2611 },
    };
    const size_t n = sizeof(golden) / sizeof(golden[0]);
    REQUIRE(got.size() == n);

    // Absolute, not relative, tolerances, for the reason spelled out at the
    // FLOW golden vector: pos lives in the tens of thousands, and a relative
    // epsilon would swallow the sub-sample shifts this table exists to catch.
    // slice and len are integers and are compared exactly.
    for (size_t i = 0; i < n; ++i) {
        INFO("spawn #", i);
        CHECK(got[i].slice == golden[i].slice);
        CHECK(std::fabs(got[i].pos - golden[i].pos) < 0.01f);
        CHECK(std::fabs(got[i].pan - golden[i].pan) < 0.00002f);
        CHECK(got[i].len == golden[i].len);
    }
}
