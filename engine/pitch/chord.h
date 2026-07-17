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

    // Full rebuild at trigger time. Searches the lay (each non-root slot may
    // shift by an octave) for minimal movement vs. the previous chord, span-
    // limited by COLOR. Latches the slot intervals, returns n, fills
    // out_norm[kMaxNotes].
    int build(float root_norm, uint16_t mask, int root_semi, float* out_norm) {
        const float root_s = clampf(root_norm, 0.f, 1.f) * Quantizer::SPAN_SEMIS;
        const int ref = nearest(static_cast<int>(root_s + 0.5f), mask, root_semi);
        const int n = _count;
        int nom[kMaxNotes] = { 0, 0, 0, 0 };   // _nominal fills only [0..n-1]
        _nominal(ref, mask, root_semi, n, _ninth, nom);

        // Lay search: slot 0 (the root) is fixed; every other slot may shift
        // by one octave either way. Minimal total movement vs the previous
        // chord (nearest-tone cost); no history -> prefer the nominal lay.
        // 3^(n-1) <= 27 candidates, trigger rate only.
        static constexpr int kOff[3] = { -12, 0, 12 };
        const float span_max = _span_max();
        int   best[kMaxNotes] = { nom[0], nom[1], nom[2], nom[3] };
        float best_cost = 1e9f, best_sum = 1e9f;
        int total = 1;
        for (int i = 1; i < n; ++i) total *= 3;
        for (int m = 0; m < total; ++m) {
            int off[kMaxNotes] = { 0, 0, 0, 0 };
            int mm = m;
            for (int i = 1; i < n; ++i) { off[i] = kOff[mm % 3]; mm /= 3; }
            float cand[kMaxNotes];
            float lo = 1e9f, hi = -1e9f, sum = 0.f;
            bool ok = true;
            for (int i = 0; i < n; ++i) {
                cand[i] = root_s + static_cast<float>(nom[i] + off[i]);
                if (cand[i] < 0.f || cand[i] > Quantizer::SPAN_SEMIS) ok = false;
                if (cand[i] < lo) lo = cand[i];
                if (cand[i] > hi) hi = cand[i];
                sum += cand[i];
            }
            if (!ok || hi - lo > span_max) continue;
            for (int i = 0; i < n && ok; ++i)      // two slots on one semitone: no
                for (int j = i + 1; j < n; ++j)
                    if (nom[i] + off[i] == nom[j] + off[j]) ok = false;
            if (!ok) continue;
            float cost = 0.f;
            if (_prev_n > 0) {
                for (int i = 0; i < n; ++i) {      // nearest previous tone
                    float d = 1e9f;
                    for (int j = 0; j < _prev_n; ++j) {
                        const float dd = std::fabs(cand[i] - _prev[j]);
                        if (dd < d) d = dd;
                    }
                    cost += d;
                }
            } else {
                for (int i = 0; i < n; ++i)
                    cost += static_cast<float>(off[i] < 0 ? -off[i] : off[i]);
            }
            if (cost < best_cost - 1e-4f ||
                (cost < best_cost + 1e-4f && sum < best_sum)) {   // tie: lower lay
                best_cost = cost;
                best_sum = sum;
                for (int i = 0; i < n; ++i) best[i] = nom[i] + off[i];
            }
        }

        for (int i = 0; i < n; ++i) _intervals[i] = best[i];
        _n = n;
        _ninth_applied = _ninth;
        const int r = apply(root_norm, mask, root_semi, out_norm);
        _prev_n = r;                                // voice-leading memory
        for (int i = 0; i < r; ++i)
            _prev[i] = out_norm[i] * Quantizer::SPAN_SEMIS;
        return r;
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

    float _span_max() const {
        const float t = clampf((_color - kEdge4) / (1.f - kEdge4), 0.f, 1.f);
        return kSpanClose + (kSpanOpen - kSpanClose) * t;
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
