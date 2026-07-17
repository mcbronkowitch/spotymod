#include <doctest/doctest.h>
#include <cmath>
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
