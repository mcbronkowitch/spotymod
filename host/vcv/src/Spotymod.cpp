#include <cmath>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstdlib>
#include <osdialog.h>
#include "plugin.hpp"
#include "generated_panel.hpp"   // enums + control table (generated from res/gen_panel.py)

// The portable engine core -- exactly the same headers the desktop render host
// and (later) the Daisy firmware use. No hardware type crosses this boundary.
#include "instrument.h"
#include "fx/flux.h"
#include "mod/divisions.h"
#include "sampler_ui.hpp"

using namespace spkyvcv;

// RATE tooltip: the division name while SYNC is on, free Hz otherwise.
struct RateQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        if (module && module->params[SYNC].getValue() > 0.5f)
            return spky::kDivisions[spky::division_index(getValue())].name;
        return string::f("%.3f Hz", spky::free_hz(getValue()));
    }
};

// TIDE tooltip: the ratio-ladder rung while SYNC is on, the free multiplier
// otherwise (same table the engine snaps to, mod/divisions.h).
struct TideQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        if (module && module->params[SYNC].getValue() > 0.5f)
            return spky::kTideNames[spky::tide_index(getValue())];
        return string::f("x%.2f", spky::tide_free(getValue()));
    }
};

// FLUX RATE tooltip: the synced division name (always synced).
struct FluxRateQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        int k = spky::kFluxRateOffset + spky::flux_division_index(getValue());
        return spky::kDivisions[k].name;
    }
};

// FLUX FB tooltip: percent, reaching >100% into the tanh bloom.
struct FluxFbQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        return string::f("%.0f%%", getValue() * 120.f);
    }
};

// ROT tooltip: how far apart the two taps are spread spectrally. 0 = both
// filters open and the taps read as a plain two-tap delay; 1 = tap 0 dark,
// tap 1 bright, which is what stops them sounding like the echo.
struct RotQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        return string::f("SPREAD %.0f%%", getValue() * 100.f);
    }
};

// DUST tooltip: the tap morph. Tap 0 fades in over the first half of the
// knob, tap 1 over the second, so the middle is an accent hierarchy.
struct DustQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        const float d = getValue();
        if (d <= 0.f) return "OFF";
        if (d < 0.5f) return string::f("1 TAP %.0f%%", d * 200.f);
        return string::f("2 TAPS %.0f%%", (d - 0.5f) * 200.f);
    }
};

// Part A params occupy [0..PART_STRIDE), part B the next PART_STRIDE. The
// generator lays both part blocks out identically (same order, mirrored x) and
// emits PART_STRIDE; these guards catch any drift.
static_assert(RATE_B == RATE_A + PART_STRIDE, "part-block stride drifted from generator");
static_assert(TUNE_B == TUNE_A + PART_STRIDE, "part-block stride drifted from generator");
static_assert(TRIGGER_B == TRIGGER_A + PART_STRIDE, "part-block stride drifted from generator");

struct Spotymod : Module {
    spky::Instrument inst;
    spky::FxMem fxmem;

    // FX memory the engine's "no heap" contract requires the host to own.
    // ~3.8 MB of echo buffer + one ~130 KB reverb, held per module instance.
    float echo[spky::PART_COUNT][2][spky::Flux::kMaxSamples];
    spky::AmbientReverb reverb;

    // The texture deck's record buffers. Unlike the echo/reverb memory above
    // these are NOT held by value: 42 s of stereo at 48 kHz is ~16 MB per
    // part. The engine's "no heap" contract binds engine/, not the host --
    // hosts allocate (desktop: std::vector, M6 firmware: SDRAM).
    static constexpr double kSamplerBufferSeconds = 42.0;
    std::vector<spky::SampleBuffer::Frame> samplerMem[spky::PART_COUNT];
    spkyvcv::SamplerPartState smp[spky::PART_COUNT];

    // First-use factory sample (Task 8): flipping ENG to Sampler on an empty
    // part autoloads res/factory.wav so the deck sounds within one gesture.
    // factoryTried[p] stops a failed load or a deliberate Clear from
    // re-triggering on the next control tick -- only onReset() clears it.
    //
    // The WAV itself must never be read from disk on the audio thread, so the
    // read is split from the decision: factoryNative holds the file decoded
    // at its OWN (native) sample rate, read at most once per module instance
    // in onAdd() (main thread, before process() ever runs for this
    // instance -- see onAdd() below). factoryL/factoryR hold that same audio
    // resampled to curSr, rebuilt in reinit() every time the engine rate
    // changes (so a device-rate change between part A's and part B's first
    // flip can't leave part B with audio pitched for the old rate).
    // pushParams (audio thread) only ever reads factoryL/factoryR -- a
    // memcpy via inst.load_sample, no disk I/O and no resample.
    bool factoryTried[spky::PART_COUNT] = {false, false};
    spky::WavData factoryNative;
    bool factoryNativeTried = false;
    std::vector<float> factoryL, factoryR;

    float curSr = 0.f;
    dsp::ClockDivider ctrlDiv;              // throttle param push to control rate
    dsp::SchmittTrigger clockTrig, resetTrig;
    dsp::BooleanTrigger triggerTrig[2], spotTrig, settleTrig;
    dsp::BooleanTrigger principleTrig[2], newPhraseTrig[2];
    int principleIdx[2] = {0, 0};   // current principle per part (0=TwoMotif)
    float clkSamples = 0.f;                 // samples since last external clock edge
    float gateFilt[2] = {0.f, 0.f};
    float recPhase[2] = {0.f, 0.f};        // REC LED pulse while recording
    std::atomic<bool> resyncReq { false };  // menu "Resync to bar" -> audio thread
    bool pendingRestore = false;    // dataFromJson ran before onAdd; content reload deferred

