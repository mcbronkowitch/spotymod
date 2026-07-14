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

// Generate one motif's content (pitch + gate), length L. Gate uses motif-RELATIVE
// metric weight so sibling instances copy byte-identically; for aligned L this
// equals the absolute-slot weight (§spec). CallResponse role is by id parity.
inline void pg_gen_motif(Principle p, Rng& rng, int motif_id, int L,
                         float* pitch, bool* gate) {
    switch (p) {
    case Principle::Ostinato: {
        pg_contour_walk(rng, pitch, L, 0.0f, 0.05f, 0.30f); // near-static
        for (int i = 0; i < L; ++i) gate[i] = pg_metric_weight(i) >= 0.30f;
        break;
    }
    case Principle::Hierarchical: {
        int cl = (L >= 6) ? 4 : 2;              // cell length 2 or 4
        if (cl > L) cl = L;
        float cell[4]; bool cellg[4];
        pg_contour_walk(rng, cell, cl, 0.0f, 0.6f, 0.10f);
        for (int i = 0; i < cl; ++i) cellg[i] = pg_metric_weight(i) >= 0.25f;
        for (int i = 0; i < L; ++i) { pitch[i] = cell[i % cl]; gate[i] = cellg[i % cl]; }
        break;
    }
    case Principle::CallResponse: {
        bool answer = (motif_id & 1) != 0;
        pg_contour_walk(rng, pitch, L, answer ? 0.5f : 0.0f, 0.6f, answer ? 0.25f : 0.05f);
        if (L > 0) {
            if (answer) pitch[L - 1] = 0.0f;                              // resolve
            else if (std::fabs(pitch[L - 1]) < 0.3f) pitch[L - 1] = 0.5f; // stay open
        }
        for (int i = 0; i < L; ++i) gate[i] = pg_metric_weight(i) >= 0.25f;
        break;
    }
    case Principle::TwoMotif:
    case Principle::OneMotif:
    default: {
        pg_contour_walk(rng, pitch, L, 0.0f, 0.6f, 0.12f);
        for (int i = 0; i < L; ++i) gate[i] = pg_metric_weight(i) >= 0.25f;
        break;
    }
    }
}

// Fill pitch/gate/motif_id[0..n) for n = min(steps,32); write the layout.
// Deterministic per rng. RNG is consumed in ascending motif-id order, then tail.
inline void generate_phrase(Principle p, Rng& rng, int steps,
                            float* pitch, bool* gate, uint8_t* motif_id,
                            PhraseLayout& out) {
    int n = steps; if (n > 32) n = 32; if (n < 1) n = 1;
    int k, L, r;
    pg_derive_sizing(p, n, k, L, r);

    uint8_t moti[32], uniti[32];
    int motif_count, unit_count;
    pg_build_arrangement(p, k, moti, uniti, motif_count, unit_count);

    int n_ids = 1;
    for (int j = 0; j < k; ++j) if (moti[j] + 1 > n_ids) n_ids = moti[j] + 1;

    // Generate distinct content once per id (ascending), then scatter to instances.
    float cpitch[32]; bool cgate[32];              // n_ids*L <= n <= 32
    for (int id = 0; id < n_ids; ++id)
        pg_gen_motif(p, rng, id, L, cpitch + id * L, cgate + id * L);

    for (int j = 0; j < k; ++j) {
        int id = moti[j];
        for (int i = 0; i < L; ++i) {
            int slot = j * L + i;
            pitch[slot]    = cpitch[id * L + i];
            gate[slot]     = cgate[id * L + i];
            motif_id[slot] = static_cast<uint8_t>(id);
        }
    }

    if (r > 0) {                                    // tail motif, its own id
        float tp[32]; bool tg[32];
        pg_gen_motif(p, rng, n_ids, r, tp, tg);
        for (int i = 0; i < r; ++i) {
            int slot = k * L + i;
            pitch[slot]    = tp[i];
            gate[slot]     = tg[i];
            motif_id[slot] = static_cast<uint8_t>(n_ids);
        }
    }

    out.motif_len   = static_cast<uint8_t>(L);
    out.tail_len    = static_cast<uint8_t>(r);
    out.inst_count  = static_cast<uint8_t>(k);
    out.motif_count = static_cast<uint8_t>(unit_count);
}

} // namespace spky
