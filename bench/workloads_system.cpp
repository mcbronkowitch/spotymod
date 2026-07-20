#include "workload.h"
#include "mem.h"
#include "instrument.h"
#include "mod/super_modulator.h"
#include "mod/lane_id.h"
#include "center/center.h"
#include "parts/part.h"
#include "synth/synth_engine.h"
#include "fx/part_fx.h"
#include "fx/taps.h"

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
// Does polyphony scale linearly? One SynthEngine, triggered exactly once in
// setup for the row's intended voice count and never topped up.
//
// SynthEngine::_do_trigger allocates round-robin over *inactive* voices
// (synth_engine.cpp), so retriggering the full set mid-measurement -- as an
// earlier version of this row did -- doesn't refresh voices, it ADDS them:
// with this decay/cycle pairing giving ~16 s of envelope, none of the
// original voices had freed up by the next retrigger, and the "1 voice" row
// silently grew to 2, 3, then 4 voices over the measured window. The fix is
// structural: trigger the intended count once, and rely on the same long
// decay (now a feature, not a bug) to keep exactly those voices sounding,
// unrefreshed, for the whole ~2.2 s measured window.
SynthEngine g_synth;
int         g_synth_voices = 1;

void setup_synth_n(int n)
{
    g_synth_voices = n;
    g_synth.set_seed(3u);
    g_synth.init(kSampleRate);
    g_synth.set_decay(1.f);       // 8x cycle: ~16s decay outlives the measured window
    g_synth.set_cycle(2.f);
    g_synth.set_flow(false);
    for (int v = 0; v < n; ++v) g_synth.trigger(0.3f + 0.1f * v);
}
void setup_synth_1() { setup_synth_n(1); }
void setup_synth_2() { setup_synth_n(2); }
void setup_synth_4() { setup_synth_n(4); }

float proc_synth()
{
    float acc = 0.f, l, r;
    for (size_t i = 0; i < kBlock; ++i) {
        g_synth.process(l, r);
        acc += l + r;
    }
    // Cheap regression guard: fold the actual active voice count into the
    // returned value so it reaches the checksum. If a future change makes
    // this row hold the wrong number of voices, the checksum moves -- no
    // print inside the measured loop, no silent pass.
    acc += static_cast<float>(g_synth.active_voices());
    return acc;
}

// --- 6. FX blocks, one at a time -------------------------------------------
// PartFx carries GRIT, FLUX and COMP; each row turns on exactly one so the
// 8-10 % FX estimate decomposes. `FxBlock` is an enum class with only Flux and
// Grit -- COMP is not a block, it is set_comp(amount) and bypasses bit-exactly
// at 0, so the selector here is a plain int, not an FxBlock.
//
// SEL_NONE runs the identical PartFx::process shell with every block
// disabled (GRIT off, FLUX off, set_comp(0.f), which the engine bypasses
// bit-exactly). Without this row, fx_grit/fx_flux_sdram/fx_comp each measure
// "shell + one block" and there is no way to isolate a block's own cost --
// this is the row that makes fx_X - fx_none the block's isolated cost.
enum FxSel { SEL_GRIT = 0, SEL_FLUX = 1, SEL_COMP = 2, SEL_NONE = 3 };

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
void setup_fx_none() { setup_fx(SEL_NONE); }
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
int        g_inst_ctr = 0;

void setup_inst_common()
{
    g_inst.init(kSampleRate, fx_mem());
    g_inst.set_tempo_bpm(120.f);
    // Reset the retrigger phase here, not just at file-scope initialization:
    // without this, instrument_worst's phase silently inherits whatever
    // instrument_init's process() loop left it at (they share g_inst_ctr and
    // run in table order). Deterministic today because nothing runs between
    // them, but a row inserted before either would then shift both retrigger
    // phases with it, unnoticed.
    g_inst_ctr = 0;
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
    // Same guard proc_synth uses: fold both parts' active voice counts into
    // the returned value so a wrong voice count -- the exact failure that
    // shipped undetected as a "1 voice" row measuring 2.8 voices -- moves
    // the checksum instead of passing silently. No printing inside the
    // measured loop.
    acc += static_cast<float>(g_inst.active_voices(PART_A));
    acc += static_cast<float>(g_inst.active_voices(PART_B));
    return acc;
}

