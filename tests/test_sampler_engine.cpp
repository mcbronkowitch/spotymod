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

    const size_t win = 2400;               // 50 ms
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
