#pragma once
#include <cmath>
#include <cstdint>
#include "pitch/quantizer.h"
#include "util/math.h"

namespace spky {

// Diatonic chord builder behind the COLOR knob (spec 2026-07-17 chord-layer).
// Pitch-layer module: consumes the (quantized or free) melody root plus the
// quantizer's mask/root, produces up to 4 chord tones on the 0..1 = 36-semi
// pitch contract. State: zone hysteresis, latched slot intervals (semitones
// relative to the root), and the previous chord lay (voice-leading memory,
// Task 2). No heap, no RNG; build() runs at trigger rate, apply() per sample.
//
// Slot ladder in ADD order — a live COLOR sweep only ever adds/removes the
// LAST slot, so each zone is the previous zone plus one tone (the live-fader
// property the spec amendment demands; supersedes the spec table's literal
// "close triad" wording — triads here voice fifth-below/root/third):
//   slot 0: root (never octave-displaced — the root IS the melody)
//   slot 1: fifth, one octave down (power feel); ninth zone: the ninth above
//   slot 2: third
//   slot 3: seventh
// "third/fifth/seventh/ninth" = stack walk: every 2nd allowed scale note
// above the root reference, so quality is emergent from the mask (Dorian
// tonic = minor, IV = major; pentatonics go quartal).
class ChordBuilder {
public:
    static constexpr int   kMaxNotes = 4;
    static constexpr float kHyst   = 0.02f;    // zone-edge hysteresis (ear-tunable)
    static constexpr float kEdge2  = 0.125f;   // 1|2 notes
    static constexpr float kEdge3  = 0.375f;   // 2|3
    static constexpr float kEdge4  = 0.625f;   // 3|4
    static constexpr float kEdge9  = 0.85f;    // fifth-slot -> ninth
    static constexpr float kSpanClose = 18.f;  // lay span limit, closed (ear-tunable)
    static constexpr float kSpanOpen  = 30.f;  // ... fully open at COLOR 1

    void init() {
        _color = 0.f;
        _above2 = _above3 = _above4 = _ninth = false;
        _count = 1;
        _n = 1;
        for (int i = 0; i < kMaxNotes; ++i) _intervals[i] = 0;
        _ninth_applied = false;
        _prev_n = 0;
    }

    void set_color(float c) {
        _color = clampf(c, 0.f, 1.f);
        _above2 = _hyst(_above2, kEdge2);
        _above3 = _hyst(_above3, kEdge3);
        _above4 = _hyst(_above4, kEdge4);
        _ninth  = _hyst(_ninth,  kEdge9);
        _count = 1 + (_above2 ? 1 : 0) + (_above3 ? 1 : 0) + (_above4 ? 1 : 0);
    }

    int note_count() const { return _count; }   // knob side (post-hysteresis)
    int size() const       { return _n; }       // currently latched chord size

    // Full rebuild at trigger time. Task 1: nominal lay only; Task 2 adds the
    // voice-leading search. Latches the slot intervals, returns n, fills
    // out_norm[kMaxNotes].
    int build(float root_norm, uint16_t mask, int root_semi, float* out_norm) {
        const float root_s = clampf(root_norm, 0.f, 1.f) * Quantizer::SPAN_SEMIS;
        const int ref = nearest(static_cast<int>(root_s + 0.5f), mask, root_semi);
        _nominal(ref, mask, root_semi, _count, _ninth, _intervals);
        _n = _count;
        _ninth_applied = _ninth;
        return apply(root_norm, mask, root_semi, out_norm);
    }

    // Cheap per-sample surface refresh: re-applies the latched intervals to
    // the current root and follows COLOR count/ninth changes incrementally
    // (grow = take the new slot's nominal tone; shrink = drop the last slot).
    // Never runs a lay search. The zero-interval slot returns the root
    // BIT-EXACTLY (never through the semis round-trip) — invariant.
    int apply(float root_norm, uint16_t mask, int root_semi, float* out_norm) {
        if (_n != _count || _ninth_applied != _ninth)
            _refresh_slots(root_norm, mask, root_semi);
        const float root_s = clampf(root_norm, 0.f, 1.f) * Quantizer::SPAN_SEMIS;
        for (int i = 0; i < _n; ++i) {
            if (_intervals[i] == 0) { out_norm[i] = root_norm; continue; }
            float s = root_s + static_cast<float>(_intervals[i]);
            while (s > Quantizer::SPAN_SEMIS) s -= 12.f;
            while (s < 0.f) s += 12.f;
            out_norm[i] = s / Quantizer::SPAN_SEMIS;
        }
        return _n;
    }

    // --- mask helpers (quantizer conventions: mask is root-relative) --------
    static bool allowed(int k, uint16_t mask, int root) {
        int deg = (k - root) % 12;
        if (deg < 0) deg += 12;
        return (mask >> deg) & 1;
    }
    static int nearest(int center, uint16_t mask, int root) {
        for (int d = 0; d <= 12; ++d) {           // lower wins ties (deterministic)
            if (center - d >= 0 && allowed(center - d, mask, root)) return center - d;
            if (allowed(center + d, mask, root)) return center + d;
        }
        return center;                             // unreachable, non-empty mask
    }
    static int up(int k, uint16_t mask, int root) {   // next allowed strictly above
        for (int s = k + 1; s <= k + 13; ++s)
            if (allowed(s, mask, root)) return s;
        return k + 12;                             // unreachable, non-empty mask
    }
    static int stack_tone(int ref, int idx, uint16_t mask, int root) {
        int t = ref;
        for (int j = 0; j < 2 * idx; ++j) t = up(t, mask, root);
        return t;
    }

private:
    bool _hyst(bool above, float edge) const {
        if (above) return _color > edge - kHyst;
        return _color >= edge + kHyst;
    }

    // Nominal (pre-lay) slot intervals relative to the root, ADD order.
    static void _nominal(int ref, uint16_t mask, int root, int n, bool ninth,
                         int* iv) {
        iv[0] = 0;
        if (n >= 2) iv[1] = ninth
            ? stack_tone(ref, 4, mask, root) - ref          // ninth, above
            : stack_tone(ref, 2, mask, root) - ref - 12;    // fifth, octave down
        if (n >= 3) iv[2] = stack_tone(ref, 1, mask, root) - ref;   // third
        if (n >= 4) iv[3] = stack_tone(ref, 3, mask, root) - ref;   // seventh
    }

    void _refresh_slots(float root_norm, uint16_t mask, int root_semi) {
        const float root_s = clampf(root_norm, 0.f, 1.f) * Quantizer::SPAN_SEMIS;
        const int ref = nearest(static_cast<int>(root_s + 0.5f), mask, root_semi);
        int nom[kMaxNotes];
        _nominal(ref, mask, root_semi, kMaxNotes, _ninth, nom);
        if (_ninth_applied != _ninth && _count >= 2)
            _intervals[1] = nom[1];                // fifth <-> ninth swap, live
        for (int i = _n; i < _count; ++i)          // grow: new slots take nominal
            _intervals[i] = nom[i];
        _n = _count;                               // shrink: just drop the tail
        _ninth_applied = _ninth;
    }

    float _color = 0.f;
    bool  _above2 = false, _above3 = false, _above4 = false, _ninth = false;
    int   _count = 1;

    int   _n = 1;
    int   _intervals[kMaxNotes] = { 0, 0, 0, 0 };
    bool  _ninth_applied = false;

    // Task 2: previous chord lay (absolute semis) for voice-leading
    float _prev[kMaxNotes] = { 0.f, 0.f, 0.f, 0.f };
    int   _prev_n = 0;
};

} // namespace spky
