#include "workload.h"
#include "mem.h"
#include "sampler/sampler_engine.h"
#include "instrument.h"
#include <cmath>

namespace bench {
namespace {

using namespace spky;

// The M5 texture deck, priced on the part it has to run on.
//
// Until now the only hardware figure standing in for it was family 3's
// grain-read proxy (workloads_memory.cpp) -- eight scattered interpolated
// stereo reads per sample, written when "a sampler existed yet" was still
// false. The rows here replace the standing-in: same access pattern, but
// produced by SamplerEngine itself, with its scheduler, its window, its
// filter and its overlap normalization in the price.
//
// The other number these rows exist to correct is the desktop wall-clock
// pair quoted in SamplerEngine's kOverlap comment. That comment says plainly
// what it cannot see: "the Daisy's real limit at that density is SDRAM
// traffic from interpolated reads scattered across a ~32 MB record buffer,
// defeating the cache in a way a desktop CPU with megabytes of cache does
// not feel at all." sampler_flow_worst is that case measured instead of
// argued.

// --- source material --------------------------------------------------------
// load_sample() copies from two float arrays, so the 8 MB SDRAM arena doubles
// as the source: L reads from the front, R from an offset view, which leaves
// the two channels decorrelated without a second buffer. The offset is what
// makes both windows land inside kSdramFloats exactly.
//
// Clobbering the arena is safe because the sampler family runs LAST in
// main.cpp -- family 3 and the taps rows are long done with it -- and every
// setup below refills whatever it is about to read.
constexpr size_t kSrcOffset = kSdramFloats - kSamplerFrames;   // 81 152
static_assert(kSdramFloats >= kSamplerFrames + 1, "source arena too small");

void fill_source()
{
    float* src = sdram_arena();
    // Seeded LCG through a slow sine, so the material is broadband (the
    // interpolated read has something to interpolate) but not noise the
    // filter would sit on. Deterministic: this feeds the checksum.
    uint32_t s = 7717u;
    for (size_t i = 0; i < kSdramFloats; ++i) {
        s = s * 1664525u + 1013904223u;
        const float n = (static_cast<float>(s >> 8) / 8388608.f) - 1.f;
        src[i] = 0.6f * sinf(static_cast<float>(i) * 0.0013f) + 0.4f * n;
    }
}

// --- the solo engine --------------------------------------------------------
SamplerEngine g_s;
int           g_scan_ctr = 0;
bool          g_recording = false;

// Worst case, and every part of it is reachable from the panel:
//   DENS 1     -> overlap 8, the ceiling kOverlapMax. Measured on the desktop
//                 probe: ~7.7 grains live on average, peaks at 12-14 of 16.
//   SIZE 0.05  -> 128-sample grains, spawning every 16 samples. Both ends of
//                 the cost at once: the highest spawn rate any SIZE reaches
//                 (0.10 gives 42, 0.20 gives 301, 0.50 gives 1200) AND the
//                 shortest read run per grain, so the SDRAM sees a fresh
//                 random start ~3000 times a second instead of following a
//                 run. It is a buzz, not a texture -- that is what a worst
//                 case sounds like.
//   MOTION 1   -> kScatterPosFrac = 1.0, so spawn positions scatter over the
//                 WHOLE 42 s of content. This is the row's real subject.
//   SCAN 0.5   -> the playhead runs, so the scatter centre moves too and no
//                 region of the buffer stays warm.
//   FILT/RES   -> the Svf pair off its cheap rails.
void patch_worst(SamplerEngine& s)
{
    const float t[LANE_COUNT] = {
        0.5f,    // SOURCE  -- ORGANIZE, centre; SCAN moves off it anyway
        0.05f,   // SIZE
        0.5f,    // PITCH   -- unity, so read stride is 1.0 and cannot flatter
                 //            the cache with a short stride
        1.f,     // MOTION
        0.8f,    // LEVEL
    };
    s.set_targets(t, 0.5f);
    s.set_overlap(1.f);
    s.set_scan(0.5f);
    s.set_window_attack(0.3f);
    s.set_window_decay(0.3f);
    s.set_filt(0.4f);
    s.set_resonance(0.6f);
    s.set_flow(true);
}

// The musical end of the same deck: a standing cloud at DENS ~4, half-second
// grains, MOTION half up, playhead parked. ~4.1 grains live.
void patch_typ(SamplerEngine& s)
{
    const float t[LANE_COUNT] = { 0.5f, 0.35f, 0.5f, 0.5f, 0.8f };
    s.set_targets(t, 0.5f);
    s.set_overlap(0.45f);
    s.set_scan(0.f);
    s.set_window_attack(0.3f);
    s.set_window_decay(0.3f);
    s.set_filt(0.f);
    s.set_resonance(0.15f);
    s.set_flow(true);
}

// Content, then settle. The settle matters: _norm smooths 1/sqrt(active) over
// 10 ms and the spawn schedule needs a moment to fill the pool, so a cold
// engine measures a cloud that is still assembling itself. 20 000 samples is
// well past both.
void settle(int n = 20000)
{
    float l, r;
    for (int i = 0; i < n; ++i) g_s.process(l, r);
}

void setup_solo(SampleBuffer::Frame* buf, size_t frames, bool worst)
{
    fill_source();
    g_s.set_seed(0x5A11E20Du);
    g_s.set_memory(buf, frames);
    g_s.init(kSampleRate);
    g_s.load_sample(sdram_arena(), sdram_arena() + kSrcOffset, frames);
    if (worst) patch_worst(g_s); else patch_typ(g_s);
    g_scan_ctr  = 0;
    g_recording = false;
    settle();
}

void setup_flow_worst() { setup_solo(sampler_arena(0), kSamplerFrames, true);  }
void setup_flow_typ()   { setup_solo(sampler_arena(0), kSamplerFrames, false); }

// --- the region contrast, with the real engine ------------------------------
// grain_read_sram / grain_read_sdram (family 3) run one hand-written pattern
// over two regions. These two run the ENGINE over two regions, at identical
// settings and identical content length, so the ratio is still the region's
// factor and nothing else -- but now with the scheduler, window and filter
// included, which is what dilutes it: those costs are region-blind, so the
// engine ratio is expected to come out well below the proxy's.
//
// 8192 frames is the SRAM arena's exact capacity (16 384 floats = 65 536 B =
// 8192 x 8 B frames). The SDRAM twin takes the first 8192 frames of part 0's
// record buffer rather than the arena, because the arena is the load source.
constexpr size_t kWinFrames = kSramFloats / 2;
static_assert(kWinFrames * sizeof(SampleBuffer::Frame) == kSramFloats * sizeof(float),
              "the SRAM window must be the arena exactly");

void setup_win_sram()
{
    setup_solo(reinterpret_cast<SampleBuffer::Frame*>(sram_arena()), kWinFrames, true);
}
void setup_win_sdram()
{
    setup_solo(sampler_arena(0), kWinFrames, true);
}

// --- overdub under the cloud ------------------------------------------------
// The deck's genuine worst case, because it is the one setting where the
// per-sample write path runs at the same time as the reads: SampleBuffer::write
// does a read-modify-write of the frame under the head with a feedback
// multiply, and above kFbSatKnee a fast_tanh on top. Feedback is set to 0.95
// -- the shipping default knob position, which the post-knee curve maps to
// ~0.817, i.e. below kFbSatKnee, so this row prices the ordinary overdub and
// not the saturating one.
void setup_overdub_worst()
{
    setup_flow_worst();
    g_s.set_feedback(sampler_cfg::kDefaultFeedback);
    g_s.set_recording(true);   // punches in at frame 0, over live content
    g_recording = true;
    settle(4000);              // past kRecordFade and into sustain
}

// --- SCAN's control-rate pow ------------------------------------------------
// SamplerEngine::set_scan calls std::pow, and its header comment names the
// caller that matters: the VCV host drives it every 16 samples, six times
// more often than the engine's own 96-sample control tick. "Measured
// affordable at that rate" was a desktop measurement. This is that rate on
// the Daisy, against sampler_flow_worst as the baseline -- the delta is the
// pow.
void setup_scan_ctrl() { setup_flow_worst(); }

float proc_solo()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l = 0.f, r = 0.f;
        g_s.process(l, r);
        acc += l + r;
    }
    // Same guard the instrument rows use for voice count: a cloud that fails
    // to spawn -- empty buffer, engine not in FLOW -- would otherwise be a
    // cheap row that looks like good news. Folding the live grain count into
    // the checksum makes that failure move the checksum instead.
    acc += static_cast<float>(g_s.active_grains());
    return acc;
}