// --- 10. the whole instrument, taps engaged --------------------------------
// instrument_worst never calls set_dust/set_rot -- the tap bank's isolated
// cost is measured separately (taps_2_opt, bench/workloads_taps.cpp) -- so
// the combined worst case has only ever been an extrapolation (~92.6 + ~7).
// This row measures instrument_worst's exact configuration plus DUST 1
// (both taps at full gain) and ROT 0.5 (mid spread, so neither one-pole's
// coefficient collapses to a cheap 0/1 edge) on both parts.
//
// TapBank::process (fx/taps.h) skips a tap's read the instant its offset is
// tap_tuning::kMuted OR its gain is 0. A part's offsets are placed by the
// OTHER part's PITCH-lane rhythm (fx/taps.cpp's derive_offsets, cross-fed at
// Instrument's control tick, instrument.cpp), and that rhythm is not valid
// until its lane has recorded 3 onsets AND wrapped a 4th time --
// SuperModulator::process latches `valid = onsets>=3` BEFORE folding in the
// current wrap's own onset (mod/super_modulator.cpp), so the 3rd onset only
// becomes visible one wrap later. Both parts boot in FLOW (neither
// setup_inst_common nor setup_inst_worst calls set_step), where a wrap IS an
// onset, at the RATE 0.8 setup_inst_worst already sets: free_hz(0.8) is
// ~6.95 Hz, a ~6907-sample cycle, so validity needs ~4 * 6907 ~= 27600
// samples -- an order of magnitude past the runner's fixed 100-block
// (9600-sample) warm-up (workload.h), which is shared across every row and
// not this row's to change. So this setup runs its own extra process()
// blocks first: it polls Instrument::rhythm() on both parts and re-derives
// the offsets with the exact same pure function instrument.cpp itself calls
// (derive_offsets), stopping only once BOTH parts' derived offsets are
// non-muted -- established, not assumed -- then runs a further fixed margin
// so the control tick and TapBank's own re-latch dip (each a mute->live
// transition, answered by an immediate dip-out + dip-in of kDipSeconds each,
// tap_tuning.h) have cleared before returning.
//
// Verified against the real engine on the desktop host (this exact setup,
// outside the bench toolchain, not committed): offsets go live at block 287
// (sample 27648), matching the estimate above, and an FNV checksum folded
// the same way runner.cpp does, over the runner's own warm-up+measure window
// (1100 blocks) starting from this setup, differs from the identical row
// with DUST/ROT left at 0. See .superpowers/sdd/worst-taps-workload.md.
void setup_inst_worst_taps()
{
    setup_inst_worst();
    for (int p = 0; p < PART_COUNT; ++p) {
        g_inst.set_dust(p, 1.f);   // both taps at full gain
        g_inst.set_rot(p, 0.5f);   // mid spread -- neither one-pole collapses
    }

    const float* in = test_input();
    constexpr int32_t kTapeLen = static_cast<int32_t>(Flux::kMaxSamples);
    // Generous safety cap: ~7x the ~288 blocks this configuration actually
    // needs (verified above). If this cap is ever hit -- a future change to
    // RATE, STEP mode, or the onset-gap ring's arithmetic -- the loop simply
    // gives up with the taps still muted, which the checksum guard this row
    // exists for (see the design doc) is what catches: a silently identical
    // checksum to instrument_worst means this cap was hit and the row needs
    // re-deriving, not trusting.
    constexpr int kSafetyBlocks = 2000;
    bool ready = false;
    for (int b = 0; b < kSafetyBlocks && !ready; ++b) {
        g_inst.process(in, in, g_out_l, g_out_r, kBlock);
        const RhythmView& ra = g_inst.rhythm(PART_A);
        const RhythmView& rb = g_inst.rhythm(PART_B);
        if (ra.valid && rb.valid) {
            int32_t off_a[tap_tuning::kTaps], off_b[tap_tuning::kTaps];
            derive_offsets(rb, kTapeLen, off_a);   // A's taps read B's rhythm
            derive_offsets(ra, kTapeLen, off_b);   // B's taps read A's rhythm
            const bool a_live = off_a[0] != tap_tuning::kMuted || off_a[1] != tap_tuning::kMuted;
            const bool b_live = off_b[0] != tap_tuning::kMuted || off_b[1] != tap_tuning::kMuted;
            ready = a_live && b_live;
        }
    }
    // Settle margin: the control tick that pushes the now-valid offsets into
    // each TapBank runs at the start of the next 96-sample block
    // (Center::kCtrlInterval == kBlock), and TapBank::set_offsets answers
    // the mute->live transition with an immediate dip-out + dip-in (2 x
    // kDipSeconds = 2 x 96 samples @ 48 kHz) before the first live read. 50
    // blocks (4800 samples) is >15x that ~288-sample worst case.
    for (int b = 0; b < 50; ++b) g_inst.process(in, in, g_out_l, g_out_r, kBlock);
}

} // namespace

const Workload kCoreWorkloads[] = {
    { "system", "empty_callback",     setup_empty,     proc_empty   },
    { "system", "mod_plane_2x_center",setup_mod,       proc_mod     },
    { "system", "synth_1_voice",      setup_synth_1,   proc_synth   },
    { "system", "synth_2_voices",     setup_synth_2,   proc_synth   },
    { "system", "synth_4_voices",     setup_synth_4,   proc_synth   },
    { "system", "fx_none",            setup_fx_none,   proc_fx      },
    { "system", "fx_grit",            setup_fx_grit,   proc_fx      },
    { "system", "fx_flux_sdram",      setup_fx_flux,   proc_fx      },
    { "system", "fx_comp",            setup_fx_comp,   proc_fx      },
    { "system", "oliverb_solo_sram",  setup_reverb,    proc_reverb  },
    { "system", "instrument_init",    setup_inst_init, proc_inst    },
    { "system", "instrument_worst",   setup_inst_worst,proc_inst    },
    { "system", "instrument_worst_taps", setup_inst_worst_taps, proc_inst },
};
const int kCoreCount = sizeof(kCoreWorkloads) / sizeof(kCoreWorkloads[0]);

} // namespace bench
