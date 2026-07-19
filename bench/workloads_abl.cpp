#include "workload.h"
#include "mem.h"
#include <cmath>
#include "instrument.h"
#include "parts/part.h"
#include "fx/part_fx.h"
#include "fx/flux.h"
#include "fx/grit.h"
#include "fx/limiter.h"
#include "util/fast_sin.h"

namespace bench {
namespace {

using namespace spky;

float g_out_l[kBlock], g_out_r[kBlock];

// --- micro rows: one primitive, 96 data-dependent calls ---------------------
//
// These four price every per-sample libm suspicion in one sweep: FLUX's tanh
// (x2 channels x2 parts), the driven limiter's tanh (x2), PartFx's rev-send
// sinf (x2 parts), and the engine's own fast_sin as the yardstick. Arguments
// come from the noise block so -ffast-math cannot fold the loop; results feed
// the checksum. 96 calls = the per-sample-per-block cost of ONE call site.
void setup_micro() { (void)test_input(); }

float proc_micro_sinf() {
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += std::sin(in[i] * 3.1f);
    return acc;
}
float proc_micro_tanhf() {
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += std::tanh(in[i] * 2.5f);
    return acc;
}
float proc_micro_powf() {
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += std::pow(2.f, in[i]);
    return acc;
}
float proc_micro_fast_sin() {
    const float* in = test_input();
    float acc = 0.f;
    // fast_sin(p) is sin(2*pi*p) on p in [0,1): map the bipolar noise inside.
    for (size_t i = 0; i < kBlock; ++i) acc += fast_sin(in[i] * 0.49f + 0.5f);
    return acc;
}

// --- part glue: a full Part that never sounds a voice -----------------------
//
// Everything Part::process does per sample EXCEPT audible voices: 11 lane
// target evaluations, the quantizer, ChordBuilder::set_color + apply, the
// virtual set_targets/set_chord/process calls, and the PartFx shell (blocks
// off). Boot order matters: init() arms the FLOW drone promise
// (_auto_pending), and set_inhibit(true) -> set_hold(true) clears it BEFORE
// the first process(), so no voice ever triggers. Lanes stay in FLOW at full
// cost — deliberately, since STEP lanes are fire-gated and cheaper.
//
//   glue = this row − super_mod_5lanes − engine intercept − fx_none
//
// where the engine intercept is the synth family's zero-voice fixed cost
// (extrapolated: synth_1 − (synth_2 − synth_1), ~16k on the 7e99b74 run).
// No echo buffers are passed, so FLUX can never engage and the row never
// touches SDRAM.
Part g_glue;

void setup_part_glue()
{
    g_glue.init(kSampleRate, 7u);
    g_glue.set_inhibit(true);     // clears the boot drone; lane fires suppressed
}
float proc_part_glue()
{
    float acc = 0.f, l, r, sl, sr;
    for (size_t i = 0; i < kBlock; ++i) {
        g_glue.process(l, r, sl, sr);
        acc += l + r + sl + sr;
    }
    // Guard: if any voice ever sounds, this moves the checksum. The row's
    // meaning depends on max_voice_env() == 0 for the whole window.
    acc += g_glue.max_voice_env();
    return acc;
}

// --- instrument ablations: worst case with exactly one change ---------------
//
// setup_worst() mirrors workloads_system.cpp's setup_inst_worst exactly (keep
// them in sync by eye when either changes). Each variant then removes or adds
// ONE thing; the delta against instrument_worst is that thing's in-context
// cost, and the excess of that delta over the solo rows is composition
// coupling (cache eviction between components) — the effect no solo row can
// see. Own instance + own retrigger counter: the system family's g_inst_ctr
// phase must not leak in here (same trap the system file documents).
Instrument g_abl_inst;
int        g_abl_ctr = 0;

void setup_worst(bool flux_on, float reverb_mix)
{
    g_abl_inst.init(kSampleRate, fx_mem());
    g_abl_inst.set_tempo_bpm(120.f);
    g_abl_ctr = 0;
    for (int p = 0; p < PART_COUNT; ++p) {
        g_abl_inst.set_color(p, 1.f);
        g_abl_inst.set_density(p, 1.f);
        g_abl_inst.set_depth(p, 1.f);
        g_abl_inst.set_rate(p, 0.8f);
        g_abl_inst.set_fx_on(p, FxBlock::Grit, true);
        g_abl_inst.set_fx_on(p, FxBlock::Flux, flux_on);
        g_abl_inst.set_grit_mix(p, 1.f);
        g_abl_inst.set_flux_mix(p, 1.f);
        g_abl_inst.set_comp(p, 1.f);
        g_abl_inst.set_voice_decay(p, 1.f);
        g_abl_inst.trigger_manual(p);
    }
    g_abl_inst.set_reverb_mix(reverb_mix);
    g_abl_inst.set_reverb_size(1.f);
    g_abl_inst.set_reverb_decay(0.95f);
    g_abl_inst.set_reverb_diffusion(0.9f);
    g_abl_inst.set_reverb_smear(1.f);
    g_abl_inst.set_reverb_mod(1.f);
    g_abl_inst.set_master_drive(1.f);
}

void setup_worst_noflux()   { setup_worst(false, 0.5f); }
void setup_worst_noreverb() { setup_worst(true,  0.f);  }

// CHOKE engaged — the state the system family's worst case runs without.
// choke = -1 makes part A the priority side; A goes to STEP so its gate is
// only high ~5 ms per fire and the stage-2 window falls through to the
// max_voice_env() per-sample scan (instrument.cpp) — the exact code path
// that is currently unpriced. B keeps its FLOW voices decaying (~16 s at
// decay 1), so the row still carries 8 active voices.
void setup_worst_choked()
{
    setup_worst(true, 0.5f);
    g_abl_inst.set_step(PART_A, true, 8);
    g_abl_inst.set_choke(-1.f);
    g_abl_inst.trigger_manual(PART_A);
}

float proc_abl_inst()
{
    const float* in = test_input();
    g_abl_inst.process(in, in, g_out_l, g_out_r, kBlock);
    if (++g_abl_ctr >= 250) {
        g_abl_ctr = 0;
        g_abl_inst.trigger_manual(PART_A);
        g_abl_inst.trigger_manual(PART_B);
    }
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_out_l[i] + g_out_r[i];
    acc += static_cast<float>(g_abl_inst.active_voices(PART_A));
    acc += static_cast<float>(g_abl_inst.active_voices(PART_B));
    return acc;
}

// --- master limiter, clean vs driven ----------------------------------------
//
// At drive 0 with peaks under the -1 dBFS knee the limiter is a bit-exact
// early return. At drive 1 the pre-gain (x4) defeats that bypass and shape()
// runs libm tanh on BOTH channels EVERY sample. instrument_worst sets
// master_drive(1), so the delta between these rows is inside THE number, and
// no existing row prices it.
Limiter g_lim;

void setup_lim_clean()  { g_lim.init(); g_lim.set_drive(0.f); }
void setup_lim_driven() { g_lim.init(); g_lim.set_drive(1.f); }

float proc_lim()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l = in[i] * 0.7f, r = in[i] * 0.63f;
        g_lim.process(l, r);
        acc += l + r;
    }
    return acc;
}

