#include <doctest/doctest.h>
#include <cmath>
#include <initializer_list>
#include "pitch/quantizer.h"
using namespace spky;

TEST_CASE("quantizer: FREE is a bit-identical passthrough") {
    Quantizer q;
    q.init(48000.f);
    q.set_mode(QuantMode::Free);
    for (float v : {0.f, 0.123456f, 0.5f, 0.987654f, 1.f})
        CHECK(q.process(v) == v);   // exact, not Approx
}

TEST_CASE("quantizer: CHROM snaps to the nearest semitone (36-semi contract)") {
    Quantizer q;
    q.init(48000.f);
    q.set_mode(QuantMode::Chrom);
    CHECK(q.process(17.4f / 36.f) == doctest::Approx(17.f / 36.f));
    Quantizer q2;
    q2.init(48000.f);
    q2.set_mode(QuantMode::Chrom);
    CHECK(q2.process(17.6f / 36.f) == doctest::Approx(18.f / 36.f));
}

TEST_CASE("quantizer: SCALE default is dorian, ties resolve to the lower note") {
    Quantizer q;
    q.init(48000.f);
    // 18 semis: degree 6 is not in dorian; 17 and 19 are equidistant -> 17
    CHECK(q.process(0.5f) == doctest::Approx(17.f / 36.f));
}

TEST_CASE("quantizer: root shifts the allowed degrees") {
    Quantizer q;
    q.init(48000.f);
    q.set_root(1);   // dorian on root 1: degree (18-1)%12 = 5 is allowed
    CHECK(q.process(0.5f) == doctest::Approx(18.f / 36.f));
}

TEST_CASE("quantizer: every scale mask maps output onto its own degrees") {
    for (int s = 0; s < SCALE_LIST_COUNT; ++s) {
        Quantizer q;
        q.init(48000.f);
        q.set_scale(SCALE_MASKS[s]);
        for (float v : {0.f, 0.2f, 0.4f, 0.6f, 0.8f, 1.f}) {
            Quantizer fresh;
            fresh.init(48000.f);
            fresh.set_scale(SCALE_MASKS[s]);
            float semis = fresh.process(v) * 36.f;
            int k = static_cast<int>(semis + 0.5f);
            CHECK(std::fabs(semis - k) < 1e-4f);
            CHECK(((SCALE_MASKS[s] >> (k % 12)) & 1) == 1);
        }
    }
}
