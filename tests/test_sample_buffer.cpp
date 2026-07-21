#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "sampler/sample_buffer.h"
#include "sampler/sampler_config.h"
using namespace spky;

static constexpr size_t kCap = 4800;   // 100 ms @ 48k -- plenty, and fast

// A buffer with its storage. Kept in one place so no test forgets init().
struct Fixture {
    std::vector<SampleBuffer::Frame> mem{kCap};
    SampleBuffer buf;
    Fixture() { buf.init(mem.data(), kCap, 48000.f); }
};

// Record `n` samples of a constant value; returns the peak |delta| between
// consecutive written frames, which is what a missing fade shows up as.
static float record_const(SampleBuffer& b, float v, size_t n) {
    b.set_recording(true);
    for (size_t i = 0; i < n; ++i) b.write(v, v);
    b.set_recording(false);
    // drain the fade-out: it needs kRecordFade more write() calls to finish
    for (size_t i = 0; i < sampler_cfg::kRecordFade + 2; ++i) b.write(v, v);
    return 0.f;
}

TEST_CASE("sample_buffer: record fades in and out with no discontinuity") {
    Fixture f;
    record_const(f.buf, 1.f, 2400);
    REQUIRE(f.buf.rec_size() > sampler_cfg::kRecordFade * 2);

    // Walk the recorded content: no step between neighbours may exceed the
    // fade's own per-sample slope. A missing fade produces a step of ~1.0.
    float worst = 0.f;
    for (size_t i = 1; i < f.buf.rec_size(); ++i) {
        const float d = std::fabs(f.mem[i].l - f.mem[i - 1].l);
        if (d > worst) worst = d;
    }
    // The steepest point of a 192-sample Hann rise is ~pi/(2*192) ~= 0.0082.
    CHECK(worst < 0.02f);
    // ...and the loop seam is a real crossfade (Fix 1): stopping without a
    // prior cut() writes the fade-out back over frame 0, and a Hann fade-in
    // at position x plus the complementary fade-out sum to sin^2+cos^2 == 1,
    // so frame 0 sits at full level, not near silence.
    CHECK(f.mem[0].l > 0.9f);
    // ...while the middle is at full level (else "no discontinuity" is
    // trivially satisfied by recording nothing at all).
    CHECK(f.mem[1200].l > 0.95f);
}

TEST_CASE("sample_buffer: stopping with zero writes leaves the buffer usable") {
    Fixture f;
    f.buf.set_recording(true);
    f.buf.set_recording(false);          // no write() in between
    REQUIRE(f.buf.is_recording());        // the fadeout state is armed
    f.buf.write(1.f, 1.f);                // one write must resolve it...
    CHECK_FALSE(f.buf.is_recording());    // ...without wrapping the counter
    CHECK(f.buf.is_empty());              // ...and nothing was captured

    // The buffer must still work afterwards WITHOUT clear(): cutting an
    // empty buffer would have locked the loop at zero length forever.
    f.buf.set_recording(true);
    for (size_t i = 0; i < 2400; ++i) f.buf.write(1.f, 1.f);
    f.buf.set_recording(false);
    for (size_t i = 0; i < sampler_cfg::kRecordFade + 2; ++i) f.buf.write(1.f, 1.f);
    CHECK(f.buf.rec_size() > 2000);
    CHECK(f.mem[1200].l > 0.9f);          // and it really captured the signal
}

TEST_CASE("sample_buffer: overdub attenuates the old content by the feedback") {
    Fixture f;
    // Pass 1: fill with 1.0, then lock the loop length.
    f.buf.set_recording(true);
    for (size_t i = 0; i < 2400; ++i) f.buf.write(1.f, 1.f);
    f.buf.set_recording(false);
    for (size_t i = 0; i < sampler_cfg::kRecordFade + 2; ++i) f.buf.write(1.f, 1.f);
    f.buf.cut();
    const size_t len = f.buf.rec_size();
    REQUIRE(len > 0);

    // Pass 2: overdub silence at feedback 0.5. The old content must be
    // scaled by ~0.5 in the sustain region, not left alone and not erased.
    f.buf.set_feedback(0.5f);          // knob 0.5 -> -30 dB, NOT a 0.5 factor
    const float fb = std::pow(10.f, (60.f * (0.5f - 1.f)) * 0.05f);
    f.buf.set_recording(true);
    for (size_t i = 0; i < len; ++i) f.buf.write(0.f, 0.f);
    f.buf.set_recording(false);

    // Sample well inside the loop, past the fade-in and before the fade-out.
    const size_t probe = len / 2;
    CHECK(f.mem[probe].l == doctest::Approx(fb).epsilon(0.05));
    // Guard the precondition: if the loop were shorter than the two fades,
    // `probe` would sit inside a fade and this test would prove nothing.
    REQUIRE(len > sampler_cfg::kRecordFade * 4);
}

