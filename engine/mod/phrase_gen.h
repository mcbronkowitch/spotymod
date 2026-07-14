#pragma once
// Deterministic, allocation-free melodic phrase generator (PITCH lane).
// No heap, no virtual dispatch, no libDaisy. Every random draw goes through the
// caller's Rng so the engine's bit-determinism invariant holds.
#include <cstdint>
#include <cmath>
#include "mod/rng.h"

namespace spky {

enum class Principle : uint8_t {
    TwoMotif = 0, OneMotif, Hierarchical, CallResponse, Ostinato, kCount
};

// motif_count = number of RENEW renewal units (regenerate_unit's `unit` domain).
struct PhraseLayout {
    uint8_t motif_len   = 0;  // L: slots per instance
    uint8_t tail_len    = 0;  // r: trailing slots (0..L-1)
    uint8_t inst_count  = 0;  // k: number of instances
    uint8_t motif_count = 0;  // number of renewal units
};

inline float pg_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Metric weight of an absolute slot: downbeat strongest, then binary
// subdivisions (by trailing zeros of the slot index). Higher = stronger beat.
// Used for gate placement (motif-relative) and DENSITY (absolute).
inline float pg_metric_weight(int pos) {
    if (pos <= 0) return 1.0f;
    unsigned p = static_cast<unsigned>(pos);
    unsigned tz = 0;
    while ((p & 1u) == 0u && tz < 5u) { p >>= 1u; ++tz; }
    float w = 0.2f + 0.15f * static_cast<float>(tz);
    return w > 1.0f ? 1.0f : w;
}

// Bounded random walk: a line, not independent draws. Cubed step (small
// intervals common, leaps rare) with mild gravity pulling toward 0 (the root).
inline void pg_contour_walk(Rng& rng, float* out, int len,
                            float start, float width, float gravity) {
    float v = start;
    for (int i = 0; i < len; ++i) {
        float r = rng.next_bipolar();
        float step = r * r * r * width;
        v = pg_clampf((v + step) * (1.0f - gravity), -1.0f, 1.0f);
        out[i] = v;
    }
}

// Per-principle target motif length (slots). Tuned by ear; fixed (YAGNI).
inline int pg_target_len(Principle) { return 8; }

// Derive motif sizing from n (= min(steps,32)) and target length:
// k instances of length L, plus an r-slot tail. Invariant k*L + r == n.
inline void pg_derive_sizing(Principle p, int n, int& k, int& L, int& r) {
    int Lt = pg_target_len(p);
    if (n < 1) n = 1;
    k = static_cast<int>(std::lround(static_cast<float>(n) / static_cast<float>(Lt)));
    if (k < 1) k = 1;
    if (k > n) k = n;
    L = n / k;
    if (L < 1) { L = 1; k = n; }
    r = n - k * L;
}

// Fill per-instance motif id (which content block a slot copies; siblings share
// content) and unit id (which RENEW renewal unit an instance belongs to).
// Pure structure, no RNG. Ids are dense from 0. motif/unit counts reported.
inline void pg_build_arrangement(Principle p, int k,
                                 uint8_t* motif_of_inst, uint8_t* unit_of_inst,
                                 int& motif_count, int& unit_count) {
    if (k < 1) k = 1;
    switch (p) {
    case Principle::TwoMotif: {
        // A A B A rolled; degrades: k1->A, k2->A B, k3->A A B, k>=4->A..B A
        if (k == 1) { motif_of_inst[0] = 0; motif_count = 1; }
        else if (k == 2) { motif_of_inst[0] = 0; motif_of_inst[1] = 1; motif_count = 2; }
        else {
            for (int j = 0; j < k; ++j)
                motif_of_inst[j] = static_cast<uint8_t>((j == k - 2) ? 1 : 0);
            motif_count = 2;
        }
        for (int j = 0; j < k; ++j) unit_of_inst[j] = motif_of_inst[j];
        unit_count = motif_count;
        break;
    }
    case Principle::OneMotif:
    case Principle::Ostinato: {
        for (int j = 0; j < k; ++j) { motif_of_inst[j] = 0; unit_of_inst[j] = 0; }
        motif_count = unit_count = 1;
        break;
    }
    case Principle::Hierarchical: {
        // A B A B rolled; each motif is internally cell-tiled (nested repetition).
        for (int j = 0; j < k; ++j) {
            motif_of_inst[j] = static_cast<uint8_t>(j & 1);
            unit_of_inst[j]  = static_cast<uint8_t>(j & 1);
        }
        motif_count = unit_count = (k >= 2) ? 2 : 1;
        break;
    }
    case Principle::CallResponse: {
        // Q A Q A: even instance = question, odd = answer. A renewal unit is a
        // Q&A pair (regenerated together so the answer still resolves to root).
        for (int j = 0; j < k; ++j) {
            motif_of_inst[j] = static_cast<uint8_t>(j);      // each its own content
            unit_of_inst[j]  = static_cast<uint8_t>(j / 2);  // paired
        }
        motif_count = k;
        unit_count  = (k + 1) / 2;
        break;
    }
    default: {
        for (int j = 0; j < k; ++j) { motif_of_inst[j] = 0; unit_of_inst[j] = 0; }
        motif_count = unit_count = 1;
    }
    }
}

} // namespace spky
