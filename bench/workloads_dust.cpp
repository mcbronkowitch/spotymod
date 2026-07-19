#include "workload.h"
#include "mem.h"
#include "fx/fx_util.h"
#include "util/fast_tanh.h"
#include <cmath>

namespace bench {
namespace {

using namespace spky;

// --- DUST grain-cloud proxy -------------------------------------------------
// The DUST spec (docs/superpowers/specs/2026-07-18-dust-grain-cloud-design.md)
// estimates "<< 1 %" for the grain cloud. That estimate counts arithmetic --
// "1 read + window + 2 mults" -- and the arithmetic is right. What it misses is
// that the `read` is not a read: FLUX's tape is 262 144 floats per channel per
// part (1 MB), which does not fit SRAM at any size, so every grain read is a
// scattered SDRAM access. `grain_read_sdram` already prices that pattern at
// 16.84 % max for EIGHT grains, and DUST wants sixteen (8 per part, 2 parts).
//
// These rows measure the real thing instead of extrapolating it. They differ
// from `grain_read_sdram` in three ways that all matter:
//
//   1. Integer offsets, no interpolation, ONE channel per grain (spec §1),
//      against that row's interpolated stereo read. Fewer loads -- but the
//      loads it drops are adjacent, so they share a cache line and the miss
//      count is unchanged. `dust_8_full` is the row that proves or refutes
//      that reasoning: it is `grain_read_sdram`'s grain count with DUST's
//      access shape.
//   2. The Hann window and equal-power pan the proxy has no equivalent for.
//   3. A spray WINDOW. This is the whole point of the family. Cost is linear
//      in cache MISSES, not in grains, so grains confined to the most recent
//      slice of tape -- the region the write head and echo head are already
//      touching -- should stay cache-warm and cost a fraction. Zone F's
//      "spray widens up to the full 5 s" (spec §3) is a tuning constant, not
//      a structural requirement, so if the narrow rows come in cheap, the
//      feature survives by bounding one number.
//
// All rows run in SDRAM. There is no SRAM twin because the premise is that the
// tape cannot live there; `grain_read_sram` (3.09 % max) stands as the
// cache-resident floor for the same access shape.

constexpr size_t kTapeFrames = 262144;              // EchoDelay<kMaxSamples>
constexpr int32_t kTapeMask  = static_cast<int32_t>(kTapeFrames) - 1;
constexpr int    kMaxGrains  = 16;                  // 8 per part, 2 parts
constexpr float  kSpray5s    = static_cast<float>(kTapeFrames);  // the full
                                     // tape reach (262144 samples, ~5.46 s @
                                     // 48 kHz) — previously a bare 240000.f
                                     // (91.5 % of the tape), the same "5 s"
                                     // mistake fx/dust.cpp's tape_max_s made
                                     // (design spec §8 Finding 1)
constexpr float  kSpray05s   = 24000.f;             // 0.5 s
constexpr float  kSpray01s   = 4800.f;              // 0.1 s

struct Grain {
    int32_t offset;      // samples behind the write head
    int32_t delta;       // 0 forward (head-relative 1x), +2 reverse
    int32_t age;
    int32_t length;
    float   pan_l;
    float   pan_r;
    int     src;         // 0 = L tape, 1 = R tape (free stereo decorrelation)
};

float*   g_tape_l = nullptr;
float*   g_tape_r = nullptr;
int32_t  g_write  = 0;
uint32_t g_rng    = 0;

Grain    g_grains[kMaxGrains];
int      g_grain_count = kMaxGrains;
float    g_spray       = kSpray5s;
bool     g_reverse     = false;
bool     g_writeback   = false;
bool     g_erode       = false;

inline uint32_t next_rng()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
inline float next_unipolar() { return static_cast<float>(next_rng() >> 8) / 16777216.f; }

void spawn(Grain& gr)
{
    // Offset anywhere inside the spray window, never nearer than one grain
    // length to the head (a grain must not overrun the write pointer).
    gr.length = 1200 + static_cast<int32_t>(next_unipolar() * 18000.f);   // 25-400 ms
    const float reach = g_spray - static_cast<float>(gr.length) * 2.f;
    gr.offset = gr.length * 2 + static_cast<int32_t>(next_unipolar() * (reach > 0.f ? reach : 1.f));
    gr.age    = 0;
    gr.delta  = g_reverse ? 2 : 0;
    gr.src    = static_cast<int>(next_rng() & 1u);
    const float p = next_unipolar();
    gr.pan_l  = std::sqrt(1.f - p);
    gr.pan_r  = std::sqrt(p);
}

void setup_common()
{
    float* arena = sdram_arena();
    g_tape_l = arena;
    g_tape_r = arena + kTapeFrames;
    for (size_t i = 0; i < kTapeFrames * 2; ++i)
        arena[i] = sinf(static_cast<float>(i) * 0.0007f) * 0.5f;
    g_write = 0;
    g_rng   = 0x9E3779B9u;
    for (int i = 0; i < kMaxGrains; ++i) {
        spawn(g_grains[i]);
        g_grains[i].age = static_cast<int32_t>(next_unipolar() * static_cast<float>(g_grains[i].length));
    }
}

void setup_16_full()   { g_grain_count = 16; g_spray = kSpray5s;  g_reverse = false; g_writeback = false; g_erode = false; setup_common(); }
void setup_16_win05()  { g_grain_count = 16; g_spray = kSpray05s; g_reverse = false; g_writeback = false; g_erode = false; setup_common(); }
void setup_16_win01()  { g_grain_count = 16; g_spray = kSpray01s; g_reverse = false; g_writeback = false; g_erode = false; setup_common(); }
void setup_8_full()    { g_grain_count = 8;  g_spray = kSpray5s;  g_reverse = false; g_writeback = false; g_erode = false; setup_common(); }
void setup_16_rev()    { g_grain_count = 16; g_spray = kSpray5s;  g_reverse = true;  g_writeback = false; g_erode = false; setup_common(); }
void setup_16_wb()     { g_grain_count = 16; g_spray = kSpray5s;  g_reverse = false; g_writeback = true;  g_erode = false; setup_common(); }
void setup_16_erode()  { g_grain_count = 16; g_spray = kSpray5s;  g_reverse = false; g_writeback = true;  g_erode = true;  setup_common(); }

float proc_dust()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t s = 0; s < kBlock; ++s) {
        float sum_l = 0.f, sum_r = 0.f;
        for (int i = 0; i < g_grain_count; ++i) {
            Grain& gr = g_grains[i];
            // hann_value_at is sin^2(pi/2 * x) over 192 entries, so folding the
            // age about the midpoint yields a true sin^2(pi*age) grain window
            // with no new table (spec §"Existing infrastructure").
            const float a = static_cast<float>(gr.age) / static_cast<float>(gr.length);
            const float w = hann_value_at(a < 0.5f ? a * 2.f : (1.f - a) * 2.f);
            const float* tape = gr.src ? g_tape_r : g_tape_l;
            const float v = tape[(g_write + gr.offset) & kTapeMask] * w;
            sum_l += v * gr.pan_l;
            sum_r += v * gr.pan_r;
            gr.offset += gr.delta;
            if (++gr.age >= gr.length) spawn(gr);
        }
        // Tape store. Normal echo write, plus zone-R writeback / erosion.
        float wr_l = in[s], wr_r = in[s] * 0.9f;
        if (g_writeback) {
            wr_l += fast_tanh(sum_l * 0.5f);
            wr_r += fast_tanh(sum_r * 0.5f);
        }
        if (g_erode) {
            // Frozen: read-modify-write at the pointer, wear slightly < 1.
            wr_l = fast_tanh(g_tape_l[g_write] * 0.995f + wr_l);
            wr_r = fast_tanh(g_tape_r[g_write] * 0.995f + wr_r);
        }
        g_tape_l[g_write] = wr_l;
        g_tape_r[g_write] = wr_r;
        g_write = (g_write - 1) & kTapeMask;
        acc += sum_l + sum_r;
    }
    return acc;
}