// --- EchoDelay core, SRAM vs SDRAM ------------------------------------------
//
// The same template FLUX uses (EchoDelay<N> is header-only), instantiated
// small enough to fit the 16k-float SRAM arena, mono, identical settings in
// both regions. 0.25 s at 48 kHz = a 12 000-sample read offset — far outside
// any cache, so the SDRAM row's scatter is honest. The pair splits FLUX's
// isolated 77k into memory tax (sdram − sram) and compute (bpf + tanh +
// per-sample SetDelay; cross-check the tanh share against micro_tanhf).
// One shipping FLUX = 2 channels; the full instrument runs 2 parts = x4.
//
// NOT COMPARABLE TO THE 9be5df9 RUN. `perf(flux): one shared delay-time slew
// for both channels` moved the delay-time one-pole out of EchoDelay and into
// Flux, which now hands the smoothed length in as `delay_samples`; SetLagTime
// and SetDelayTime no longer exist. These two rows therefore no longer carry
// the per-sample slew they carried before, and read lower by that amount. The
// row still measures bpf + tanh + per-sample SetDelay as the comment above
// says — only the fonepole is gone. Whoever next re-defines these rows should
// decide whether the bench should reproduce Flux's slew itself; until then,
// do not diff echo_short_* against the 9be5df9 report.
constexpr size_t kShortEcho = 16 * 1024;   // == kSramFloats, whole arena, mono