TEST_CASE("sample_buffer: empty buffer reads silence and never hangs") {
    Fixture f;
    REQUIRE(f.buf.is_empty());
    float l = 1.f, r = 1.f;
    f.buf.read_linear(0.f, l, r);        // the original divides by zero here
    CHECK(l == 0.f);
    CHECK(r == 0.f);
    f.buf.read_linear(-5.f, l, r);       // the original loops forever here
    CHECK(l == 0.f);
    CHECK(r == 0.f);
}

TEST_CASE("sample_buffer: nullptr memory is inert, not a crash") {
    SampleBuffer b;                       // never init()ed
    CHECK_FALSE(b.valid());
    b.set_recording(true);
    b.write(1.f, 1.f);                    // must not dereference
    float l = 1.f, r = 1.f;
    b.read_linear(3.5f, l, r);
    CHECK(l == 0.f);
    CHECK(r == 0.f);
    CHECK(b.is_empty());
}

TEST_CASE("sample_buffer: read_linear interpolates and wraps at the content length") {
    Fixture f;
    // Hand-place a 4-frame ramp and declare it the content.
    for (size_t i = 0; i < 4; ++i) { f.mem[i].l = float(i); f.mem[i].r = -float(i); }
    f.buf.set_rec_size(4);
    REQUIRE(f.buf.rec_size() == 4);

    float l = 0.f, r = 0.f;
    f.buf.read_linear(1.5f, l, r);
    CHECK(l == doctest::Approx(1.5f));
    CHECK(r == doctest::Approx(-1.5f));

    // Frame 3 -> frame 0 is the wrap seam: 3 + 0.5*(0 - 3) = 1.5
    f.buf.read_linear(3.5f, l, r);
    CHECK(l == doctest::Approx(1.5f));

    // Past the end and before the start both fold back into range.
    f.buf.read_linear(5.5f, l, r);
    CHECK(l == doctest::Approx(1.5f));
    f.buf.read_linear(-2.5f, l, r);
    CHECK(l == doctest::Approx(1.5f));
}

TEST_CASE("sample_buffer: recording past capacity auto-stops with the loop locked") {
    Fixture f;
    f.buf.set_recording(true);
    for (size_t i = 0; i < kCap + 500; ++i) f.buf.write(0.5f, 0.5f);
    CHECK(f.buf.rec_size() == kCap);
    CHECK(f.buf.fill() == doctest::Approx(1.f));
    CHECK(f.buf.is_overdubbing());   // the loop is locked; further writes overdub
}

TEST_CASE("sample_buffer: the loop seam is crossfaded, not two dips") {
    Fixture f;
    // Record and stop WITHOUT cutting first: the fade-out must be written
    // over the fade-in at frame 0, so the wrap point is continuous.
    f.buf.set_recording(true);
    for (size_t i = 0; i < 2400; ++i) f.buf.write(1.f, 1.f);
    f.buf.set_recording(false);
    for (size_t i = 0; i < sampler_cfg::kRecordFade + 2; ++i) f.buf.write(1.f, 1.f);

    const size_t n = f.buf.rec_size();
    REQUIRE(n > sampler_cfg::kRecordFade * 4);

    // The seam: reading across the wrap must not step. Without the
    // write-head reset the head is a fade-in ramp from ~0 and the tail is a
    // fade-out ramp to ~0, so the wrap shows a deep notch instead.
    float l0 = 0.f, r0 = 0.f, ln = 0.f, rn = 0.f;
    f.buf.read_linear(0.f, l0, r0);
    f.buf.read_linear(float(n - 1), ln, rn);
    CHECK(l0 > 0.9f);                    // the head is at full level...
    CHECK(ln > 0.9f);                    // ...and so is the tail
    CHECK(std::fabs(l0 - ln) < 0.1f);    // ...and they meet without a step

    // And the content did not grow by a fade's worth: stopping crossfades
    // into the existing material rather than appending to it.
    CHECK(n < 2400 + sampler_cfg::kRecordFade);
}