float proc_solo_in()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l = 0.f, r = 0.f;
        g_s.process_in(in[i], in[i] * 0.9f);
        g_s.process(l, r);
        acc += l + r;
    }
    acc += static_cast<float>(g_s.active_grains());
    return acc;
}

float proc_solo_scan()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        if (++g_scan_ctr >= 16) {
            g_scan_ctr = 0;
            // A moving value, not a constant: set_scan has no early-out on an
            // unchanged argument, but a constant would still let the branch
            // predictor and the pow's own path settle into one case.
            acc += 0.f;
            g_s.set_scan(0.4f + 0.2f * sinf(static_cast<float>(i) * 0.07f));
        }
        float l = 0.f, r = 0.f;
        g_s.process(l, r);
        acc += l + r;
    }
    acc += static_cast<float>(g_s.active_grains());
    return acc;
}

// --- the whole instrument, both parts on the sampler ------------------------
// The counterpart to instrument_worst, which is the same box with both parts
// on the SYNTH. Everything downstream is identical -- GRIT, FLUX, COMP, the
// SRAM reverb, the driven limiter -- so the difference between the two rows
// is exactly what swapping the engine costs, and inst_sampler_worst itself is
// the answer to "does a two-part texture deck fit in a block."
//
// Own Instrument and own retrigger counter, per the trap workloads_system.cpp
// and workloads_abl.cpp both document: phase inherited across rows is state,
// and shared state between families is how a checksum moves for no visible
// reason.
Instrument g_inst;
int        g_inst_ctr = 0;
float      g_out_l[kBlock];
float      g_out_r[kBlock];

