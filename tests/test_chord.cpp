#include <doctest/doctest.h>
#include <cmath>
#include <initializer_list>
#include "pitch/chord.h"
using namespace spky;

namespace {
constexpr uint16_t DORIAN  = SCALE_MASKS[SCALE_DORIAN];
constexpr uint16_t MINPENT = SCALE_MASKS[SCALE_MIN_PENT];
// collect built semis, sorted ascending
static int semis(const float* out, int n, float* dst) {
    for (int i = 0; i < n; ++i) dst[i] = out[i] * 36.f;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (dst[j] < dst[i]) { float t = dst[i]; dst[i] = dst[j]; dst[j] = t; }
    return n;
}
} // namespace

TEST_CASE("chord: COLOR 0 is a single, bit-exact root passthrough") {
    ChordBuilder cb; cb.init();
    cb.set_color(0.f);
    float out[ChordBuilder::kMaxNotes];
    CHECK(cb.build(0.31f, DORIAN, 0, out) == 1);
    CHECK(out[0] == 0.31f);                       // exact, not Approx (invariant)
    CHECK(cb.apply(0.77f, DORIAN, 0, out) == 1);
    CHECK(out[0] == 0.77f);
}

TEST_CASE("chord: COLOR zones give 1/2/3/4 notes") {
    ChordBuilder cb; cb.init();
    float out[ChordBuilder::kMaxNotes];
    cb.set_color(0.f);    CHECK(cb.note_count() == 1);
    cb.set_color(0.25f);  CHECK(cb.note_count() == 2);
    cb.set_color(0.5f);   CHECK(cb.note_count() == 3);
    cb.set_color(0.75f);  CHECK(cb.note_count() == 4);
    cb.set_color(0.95f);  CHECK(cb.note_count() == 4);
    (void)out;
}

TEST_CASE("chord: zone edges carry hysteresis") {
    ChordBuilder cb; cb.init();
    cb.set_color(0.124f); CHECK(cb.note_count() == 1);
    cb.set_color(0.130f); CHECK(cb.note_count() == 1);   // below edge + kHyst
    cb.set_color(0.150f); CHECK(cb.note_count() == 2);   // committed
    cb.set_color(0.110f); CHECK(cb.note_count() == 2);   // above edge - kHyst: holds
    cb.set_color(0.100f); CHECK(cb.note_count() == 1);   // released
}

TEST_CASE("chord: dorian quality is emergent — minor tonic, major IV") {
    ChordBuilder cb; cb.init();
    cb.set_color(0.5f);                                   // triad zone
    float out[ChordBuilder::kMaxNotes], s[ChordBuilder::kMaxNotes];
    // tonic: root at semi 12 (degree 0). Ladder: fifth an octave down, root, third.
    int n = cb.build(12.f / 36.f, DORIAN, 0, out);
    REQUIRE(n == 3);
    semis(out, n, s);
    CHECK(s[0] == doctest::Approx(7.f));                  // fifth below (12+7-12)
    CHECK(s[1] == doctest::Approx(12.f));
    CHECK(s[2] == doctest::Approx(15.f));                 // MINOR third (+3)
    // IV: root at semi 17 (degree 5) -> major third (+4), perfect fifth (+7)
    ChordBuilder cb2; cb2.init(); cb2.set_color(0.5f);
    n = cb2.build(17.f / 36.f, DORIAN, 0, out);
    REQUIRE(n == 3);
    semis(out, n, s);
    CHECK(s[0] == doctest::Approx(12.f));                 // 17+7-12
    CHECK(s[1] == doctest::Approx(17.f));
    CHECK(s[2] == doctest::Approx(21.f));                 // MAJOR third (+4)
}

TEST_CASE("chord: pentatonic stacks go quartal") {
    ChordBuilder cb; cb.init();
    cb.set_color(0.5f);
    float out[ChordBuilder::kMaxNotes], s[ChordBuilder::kMaxNotes];
    // min pent {0,3,5,7,10}: "third" (2 walk steps up from 12) = 17 -> a FOURTH
    int n = cb.build(12.f / 36.f, MINPENT, 0, out);
    REQUIRE(n == 3);
    semis(out, n, s);
    CHECK(s[2] == doctest::Approx(17.f));                 // +5, quartal color
}

TEST_CASE("chord: ninth zone swaps the fifth slot for the ninth") {
    float out[ChordBuilder::kMaxNotes], s[ChordBuilder::kMaxNotes];
    ChordBuilder cb; cb.init();
    cb.set_color(0.75f);                                  // 7th zone: has fifth-down
    int n = cb.build(12.f / 36.f, DORIAN, 0, out);
    REQUIRE(n == 4);
    semis(out, n, s);
    CHECK(s[0] == doctest::Approx(7.f));                  // fifth an octave down
    ChordBuilder cb2; cb2.init();
    cb2.set_color(0.95f);                                 // ninth zone
    n = cb2.build(12.f / 36.f, DORIAN, 0, out);
    REQUIRE(n == 4);
    semis(out, n, s);
    // no fifth-down anymore; ninth (+14 over the root) is in the set
    CHECK(s[0] == doctest::Approx(12.f));
    bool has_ninth = false;
    for (int i = 0; i < n; ++i) if (std::fabs(s[i] - 26.f) < 0.01f) has_ninth = true;
    CHECK(has_ninth);
}