    Spotymod() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configControls();
        for (int p = 0; p < spky::PART_COUNT; ++p)
            for (int c = 0; c < 2; ++c) fxmem.echo[p][c] = echo[p][c];
        fxmem.reverb = &reverb;
        ctrlDiv.setDivision(16);
    }

    void configControls() {
        for (const auto& c : kParamCtls) {
            const std::string lbl = c.label;
            switch (c.kind) {
                case WK_BIGKNOB:
                case WK_SMKNOB:
                    if (c.id == RATE_A || c.id == RATE_B)
                        configParam<RateQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    else if (c.id == CHOKE) {  // event-priority, 5 snapped states
                        configSwitch(c.id, -2.f, 2.f, 0.f, lbl,
                                     {"A chokes B thru decay", "A chokes B while playing",
                                      "Off", "B chokes A while playing", "B chokes A thru decay"});
                        getParamQuantity(c.id)->snapEnabled = true;
                    }
                    else if (c.id == FILT_A || c.id == FILT_B)  // bipolar cutoff trim
                        configParam(c.id, -1.f, 1.f,
                                    c.id == FILT_A ? -0.061f : -0.230f, lbl);
                    else if (c.id == TIDE)  // texture-lane rate, snaps under SYNC
                        configParam<TideQuantity>(c.id, 0.f, 1.f, 0.5f, lbl);
                    else if (c.id == FLUXRATE_A || c.id == FLUXRATE_B)
                        configParam<FluxRateQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    else if (c.id == FLUXFB_A || c.id == FLUXFB_B)
                        configParam<FluxFbQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    else if (c.id == DUST_A || c.id == DUST_B)
                        configParam<DustQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    else if (c.id == ROT_A || c.id == ROT_B)
                        configParam<RotQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    else
                        configParam(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    break;
                case WK_KNOBC:  // MELO (bipolar): both decks loop — A drifts a little, B is frozen
                    configParam(c.id, -1.f, 1.f,
                                c.id == MELODY_A ? -0.728f : -1.f, lbl); break;
                case WK_KNOBI:
                    if (c.id == SCALE)  // init patch is Lydian — the bright end of the sweep
                        configParam(c.id, 0.f, (float)(spky::SCALE_LIST_COUNT - 1),
                                    (float)spky::SCALE_LYDIAN, "Scale");
                    else  // STEPS_A / STEPS_B
                        configParam(c.id, 2.f, 16.f, 8.f, "Steps");
                    getParamQuantity(c.id)->snapEnabled = true;
                    break;
                case WK_SW2:  // init patch runs the instrument on the grid
                    configSwitch(c.id, 0.f, 1.f, 1.f, "Sync", {"Free", "Synced"});
                    break;
                case WK_LATCH:
                    if (c.id == REC_A || c.id == REC_B)
                        configSwitch(c.id, 0.f, 1.f, 0.f, "Record",
                                     {"Stopped", "Recording"});
                    else if (c.id == ENGINE_A || c.id == ENGINE_B)
                        configSwitch(c.id, 0.f, 1.f, 0.f, "Engine", {"Synth", "Sampler"});
                    else if (c.id == GRITMODE_A || c.id == GRITMODE_B)
                        configSwitch(c.id, 0.f, 1.f, c.id == GRITMODE_A ? 1.f : 0.f,
                                     "Grit mode", {"Drive", "Reduce"});   // init: A=Reduce
                    else  // STEP (on for the init patch's stepped sequences) / PRINCIPLE / NEWPHRASE
                        configSwitch(c.id, 0.f, 1.f,
                                     (c.id == STEP_A || c.id == STEP_B) ? 1.f : 0.f,
                                     lbl, {"Off", "On"});
                    break;
                case WK_SMBTN: configButton(c.id, lbl); break;
                default: break;
            }
        }
        // panel labels are short ("L", "PIT"); the group legend carries the rest,
        // so tooltips use the control table's spelled-out tip instead
        for (const auto& c : kInputCtls)  configInput(c.id, c.tip);
        for (const auto& c : kOutputCtls) configOutput(c.id, c.tip);
    }

    // Init "patch" (Rack Initialize / fresh instance): part A = a slow chord
    // pad (COLOR up, the chord layer sounding from the first note), part B = a
    // short plucked bass under it, both in Lydian. Knob values are a snapshot
    // of a hand-dialled panel state (cpu.vcvm, 2026-07-19, superseding the
    // 2026-07-18 chord_init snapshot): MORPH left of centre so both decks are
    // audible, COUPLE/DRIFT full, both MELO knobs negative so the progression
    // loops instead of wandering off, both FLUX lanes well up.
    // Only knob params (WK_BIGKNOB/WK_SMKNOB) come through here;
    // STEP/SYNC/STEPS/SCALE/MELO/FILT defaults live in configControls().
    // RATE values are expressed on the 17-rung ladder introduced 2026-07-16
    // (0.0625 = 4 bars, 0.5 = 1/4 — the rungs the snapshot lands on).
    static float defaultFor(int id) {
        switch (id) {                       // global knobs (cpu snapshot 2026-07-19)
            case MORPH:        return 0.312f;  // left of centre — pad forward, bass under it
            case COUPLE:       return 1.00f;   // full = hard loop lock
            case DRIFT:        return 1.00f;
            case MASTER_DRIVE: return 0.619f;
            case REV_SIZE:     return 0.851f;
            case REV_DECAY:    return 0.851f;
            case REV_TONE:     return 0.803f;
            case REV_DIFF:     return 0.863f;
            case REV_MIX:      return 0.410f;  // behind the parts — the chord already fills the width
            case REV_SMEAR:    return 0.568f;  // diffuser LFO smear (wash)
            case REV_MOD:      return 0.237f;  // tail LFO wobble
            case TEMPO:        return 0.00f;   // as saved (40 BPM floor; parts run Synced)
            case FLUXRATE_A:   return 3.f / 11.f;   // "1/4" for part A's pad echo
            case FLUXRATE_B:   return 4.f / 11.f;   // "1/8." for part B's bass echo
            case FLUXFB_A:     return 0.563f;
            case FLUXFB_B:     return 0.354f;
            case COLOR_A:      return 0.647f;  // pad blooms into a seventh/ninth stack
            case COLOR_B:      return 0.f;     // bass stays single notes
            case DUST_A:       return 0.f;     // DUST off: bit-exact with the
            case DUST_B:       return 0.f;     // pre-DUST init patch
            case ROT_A:        return 0.f;     // zone S, fully grid-locked
            case ROT_B:        return 0.f;
            default: break;
        }
        const int part = id / PART_STRIDE;  // 0 = A (chord pad), 1 = B (bass)
        switch (id % PART_STRIDE) {         // fold part B onto the *_A enum; part ? B : A
            case RATE_A:   return part ? 8.f / 16.f : 0.0625f;  // B "1/4", A "4 bars"
            case SHAPE_A:  return part ? 0.600f : 0.616f;
            case DENSITY_A: return part ? 0.370f : 0.345f;
            case SMOOTH_A: return part ? 0.300f : 1.000f;  // A fully smoothed = the pad breathes
            case RANGE_A:  return part ? 0.236f : 0.679f;  // melody ambitus
            case MOD_A:    return part ? 0.790f : 0.593f;  // texture depth
            case TUNE_A:   return part ? 0.000f : 0.550f;  // B down an octave-ish
            case ATTACK_A: return part ? 0.000f : 0.657f;  // B snaps in, A swells
            case DECAY_A:  return part ? 0.268f : 0.902f;  // A rings/stacks; B plucks short
            case RES_A:    return part ? 0.379f : 0.259f;
            case SUB_A:    return part ? 0.663f : 0.350f;  // weight under the bass
            case DETUNE_A: return part ? 0.088f : 0.773f;  // A wide, B nearly clean
            case FLUX_A:   return part ? 0.736f : 0.793f;  // both echo lanes well up
            case GRIT_A:   return part ? 0.113f : 0.000f;  // B light grit; A off
            case COMP_A:   return part ? 0.547f : 0.363f;
            default: break;
        }
        return 0.5f;
    }

    // Re-init the engine for a new sample rate. Without the snapshot below,
    // every rate change silently discarded a loaded or recorded sample (an
    // M5a finding). Two things destroy it: the buffers are sized in FRAMES so
    // a rate change resizes them, and inst.init() ends in SampleBuffer::clear()
    // which memsets the whole buffer. So the content must be copied OUT into
    // separate storage first and pushed back afterwards -- reading it out of
    // the buffer after init() would read zeroes.
    //
    // Up to 16 MB per part, twice, but only when the user changes their audio
    // device, and onSampleRateChange runs on the main thread with the engine
    // paused. The snapshot is NOT resampled -- it plays transposed at the new
    // rate. That is varispeed, and it is the instrument's idiom. (File LOADS
    // do resample, see sampler_ui.hpp: importing at the wrong pitch is a bug,
    // re-rating material already in the buffer is tape.)
    void reinit(float sr) {
        std::vector<float> snapL[spky::PART_COUNT], snapR[spky::PART_COUNT];
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            const size_t n = inst.sampler_rec_size(p);
            const spky::SampleBuffer::Frame* f = inst.sampler_data(p);
            if (!n || !f) continue;
            snapL[p].resize(n);
            snapR[p].resize(n);
            for (size_t i = 0; i < n; ++i) { snapL[p][i] = f[i].l; snapR[p][i] = f[i].r; }
        }

        const float prevSr = curSr;
        curSr = sr;
        const size_t frames = (size_t)(kSamplerBufferSeconds * (double)sr);
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            if (samplerMem[p].size() != frames) samplerMem[p].resize(frames);
            fxmem.sampler_buf[p] = samplerMem[p].data();
        }
        fxmem.sampler_frames = frames;

        inst.init(sr, fxmem);

        for (int p = 0; p < spky::PART_COUNT; ++p)
            if (!snapL[p].empty())
                inst.load_sample(p, snapL[p].data(), snapR[p].data(), snapL[p].size());

        // Rebuild the factory-drone cache for the (possibly new) rate.
        // factoryNative is decoded from disk exactly once, in onAdd() --
        // this only resamples the already-decoded buffer, so reinit() stays
        // disk-free even when it's called from process()'s reactive
        // sample-rate-change fallback. Skipped when the rate hasn't actually
        // changed and the cache is already built, so calling reinit() twice
        // in a row at the same rate (documented above, and onReset() does
        // exactly this) doesn't redo the resample for nothing.
        if (!factoryNative.l.empty() && (sr != prevSr || factoryL.empty())) {
            factoryL = factoryNative.l;
            factoryR = factoryNative.r;
            if (factoryNative.sample_rate > 0
                && (float)factoryNative.sample_rate != sr) {
                const double ratio = (double)sr / (double)factoryNative.sample_rate;
                spkyvcv::resample_linear(factoryL, ratio);
                spkyvcv::resample_linear(factoryR, ratio);
            }
        }
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        reinit(e.sampleRate);
    }

    // Read a per-part param: baseId is the PART A enum, part in {0,1}.
    inline float pp(int baseA, int part) {
        return params[baseA + part * PART_STRIDE].getValue();
    }
    inline bool ppb(int baseA, int part) { return pp(baseA, part) > 0.5f; }

    void pushParams() {
        for (int p = 0; p < 2; ++p) {
            inst.set_rate(p, pp(RATE_A, p));
            inst.set_shape(p, pp(SHAPE_A, p));
            inst.set_density(p, pp(DENSITY_A, p));
            inst.set_smooth(p, pp(SMOOTH_A, p));
            inst.set_range(p, pp(RANGE_A, p));
            inst.set_variation(p, pp(MELODY_A, p));          // -1..+1 RENEW<-LOOP->GROW
            inst.set_depth(p, pp(MOD_A, p));
            inst.set_tune(p, pp(TUNE_A, p));

            inst.set_voice_attack(p, pp(ATTACK_A, p));
            inst.set_voice_decay(p, pp(DECAY_A, p));
            inst.set_voice_resonance(p, pp(RES_A, p));
            inst.set_voice_filt(p, params[p ? FILT_B : FILT_A].getValue());
            inst.set_color(p, params[p ? COLOR_B : COLOR_A].getValue());
            inst.set_voice_sub(p, pp(SUB_A, p));
            inst.set_voice_detune(p, pp(DETUNE_A, p));

            inst.set_flux_mix(p, pp(FLUX_A, p));
            inst.set_flux_rate(p, spky::flux_division_index(
                params[p ? FLUXRATE_B : FLUXRATE_A].getValue()));
            inst.set_fx_target_base(p, spky::FXT_FLUX_FB,
                params[p ? FLUXFB_B : FLUXFB_A].getValue());
            inst.set_grit_mix(p, pp(GRIT_A, p));
            // Appended params are outside the stride, so pp() would compute the
            // wrong id — the explicit ternary is required (see FLUXRATE/FLUXFB).
            inst.set_dust(p, params[p ? DUST_B : DUST_A].getValue());
            inst.set_rot(p, params[p ? ROT_B : ROT_A].getValue());
            // The FX blocks are gated by an explicit on/off (a pad on hardware,
            // a scenario action on the desktop). VCV has no such pad, so the mix
            // knob doubles as the on switch: knob up == engaged. At 0 the block
            // stays idle and the whole chain is skipped (bit-exact bypass).
            inst.set_fx_on(p, spky::FxBlock::Flux, pp(FLUX_A, p) > 1e-4f);
            inst.set_fx_on(p, spky::FxBlock::Grit, pp(GRIT_A, p) > 1e-4f);
            inst.set_comp(p, pp(COMP_A, p));

            // ENG picks Synth or Sampler. The test tone survives as a dev tool
            // in the context menu: with testTone set, ENG's second position
            // plays it instead of the sampler. A patch saved before M5b that
            // had "test tone" selected therefore opens as sampler -- accepted
            // (spec 2026-07-18 "VCV layer"), no real patch uses it.
            const bool eng2 = ppb(ENGINE_A, p);
            inst.set_engine(p, !eng2 ? spky::ENGINE_SYNTH
                                     : (smp[p].testTone ? spky::ENGINE_TEST_TONE
                                                        : spky::ENGINE_SAMPLER));

            // First-user experience: flipping ENG to Sampler on an empty part
            // loads the factory drone, so one pad press makes sound. It never
            // overwrites content -- sampler_empty() is the whole guard, and
            // once loaded it behaves like any other sample (REC overdubs it,
            // Clear clears it, and factoryLoaded keeps it out of patch
            // storage).
            // factoryL/factoryR are prepared off the audio thread (onAdd()
            // reads+decodes, reinit() resamples -- see the members' comment
            // above). load_sample() itself still begins with SampleBuffer::
            // clear(), which used to memset the WHOLE 42 s allocation (16.1
            // MB) unconditionally -- 1.5-3 ms inside one process() call
            // against a 5.3 ms budget at a 256-sample block, worse at 128/64
            // (I-3). This branch only ever fires when sampler_empty(p) is
            // true, i.e. the buffer's _size is already 0, so with
            // SampleBuffer::clear()'s _size==0 fast path (sample_buffer.cpp)
            // that memset is skipped here: what actually runs is the guard
            // check above plus a ~3.4 MB memcpy of the factory sample.
            if (eng2 && !smp[p].testTone && inst.sampler_empty(p)
                     && !factoryTried[p]) {
                factoryTried[p] = true;
                if (!factoryL.empty()) {
                    inst.load_sample(p, factoryL.data(), factoryR.data(),
                                     factoryL.size());
                    smp[p].factoryLoaded = true;
                }
            }

            inst.sampler_speed_mode(p, smp[p].tapeIdx != 0);
            inst.sampler_reverse(p, smp[p].reverse);
            inst.sampler_feedback(p, smp[p].feedback);

            // REC is a latch, so its value IS the desired state -- an edge
            // trigger would miss a state restored from a saved patch. The
            // engine's set_recording is idempotent, and sampler_record flips
            // monitoring with it, so pushing every control tick is correct.
            // On a synth part REC is inert: ENG is the only mode selector.
            const bool wantRec = ppb(REC_A, p) && eng2 && !smp[p].testTone;
            if (wantRec != inst.sampler_is_recording(p)) {
                inst.sampler_record(p, wantRec);
                // path/factoryLoaded mean "the buffer still holds exactly
                // what that source provided" -- once recording starts, the
                // buffer no longer matches either source, so the part must
                // stop claiming one.
                if (wantRec) {
                    smp[p].path.clear();
                    smp[p].factoryLoaded = false;
                }
            }

            // --- sampler control surface (spec 2026-07-21 morphagene-controls) ---
            // Four knobs that do nothing in the sampler's FLOW cloud get a
            // job of their own. The param ids do not change, so no saved
            // patch moves; only what the knob means when ENG says Sampler.
            //
            // set_variation and set_density above keep firing unconditionally
            // -- the "push to both, let the inactive side ignore it" pattern
            // the voice row already uses. DENS is the one knob that genuinely
            // does two things in sampler STEP mode: it still thins the groove
            // gate AND now sets grain overlap. Both point the same direction
            // (sparser), so this is left as-is and flagged for the listening
            // pass rather than special-cased.
            const bool samplerPart = eng2 && !smp[p].testTone;
            inst.sampler_overlap(p, pp(DENSITY_A, p));
            inst.sampler_scan(p, pp(MELODY_A, p));

            // GENE SIZE and ORGANIZE ride the lane BASES, so they must be
            // gated: in the synth these two slots drive the filter and the
            // timbre position, and writing SUB/DTUN into them there would be
            // wrong. The else branch is load-bearing -- a base left behind on
            // an engine flip would silently stick.
            if (samplerPart) {
                inst.set_target_base(p, spky::LANE_SIZE,   pp(SUB_A, p));
                inst.set_target_base(p, spky::LANE_SOURCE, pp(DETUNE_A, p));
            } else {
                inst.set_target_base(p, spky::LANE_SIZE,   0.5f);
                inst.set_target_base(p, spky::LANE_SOURCE, 0.5f);
            }

            // Stable pitch in the sampler: the lane still FIRES (that is what
            // keeps STEP triggering alive, part.cpp:183), it just stops
            // moving the pitch. Sample material and a synth deck can then sit
            // in the same key.
            inst.set_target_active(p, spky::LANE_PITCH, !samplerPart);

            inst.set_grit_mode(p, ppb(GRITMODE_A, p) ? spky::GritMode::Reduce
                                                     : spky::GritMode::Drive);
            inst.set_step(p, ppb(STEP_A, p), (int)std::round(pp(STEPS_A, p)));

            if (principleTrig[p].process(ppb(PRINCIPLE_A, p))) {
                principleIdx[p] = (principleIdx[p] + 1) % 5;   // cycle the 5 principles
                inst.set_principle(p, principleIdx[p]);
            }
            // NEW is "new gene now" in the sampler: the playhead returns to
            // ORGANIZE and a grain spawns immediately. Without it the long
            // end of GENE SIZE is unplayable -- every knob stays dead until
            // the next scheduled spawn, tens of seconds away at overlap 1.
            if (newPhraseTrig[p].process(ppb(NEWPHRASE_A, p))) {
                if (samplerPart) inst.sampler_punch(p);
                else             inst.new_phrase(p);
            }
            // TRIG punches AND triggers. trigger_manual alone is inert in the
            // sampler's FLOW cloud -- _next_ratio reads the burst latch only
            // when !_flow (sampler_engine.cpp:226) -- so the pad has been
            // dead there since M5b. The punch fixes FLOW; the trigger keeps
            // STEP behaving as it does today.
            if (triggerTrig[p].process(ppb(TRIGGER_A, p))) {
                if (samplerPart) inst.sampler_punch(p);
                inst.trigger_manual(p);
            }
        }

        inst.set_morph(params[MORPH].getValue());
        inst.set_couple(params[COUPLE].getValue());
        inst.set_drift(params[DRIFT].getValue());
        inst.set_tide(params[TIDE].getValue());
        inst.set_sync(params[SYNC].getValue() > 0.5f);
        inst.set_choke(params[CHOKE].getValue() * 0.5f);   // snap -2..+2 -> zones
        inst.set_reverb_size(params[REV_SIZE].getValue());
        inst.set_reverb_decay(params[REV_DECAY].getValue());
        inst.set_reverb_tone(params[REV_TONE].getValue());
        inst.set_reverb_diffusion(params[REV_DIFF].getValue());
        inst.set_reverb_mix(params[REV_MIX].getValue());
        inst.set_reverb_smear(params[REV_SMEAR].getValue());
        inst.set_reverb_mod(params[REV_MOD].getValue());
        inst.set_master_drive(params[MASTER_DRIVE].getValue());
        inst.set_scale((int)std::round(params[SCALE].getValue()));

        if (spotTrig.process(params[SPOT].getValue() > 0.5f))     inst.spot();
        if (settleTrig.process(params[SETTLE].getValue() > 0.5f)) inst.settle();

        // Tempo: an external clock (one pulse per beat) overrides the knob.
        float bpm = 40.f + params[TEMPO].getValue() * 200.f;
        if (inputs[CLOCK].isConnected() && clkSamples > 1.f && curSr > 0.f) {
            float measured = 60.f * curSr / clkSamples;
            if (measured >= 20.f && measured <= 400.f) bpm = measured;
        }
        inst.set_tempo_bpm(bpm);
    }

    void process(const ProcessArgs& args) override {
        if (args.sampleRate != curSr) reinit(args.sampleRate);

        // external clock edge -> remember the period, and phase-align the transport
        if (inputs[CLOCK].isConnected()) {
            clkSamples += 1.f;
            if (clockTrig.process(inputs[CLOCK].getVoltage(), 0.1f, 1.f)) {
                clkSamples = 0.f;
                inst.clock_pulse();
            }
        }

        if ((inputs[RESET].isConnected() &&
             resetTrig.process(inputs[RESET].getVoltage(), 0.1f, 1.f)) ||
            resyncReq.exchange(false))
            inst.reset_transport();

        if (ctrlDiv.process()) pushParams();

        const bool haveIn = inputs[IN_L].isConnected() || inputs[IN_R].isConnected();
        float inl = inputs[IN_L].getVoltage() * 0.2f;   // ±5V -> ±1
        float inr = inputs[IN_R].isConnected() ? inputs[IN_R].getVoltage() * 0.2f : inl;
        float outl = 0.f, outr = 0.f;
        inst.process(haveIn ? &inl : nullptr, haveIn ? &inr : nullptr,
                     &outl, &outr, 1);

        outputs[OUT_L].setVoltage(clamp(outl, -1.f, 1.f) * 5.f);
        outputs[OUT_R].setVoltage(clamp(outr, -1.f, 1.f) * 5.f);

        // per-part modulation taps -> the rest of the rack
        outputs[PITCH_A].setVoltage(clamp(inst.pitch_cv(0), -1.f, 1.f) * 5.f);
        outputs[PITCH_B].setVoltage(clamp(inst.pitch_cv(1), -1.f, 1.f) * 5.f);
        outputs[GATE_A].setVoltage(inst.gate(0) ? 10.f : 0.f);
        outputs[GATE_B].setVoltage(inst.gate(1) ? 10.f : 0.f);

        for (int p = 0; p < 2; ++p) {
            float g = inst.gate(p) ? 1.f : 0.f;
            gateFilt[p] += (g - gateFilt[p]) * 0.05f;
        }
        lights[GATE_A_L].setBrightness(gateFilt[0]);
        lights[GATE_B_L].setBrightness(gateFilt[1]);

        // REC LED: pulsing while recording, steady at the fill level when the
        // part holds content, dark when empty or when ENG is on Synth. Content
        // left over from a part that was switched away from Sampler must not
        // relight the LED -- ENG, not buffer state, decides what's shown.
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            float b = 0.f;
            const bool samplerPart = ppb(ENGINE_A, p) && !smp[p].testTone;
            if (inst.sampler_is_recording(p)) {
                recPhase[p] += 2.f / args.sampleRate;      // 2 Hz pulse
                if (recPhase[p] >= 1.f) recPhase[p] -= 1.f;
                b = recPhase[p] < 0.5f ? 1.f : 0.25f;
            } else if (samplerPart && !inst.sampler_empty(p)) {
                b = 0.15f + 0.55f * inst.sampler_fill(p);
            }
            lights[p ? REC_B_L : REC_A_L].setBrightness(b);
        }
    }

    void onReset() override {
        // Rack Initialize is the only gesture that should let the factory
        // sample autoload again -- a mid-session Clear must stay cleared.
        for (int p = 0; p < spky::PART_COUNT; ++p) factoryTried[p] = false;
        reinit(curSr > 0.f ? curSr : 48000.f);
    }

    // --- persistence -----------------------------------------------------
    // The module had no dataToJson at all: everything below is non-param
    // state (Tasks 4-6's edit layer, the sample path, and principleIdx,
    // which the PRIN button cycles and no param ever recorded) that would
    // otherwise die with the session.
    json_t* dataToJson() override {
        json_t* root = json_object();
        json_t* pr = json_array();
        for (int p = 0; p < spky::PART_COUNT; ++p)
            json_array_append_new(pr, json_integer(principleIdx[p]));
        json_object_set_new(root, "principle", pr);

        json_t* parts = json_array();
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            json_t* o = json_object();
            json_object_set_new(o, "path", json_string(smp[p].path.c_str()));
            json_object_set_new(o, "tape", json_integer(smp[p].tapeIdx));
            json_object_set_new(o, "reverse", json_boolean(smp[p].reverse));
            json_object_set_new(o, "feedback", json_real(smp[p].feedback));
            json_object_set_new(o, "testTone", json_boolean(smp[p].testTone));
            json_object_set_new(o, "factory", json_boolean(smp[p].factoryLoaded));
            // "autoload already consumed / user cleared this" -- without
            // persisting it, Clear -> Save -> reopen resets factoryTried to
            // its construction-time false, and the factory drone refills the
            // deliberately-cleared part on the first control tick after
            // patch open with no user gesture at all (I-1b).
            json_object_set_new(o, "factoryTried", json_boolean(factoryTried[p]));
            json_array_append_new(parts, o);
        }
        json_object_set_new(root, "sampler", parts);
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (!root) return;
        if (json_t* pr = json_object_get(root, "principle")) {
            for (int p = 0; p < spky::PART_COUNT; ++p) {
                json_t* v = json_array_get(pr, p);
                if (v) {
                    principleIdx[p] = (int)json_integer_value(v);
                    inst.set_principle(p, principleIdx[p]);
                }
            }
        }
        json_t* parts = json_object_get(root, "sampler");
        if (!parts) return;
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            json_t* o = json_array_get(parts, p);
            if (!o) continue;
            if (json_t* v = json_object_get(o, "path"))
                smp[p].path = json_string_value(v) ? json_string_value(v) : "";
            if (json_t* v = json_object_get(o, "tape"))     smp[p].tapeIdx = (int)json_integer_value(v);
            if (json_t* v = json_object_get(o, "reverse"))  smp[p].reverse = json_boolean_value(v);
            if (json_t* v = json_object_get(o, "feedback")) smp[p].feedback = (float)json_real_value(v);
            if (json_t* v = json_object_get(o, "testTone")) smp[p].testTone = json_boolean_value(v);
            if (json_t* v = json_object_get(o, "factory"))  smp[p].factoryLoaded = json_boolean_value(v);
            // Explicit default (not the `if (v)` pattern used above): a
            // legacy patch with no such key must still zero this out, not
            // silently inherit whatever the live module's factoryTried
            // happened to be before this load. Persisted intent otherwise
            // always wins -- see restoreSamplerContent()'s terminal branch,
            // which no longer overwrites this.
            json_t* v = json_object_get(o, "factoryTried");
            factoryTried[p] = v ? json_boolean_value(v) : false;
        }
        // On a whole-patch open, dataFromJson runs before the module is in
        // the engine (curSr == 0.f, reinit() hasn't sized the sampler
        // buffers), so content restore is deferred to onAdd(). But Rack also
        // calls dataFromJson on an already-live module -- right-click preset
        // load and module paste go through ModuleWidget::loadAction/
        // pasteJsonAction, not Engine::addModule_NoLock, so onAdd() never
        // fires again for those. curSr > 0.f (Task 7) is the "already
        // initialised" test: restore immediately in that case instead of
        // leaving the buffer stale forever. pendingRestore stays reserved
        // for the fresh-add path -- exactly one of the two restores the
        // content for a given JSON load, never both.
        if (curSr > 0.f) {
            pendingRestore = false;
            restoreSamplerContent();
        } else {
            pendingRestore = true;
        }
    }

    std::string storedWavPath(int p) {
        return system::join(createPatchStorageDirectory(),
                            p ? "sample_b.wav" : "sample_a.wav");
    }

    // Same target file as storedWavPath(), but never creates the patch
    // storage directory. For check-and-delete callers: creating an empty
    // modules/<id>/ directory on every save (via createPatchStorageDirectory)
    // even for instances that never touched the sampler is pure litter.
    std::string storedWavPathNoCreate(int p) {
        return system::join(getPatchStorageDirectory(),
                            p ? "sample_b.wav" : "sample_a.wav");
    }

    void onSave(const SaveEvent& e) override {
        Module::onSave(e);
        // The recorded texture has no file of its own -- without this it dies
        // with the session. Only content that did not come from a file or the
        // factory WAV needs writing: those two reload from their source.
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            // Any part this save will NOT write its own stored WAV for --
            // it now has a file path, is factory-loaded, or has nothing
            // recorded -- must not be left holding a stale one from an
            // earlier save. Rack garbage-collects patch storage per MODULE,
            // not per file, so e.g. record -> save (writes a stored WAV) ->
            // load a WAV over it -> save again would otherwise leave that
            // first recording (up to 16 MB) sitting in patch storage
            // forever, unreachable but real disk weight inside the user's
            // patch (I-1a's empty-part case, generalised to every reason a
            // part might skip writing). Hoisted above the old skip guard so
            // it fires for the path/factory cases too, not just the empty
            // one. Deliberately structured so this can never race the write
            // below: willWrite and the delete are mutually exclusive by
            // construction, so a part about to be (re)written this save
            // never has its about-to-be-overwritten file deleted out from
            // under it first.
            const bool willWrite = smp[p].path.empty() && !smp[p].factoryLoaded
                                  && inst.sampler_rec_size(p) != 0;
            if (!willWrite) {
                // Uses the no-create variant: a pure check-and-delete must
                // not mkdir a patch storage directory that would otherwise
                // never exist for an instance that never touched the
                // sampler.
                const std::string stored = storedWavPathNoCreate(p);
                if (system::isFile(stored) && !system::remove(stored)) {
                    // Windows in particular can leave a locked or read-only
                    // file undeletable; failing silently here would let the
                    // stale WAV survive and restoreSamplerContent() reload it
                    // right back on reopen -- the original I-1a bug, with no
                    // diagnostic to explain why.
                    WARN("Spotymod: could not remove stale sampler %d WAV %s",
                         p, stored.c_str());
                }
                continue;
            }
            std::string err;
            if (!spkyvcv::save_wav_from(inst, p, storedWavPath(p), curSr, err))
                WARN("Spotymod: could not store sampler %d: %s", p, err.c_str());
        }
    }

    // Shared by onAdd() (fresh patch-open add) and dataFromJson() (preset
    // load / paste on an already-live module) -- exactly one of those two
    // call sites invokes this per JSON load, see the comments at each.
    void restoreSamplerContent() {
        // Engine::addModule_NoLock dispatches AddEvent *before*
        // SampleRateChangeEvent, so for a freshly-added instance curSr is
        // still its construction-time 0 and inst/samplerMem have never been
        // sized (reinit() has not run yet) -- loading here would write into
        // an uninitialised engine. reinit() is safe to call twice in a row
        // (it snapshots any already-loaded content out and back in around
        // the resize/init), so forcing it now does not race the
        // SampleRateChangeEvent that follows this call. On the already-live
        // path (called from dataFromJson) curSr is already > 0.f, so this
        // is a no-op there.
        // Captured BEFORE the reinit() above can change curSr: it is the
        // "this is the fresh-add path" test, not merely "was curSr 0 a
        // moment ago" -- see the stored-WAV comment below.
        const bool freshAdd = (curSr <= 0.f);
        if (freshAdd) reinit(APP->engine->getSampleRate());
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            std::string err;
            if (!smp[p].path.empty()) {
                if (spkyvcv::load_wav_into(inst, p, smp[p].path, curSr, err))
                    continue;
                WARN("Spotymod: sampler %d could not reload %s: %s",
                     p, smp[p].path.c_str(), err.c_str());
                // Deliberately falls through instead of continue-ing: the
                // file may have moved, been renamed, or the patch may have
                // been opened on another machine -- the ordinary case for a
                // user-loaded WAV, not a rare one. Without this the live
                // buffer keeps whatever audio the module was already playing
                // while path/menu/JSON all describe a load that never
                // actually happened (the same I-2 symptom, surviving in this
                // error path). Falls into the stored-WAV check below (fresh-
                // add only, harmless no-op on the live path) and then the
                // terminal clear if that also finds nothing.
            }
            // getPatchStorageDirectory() is scoped to the CURRENT patch and
            // module id, not to the preset being loaded. On a live preset
            // load or module paste (freshAdd == false) it points at THIS
            // patch's own autosave directory, so reading a stored WAV there
            // would pull content that belongs to neither the preset nor the
            // user's intent into a part the preset describes as empty.
            // Restrict the stored-WAV branch to the fresh-add path, where
            // the directory really is the patch being opened (I-2 note).
            if (freshAdd) {
                const std::string stored = system::join(getPatchStorageDirectory(),
                                                        p ? "sample_b.wav" : "sample_a.wav");
                if (system::isFile(stored)) {
                    if (spkyvcv::load_wav_into(inst, p, stored, curSr, err))
                        continue;
                    WARN("Spotymod: sampler %d could not reload stored %s: %s",
                         p, stored.c_str(), err.c_str());
                }
            }
            // Neither a file path nor (fresh-add only) a stored WAV exists
            // for this part. This function must be total: on the live path
            // (preset load / paste) the buffer may already hold whatever the
            // module was playing before, and nothing else will clear it --
            // the menu, the JSON and the REC LED would all describe the new
            // preset while the old audio kept playing (I-2). Safe on the
            // fresh-add path too: the buffer there is already zeroed by the
            // reinit() above.
            //
            // Deliberately does NOT touch factoryTried[p] here. dataFromJson
            // just restored it from this same JSON (defaulting to false for
            // legacy patches with no such key) -- overwriting it back to
            // false would discard exactly the intent a deliberate Clear ->
            // Save persisted, and the factory drone would refill a part the
            // user emptied on purpose, on the very first control tick after
            // reopen with no user gesture at all. onReset() (Rack
            // Initialize) remains the only thing that un-consumes this guard.
            inst.sampler_clear(p);
        }
    }

    // Decode res/factory.wav at its native rate, at most once per module
    // instance. Must only ever be called from onAdd() (main thread): onAdd()
    // completes as part of the synchronous addModule() call, before the
    // module widget exists for the user to touch and before process() ever
    // runs for this instance -- the same happens-before guarantee
    // restoreSamplerContent()'s onAdd() call already relies on. reinit()
    // deliberately does NOT read disk itself (only resamples this cache), so
    // that process()'s reactive `sampleRate != curSr` fallback -- which does
    // run on the audio thread -- can never trigger a file read.
    void loadFactoryNative() {
        if (factoryNativeTried) return;
        factoryNativeTried = true;
        std::string err;
        const std::string fp = asset::plugin(pluginInstance, "res/factory.wav");
        if (!spky::read_wav(fp, factoryNative, err)) {
            WARN("Spotymod: factory sample unavailable: %s", err.c_str());
            factoryNative.l.clear();
            factoryNative.r.clear();
        }
    }

    void onAdd(const AddEvent& e) override {
        Module::onAdd(e);
        // Must run before restoreSamplerContent() (which may force an
        // immediate reinit() below): reinit() only resamples factoryNative,
        // it doesn't read it, so the cache has to already be populated (or
        // confirmed unreadable) by the time any reinit() call happens.
        loadFactoryNative();
        if (!pendingRestore) return;
        pendingRestore = false;
        restoreSamplerContent();
    }
};