// --- optimized grain loop ---------------------------------------------------
// The rows above measure ~116 cycles per grain per sample, far more than "1
// read + window + 2 mults" justifies. Three costs in that loop are removable
// without changing what a grain IS:
//
//   1. `age / length` -- a float division per grain per sample. Replaced by a
//      window index stepped by an increment computed once at birth.
//   2. `hann_value_at()` -- an out-of-line call whose `hann_curve()` is a
//      function-local static, so every call pays a thread-safe-init guard
//      check before it can even index the table. The table pointer is hoisted
//      out of the block loop instead.
//   3. `(g_write + offset) & mask` and the L/R tape select, both per sample.
//      The read index is kept absolute and stepped by (delta - 1) -- the write
//      head moves -1 while the grain offset moves +delta -- and the tape
//      pointer is resolved at birth.
//
// A DEDICATED struct, not the one above: the naive rows must keep their exact
// layout so their numbers stay comparable to the 12f05ce run. The optimized
// rows carry their own state and their own checksum -- the two loops quantize
// the window differently and are NOT bit-comparable to each other. That is
// fine; this measures cost, not identity.

struct GrainOpt {
    const float* tape;
    int32_t rd;          // absolute read index, already masked
    int32_t rd_step;     // delta - 1
    int32_t age;
    int32_t length;
    float   widx;        // 0..191 into the Hann table, folded by sign flip
    float   wstep;
    float   pan_l;
    float   pan_r;
};

GrainOpt g_opt[kMaxGrains];

