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

// --- Final review, Important: no DC stall at low ratio on a long buffer -----

TEST_CASE("grain: a low ratio still advances near the end of a long buffer") {
    // The bug this pins: _pos was an ABSOLUTE float frame index, advanced by
    // `_pos += _ratio`. Float spacing at frame 524,288 is 0.0625 and at
    // 1,048,576 it is 0.125, so a ratio at or below the local ulp does not
    // move _pos at all -- the grain freezes on one sample and emits DC for
    // its whole window.
    //
    // Two changes on this branch made that reachable in ordinary use rather
    // than cornered: pitch now reaches +-4 octaves (a plain 2^-4 = 0.0625
    // sits exactly at the boundary at frame 524,288, and the composed
    // minimum with octave scatter, SUB and detune is ~0.0154), and MOTION now
    // scatters position across the WHOLE buffer, so on a full 42 s recording
    // roughly three quarters of spawns land above frame 524,288.
    //
    // The fix keeps the offset from the grain's start in its own float, which
    // stays small and therefore finely spaced, and forms the absolute read
    // position only at the read_linear call.
    const size_t kLong = 2016000;              // 42 s at 48 kHz, the capacity
    std::vector<SampleBuffer::Frame> mem(kLong);
    SampleBuffer buf;
    buf.init(mem.data(), kLong, 48000.f);
    for (size_t i = 0; i < kLong; ++i) {
        const float s = std::sin(6.2831853f * 2000.f * float(i) / 48000.f);
        mem[i].l = s;
        mem[i].r = s;
    }
    buf.set_rec_size(kLong);

    // The composed minimum reachable ratio: 2^-4 (pitch) x 0.5 (octave
    // scatter) x 0.5 (SUB) x 0.983 (detune at -35 cents).
    const float kMinRatio = 0.0625f * 0.5f * 0.5f * 0.983f;   // ~0.01536
    // A start well past 2^20, where the absolute float spacing (0.125) is
    // eight times the ratio -- the frozen case, not a marginal one.
    const float kStart = 1500000.f;

    Grain g;
    // Long grain, short window halves, so the probe below sits deep in the
    // unity region of the window and the output IS the signal.
    g.spawn(kStart, kMinRatio, 0.f, 20000, 100, 100, false);

    float l = 0.f, r = 0.f;
    for (int i = 0; i < 5000; ++i) g.process(buf, l, r);      // into the flat top

    // NOTE on what is asserted, and what cannot be: the position is checked
    // over a WINDOW of samples, not sample by sample. `_start + _off` is
    // still a float, and at frame 1.5e6 the representable grid is 0.125, so
    // no accumulator design can make a 0.0154 ratio move the ABSOLUTE index
    // on every single sample -- it advances in 0.125 steps roughly every 8
    // samples. That residual quantization is inherent to a float frame index
    // into a 2 M-frame buffer, applies at every ratio (a unity-ratio grain at
    // this frame already reads on the same 1/8-frame grid, and has since M5),
    // and is not what this test is about. The defect being pinned is the
    // grain never advancing AT ALL -- a permanent freeze, DC for the whole
    // window -- which is what the absolute accumulator did and what the
    // average-rate check below catches.
    const float pos_before = g.read_pos();
    float prev_pos = pos_before;
    float lo = l, hi = l;
    for (int i = 0; i < 400; ++i) {
        g.process(buf, l, r);
        CHECK(g.read_pos() >= prev_pos);       // never goes backwards
        prev_pos = g.read_pos();
        lo = l < lo ? l : lo;
        hi = l > hi ? l : hi;
    }
    // The average rate is right: 400 samples at ratio 0.01536 must walk
    // ~6.1 frames. Under the old absolute accumulator this advance was
    // exactly 0.0 -- the assertion that dies without the fix.
    const float advance = g.read_pos() - pos_before;
    CHECK(advance > 5.f);
    CHECK(advance < 7.5f);

    // ...and the output is not DC. Those ~6 frames are a quarter cycle of a
    // 2 kHz sine (24-frame period), so the level must visibly move.
    CHECK(hi - lo > 0.1f);

    // Reverse uses the same accumulator and must stay symmetric.
    Grain rg;
    rg.spawn(kStart, kMinRatio, 0.f, 20000, 100, 100, true);
    for (int i = 0; i < 5000; ++i) rg.process(buf, l, r);
    const float rpos_before = rg.read_pos();
    float rprev = rpos_before;
    for (int i = 0; i < 400; ++i) {
        rg.process(buf, l, r);
        CHECK(rg.read_pos() <= rprev);
        rprev = rg.read_pos();
    }
    const float radvance = rpos_before - rg.read_pos();
    CHECK(radvance > 5.f);
    CHECK(radvance < 7.5f);

    // The start is honoured exactly: the offset is relative, the reported
    // position is absolute, and the two must not have drifted apart.
    Grain fresh;
    fresh.spawn(kStart, kMinRatio, 0.f, 20000, 100, 100, false);
    CHECK(fresh.read_pos() == kStart);
}

