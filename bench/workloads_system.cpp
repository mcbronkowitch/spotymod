#include "workload.h"
#include "mem.h"
#include "instrument.h"
#include "mod/super_modulator.h"
#include "mod/lane_id.h"
#include "center/center.h"
#include "parts/part.h"
#include "synth/synth_engine.h"
#include "fx/part_fx.h"

namespace bench {
namespace {

using namespace spky;

float g_out_l[kBlock], g_out_r[kBlock];

// --- 1. baseline ------------------------------------------------------------
void setup_empty() {}
float proc_empty() { return 0.f; }

// --- 2. modulation plane only ----------------------------------------------
// Two SuperModulators plus the Center, no voices, no FX: the lanes budget the
// design spec estimates at 4-6 %.
//
// Center::update needs two Parts to write its hooks into, so two live here --
// but they are never process()ed. What this row measures is the mod plane and
// the control tick, not the parts.
SuperModulator g_mod_a, g_mod_b;
Center         g_center;
Part           g_hook_a, g_hook_b;

void setup_mod()
{
    g_mod_a.init(kSampleRate, 1u);
    g_mod_b.init(kSampleRate, 2u);
    g_center.init(kSampleRate, 11u);
    g_hook_a.init(kSampleRate, 1u);
    g_hook_b.init(kSampleRate, 2u);
    g_mod_a.set_rate(0.5f); g_mod_b.set_rate(0.6f);
    g_mod_a.set_density(0.7f); g_mod_b.set_density(0.7f);
}
float proc_mod()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        g_mod_a.process();
        g_mod_b.process();
        acc += g_mod_a.lane_output(LANE_PITCH) + g_mod_b.lane_output(LANE_PITCH);
    }
    // Control rate: one tick per Center::kCtrlInterval (96) samples, which is
    // exactly one per block. Running it per sample would measure a cadence the
    // firmware never has.
    g_center.update(g_mod_a, g_mod_b, g_hook_a, g_hook_b);
    return acc;
}

// --- 3-5. synth voices, 1 / 2 / 4 -------------------------------------------
// Does polyphony scale linearly? One SynthEngine, triggered often enough that
// the requested number of voices is genuinely sounding for the whole window.
SynthEngine g_synth;
int         g_synth_voices = 1;
int         g_trig_ctr = 0;

void setup_synth_n(int n)
{
    g_synth_voices = n;
    g_synth.set_seed(3u);
    g_synth.init(kSampleRate);
    g_synth.set_decay(1.f);       // 8x cycle: notes stay up, no silent blocks
    g_synth.set_cycle(2.f);
    g_synth.set_flow(false);
    g_trig_ctr = 0;
    for (int v = 0; v < n; ++v) g_synth.trigger(0.3f + 0.1f * v);
}
void setup_synth_1() { setup_synth_n(1); }
void setup_synth_2() { setup_synth_n(2); }
void setup_synth_4() { setup_synth_n(4); }

float proc_synth()
{
    float acc = 0.f, l, r;
    for (size_t i = 0; i < kBlock; ++i) {
        // Retrigger the whole set periodically so envelopes never all decay
        // out from under the measurement.
        if (++g_trig_ctr >= 24000) {
            g_trig_ctr = 0;
            for (int v = 0; v < g_synth_voices; ++v) g_synth.trigger(0.3f + 0.1f * v);
        }
        g_synth.process(l, r);
        acc += l + r;
    }
    return acc;
}

// --- 6. FX blocks, one at a time -------------------------------------------
// PartFx carries GRIT, FLUX and COMP; each row turns on exactly one so the
// 8-10 % FX estimate decomposes. `FxBlock` is an enum class with only Flux and
// Grit -- COMP is not a block, it is set_comp(amount) and bypasses bit-exactly
// at 0, so the selector here is a plain int, not an FxBlock.
enum FxSel { SEL_GRIT = 0, SEL_FLUX = 1, SEL_COMP = 2 };

PartFx g_fx;
float  g_fxv[FXT_COUNT];

void setup_fx(int sel)
{
    const FxMem& m = fx_mem();
    g_fx.init(kSampleRate, m.echo[0][0], m.echo[0][1]);
    // immediate = true: the soft switches would otherwise fade in over the
    // warm-up and the measured window would see a partly-engaged chain.
    g_fx.set_fx_on(FxBlock::Grit, sel == SEL_GRIT, true);
    g_fx.set_fx_on(FxBlock::Flux, sel == SEL_FLUX, true);
    g_fx.set_comp(sel == SEL_COMP ? 0.8f : 0.f);
    g_fx.set_grit_mix(1.f);
    g_fx.set_flux_mix(1.f);
    g_fx.set_bpm(120.f);

    // Already-modulated target values, as Part::fx_target_value() would hand
    // them over. Fixed here: this row measures the FX, not the modulation.
    g_fxv[FXT_GRIT_INT]  = 0.8f;
    g_fxv[FXT_FLUX_TIME] = 0.5f;
    g_fxv[FXT_FX_MIX]    = 1.f;
    g_fxv[FXT_REV_SEND]  = 0.5f;
    g_fxv[FXT_FLUX_FB]   = 0.7f;
}
void setup_fx_grit() { setup_fx(SEL_GRIT); }
void setup_fx_flux() { setup_fx(SEL_FLUX); }
void setup_fx_comp() { setup_fx(SEL_COMP); }

