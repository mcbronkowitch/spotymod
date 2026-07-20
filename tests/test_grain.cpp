#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "sampler/grain.h"
#include "sampler/sample_buffer.h"
using namespace spky;

static constexpr size_t kCap = 48000;

// A buffer holding one cycle-accurate sine at `hz`, declared as content.
struct SineBuf {
    std::vector<SampleBuffer::Frame> mem{kCap};
    SampleBuffer buf;
    explicit SineBuf(float hz = 441.f, size_t len = 48000) {
        // init() clears the buffer (SampleBuffer::init -> clear() memsets),
        // so content MUST be written after init, same idiom as
        // tests/test_sample_buffer.cpp -- otherwise every read is silence.
        buf.init(mem.data(), kCap, 48000.f);
        for (size_t i = 0; i < len; ++i) {
            const float s = std::sin(6.2831853f * hz * float(i) / 48000.f);
            mem[i].l = s;
            mem[i].r = s;
        }
        buf.set_rec_size(len);
    }
};

// A buffer whose L channel is a constant 1.0 and R a constant -1.0, so pan
// and window are readable without any signal shape in the way.
struct FlatBuf {
    std::vector<SampleBuffer::Frame> mem{kCap};
    SampleBuffer buf;
    FlatBuf() {
        // Same ordering requirement as SineBuf: init() clears the buffer.
        buf.init(mem.data(), kCap, 48000.f);
        for (size_t i = 0; i < kCap; ++i) { mem[i].l = 1.f; mem[i].r = -1.f; }
        buf.set_rec_size(kCap);
    }
};

static int zero_crossings(const std::vector<float>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] <= 0.f && v[i] > 0.f) ++n;
    return n;
}

TEST_CASE("grain: lifetime is exactly len samples, then inactive") {
    FlatBuf f;
    Grain g;
    g.spawn(0.f, 1.f, 0.f, 100, 10, 10, false);
    REQUIRE(g.active());
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 100; ++i) {
        REQUIRE(g.active());          // still alive on every one of the 100
        g.process(f.buf, l, r);
    }
    CHECK_FALSE(g.active());
    g.process(f.buf, l, r);            // an inactive grain is silent
    CHECK(l == 0.f);
    CHECK(r == 0.f);
}

TEST_CASE("grain: window rises from silence, holds, and falls to silence") {
    FlatBuf f;
    Grain g;
    const int len = 400, atk = 100, dec = 100;
    g.spawn(0.f, 1.f, 0.f, len, atk, dec, false);

    std::vector<float> env;
    for (int i = 0; i < len; ++i) {
        float l = 0.f, r = 0.f;
        g.process(f.buf, l, r);
        env.push_back(l);              // L is a constant 1.0 -> l IS the window * pan gain
    }
    REQUIRE(env.size() == size_t(len));

    // Ends at silence on both sides -- this is the anti-click contract.
    CHECK(env.front() == doctest::Approx(0.f).epsilon(0.001));
    CHECK(env.back()  == doctest::Approx(0.f).epsilon(0.02));
    // Rises monotonically through the attack...
    for (int i = 1; i < atk; ++i) CHECK(env[i] >= env[i - 1]);
    // ...falls monotonically through the decay...
    for (int i = len - dec + 1; i < len; ++i) CHECK(env[i] <= env[i - 1]);
    // ...and genuinely plateaus in between (else "rises then falls" is
    // satisfied by a triangle, which is not the window we specified).
    const float mid = env[len / 2];
    CHECK(mid > env[atk / 2] * 1.5f);
    CHECK(env[atk + 10] == doctest::Approx(mid).epsilon(0.01));
}

TEST_CASE("grain: attack and decay halves are clamped so they never overlap") {
    FlatBuf f;
    Grain g;
    // Ask for halves that sum to more than the grain: they must be scaled
    // down, not allowed to fight. Without the clamp, the attack branch (which
    // is checked first) governs all the way to i == atk - 1 and then the
    // decay branch's already-small value takes over at i == atk -- a cliff
    // in the middle of the window, not an out-of-range sample. Amplitude
    // bounds alone (peak/lowest) can't see a cliff, so this also tracks the
    // largest sample-to-sample jump.
    g.spawn(0.f, 1.f, 0.f, 100, 90, 90, false);
    float peak = 0.f, lowest = 1e9f, prev = 0.f, max_jump = 0.f;
    for (int i = 0; i < 100; ++i) {
        float l = 0.f, r = 0.f;
        g.process(f.buf, l, r);
        if (l > peak) peak = l;
        if (l < lowest) lowest = l;
        if (i > 0) {
            const float jump = std::fabs(l - prev);
            if (jump > max_jump) max_jump = jump;
        }
        prev = l;
    }
    CHECK(lowest >= -0.001f);          // never negative
    CHECK(peak > 0.5f);                // still opens properly
    CHECK(peak <= 1.001f);             // and never exceeds unity
    // The scaled attack and decay meet continuously -- no click in the middle.
    CHECK(max_jump < 0.1f);
}