// --- live LED ring ------------------------------------------------------------
// One per part. Draws 32 dots in the light layer (glows additively over Rack's
// darkened room). Each of the five lanes lights a moving dot at its position;
// a lane that just fired flashes. Idle -> dark (no fake motion).
struct SpkyRing : Widget {
    Spotymod* module = nullptr;
    int part = 0;

    SpkyRing(Spotymod* m, int p) : module(m), part(p) {
        float d = mm2px(2.f * (kRingR + kRingDotR + 0.5f));
        box.size = Vec(d, d);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;
        const float TWO_PI = 6.2831853f;
        Vec c = box.size.div(2.f);
        float R  = mm2px(kRingR);
        float dr = mm2px(kRingDotR);

        float bright[kRingDots] = {};
        if (module) {
            for (int s = 0; s < spky::LANE_COUNT; ++s) {
                float v = clamp(module->inst.lane_output(part, s), -1.f, 1.f);
                float posf = (v * 0.5f + 0.5f) * kRingDots;      // 0..32
                int i0 = ((int)std::floor(posf)) % kRingDots;
                if (i0 < 0) i0 += kRingDots;
                int i1 = (i0 + 1) % kRingDots;
                float frac = posf - std::floor(posf);
                float boost = module->inst.lane_fired(part, s) ? 1.f : 0.6f;
                bright[i0] = std::max(bright[i0], (1.f - frac) * boost);
                bright[i1] = std::max(bright[i1], frac * boost);
            }
        }
        // per-part glow colour from the generator (A solder green, B copper)
        const unsigned gc = kColGlow[part];
        const float cr = ((gc >> 16) & 0xFF) / 255.f;
        const float cg = ((gc >> 8)  & 0xFF) / 255.f;
        const float cb = ( gc        & 0xFF) / 255.f;
        for (int i = 0; i < kRingDots; ++i) {
            float b = bright[i];
            if (b <= 0.02f) continue;
            float a = TWO_PI * i / kRingDots;
            Vec p = c.plus(Vec(std::sin(a), -std::cos(a)).mult(R));
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, p.x, p.y, dr * 2.6f);
            nvgFillColor(args.vg, nvgRGBAf(cr, cg, cb, 0.25f * b));
            nvgFill(args.vg);
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, p.x, p.y, dr);
            nvgFillColor(args.vg, nvgRGBAf(cr, cg, cb, b));
            nvgFill(args.vg);
        }

        // --- sampler read position (spec 2026-07-21 morphagene-controls) ---
        // Drawn separately rather than through bright[]: that array carries
        // brightness only, and every dot in it is kColGlow[part], so a head
        // folded into it would be indistinguishable from a lane. Warm white
        // at full brightness reads as "this one is different" without a
        // second palette. On a synth part, or with nothing recorded, nothing
        // is drawn -- an idle ring stays dark, as it does today.
        if (module && module->inst.engine_id(part) == spky::ENGINE_SAMPLER) {
            const size_t content = module->inst.sampler_rec_size(part);
            if (content > 0) {
                const float frac =
                    module->inst.sampler_scan_pos(part) / float(content);
                const float a = TWO_PI * frac;
                Vec hp = c.plus(Vec(std::sin(a), -std::cos(a)).mult(R));
                nvgBeginPath(args.vg);
                nvgCircle(args.vg, hp.x, hp.y, dr * 3.0f);
                nvgFillColor(args.vg, nvgRGBAf(1.f, 0.95f, 0.85f, 0.20f));
                nvgFill(args.vg);
                nvgBeginPath(args.vg);
                nvgCircle(args.vg, hp.x, hp.y, dr * 1.15f);
                nvgFillColor(args.vg, nvgRGBAf(1.f, 0.95f, 0.85f, 0.95f));
                nvgFill(args.vg);
            }
        }
    }
};