// --- SIZE is asymmetrically live: turning it down trims what is sounding ---
//
// Grain latches its length at spawn() and process() reads no parameter at
// all, which is the whole design (see the class comment). That made SIZE a
// one-way street: a 42 s grain kept sounding for its full 42 s no matter
// where the knob went, and nothing short of CHOKE could stop it. trim_total
// is the deliberate exception -- it caps a grain's TOTAL life, counted from
// spawn, and can only ever shorten.

// How many process() calls until the grain retires, capped at `limit`.
static int life_left(Grain& g, const SampleBuffer& buf, int limit = 200000) {
    float l = 0.f, r = 0.f;
    int n = 0;
    while (g.active() && n < limit) { g.process(buf, l, r); ++n; }
    return n;
}

TEST_CASE("grain: trim_total caps the remaining life, counted from spawn") {
    FlatBuf f;
    float l = 0.f, r = 0.f;

    Grain g;
    g.spawn(0.f, 1.f, 0.f, 10000, 100, 100, false);
    for (int i = 0; i < 1000; ++i) g.process(f.buf, l, r);   // 1000 played

    g.trim_total(3000, 192);
    // 3000 total minus the 1000 already played leaves 2000, and the grain
    // must retire on exactly that -- not on 3000, which would count the
    // played samples twice.
    CHECK(life_left(g, f.buf) == 2000);
}

TEST_CASE("grain: trim_total never extends a grain") {
    FlatBuf f;
    float l = 0.f, r = 0.f;

    // A cap beyond the grain's own length is a no-op: it runs its full 5000.
    Grain up;
    up.spawn(0.f, 1.f, 0.f, 5000, 100, 100, false);
    up.trim_total(50000, 192);
    CHECK(life_left(up, f.buf) == 5000);

    // And a second, LOOSER trim after a tight one does not undo it. This is
    // the case a knob sweep produces: down to a small SIZE, back up.
    Grain back;
    back.spawn(0.f, 1.f, 0.f, 10000, 100, 100, false);
    for (int i = 0; i < 500; ++i) back.process(f.buf, l, r);
    back.trim_total(1500, 192);
    back.trim_total(9000, 192);
    CHECK(life_left(back, f.buf) == 1000);
}

TEST_CASE("grain: a cap already in the past still fades, it does not cut") {
    FlatBuf f;
    float l = 0.f, r = 0.f;

    Grain g;
    g.spawn(0.f, 1.f, 0.f, 40000, 100, 100, false);
    for (int i = 0; i < 20000; ++i) g.process(f.buf, l, r);
    // Sitting on the window plateau: the window is 1.0 there, and 0.7071 is
    // the centre-pan gain FlatBuf's constant 1.0 gets multiplied by.
    const float before = l;
    REQUIRE(before > 0.7f);

    // SIZE dropped so far that this grain should already have ended. It gets
    // the floor, not a hard stop -- a truncated plateau is a click.
    g.trim_total(100, 192);
    CHECK(life_left(g, f.buf) == 192);

    // ...and the first sample of that fade continues from where the window
    // was, rather than jumping.
    Grain h;
    h.spawn(0.f, 1.f, 0.f, 40000, 100, 100, false);
    for (int i = 0; i < 20000; ++i) h.process(f.buf, l, r);
    const float last_before = l;
    h.trim_total(100, 192);
    h.process(f.buf, l, r);
    CHECK(std::fabs(l - last_before) < 0.02f);
}

TEST_CASE("grain: trimming mid-fade shortens it, and stays continuous") {
    FlatBuf f;
    float l = 0.f, r = 0.f;

    Grain g;
    g.spawn(0.f, 1.f, 0.f, 40000, 100, 100, false);
    for (int i = 0; i < 10000; ++i) g.process(f.buf, l, r);
    g.trim_total(14000, 192);              // 4000 of fade left
    for (int i = 0; i < 1000; ++i) g.process(f.buf, l, r);
    const float mid = l;
    REQUIRE(mid > 0.2f);                   // well inside the fade

    g.trim_total(11500, 192);              // shorten it to 500 from now
    g.process(f.buf, l, r);
    CHECK(std::fabs(l - mid) < 0.02f);     // no jump at the re-arm
    CHECK(life_left(g, f.buf) == 499);     // 500 total, one already consumed
}
