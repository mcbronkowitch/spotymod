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

// Generate one motif's content (pitch; gate is all-true — rhythm lives in the
// GrooveCell ranking, not per-slot rests). CallResponse role is by id parity.
inline void pg_gen_motif(Principle p, Rng& rng, int motif_id, int L,
                         float* pitch, bool* gate) {
    switch (p) {
    case Principle::Ostinato: {
        pg_contour_walk(rng, pitch, L, 0.0f, 0.05f, 0.30f); // near-static
        for (int i = 0; i < L; ++i) gate[i] = true;
        break;
    }
    case Principle::Hierarchical: {
        int cl = (L >= 6) ? 4 : 2;              // cell length 2 or 4
        if (cl > L) cl = L;
        float cell[4];
        pg_contour_walk(rng, cell, cl, 0.0f, 0.6f, 0.10f);
        for (int i = 0; i < L; ++i) { pitch[i] = cell[i % cl]; gate[i] = true; }
        break;
    }
    case Principle::CallResponse: {
        bool answer = (motif_id & 1) != 0;
        pg_contour_walk(rng, pitch, L, answer ? 0.5f : 0.0f, 0.6f, answer ? 0.25f : 0.05f);
        if (L > 0) {
            if (answer) pitch[L - 1] = 0.0f;                              // resolve
            else if (std::fabs(pitch[L - 1]) < 0.3f) pitch[L - 1] = 0.5f; // stay open
        }
        for (int i = 0; i < L; ++i) gate[i] = true;
        break;
    }
    case Principle::TwoMotif:
    case Principle::OneMotif:
    default: {
        pg_contour_walk(rng, pitch, L, 0.0f, 0.6f, 0.12f);
        for (int i = 0; i < L; ++i) gate[i] = true;
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

// Regenerate renewal unit `unit` in place across ALL its slots (every sibling
// instance of its motif id(s); for CallResponse both the Q and A of the pair).
// Arrangement is deterministic and re-derived here, so motif_id[]/layout stay
// authoritative and untouched. RNG consumed in ascending motif-id order.
inline void regenerate_unit(Principle p, Rng& rng, const PhraseLayout& layout,
                            const uint8_t* /*motif_id*/, int unit,
                            float* pitch, bool* gate) {
    int k = layout.inst_count;
    int L = layout.motif_len;
    if (k < 1 || L < 1) return;

    uint8_t moti[32], uniti[32];
    int mc, uc;
    pg_build_arrangement(p, k, moti, uniti, mc, uc);

    // Which motif ids belong to this unit?
    bool idsel[32] = {};
    int maxid = 0;
    for (int j = 0; j < k; ++j) {
        if (uniti[j] == unit) idsel[moti[j]] = true;
        if (moti[j] > maxid) maxid = moti[j];
    }
    // Regenerate each selected id once (ascending), scatter to its instances.
    for (int id = 0; id <= maxid; ++id) {
        if (!idsel[id]) continue;
        float cp[32]; bool cg[32];
        pg_gen_motif(p, rng, id, L, cp, cg);
        for (int j = 0; j < k; ++j) {
            if (moti[j] != id) continue;
            for (int i = 0; i < L; ++i) {
                int slot = j * L + i;
                pitch[slot] = cp[i];
                gate[slot]  = cg[i];
            }
        }
    }
}

// One groove cell per phrase (spec 2026-07-16-rhythm-groove-design.md §1):
// rank_of_slot[i] = firing order of cell slot i (0 = the first note DENSE
// reveals), note_len[i] = composed note length in slots. Period = motif
// length L, tiled across the phrase like the pitch motifs, truncated over
// the tail. The downbeat anchor (slot 0) is pinned to rank 0 by construction.
struct GrooveCell {
    uint8_t rank_of_slot[32] = {};
    uint8_t note_len[32] = {};
    uint8_t len = 1;   // L, >= 1
};

// Compose the groove. Syncopation comes from displacement: a strong beat may
// hand its emphasis to the off-beat before it (a push/anticipation), which
// then OUTRANKS the beat it anticipates. Draw order is fixed (sync degree,
// one draw per push candidate, jitter, lengths) and the draw count depends
// only on L, so determinism is stable across outcomes.
inline void pg_gen_groove(Rng& rng, int L, GrooveCell& out) {
    if (L < 1) L = 1;
    if (L > 32) L = 32;
    out.len = static_cast<uint8_t>(L);

    float score[33];
    for (int i = 0; i < L; ++i) score[i] = pg_metric_weight(i);

    // Phrase-wide syncopation degree (mild -> spicy). Tuned by ear.
    float sync = 0.15f + 0.60f * rng.next_unipolar();

    // Pushes: every strong beat (even slot; s == L is the NEXT cell's wrapped
    // downbeat) may displace onto the off-beat before it.
    for (int s = 2; s <= L; s += 2) {
        bool push = rng.next_unipolar() < sync;      // always drawn: fixed count
        if (!push) continue;
        float beat_w = (s == L) ? 1.0f : score[s];
        score[s - 1] = beat_w + 0.05f;               // anticipation outranks its beat
        if (s < L) score[s] *= 0.35f;                // the displaced beat recedes
    }

    // Seeded jitter for tie-breaking variety; slot 0 pinned above everything
    // (spec: anchor rank 0 is enforced, not emergent).
    for (int i = 1; i < L; ++i) score[i] += (rng.next_unipolar() - 0.5f) * 0.06f;
    score[0] = 2.0f;

    // Stable insertion sort, descending score -> firing order.
    uint8_t order[32];
    for (int i = 0; i < L; ++i) order[i] = static_cast<uint8_t>(i);
    for (int i = 1; i < L; ++i) {
        uint8_t o = order[i];
        int j = i - 1;
        while (j >= 0 && score[order[j]] < score[o]) { order[j + 1] = order[j]; --j; }
        order[j + 1] = o;
    }
    for (int r = 0; r < L; ++r) out.rank_of_slot[order[r]] = static_cast<uint8_t>(r);

    // Note lengths, biased short (staccato common, sustains rare). Tuned by ear.
    for (int i = 0; i < L; ++i) {
        float u = rng.next_unipolar();
        out.note_len[i] = static_cast<uint8_t>(u < 0.55f ? 1 : u < 0.80f ? 2 : u < 0.95f ? 3 : 4);
    }
}

// --- Groove variation-zone mutators (spec 2026-07-16-groove-variation-zones) ---
// All preserve the GrooveCell invariants: rank permutation, slot-0 anchor at
// rank 0, note_len in [1,4]. Called by the lane at cycle wraps only; the lane
// owns the zoning dice. Draws are conditional (like regenerate_unit).

// Shared: pick one slot, nudge its composed length by +/-1 (clamped).
inline void pg_groove_nudge_len(Rng& rng, GrooveCell& g) {
    const int L = g.len;
    int i = static_cast<int>(rng.next_unipolar() * static_cast<float>(L));
    if (i > L - 1) i = L - 1;
    int v = static_cast<int>(g.note_len[i]) + (rng.next_unipolar() < 0.5f ? -1 : 1);
    if (v < 1) v = 1;
    if (v > 4) v = 4;
    g.note_len[i] = static_cast<uint8_t>(v);
}

// GROW-side drift: 50/50 adjacent-rank swap (one note moves one place in the
// order DENSE reveals notes; rank 0 excluded) or length nudge.
inline void pg_groove_mutate_grow(Rng& rng, GrooveCell& g) {
    const int L = g.len;
    if (rng.next_unipolar() < 0.5f) {
        if (L < 4) return;                            // no swappable pair beside the anchor
        int j = 1 + static_cast<int>(rng.next_unipolar() * static_cast<float>(L - 2));
        if (j > L - 2) j = L - 2;                     // swap ranks j <-> j+1, j in 1..L-2
        int s1 = -1, s2 = -1;
        for (int i = 0; i < L; ++i) {
            if (g.rank_of_slot[i] == j)     s1 = i;
            if (g.rank_of_slot[i] == j + 1) s2 = i;
        }
        g.rank_of_slot[s1] = static_cast<uint8_t>(j + 1);
        g.rank_of_slot[s2] = static_cast<uint8_t>(j);
    } else {
        pg_groove_nudge_len(rng, g);
    }
}

// RENEW-side re-decision: 70% push flip — swap the off-beat s-1 with its even
// beat s, the exact semantic toggle of pg_gen_groove's displacement (s == L,
// the wrapped downbeat, is excluded: flipping it would demote the anchor) —
// else length nudge. `reroll` regenerates the whole cell instead.
inline void pg_groove_mutate_renew(Rng& rng, GrooveCell& g, bool reroll) {
    const int L = g.len;
    if (reroll) { pg_gen_groove(rng, L, g); return; }
    if (rng.next_unipolar() < 0.7f) {
        int nc = (L - 2) / 2;                         // candidates s = 2, 4, ..., <= L-2
        if (nc < 1) return;
        int c = static_cast<int>(rng.next_unipolar() * static_cast<float>(nc));
        if (c > nc - 1) c = nc - 1;
        int s = 2 + 2 * c;
        uint8_t t = g.rank_of_slot[s - 1];
        g.rank_of_slot[s - 1] = g.rank_of_slot[s];
        g.rank_of_slot[s] = t;
    } else {
        pg_groove_nudge_len(rng, g);
    }
}

} // namespace spky
