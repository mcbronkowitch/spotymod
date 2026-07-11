#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "fx/reverb.h"
using namespace spky;

// ~530 KB object: static, never on the stack. init() re-seeds all state, so
// sharing one instance across cases is safe.
static AmbientReverb s_rev;

static std::vector<float> impulse_response(AmbientReverb& rv, int n,
                                           bool left_channel) {
    std::vector<float> out(n);
    for (int i = 0; i < n; ++i) {
        float wl = 0.f, wr = 0.f;
        float in = (i == 0) ? 1.f : 0.f;
        rv.process(in, in, wl, wr);
        out[i] = left_channel ? wl : wr;
    }
    return out;
}

TEST_CASE("reverb: silence in, exact silence out") {
    s_rev.init(48000.f);
    for (int i = 0; i < 2000; ++i) {
        float wl = 1.f, wr = 1.f;
        s_rev.process(0.f, 0.f, wl, wr);
        CHECK(wl == 0.f);
        CHECK(wr == 0.f);
    }
}

TEST_CASE("reverb: mono impulse produces a persistent stereo tail") {
    s_rev.init(48000.f);
    auto l = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    auto r = impulse_response(s_rev, 48000, false);
    float tail = 0.f, decorr = 0.f;
    for (int i = 24000; i < 48000; ++i) tail += l[i] * l[i];
    for (int i = 0; i < 48000; ++i) decorr = std::max(decorr, std::fabs(l[i] - r[i]));
    CHECK(tail > 1e-6f);     // still ringing after 0.5 s at size 0.7
    CHECK(decorr > 1e-4f);   // L and R differ
}

TEST_CASE("reverb: shimmer 0 leaves the pitch shifter untouched") {
    s_rev.init(48000.f);
    auto plain = impulse_response(s_rev, 9600, true);
    s_rev.init(48000.f);
    s_rev.set_shimmer(0.7f);   // momentarily on...
    s_rev.set_shimmer(0.f);    // ...but 0 when processing starts
    auto toggled = impulse_response(s_rev, 9600, true);
    for (int i = 0; i < 9600; ++i) CHECK(plain[i] == toggled[i]);
}

TEST_CASE("reverb: shimmer changes the tail") {
    s_rev.init(48000.f);
    auto plain = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    s_rev.set_shimmer(0.8f);
    auto shim = impulse_response(s_rev, 48000, true);
    int diff = 0;
    for (int i = 4800; i < 48000; ++i)
        if (std::fabs(plain[i] - shim[i]) > 1e-6f) ++diff;
    CHECK(diff > 1000);
}

// Regression: the shimmer feedback loop must stay bounded at the most extreme
// settings a user can dial (max room + max shimmer) under a sustained input.
// The original fixed 0.5 feedback gain drove the loop gain past 1 and the tail
// grew ~4x every half second to +Inf (which plays back as silence after a
// click). The size-compensated, soft-clipped feedback keeps it finite.
TEST_CASE("reverb: shimmer feedback stays bounded at extreme settings") {
    s_rev.init(48000.f);
    s_rev.set_size(0.99f);        // maximum room
    s_rev.set_tone(0.55f);
    s_rev.set_shimmer(1.f);       // maximum shimmer
    float peak = 0.f;
    bool finite = true;
    for (int i = 0; i < 48000 * 8; ++i) {   // 8 s of sustained drive
        float in = 0.4f * std::sin(6.2831853f * 220.f * i / 48000.f);
        float wl = 0.f, wr = 0.f;
        s_rev.process(in, in, wl, wr);
        if (!std::isfinite(wl) || !std::isfinite(wr)) { finite = false; break; }
        peak = std::max(peak, std::max(std::fabs(wl), std::fabs(wr)));
    }
    CHECK(finite);       // never runs away to +Inf
    CHECK(peak < 8.f);   // bounded well below a blow-up
}