// --- panel text ---------------------------------------------------------------
// Rack's SVG loader (NanoSVG) ignores <text>, so the faceplate ships with none
// of its lettering visible. Every caption is drawn here with nvgText, straight
// out of the generated tables: position, anchor, size and colour all come from
// res/gen_panel.py (PanelCtl::lbl/anchor/lblSize/lblRgb), so the SVG preview and
// Rack can never drift apart. Font is a stock Rack asset, present in every
// v2 install -- note it has no bold cut, so the SVG's bold legends render
// regular here. That is accepted.
struct PanelText : Widget {
    void draw(const DrawArgs& args) override {
        std::shared_ptr<Font> font =
            APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (!font) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

        auto col = [](unsigned rgb) {
            return nvgRGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        };
        auto text = [&](float xmm, float ymm, float szmm, NVGcolor c, const char* s) {
            nvgFontSize(args.vg, mm2px(szmm));
            nvgFillColor(args.vg, c);
            Vec p = mm2px(Vec(xmm, ymm));
            nvgText(args.vg, p.x, p.y, s, NULL);
        };
        auto alignOf = [](unsigned char a) {
            return a == 1 ? NVG_ALIGN_LEFT : a == 2 ? NVG_ALIGN_RIGHT
                                                    : NVG_ALIGN_CENTER;
        };
        auto captions = [&](const PanelCtl* t, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                if (!t[i].label[0]) continue;
                nvgTextAlign(args.vg, alignOf(t[i].anchor) | NVG_ALIGN_BASELINE);
                text(t[i].lbl.x, t[i].lbl.y, t[i].lblSize, col(t[i].lblRgb),
                     t[i].label);
            }
            nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
        };
        captions(kParamCtls,  sizeof(kParamCtls)  / sizeof(kParamCtls[0]));
        captions(kInputCtls,  sizeof(kInputCtls)  / sizeof(kInputCtls[0]));
        captions(kOutputCtls, sizeof(kOutputCtls) / sizeof(kOutputCtls[0]));