TEST_CASE("grain: equal-power pan, hard left and hard right") {
    FlatBuf f;
    float l = 0.f, r = 0.f;

    Grain centre;
    centre.spawn(0.f, 1.f, 0.f, 200, 1, 1, false);
    for (int i = 0; i < 100; ++i) centre.process(f.buf, l, r);
    // L is +1 and R is -1 in the buffer, so equal gains means |l| == |r|.
    CHECK(std::fabs(l) == doctest::Approx(std::fabs(r)).epsilon(0.01));
    CHECK(std::fabs(l) == doctest::Approx(0.7071f).epsilon(0.02));

    Grain left;
    left.spawn(0.f, 1.f, -1.f, 200, 1, 1, false);
    for (int i = 0; i < 100; ++i) left.process(f.buf, l, r);
    CHECK(std::fabs(l) > 0.99f);
    CHECK(std::fabs(r) < 0.01f);

    Grain right;
    right.spawn(0.f, 1.f, 1.f, 200, 1, 1, false);
    for (int i = 0; i < 100; ++i) right.process(f.buf, l, r);
    CHECK(std::fabs(l) < 0.01f);
    CHECK(std::fabs(r) > 0.99f);
}

TEST_CASE("grain: ratio 2.0 reads the material an octave up") {
    SineBuf f(441.f);
    Grain g;
    // 12000 samples of output at ratio 1 -> 441 * 0.25 = ~110 crossings.
    Grain base;
    base.spawn(0.f, 1.f, 0.f, 12000, 1, 1, false);
    std::vector<float> v1;
    for (int i = 0; i < 12000; ++i) {
        float l = 0.f, r = 0.f;
        base.process(f.buf, l, r);
        v1.push_back(l);
    }
    const int n1 = zero_crossings(v1);
    CHECK(n1 >= 105);
    CHECK(n1 <= 115);

    g.spawn(0.f, 2.f, 0.f, 12000, 1, 1, false);
    std::vector<float> v2;
    for (int i = 0; i < 12000; ++i) {
        float l = 0.f, r = 0.f;
        g.process(f.buf, l, r);
        v2.push_back(l);
    }
    const int n2 = zero_crossings(v2);
    // Exactly double, within a cycle either way.
    CHECK(n2 >= 2 * n1 - 3);
    CHECK(n2 <= 2 * n1 + 3);
}

TEST_CASE("grain: reverse walks backwards through the material") {
    SineBuf f(441.f);
    // A grain starting at 0 going forwards and one starting at 0 going
    // backwards read different material -- unless reverse is ignored.
    Grain fwd, rev;
    fwd.spawn(1000.f, 1.f, 0.f, 2000, 1, 1, false);
    rev.spawn(1000.f, 1.f, 0.f, 2000, 1, 1, true);
    float diff = 0.f;
    for (int i = 0; i < 2000; ++i) {
        float fl = 0.f, fr = 0.f, rl = 0.f, rr = 0.f;
        fwd.process(f.buf, fl, fr);
        rev.process(f.buf, rl, rr);
        diff += std::fabs(fl - rl);
    }
    CHECK(diff > 100.f);
    // And the reverse grain is real audio, not silence (which would also
    // satisfy the assertion above).
    Grain rev2;
    rev2.spawn(1000.f, 1.f, 0.f, 2000, 1, 1, true);
    float energy = 0.f;
    for (int i = 0; i < 2000; ++i) {
        float l = 0.f, r = 0.f;
        rev2.process(f.buf, l, r);
        energy += l * l;
    }
    CHECK(energy > 100.f);
}

TEST_CASE("grain: release fades out from the current level, click-free") {
    FlatBuf f;
    Grain g;
    // Release mid-ATTACK (atk=1000, released at i=800), not on the plateau:
    // at the plateau _window() is already 1.0, so a broken release() that
    // hardcodes _rel_from = 1.f would be indistinguishable from the correct
    // "freeze the current level" -- it needs to be caught latched below the
    // ceiling to prove release() actually reads the window instead of
    // assuming it is fully open.
    g.spawn(0.f, 1.f, 0.f, 10000, 1000, 50, false);
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 800; ++i) g.process(f.buf, l, r);   // still rising
    const float before = l;
    REQUIRE(before > 0.6f);
    REQUIRE(before < 0.7f);            // mid-attack, well short of the 0.7071 ceiling

    g.release(200);
    std::vector<float> tail;
    for (int i = 0; i < 200; ++i) {
        g.process(f.buf, l, r);
        tail.push_back(l);
    }
    // No step at the moment of release...
    CHECK(std::fabs(tail.front() - before) < 0.02f);
    // ...monotonic decay...
    for (size_t i = 1; i < tail.size(); ++i) CHECK(tail[i] <= tail[i - 1] + 1e-5f);
    // ...ending in silence, and the grain retires.
    CHECK(tail.back() < 0.02f);
    CHECK_FALSE(g.active());
}
