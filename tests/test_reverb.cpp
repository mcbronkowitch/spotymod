#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "fx/reverb.h"
using namespace spky;

// ~130 KB object: static, never on the stack. init() fully re-seeds all
// state (buffer, filters, LFOs, RNG), so sharing one instance is safe.
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
    s_rev.set_decay(0.75f);
    auto l = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    s_rev.set_decay(0.75f);
    auto r = impulse_response(s_rev, 48000, false);
    float tail = 0.f, decorr = 0.f;
    for (int i = 24000; i < 48000; ++i) tail += l[i] * l[i];
    for (int i = 0; i < 48000; ++i) decorr = std::max(decorr, std::fabs(l[i] - r[i]));
    CHECK(tail > 1e-6f);     // still ringing after 0.5 s
    CHECK(decorr > 1e-4f);   // L and R differ
}

TEST_CASE("reverb: below 100% the impulse energy decays monotonically") {
    s_rev.init(48000.f);
    s_rev.set_decay(0.4f);
    auto ir = impulse_response(s_rev, 48000, true);
    float w[4] = { 0.f, 0.f, 0.f, 0.f };
    for (int k = 0; k < 4; ++k)
        for (int i = k * 12000; i < (k + 1) * 12000; ++i) w[k] += ir[i] * ir[i];
    CHECK(w[0] > w[1]);
    CHECK(w[1] > w[2]);
    CHECK(w[2] > w[3]);
}

TEST_CASE("reverb: decay past 100% blooms, self-sustains, stays bounded") {
    s_rev.init(48000.f);
    s_rev.set_decay(1.f);    // internal loop gain 1.05 (capped)
    float peak = 0.f, late = 0.f;
    bool finite = true;
    const int N = 48000 * 8;
    for (int i = 0; i < N; ++i) {
        float in = (i < 96000) ? 0.3f * std::sin(6.2831853f * 220.f * i / 48000.f) : 0.f;
        float wl = 0.f, wr = 0.f;
        s_rev.process(in, in, wl, wr);
        if (!std::isfinite(wl) || !std::isfinite(wr)) { finite = false; break; }
        peak = std::max(peak, std::max(std::fabs(wl), std::fabs(wr)));
        if (i >= N - 48000) late += wl * wl;
    }
    CHECK(finite);                      // never runs away
    CHECK(peak < 4.f);                  // the in-loop SoftLimit holds it
    CHECK(late / 48000.f > 0.0004f);    // still singing 6 s after input stopped
}

TEST_CASE("reverb: size ride Doppler-warps without clicks") {
    s_rev.init(48000.f);
    s_rev.set_decay(0.9f);
    s_rev.set_size(0.7f);
    // ring the room first
    for (int i = 0; i < 24000; ++i) {
        float in = (i == 0) ? 1.f : 0.2f * std::sin(6.2831853f * 330.f * i / 48000.f);
        float wl, wr;
        s_rev.process(in, in, wl, wr);
    }
    float prev = 0.f, max_step = 0.f;
    bool finite = true;
    for (int i = 0; i < 96000; ++i) {
        if (i % 480 == 0) {  // sweep 0.7 -> 0.1 in 200 steps over 1 s, then back
            float t = i / 96000.f;
            float n = t < 0.5f ? 0.7f - 1.2f * t : 0.1f + 1.2f * (t - 0.5f);
            s_rev.set_size(n);
        }
        float wl, wr;
        s_rev.process(0.f, 0.f, wl, wr);
        if (!std::isfinite(wl)) { finite = false; break; }
        max_step = std::max(max_step, std::fabs(wl - prev));
        prev = wl;
    }
    CHECK(finite);
    CHECK(max_step < 1.f);   // Doppler yes, discontinuities no
}

TEST_CASE("reverb: tone closed removes high-frequency tail energy") {
    auto hf_ratio = [](const std::vector<float>& x) {
        float diff = 0.f, tot = 1e-12f;
        for (size_t i = 4801; i < x.size(); ++i) {
            float d = x[i] - x[i - 1];
            diff += d * d;
            tot += x[i] * x[i];
        }
        return diff / tot;   // first-difference energy ~ HF content proxy
    };
    s_rev.init(48000.f);
    s_rev.set_decay(0.7f);
    s_rev.set_tone(0.9f);
    auto bright = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    s_rev.set_decay(0.7f);
    s_rev.set_tone(0.1f);
    auto dark = impulse_response(s_rev, 48000, true);
    CHECK(hf_ratio(bright) > hf_ratio(dark) * 1.5f);
}