// 0.25 s at 48 kHz, the fixed offset SetDelayTime(0.25f, true) used to set.
constexpr float kEchoDelaySamples = 0.25f * kSampleRate;

EchoDelay<kShortEcho> g_echo_abl;

void setup_echo_region(float* buf)
{
    g_echo_abl.Init(kSampleRate, buf);
    g_echo_abl.SetFeedback(0.7f);
}
void setup_echo_sram()  { setup_echo_region(sram_arena());  }
void setup_echo_sdram() { setup_echo_region(sdram_arena()); }

float proc_echo()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i)
        acc += g_echo_abl.Process(in[i], kEchoDelaySamples);
    return acc;
}

// --- GRIT, both modes -------------------------------------------------------
//
// fx_grit measures Drive-in-shell only; Reduce (decimator + bitcrush +
// reducer) has never had a row. Standalone Grit, full mix, so the two modes
// compare directly without the PartFx shell in the way.
Grit g_grit;

void setup_grit_mode(GritMode m)
{
    g_grit.init(kSampleRate);
    g_grit.set_mode(m);
    g_grit.set_on(true, true);
    g_grit.set_intensity(0.8f);
    g_grit.set_mix(1.f);
}
void setup_grit_drive()  { setup_grit_mode(GritMode::Drive);  }
void setup_grit_reduce() { setup_grit_mode(GritMode::Reduce); }

float proc_grit()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l = in[i], r = in[i] * 0.9f;
        g_grit.process(l, r);
        acc += l + r;
    }
    return acc;
}

} // namespace

const Workload kAblWorkloads[] = {
    { "abl", "micro_sinf",          setup_micro,          proc_micro_sinf     },
    { "abl", "micro_tanhf",         setup_micro,          proc_micro_tanhf    },
    { "abl", "micro_powf",          setup_micro,          proc_micro_powf     },
    { "abl", "micro_fast_sin",      setup_micro,          proc_micro_fast_sin },
    { "abl", "part_glue_flow",      setup_part_glue,      proc_part_glue      },
    { "abl", "inst_worst_noflux",   setup_worst_noflux,   proc_abl_inst       },
    { "abl", "inst_worst_noreverb", setup_worst_noreverb, proc_abl_inst       },
    { "abl", "inst_worst_choked",   setup_worst_choked,   proc_abl_inst       },
    { "abl", "limiter_clean",       setup_lim_clean,      proc_lim            },
    { "abl", "limiter_driven",      setup_lim_driven,     proc_lim            },
    { "abl", "echo_short_sram",     setup_echo_sram,      proc_echo           },
    { "abl", "echo_short_sdram",    setup_echo_sdram,     proc_echo           },
    { "abl", "grit_drive_solo",     setup_grit_drive,     proc_grit           },
    { "abl", "grit_reduce_solo",    setup_grit_reduce,    proc_grit           },
};
const int kAblCount = sizeof(kAblWorkloads) / sizeof(kAblWorkloads[0]);

} // namespace bench