TEST_CASE("chord: voice-leading minimizes movement on a root change") {
    ChordBuilder cb; cb.init();
    cb.set_color(0.5f);
    float out[ChordBuilder::kMaxNotes], s[ChordBuilder::kMaxNotes];
    cb.build(12.f / 36.f, DORIAN, 0, out);        // tonic lay: {7,12,15}
    int n = cb.build(17.f / 36.f, DORIAN, 0, out); // IV — nominal would be {12,17,21}
    REQUIRE(n == 3);
    semis(out, n, s);
    // minimal-movement lay pulls the third down an octave: {9,12,17}
    // (cost 4 vs nominal's 8; 12 is a held common tone)
    CHECK(s[0] == doctest::Approx(9.f));
    CHECK(s[1] == doctest::Approx(12.f));
    CHECK(s[2] == doctest::Approx(17.f));
}

TEST_CASE("chord: the root slot is never octave-displaced") {
    ChordBuilder cb; cb.init();
    cb.set_color(0.95f);
    float out[ChordBuilder::kMaxNotes];
    for (float r : { 4.f, 12.f, 19.f, 30.f }) {
        int n = cb.build(r / 36.f, DORIAN, 0, out);
        CHECK(out[0] == doctest::Approx(r / 36.f));   // slot 0 == root, always
        (void)n;
    }
}

TEST_CASE("chord: apply follows the root without re-laying") {
    ChordBuilder cb; cb.init();
    cb.set_color(0.5f);
    float out[ChordBuilder::kMaxNotes], s[ChordBuilder::kMaxNotes];
    cb.build(12.f / 36.f, DORIAN, 0, out);        // latches {-5, 0, +3}
    int n = cb.apply(13.f / 36.f, DORIAN, 0, out);
    REQUIRE(n == 3);
    semis(out, n, s);
    CHECK(s[0] == doctest::Approx(8.f));
    CHECK(s[1] == doctest::Approx(13.f));
    CHECK(s[2] == doctest::Approx(16.f));
}

TEST_CASE("chord: COLOR growth/shrink is incremental — old slots keep their lay") {
    ChordBuilder cb; cb.init();
    cb.set_color(0.5f);
    float out[ChordBuilder::kMaxNotes];
    cb.build(12.f / 36.f, DORIAN, 0, out);
    const float slot1 = out[1], slot2 = out[2];
    cb.set_color(0.75f);                           // grow to 4 without a trigger
    int n = cb.apply(12.f / 36.f, DORIAN, 0, out);
    REQUIRE(n == 4);
    CHECK(out[1] == doctest::Approx(slot1));       // surviving slots untouched
    CHECK(out[2] == doctest::Approx(slot2));
    cb.set_color(0.2f);                            // shrink to 2
    n = cb.apply(12.f / 36.f, DORIAN, 0, out);
    REQUIRE(n == 2);
    CHECK(out[1] == doctest::Approx(slot1));
}

TEST_CASE("chord: every tone stays inside the 36-semi contract") {
    ChordBuilder cb; cb.init();
    cb.set_color(0.95f);
    float out[ChordBuilder::kMaxNotes];
    for (float r : { 0.f, 1.f, 34.f, 36.f }) {
        int n = cb.build(r / 36.f, DORIAN, 0, out);
        for (int i = 0; i < n; ++i) {
            CHECK(out[i] >= 0.f);
            CHECK(out[i] <= 1.f);
        }
    }
}

TEST_CASE("chord: fully deterministic — same input sequence, same output") {
    ChordBuilder a, b; a.init(); b.init();
    float oa[ChordBuilder::kMaxNotes], ob[ChordBuilder::kMaxNotes];
    const float roots[] = { 12.f, 17.f, 14.f, 22.f, 9.f };
    const float colors[] = { 0.3f, 0.55f, 0.8f, 0.95f, 0.6f };
    for (int k = 0; k < 5; ++k) {
        a.set_color(colors[k]); b.set_color(colors[k]);
        int na = a.build(roots[k] / 36.f, DORIAN, 0, oa);
        int nb = b.build(roots[k] / 36.f, DORIAN, 0, ob);
        REQUIRE(na == nb);
        for (int i = 0; i < na; ++i) CHECK(oa[i] == ob[i]);   // exact
    }
}

TEST_CASE("chord: FREE-style off-grid root keeps its detune character") {
    ChordBuilder cb; cb.init();
    cb.set_color(0.5f);
    float out[ChordBuilder::kMaxNotes], s[ChordBuilder::kMaxNotes];
    // root 12.4 semis is off-grid; intervals come from the nearest on-grid
    // reference (12) but ride on the REAL root (spec §3 FREE rule)
    int n = cb.build(12.4f / 36.f, DORIAN, 0, out);
    REQUIRE(n == 3);
    CHECK(out[0] == doctest::Approx(12.4f / 36.f));
    semis(out, n, s);
    CHECK(s[2] == doctest::Approx(15.4f));         // third rides the offset
}