void spawn_opt(GrainOpt& gr)
{
    gr.length = 1200 + static_cast<int32_t>(next_unipolar() * 18000.f);
    const float reach = g_spray - static_cast<float>(gr.length) * 2.f;
    const int32_t offset = gr.length * 2
        + static_cast<int32_t>(next_unipolar() * (reach > 0.f ? reach : 1.f));
    gr.rd      = (g_write + offset) & kTapeMask;
    gr.rd_step = (g_reverse ? 2 : 0) - 1;
    gr.age     = 0;
    gr.tape    = (next_rng() & 1u) ? g_tape_r : g_tape_l;
    gr.widx    = 0.f;
    gr.wstep   = 382.f / static_cast<float>(gr.length);   // 0->191 over half
    const float p = next_unipolar();
    gr.pan_l   = std::sqrt(1.f - p);
    gr.pan_r   = std::sqrt(p);
}

void setup_opt_common()
{
    setup_common();
    g_rng = 0x9E3779B9u;
    for (int i = 0; i < kMaxGrains; ++i) {
        spawn_opt(g_opt[i]);
        g_opt[i].age  = static_cast<int32_t>(next_unipolar() * static_cast<float>(g_opt[i].length));
        g_opt[i].widx = static_cast<float>(g_opt[i].age) * g_opt[i].wstep;
        if (g_opt[i].widx > 191.f) { g_opt[i].widx = 382.f - g_opt[i].widx; g_opt[i].wstep = -g_opt[i].wstep; }
    }
}

void setup_16_opt()       { g_grain_count = 16; g_spray = kSpray5s; g_reverse = false; g_writeback = false; g_erode = false; setup_opt_common(); }
void setup_8_opt()        { g_grain_count = 8;  g_spray = kSpray5s; g_reverse = false; g_writeback = false; g_erode = false; setup_opt_common(); }
void setup_16_opt_erode() { g_grain_count = 16; g_spray = kSpray5s; g_reverse = false; g_writeback = true;  g_erode = true;  setup_opt_common(); }

float proc_dust_opt()
{
    const float* in = test_input();
    const float* curve = hann_curve().data();      // hoisted: no guard per grain
    const int    n = g_grain_count;
    float acc = 0.f;
    for (size_t s = 0; s < kBlock; ++s) {
        float sum_l = 0.f, sum_r = 0.f;
        for (int i = 0; i < n; ++i) {
            GrainOpt& gr = g_opt[i];
            int wi = static_cast<int>(gr.widx);
            if (wi > 190) wi = 190;
            if (wi < 0)   wi = 0;
            const float f = gr.widx - static_cast<float>(wi);
            const float w = curve[wi] + (curve[wi + 1] - curve[wi]) * f;
            const float v = gr.tape[gr.rd] * w;
            sum_l += v * gr.pan_l;
            sum_r += v * gr.pan_r;
            gr.rd    = (gr.rd + gr.rd_step) & kTapeMask;
            gr.widx += gr.wstep;
            if (gr.widx > 191.f) { gr.widx = 382.f - gr.widx; gr.wstep = -gr.wstep; }
            if (++gr.age >= gr.length) spawn_opt(gr);
        }
        float wr_l = in[s], wr_r = in[s] * 0.9f;
        if (g_writeback) {
            wr_l += fast_tanh(sum_l * 0.5f);
            wr_r += fast_tanh(sum_r * 0.5f);
        }
        if (g_erode) {
            wr_l = fast_tanh(g_tape_l[g_write] * 0.995f + wr_l);
            wr_r = fast_tanh(g_tape_r[g_write] * 0.995f + wr_r);
        }
        g_tape_l[g_write] = wr_l;
        g_tape_r[g_write] = wr_r;
        g_write = (g_write - 1) & kTapeMask;
        acc += sum_l + sum_r;
    }
    return acc;
}

} // namespace

const Workload kDustWorkloads[] = {
    { "dust", "dust_16_full",     setup_16_full,     proc_dust     },
    { "dust", "dust_16_win05",    setup_16_win05,    proc_dust     },
    { "dust", "dust_16_win01",    setup_16_win01,    proc_dust     },
    { "dust", "dust_8_full",      setup_8_full,      proc_dust     },
    { "dust", "dust_16_rev",      setup_16_rev,      proc_dust     },
    { "dust", "dust_16_wb",       setup_16_wb,       proc_dust     },
    { "dust", "dust_16_erode",    setup_16_erode,    proc_dust     },
    { "dust", "dust_16_opt",      setup_16_opt,      proc_dust_opt },
    { "dust", "dust_8_opt",       setup_8_opt,       proc_dust_opt },
    { "dust", "dust_16_opt_erode",setup_16_opt_erode,proc_dust_opt },
};
const int kDustCount = sizeof(kDustWorkloads) / sizeof(kDustWorkloads[0]);

} // namespace bench