        // section titles + brand -- the shared TEXTS table from the generator,
        // so runtime lettering matches the SVG preview one-to-one
        for (const auto& t : kPanelTexts) {
            nvgTextLetterSpacing(args.vg, mm2px(t.spacing));
            text(t.mm.x, t.mm.y, t.size, col(t.rgb), t.str);
        }
        nvgTextLetterSpacing(args.vg, 0.f);
    }
};

// --- sampler edit-layer menu ---------------------------------------------------
// Overdub feedback is a continuous value with no panel home -- the menu
// slider is its only surface. 0.95 (~-3 dB) is the engine default. The knob
// is normalised 0..1; the engine maps it to -60..0 dB internally
// (SampleBuffer::set_feedback), so the display is a percentage of the knob,
// not a dB figure.
struct FeedbackQuantity : Quantity {
    float* v;
    explicit FeedbackQuantity(float* p) : v(p) {}
    void  setValue(float x) override { *v = clamp(x, 0.f, 1.f); }
    float getValue() override        { return *v; }
    float getMinValue() override     { return 0.f; }
    float getMaxValue() override     { return 1.f; }
    float getDefaultValue() override { return 0.95f; }
    std::string getLabel() override  { return "Overdub feedback"; }
    std::string getDisplayValueString() override {
        return string::f("%.0f%%", getValue() * 100.f);
    }
};

