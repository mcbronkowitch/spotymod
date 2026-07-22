#include <doctest/doctest.h>
#include <cmath>
#include "Filters/svf.h"
#include "util/svf_lp.h"

using namespace spky;

// SvfLp exists only because daisysp::Svf computed four outputs and a drive term
// the engine never reads (see util/svf_lp.h for the measurement that motivated
// it). Its whole licence to exist is that it is the SAME filter -- so the
// invariant worth pinning is not "close enough", it is bit equality of Low()
// against the vendored original, over the call pattern the engine actually
// makes: a cutoff pushed at every control tick, a resonance pushed at every
// control tick that almost never moves, and drive nailed to zero.
//
// This is a desktop test and desktop builds do not use -ffast-math. On the
// firmware (-ffast-math -funroll-loops) dropping the `- drive_*band^3` term
// lets the compiler contract the band update differently, and the two do drift
// -- measured at 2.1e-6 absolute worst case over 1.9 M samples, i.e. about
// -113 dBFS. That is a rounding difference, not a behavioural one, and the
// engine does not claim byte-identity across toolchains. The equality below is
// what pins the ALGEBRA; the firmware figure is recorded here so nobody
// rediscovers it as a bug.
TEST_CASE("svf_lp: Low() is bit-identical to daisysp::Svf with drive 0") {
    daisysp::Svf ref;
    SvfLp        lp;
    ref.Init(48000.f);
    lp.Init(48000.f);
    ref.SetFreq(2000.f); lp.SetFreq(2000.f);
    ref.SetRes(0.15f);   lp.SetRes(0.15f);
    ref.SetDrive(0.f);

    unsigned s = 7u;
    long long differing = 0;
    for (int blk = 0; blk < 2000; ++blk) {
        // control tick: cutoff sweeps the whole musical range, resonance steps
        // occasionally (the case SvfLp's change guard has to get right)
        const float c = 60.f + 13940.f * (0.5f + 0.5f * std::sin(blk * 0.01f));
        ref.SetFreq(c); lp.SetFreq(c);
        const float r = 0.15f + (blk % 97 == 0 ? 0.5f : 0.f);
        ref.SetRes(r); lp.SetRes(r);
        for (int i = 0; i < 96; ++i) {
            s = s * 1664525u + 1013904223u;
            const float x = (float)(s >> 8) / 8388608.f - 1.f;
            ref.Process(x);
            lp.Process(x);
            if (ref.Low() != lp.Low()) ++differing;
        }
    }
    CHECK(differing == 0);
}

// The change guards must not turn into "the first value wins". A fresh filter
// has to accept its first SetFreq/SetRes even when the argument happens to
// equal the sentinel-adjacent defaults, and a repeated push must be a no-op
// rather than a re-tune -- both are covered by the equality above only because
// it pushes repeats; this pins the fresh-instance half directly.
TEST_CASE("svf_lp: a fresh filter takes its first SetFreq/SetRes") {
    SvfLp a, b;
    a.Init(48000.f);
    b.Init(48000.f);
    a.SetFreq(400.f); a.SetRes(0.6f);
    // b gets the same values, but pushed twice -- the guard must make the
    // second push change nothing at all.
    b.SetFreq(400.f); b.SetRes(0.6f);
    b.SetFreq(400.f); b.SetRes(0.6f);
    for (int i = 0; i < 512; ++i) {
        const float x = std::sin(6.2831853f * 220.f * i / 48000.f);
        a.Process(x);
        b.Process(x);
        REQUIRE(a.Low() == b.Low());
    }
    CHECK(a.Low() != 0.f);   // guard against both being silently dead
}