TEST_CASE("reverb: diffusion reshapes the room (sparse vs dense)") {
    s_rev.init(48000.f);
    s_rev.set_decay(0.8f);
    s_rev.set_diffusion(0.f);            // discrete slap-echo cluster
    auto sparse = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    s_rev.set_decay(0.8f);
    s_rev.set_diffusion(0.9f);           // dense wash
    auto dense = impulse_response(s_rev, 48000, true);
    int diff = 0;
    for (int i = 4800; i < 48000; ++i)
        if (std::fabs(sparse[i] - dense[i]) > 1e-6f) ++diff;
    CHECK(diff > 1000);
    // early-window crest: a sparse room concentrates energy in discrete
    // events; a diffused one spreads it -> lower peak-to-RMS
    auto crest = [](const std::vector<float>& x, int a, int b) {
        float pk = 0.f;
        double acc = 0.0;
        for (int i = a; i < b; ++i) {
            pk = std::max(pk, std::fabs(x[i]));
            acc += x[i] * x[i];
        }
        float rms = std::sqrt((float)(acc / (b - a))) + 1e-12f;
        return pk / rms;
    };
    CHECK(crest(sparse, 0, 9600) > crest(dense, 0, 9600));
}

TEST_CASE("reverb: diffusion ride stays bounded without clicks") {
    s_rev.init(48000.f);
    s_rev.set_decay(0.9f);
    // ring the room first
    for (int i = 0; i < 24000; ++i) {
        float in = (i == 0) ? 1.f : 0.2f * std::sin(6.2831853f * 330.f * i / 48000.f);
        float wl, wr;
        s_rev.process(in, in, wl, wr);
    }
    float prev = 0.f, max_step = 0.f;
    bool finite = true;
    for (int i = 0; i < 96000; ++i) {
        if (i % 480 == 0) {   // sweep 0 -> 1 -> 0 over 2 s in 200 steps
            float t = i / 96000.f;
            float n = t < 0.5f ? 2.f * t : 2.f * (1.f - t);
            s_rev.set_diffusion(n);
        }
        float wl, wr;
        s_rev.process(0.f, 0.f, wl, wr);
        if (!std::isfinite(wl)) { finite = false; break; }
        max_step = std::max(max_step, std::fabs(wl - prev));
        prev = wl;
    }
    CHECK(finite);
    CHECK(max_step < 1.f);   // density morph yes, discontinuities no
}

TEST_CASE("reverb: bit-deterministic across instances") {
    static AmbientReverb rvA, rvB;
    rvA.init(48000.f);
    rvB.init(48000.f);
    for (AmbientReverb* rv : { &rvA, &rvB }) {
        rv->set_size(0.65f);
        rv->set_decay(0.85f);
        rv->set_tone(0.6f);
        rv->set_diffusion(0.6f);
    }
    bool identical = true;
    for (int i = 0; i < 48000; ++i) {
        float in = (i == 0) ? 1.f : 0.2f * std::sin(6.2831853f * 110.f * i / 48000.f);
        float la, ra, lb, rb;
        rvA.process(in, in, la, ra);
        rvB.process(in, in, lb, rb);
        if (la != lb || ra != rb) { identical = false; break; }
    }
    CHECK(identical);
}

TEST_CASE("reverb: clear() empties the room but keeps the parameter state") {
    static AmbientReverb rv;             // BIG object: never stack-allocate
    rv.init(48000.f);
    rv.set_decay(0.8f);
    float l, r;
    // ring up a tail: periodic impulses for 0.25 s
    for (int i = 0; i < 12000; ++i) {
        float in = (i % 4800 == 0) ? 0.9f : 0.f;
        rv.process(in, in, l, r);
    }
    float energy = 0.f;                  // the room is audibly ringing
    for (int i = 0; i < 4800; ++i) { rv.process(0.f, 0.f, l, r); energy += l * l + r * r; }
    CHECK(energy > 1e-6f);

    rv.clear();
    // silence in -> exact silence out: buffer AND loop filter state are zeroed
    for (int i = 0; i < 4800; ++i) {
        rv.process(0.f, 0.f, l, r);
        CHECK(l == 0.f);
        CHECK(r == 0.f);
    }
    // parameters survived the clear: a fresh impulse still rings the same room
    rv.process(0.9f, 0.9f, l, r);
    float energy2 = 0.f;
    for (int i = 0; i < 9600; ++i) { rv.process(0.f, 0.f, l, r); energy2 += l * l + r * r; }
    CHECK(energy2 > 1e-6f);
}