void setup_inst_sampler_worst()
{
    fill_source();
    g_inst.init(kSampleRate, fx_mem());
    g_inst.set_tempo_bpm(120.f);
    g_inst_ctr = 0;
    for (int p = 0; p < PART_COUNT; ++p) {
        g_inst.set_engine(p, ENGINE_SAMPLER);
        // Both parts load the same source material. Their read positions do
        // not coincide: MOTION scatter is drawn from each part's own RNG,
        // seeded off Part::init's per-part seed_base.
        g_inst.load_sample(p, sdram_arena(), sdram_arena() + kSrcOffset, kSamplerFrames);
        // The sampler's own worst case, reached through the part's controls.
        g_inst.set_target_base(p, LANE_SIZE, 0.05f);
        g_inst.set_target_base(p, LANE_MOTION, 1.f);
        g_inst.sampler_overlap(p, 1.f);
        g_inst.sampler_scan(p, 0.5f);
        // ...and instrument_worst's surroundings, unchanged.
        g_inst.set_color(p, 1.f);
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

    // Past the 4 ms engine-swap SoftSwitch (set_engine fades out, swaps and
    // fades back in inside process()) and past the clouds assembling. Without
    // this the row would measure part of a crossfade.
    const float* in = test_input();
    for (int i = 0; i < 200; ++i)
        g_inst.process(in, in, g_out_l, g_out_r, kBlock);
}

float proc_inst_sampler()
{
    const float* in = test_input();
    g_inst.process(in, in, g_out_l, g_out_r, kBlock);
    if (++g_inst_ctr >= 250) {
        g_inst_ctr = 0;
        g_inst.trigger_manual(PART_A);
        g_inst.trigger_manual(PART_B);
    }
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_out_l[i] + g_out_r[i];
    // active_voices() reads 0 on a sampler part by construction (Part::
    // active_voices is synth-only), so the grain counts are the guard here.
    acc += static_cast<float>(g_inst.sampler_grains(PART_A));
    acc += static_cast<float>(g_inst.sampler_grains(PART_B));
    return acc;
}

} // namespace

// Appended in one block at the end of the sweep, and the order inside it is
// deliberate: the two window rows sit next to each other so their ratio is
// read off adjacent lines, and the instrument row goes last because it is the
// only one that leaves an Instrument warm.
const Workload kSamplerWorkloads[] = {
    { "sampler", "sampler_flow_typ",     setup_flow_typ,           proc_solo         },
    { "sampler", "sampler_flow_worst",   setup_flow_worst,         proc_solo         },
    { "sampler", "sampler_overdub_worst",setup_overdub_worst,      proc_solo_in      },
    { "sampler", "sampler_scan_ctrl",    setup_scan_ctrl,          proc_solo_scan    },
    { "sampler", "sampler_win_sram",     setup_win_sram,           proc_solo         },
    { "sampler", "sampler_win_sdram",    setup_win_sdram,          proc_solo         },
    { "sampler", "inst_sampler_worst",   setup_inst_sampler_worst, proc_inst_sampler },
};
const int kSamplerCount = sizeof(kSamplerWorkloads) / sizeof(kSamplerWorkloads[0]);

} // namespace bench