struct FeedbackSlider : ui::Slider {
    explicit FeedbackSlider(float* v) {
        box.size.x = 180.f;
        quantity = new FeedbackQuantity(v);
    }
    ~FeedbackSlider() override { delete quantity; }
};

// --- widget -------------------------------------------------------------------
struct SpotymodWidget : ModuleWidget {
    SpotymodWidget(Spotymod* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Spotymod.svg")));

        // panel lettering (NanoSVG can't render the SVG's <text>; see PanelText)
        auto* labels = new PanelText;
        labels->box.size = box.size;
        addChild(labels);

        for (const auto& c : kParamCtls) {
            Vec pos = mm2px(Vec(c.mm.x, c.mm.y));
            switch (c.kind) {
                case WK_BIGKNOB: case WK_KNOBC:
                    addParam(createParamCentered<RoundBlackKnob>(pos, module, c.id)); break;
                case WK_SMKNOB: case WK_KNOBI:
                    addParam(createParamCentered<Trimpot>(pos, module, c.id)); break;
                case WK_SW2:
                    addParam(createParamCentered<CKSS>(pos, module, c.id)); break;
                case WK_LATCH:
                    addParam(createParamCentered<VCVLatch>(pos, module, c.id)); break;
                case WK_SMBTN:
                    addParam(createParamCentered<VCVButton>(pos, module, c.id)); break;
                default: break;
            }
        }
        for (const auto& c : kInputCtls)
            addInput(createInputCentered<PJ301MPort>(mm2px(Vec(c.mm.x, c.mm.y)), module, c.id));
        for (const auto& c : kOutputCtls)
            addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(c.mm.x, c.mm.y)), module, c.id));
        for (const auto& c : kLightCtls) {
            Vec pos = mm2px(Vec(c.mm.x, c.mm.y));
            if (c.id == REC_A_L || c.id == REC_B_L)   // record = red, the one
                addChild(createLightCentered<SmallLight<RedLight>>(pos, module, c.id));
            else                                       // gate glow = warm signal hue
                addChild(createLightCentered<MediumLight<YellowLight>>(pos, module, c.id));
        }

        // live LED rings, centred on each ring (same coords as the gate lights)
        for (int p = 0; p < 2; ++p) {
            auto* ring = new SpkyRing(module, p);
            Vec ctr = mm2px(Vec(kLightCtls[p].mm.x, kLightCtls[p].mm.y));
            ring->box.pos = ctr.minus(ring->box.size.div(2.f));
            addChild(ring);
        }

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
    }

    void appendContextMenu(Menu* menu) override {
        auto* m = getModule<Spotymod>();
        menu->addChild(new MenuSeparator);
        // Same gesture as a pulse into RST: zero the downbeat and restart the
        // loops at the bar start (a live STEPS turn leaves them free-running).
        menu->addChild(createMenuItem("Resync loops to bar", "",
                                      [m]() { m->resyncReq = true; }));

        menu->addChild(new MenuSeparator);
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            const std::string name = p ? "Sampler B" : "Sampler A";
            menu->addChild(createSubmenuItem(name, "", [m, p](Menu* sub) {
                sub->addChild(createMenuItem("Load sample...", "", [m, p]() {
                    char* path = osdialog_file(OSDIALOG_OPEN, nullptr, nullptr, nullptr);
                    if (!path) return;
                    std::string err;
                    if (spkyvcv::load_wav_into(m->inst, p, path, m->curSr, err)) {
                        m->smp[p].path = path;
                        m->smp[p].factoryLoaded = false;
                    } else {
                        osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, err.c_str());
                    }
                    std::free(path);
                }));
                sub->addChild(createMenuItem("Save sample...", "", [m, p]() {
                    char* path = osdialog_file(OSDIALOG_SAVE, nullptr, "sample.wav", nullptr);
                    if (!path) return;
                    std::string err;
                    if (!spkyvcv::save_wav_from(m->inst, p, path, m->curSr, err))
                        osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, err.c_str());
                    std::free(path);
                }));
                sub->addChild(createMenuItem("Clear sample", "", [m, p]() {
                    m->inst.sampler_clear(p);
                    m->smp[p].path.clear();
                    m->smp[p].factoryLoaded = false;
                }));
                sub->addChild(new MenuSeparator);
                sub->addChild(createIndexPtrSubmenuItem(
                    "Speed mode", {"Digital", "Tape"}, &m->smp[p].tapeIdx));
                sub->addChild(createBoolPtrMenuItem("Reverse", "", &m->smp[p].reverse));
                sub->addChild(createSubmenuItem("Overdub feedback", "", [m, p](Menu* fb) {
                    fb->addChild(new FeedbackSlider(&m->smp[p].feedback));
                }));
                sub->addChild(new MenuSeparator);
                sub->addChild(createBoolPtrMenuItem("Engine: test tone (dev)", "",
                                                    &m->smp[p].testTone));
            }));
        }
    }
};

Model* modelSpotymod = createModel<Spotymod, SpotymodWidget>("Spotymod");
