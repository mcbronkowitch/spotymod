#include <doctest/doctest.h>
#include "Effects/overdrive.h"
#include "Effects/decimator.h"
#include "Effects/sampleratereducer.h"

// Sanity: the DaisySP (MIT core) modules the FX chain needs compile and run
// on desktop (clang, no ARM). The reverb no longer uses DaisySP — see
// third_party/oliverb (M4.5).
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
}