float proc_fx()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l = in[i], r = in[i] * 0.9f, sl = 0.f, sr = 0.f;
        g_fx.process(l, r, sl, sr, g_fxv);
        acc += l + r + sl + sr;
    }
    return acc;
}

// --- 7. Oliverb solo --------------------------------------------------------
// The reverb question (estimate 15-25 %), and the first METER measurement the
// firmware-shell spec has been carrying as a TODO. Worst case: big room,
// blooming decay, dense diffusion, both LFOs up.
void setup_reverb()
{
    AmbientReverb& v = reverb_sram();
    v.init(kSampleRate);
    v.clear();
    v.set_size(0.9f);
    v.set_decay(0.95f);        // above the 1.0 loop-gain crossing: bloom
    v.set_tone(0.8f);
    v.set_diffusion(0.9f);
    v.set_diffuser_mod_depth(1.f);
    v.set_mod_depth(1.f);
}
float proc_reverb()
{
    const float* in = test_input();
    AmbientReverb& v = reverb_sram();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l, r;
        v.process(in[i], in[i] * 0.9f, l, r);
        acc += l + r;
    }
    return acc;
}

// --- 8-9. the whole instrument ---------------------------------------------
Instrument g_inst;

void setup_inst_common()
{
    g_inst.init(kSampleRate, fx_mem());
    g_inst.set_tempo_bpm(120.f);
}

// Init patch: the typical load.
void setup_inst_init()
{
    setup_inst_common();
}

// Worst case: 8 voices, 4-note COLOR on both parts, every FX block on, high
// diffusion, echo at maximum. THE number.
void setup_inst_worst()
{
    setup_inst_common();
    for (int p = 0; p < PART_COUNT; ++p) {
        g_inst.set_color(p, 1.f);          // 4-note chords -> 4 voices per part
        g_inst.set_density(p, 1.f);
        g_inst.set_depth(p, 1.f);
        g_inst.set_rate(p, 0.8f);
        g_inst.set_fx_on(p, FxBlock::Grit, true);
        g_inst.set_fx_on(p, FxBlock::Flux, true);
        g_inst.set_grit_mix(p, 1.f);
        g_inst.set_flux_mix(p, 1.f);
        g_inst.set_comp(p, 1.f);
        g_inst.set_voice_decay(p, 1.f);
        g_inst.trigger_manual(p);
    }
    g_inst.set_reverb_mix(0.5f);
    g_inst.set_reverb_size(1.f);
    g_inst.set_reverb_decay(0.95f);
    g_inst.set_reverb_diffusion(0.9f);
    g_inst.set_reverb_smear(1.f);
    g_inst.set_reverb_mod(1.f);
    g_inst.set_master_drive(1.f);
}

int g_inst_ctr = 0;
float proc_inst()
{
    const float* in = test_input();
    g_inst.process(in, in, g_out_l, g_out_r, kBlock);
    // Keep the voices busy: a fire every ~half second on both parts.
    if (++g_inst_ctr >= 250) {
        g_inst_ctr = 0;
        g_inst.trigger_manual(PART_A);
        g_inst.trigger_manual(PART_B);
    }
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_out_l[i] + g_out_r[i];
    return acc;
}

} // namespace

const Workload kCoreWorkloads[] = {
    { "system", "empty_callback",     setup_empty,     proc_empty   },
    { "system", "mod_plane_2x_center",setup_mod,       proc_mod     },
    { "system", "synth_1_voice",      setup_synth_1,   proc_synth   },
    { "system", "synth_2_voices",     setup_synth_2,   proc_synth   },
    { "system", "synth_4_voices",     setup_synth_4,   proc_synth   },
    { "system", "fx_grit",            setup_fx_grit,   proc_fx      },
    { "system", "fx_flux_sdram",      setup_fx_flux,   proc_fx      },
    { "system", "fx_comp",            setup_fx_comp,   proc_fx      },
    { "system", "oliverb_solo_sram",  setup_reverb,    proc_reverb  },
    { "system", "instrument_init",    setup_inst_init, proc_inst    },
    { "system", "instrument_worst",   setup_inst_worst,proc_inst    },
};
const int kCoreCount = sizeof(kCoreWorkloads) / sizeof(kCoreWorkloads[0]);

} // namespace bench
