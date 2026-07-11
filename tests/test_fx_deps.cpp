#include <doctest/doctest.h>
#include "Effects/overdrive.h"
#include "Effects/decimator.h"
#include "Effects/sampleratereducer.h"
#include "Effects/pitchshifter.h"
#include "Effects/reverbsc.h"

// Sanity: the DaisySP modules the FX chain needs compile and run on desktop
// (clang, no ARM). ReverbSc/PitchShifter are huge objects -> static, never stack.
TEST_CASE("daisysp: fx modules init and process on desktop") {
    daisysp::Overdrive od;
    od.Init();
    od.SetDrive(0.4f);
    CHECK(od.Process(0.5f) == od.Process(0.5f));   // stateless

    daisysp::Decimator dec;
    dec.Init();
    dec.SetDownsampleFactor(0.5f);
    (void)dec.Process(0.3f);

    daisysp::SampleRateReducer srr;
    srr.Init();
    srr.SetFreq(0.3f);
    (void)srr.Process(0.3f);

    static daisysp::ReverbSc rev;
    REQUIRE(rev.Init(48000.f) == 0);
    rev.SetFeedback(0.85f);
    rev.SetLpFreq(10000.f);
    float wl = 0.f, wr = 0.f;
    rev.Process(1.f, 1.f, &wl, &wr);
    float energy = 0.f;
    for (int i = 0; i < 9600; ++i) {
        rev.Process(0.f, 0.f, &wl, &wr);
        energy += wl * wl + wr * wr;
    }
    CHECK(energy > 0.f);                            // impulse leaves a tail

    static daisysp::PitchShifter ps;
    ps.Init(48000.f);
    ps.SetTransposition(12.f);
    float in = 0.5f;
    (void)ps.Process(in);
}
