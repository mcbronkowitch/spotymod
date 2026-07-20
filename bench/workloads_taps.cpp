#include "workload.h"
#include "mem.h"
#include "fx/taps.h"
#include <cmath>

namespace bench {
namespace {

using namespace spky;

// --- TapBank: the two-tap read that replaced DustCloud ----------------------
// dust_8_opt (bench/workloads_dust.cpp, now deleted) priced one part's worth
// of the eight-grain cloud -- a scheduler, a spray window, a Hann-windowed
// sum. TapBank has none of that: engine/fx/taps.h's own comment puts it
// plainly -- "no grain pool, no scheduler, no anchor and no RNG: the bank is
// deterministic, and its worst case is constant -- two mono reads and two
// one-poles, whatever the material does." taps_2_opt below IS that worst
// case: DUST 1 (both taps at full gain, neither muted) and ROT 0.5 (both
// filters mid-sweep, so neither one-pole's `a` coefficient collapses to a
// cheap 0/1 edge). One part, because TapBank lives inside Flux and Flux is
// per-part -- the two-part instrument cost is this row twice, same as
// dust_8_opt stood in for one part of the old eight-per-part cloud.

constexpr size_t  kTapeFrames = 262144;              // Flux::kMaxSamples
constexpr int32_t kTapeMask   = static_cast<int32_t>(kTapeFrames) - 1;

float*   g_tape_l = nullptr;
float*   g_tape_r = nullptr;
int32_t  g_write  = 0;

void fill_tape(float* arena)
{
    g_tape_l = arena;
    g_tape_r = arena + kTapeFrames;
    for (size_t i = 0; i < kTapeFrames * 2; ++i)
        arena[i] = sinf(static_cast<float>(i) * 0.0007f) * 0.5f;
    g_write = 0;
}

TapBank g_bank;

void setup_taps_2_opt()
{
    fill_tape(sdram_arena());

    g_bank.init(kSampleRate);
    g_bank.set_rot(0.5f);
    g_bank.set_dust(1.f);      // both taps' gain_target > 0 -- two LIVE offsets
    const int32_t off[tap_tuning::kTaps] = { 6000, 24000 };   // well past
        // kMinGap (32) and kRelatchMin (64); tape starts at 0 (kMuted), so
        // both taps latch and begin their fade-in dip immediately.
    g_bank.set_offsets(off);

    // Settle past the gain slew (kGainSlewS = 20 ms = 960 samples) and the
    // position dip (kDipSeconds = 2 ms = 96 samples) so the measured rows
    // land on the bank's steady state -- both taps fully live, filters
    // running on real signal -- not the one-time ramp-up transient. 8000
    // samples mirrors tests/test_taps.cpp's settle() default.
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 8000; ++i) {
        l = 0.f; r = 0.f;
        TapeTap tape{ g_tape_l, g_tape_r, g_write, kTapeMask };
        g_bank.process(tape, l, r);
        g_write = (g_write - 1) & kTapeMask;
    }
}

float proc_taps_2_opt()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t s = 0; s < kBlock; ++s) {
        TapeTap tape{ g_tape_l, g_tape_r, g_write, kTapeMask };
        float l = 0.f, r = 0.f;
        g_bank.process(tape, l, r);
        // Tape store: the normal FLUX echo write. The taps are read-only
        // riders on it, same as production (engine/fx/flux.cpp writes the
        // tape independently of whether _taps.active()).
        g_tape_l[g_write] = in[s];
        g_tape_r[g_write] = in[s] * 0.9f;
        g_write = (g_write - 1) & kTapeMask;
        acc += l + r;
    }
    return acc;
}

// --- tap_read_sdram: the anchor, streaming not scattered --------------------
// grain_read_sdram (bench/workloads_memory.cpp) prices EIGHT reads at random,
// unrelated tape positions each sample -- DustCloud's shape, where every
// grain sits wherever it was spawned and the next grain has no relation to
// the last. That row is the wrong anchor for a tap: a tap's offset is set
// once by derive_offsets and held for the rhythm's duration, and the write
// head decrements by exactly 1 every sample, so a fixed offset reads
// SEQUENTIAL tape addresses -- the read walks backward through SDRAM in
// lock-step with the write, one sample at a time. That is a stream, and
// pricing it as a scatter (grain_read_sdram's 16.84 % max, EIGHT of them)
// would flatter TapBank for a cost it does not pay.
//
// This row is deliberately the plainest possible version of that stream: one
// channel, one fixed offset, no filter, no gain slew, no dip -- the memory
// access shape alone, same spirit as grain_read_sdram pricing the memory
// shape and not the whole DustCloud. It shares taps_2_opt's tape state file-
// wide (only one workload runs at a time, and setup() reinitializes fully).

constexpr int32_t kTapReadOffset = 6000;   // matches taps_2_opt's tap 0

void setup_tap_read_sdram()
{
    fill_tape(sdram_arena());
}

float proc_tap_read_sdram()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t s = 0; s < kBlock; ++s) {
        TapeTap tape{ g_tape_l, g_tape_r, g_write, kTapeMask };
        acc += tape.read(false, kTapReadOffset);
        g_tape_l[g_write] = in[s];
        g_tape_r[g_write] = in[s] * 0.9f;
        g_write = (g_write - 1) & kTapeMask;
    }
    return acc;
}

} // namespace

const Workload kTapsWorkloads[] = {
    { "taps", "taps_2_opt",      setup_taps_2_opt,      proc_taps_2_opt      },
    { "taps", "tap_read_sdram",  setup_tap_read_sdram,  proc_tap_read_sdram  },
};
const int kTapsCount = sizeof(kTapsWorkloads) / sizeof(kTapsWorkloads[0]);

} // namespace bench