TEST_CASE("sample_buffer: NaN and absurd read positions yield silence") {
    Fixture f;
    record_const(f.buf, 1.f, 2400);
    REQUIRE_FALSE(f.buf.is_empty());
    const float nan = std::nanf("");
    float l = 1.f, r = 1.f;
    f.buf.read_linear(nan, l, r);
    CHECK(l == 0.f);
    CHECK(r == 0.f);
    l = 1.f; r = 1.f;
    f.buf.read_linear(1e30f, l, r);
    CHECK(l == 0.f);
    CHECK(r == 0.f);
    // ...while an ordinary in-range read still works (else "silence" is
    // trivially satisfied by a broken reader).
    f.buf.read_linear(1200.f, l, r);
    CHECK(l > 0.9f);
}

TEST_CASE("sample_buffer: clear returns it to empty") {
    Fixture f;
    record_const(f.buf, 1.f, 2400);
    REQUIRE_FALSE(f.buf.is_empty());
    f.buf.clear();
    CHECK(f.buf.is_empty());
    CHECK(f.buf.fill() == doctest::Approx(0.f));
    CHECK_FALSE(f.buf.is_recording());
    CHECK(f.mem[1200].l == 0.f);
}

TEST_CASE("sample_buffer: set_rec_size(0) does not lock the buffer") {
    Fixture f;
    f.buf.set_rec_size(0);
    CHECK(f.buf.is_empty());
    // The buffer must still record afterwards. Cutting at zero length would
    // pin the write head and _size could never grow again.
    f.buf.set_recording(true);
    for (size_t i = 0; i < 2400; ++i) f.buf.write(1.f, 1.f);
    f.buf.set_recording(false);
    for (size_t i = 0; i < sampler_cfg::kRecordFade + 2; ++i) f.buf.write(1.f, 1.f);
    CHECK(f.buf.rec_size() > 2000);
    CHECK(f.mem[1200].l > 0.9f);
}

TEST_CASE("sample_buffer: set_rec_size clamps above capacity") {
    Fixture f;
    f.buf.set_rec_size(kCap * 10);
    CHECK(f.buf.rec_size() == kCap);
    CHECK(f.buf.fill() == doctest::Approx(1.f));
}

TEST_CASE("sample_buffer: a far out-of-range read folds correctly and cheaply") {
    Fixture f;
    for (size_t i = 0; i < 4; ++i) { f.mem[i].l = float(i); f.mem[i].r = -float(i); }
    f.buf.set_rec_size(4);
    float l = 0.f, r = 0.f;
    // 1e6 frames past the end of a 4-frame buffer: 1000000 mod 4 == 0.
    f.buf.read_linear(1000000.f, l, r);
    CHECK(l == doctest::Approx(0.f));
    // ...and far below zero: -999999.5 mod 4 == 0.5, not 2.5 as originally
    // drafted here. Floored division: -999999.5 / 4 = -249999.875, whose
    // floor is -250000, so the fold is -999999.5 - 4*(-250000) ==
    // -999999.5 + 1000000 == 0.5. That lands between frame 0 (0.0) and
    // frame 1 (1.0): 0 + 0.5*(1-0) == 0.5.
    f.buf.read_linear(-999999.5f, l, r);
    CHECK(l == doctest::Approx(0.5f));
}

TEST_CASE("sample buffer: feedback below the knee is the M5a mapping exactly") {
    // "On top, not instead". Every knob position that has been listened to
    // must give the same coefficient it always did.
    using namespace spky::sampler_cfg;
    SampleBuffer buf;
    std::vector<SampleBuffer::Frame> mem(1024);
    buf.init(mem.data(), mem.size(), 48000.f);
    for (float k : {0.f, 0.25f, 0.5f, 0.75f, 0.9f}) {
        buf.set_feedback(k);
        const float want = std::pow(10.f, (60.f * (k - 1.f)) * 0.05f);
        CHECK(buf.feedback() == doctest::Approx(want).epsilon(1e-6));
    }
}

