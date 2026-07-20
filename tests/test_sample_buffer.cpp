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
    // ...and the fade really happened: the first frame is near silence.
    CHECK(f.mem[0].l < 0.05f);
    // ...while the middle is at full level (else "no discontinuity" is
    // trivially satisfied by recording nothing at all).
    CHECK(f.mem[1200].l > 0.95f);
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