TEST_CASE("sample buffer: feedback passes unity above the knee") {
    using namespace spky::sampler_cfg;
    SampleBuffer buf;
    std::vector<SampleBuffer::Frame> mem(1024);
    buf.init(mem.data(), mem.size(), 48000.f);

    buf.set_feedback(kFbKnee);
    CHECK(buf.feedback() < 1.f);              // -6 dB
    buf.set_feedback(1.f);
    CHECK(buf.feedback() > 1.f);              // the bloom
    CHECK(buf.feedback() == doctest::Approx(std::pow(10.f, kFbMaxDb * 0.05f)).epsilon(1e-5));

    // Monotonic across the knee -- a fold-back here would make the top of
    // the knob quieter than its middle.
    float prev = 0.f;
    for (int i = 0; i <= 200; ++i) {
        buf.set_feedback(static_cast<float>(i) / 200.f);
        CHECK(buf.feedback() >= prev);
        prev = buf.feedback();
    }
}

TEST_CASE("sample buffer: the bloom stays bounded and finite") {
    // The whole reason feedback above unity is allowed at all. Fill the loop
    // with hot material, then overdub silence for a long time and watch it
    // converge instead of diverge.
    SampleBuffer buf;
    std::vector<SampleBuffer::Frame> mem(4800);
    buf.init(mem.data(), mem.size(), 48000.f);
    buf.set_feedback(1.f);                     // maximum bloom

    buf.set_recording(true);
    for (size_t i = 0; i < mem.size(); ++i) buf.write(0.9f, -0.9f);
    buf.set_recording(false);
    for (int i = 0; i < 512; ++i) buf.write(0.f, 0.f);   // let the fade finish

    // 60 s of audio at 48 kHz, overdubbing silence into a blooming loop.
    buf.set_recording(true);
    for (int i = 0; i < 48000 * 60; ++i) buf.write(0.f, 0.f);
    buf.set_recording(false);

    for (size_t i = 0; i < mem.size(); ++i) {
        CHECK(std::isfinite(mem[i].l));
        CHECK(std::isfinite(mem[i].r));
        // fast_tanh clamps to +-1 and the coefficient tops out near 1.33, so
        // the loop's fixed point sits around 1.17. 2.0 is a generous ceiling
        // that still fails loudly on genuine runaway.
        CHECK(std::fabs(mem[i].l) <= 2.f);
        CHECK(std::fabs(mem[i].r) <= 2.f);
    }
}

TEST_CASE("sample buffer: below unity the write path is untouched by the saturator") {
    // The companion property to the bloom test. fast_tanh compresses audibly
    // from about half scale up, so if it ran unconditionally every overdub
    // would gain a tape character it does not have today. Overdub hot
    // material well below the knee and check the decay is exactly linear in
    // the coefficient, which a saturator in this path would visibly bend.
    //
    // BUG FOUND IN THE BRIEF: the brief's own draft of this test used a
    // 64-frame buffer with a probe at index 32, but kRecordFade is 192 --
    // that probe never reaches the sustain fade level (fade == 1) in either
    // pass, so the true per-sample multiplier is a partial fb_fade blend,
    // not fb itself, and the test failed even on unmodified below-the-knee
    // code (verified: it already failed before sample_buffer.cpp was
    // touched). Fixed by mirroring "overdub attenuates the old content by
    // the feedback" above: a buffer many multiples of kRecordFade, an
    // explicit cut() to lock the loop, and a probe at the midpoint, which is
    // deep in the sustain region for both the original recording and the
    // overdub.
    SampleBuffer buf;
    std::vector<SampleBuffer::Frame> mem(2400);
    buf.init(mem.data(), mem.size(), 48000.f);
    buf.set_feedback(0.9f);                    // -6 dB, well below the knee

    buf.set_recording(true);
    for (size_t i = 0; i < mem.size(); ++i) buf.write(0.8f, 0.8f);
    buf.set_recording(false);
    for (int i = 0; i < sampler_cfg::kRecordFade + 2; ++i) buf.write(0.8f, 0.8f);
    buf.cut();
    const size_t len = buf.rec_size();
    const size_t probe = len / 2;
    REQUIRE(probe > sampler_cfg::kRecordFade);

    const float before = mem[probe].l;
    buf.set_recording(true);
    for (size_t i = 0; i < len; ++i) buf.write(0.f, 0.f);
    buf.set_recording(false);

    // One overdub pass of silence scales the stored value by the coefficient
    // and nothing else. tanh(0.8) is about 0.664, so a saturator in this
    // path would show up as roughly a 17% shortfall -- far outside this
    // tolerance.
    const float fb = buf.feedback();
    CHECK(mem[probe].l == doctest::Approx(before * fb).epsilon(0.02));
}
