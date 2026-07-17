#include <cmath>
#include <algorithm>
#include <atomic>
#include "plugin.hpp"
#include "generated_panel.hpp"   // enums + control table (generated from res/gen_panel.py)

// The portable engine core -- exactly the same headers the desktop render host
// and (later) the Daisy firmware use. No hardware type crosses this boundary.
#include "instrument.h"
#include "fx/flux.h"
#include "mod/divisions.h"

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

    float curSr = 0.f;
    dsp::ClockDivider ctrlDiv;              // throttle param push to control rate
    dsp::SchmittTrigger clockTrig, resetTrig;
    dsp::BooleanTrigger triggerTrig[2], spotTrig, settleTrig;
    dsp::BooleanTrigger principleTrig[2], newPhraseTrig[2];
    int principleIdx[2] = {0, 0};   // current principle per part (0=TwoMotif)
    float clkSamples = 0.f;                 // samples since last external clock edge
    float gateFilt[2] = {0.f, 0.f};
    std::atomic<bool> resyncReq { false };  // menu "Resync to bar" -> audio thread

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
                        configParam(c.id, -1.f, 1.f, 0.f, lbl);
                    else if (c.id == TIDE)  // texture-lane rate, snaps under SYNC
                        configParam<TideQuantity>(c.id, 0.f, 1.f, 0.5f, lbl);
                    else if (c.id == FLUXRATE_A || c.id == FLUXRATE_B)
                        configParam<FluxRateQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    else if (c.id == FLUXFB_A || c.id == FLUXFB_B)
                        configParam<FluxFbQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    else
                        configParam(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    break;
                case WK_KNOBC:  // MELO (bipolar): part A leans toward GROW, B centred
                    configParam(c.id, -1.f, 1.f, c.id == MELODY_A ? 0.32f : 0.f, lbl); break;
                case WK_KNOBI:
                    if (c.id == SCALE)  // init patch is Dorian (holds every min9 tone)
                        configParam(c.id, 0.f, (float)(spky::SCALE_LIST_COUNT - 1),
                                    (float)spky::SCALE_DORIAN, "Scale");
                    else  // STEPS_A / STEPS_B
                        configParam(c.id, 2.f, 16.f, 8.f, "Steps");
                    getParamQuantity(c.id)->snapEnabled = true;
                    break;
                case WK_SW2:  // init patch runs the instrument on the grid
                    configSwitch(c.id, 0.f, 1.f, 1.f, "Sync", {"Free", "Synced"});
                    break;
                case WK_LATCH:
                    if (c.id == ENGINE_A || c.id == ENGINE_B)
                        configSwitch(c.id, 0.f, 1.f, 0.f, "Engine", {"Synth", "Test tone"});
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
        for (const auto& c : kInputCtls)  configInput(c.id, c.label);
        for (const auto& c : kOutputCtls) configOutput(c.id, c.label);
    }

    // Init "patch" (Rack Initialize / fresh instance): part A = a sustained
    // minor-9 drone, part B = a low bass melody, both in Dorian (all min9 tones:
    // A C E G B). Knob values are a snapshot of a hand-dialled panel state
    // (2026-07-15): MORPH sits hard toward A, COUPLE/DRIFT high, the room forward
    // (SIZE/DECAY long, MIX 0.65), and part B runs its echo (FLUX) with light
    // grit while part A stays FX-clean. Only knob params (WK_BIGKNOB/WK_SMKNOB)
    // come through here; STEP/SYNC/STEPS/SCALE defaults live in configControls().
    // RATE values are expressed on the 17-rung ladder introduced 2026-07-16
    // (0.0625 = 4 bars, 0.125 = 2 bars — same musical rates as the snapshot).
    static float defaultFor(int id) {
        switch (id) {                       // global knobs (init.vcvm snapshot 2026-07-15)
            case MORPH:        return 0.00f;   // hard left = fully part A (only deck A active)
            case COUPLE:       return 1.00f;   // full = hard loop lock
            case DRIFT:        return 1.00f;
            case MASTER_DRIVE: return 0.419f;
            case REV_SIZE:     return 1.00f;   // full room
            case REV_DECAY:    return 0.887f;
            case REV_TONE:     return 0.803f;
            case REV_DIFF:     return 0.863f;
            case REV_MIX:      return 0.691f;  // reverb sits well forward
            case REV_SMEAR:    return 0.568f;  // diffuser LFO smear (wash)
            case REV_MOD:      return 0.237f;  // tail LFO wobble
            case TEMPO:        return 0.00f;   // as saved (40 BPM floor; parts run Synced)
            case FLUXRATE_A:   return 3.f / 11.f;   // "1/4" for part A's drone echo
            case FLUXRATE_B:   return 6.f / 11.f;   // "1/8" for part B's bass echo
            case FLUXFB_A:     return 0.45f;        // matches the retired FXT_FLUX_FB boot base
            case FLUXFB_B:     return 0.45f;
            case COLOR_A:
            case COLOR_B:      return 0.f;    // init patch = single notes (bit-identical)
            default: break;
        }
        const int part = id / PART_STRIDE;  // 0 = A (drone), 1 = B (bass)
        switch (id % PART_STRIDE) {         // fold part B onto the *_A enum; part ? B : A
            case RATE_A:   return part ? 0.125f : 0.0625f;
            case SHAPE_A:  return part ? 0.60f  : 0.40f;
            case DENSITY_A: return part ? 0.60f : 0.67f;
            case SMOOTH_A: return part ? 0.30f  : 0.10f;
            case RANGE_A:  return part ? 0.236f : 0.78f;   // melody ambitus (= old RANGE*DEPTH)
            case MOD_A:    return part ? 0.236f : 0.78f;   // texture depth  (= old RANGE*DEPTH)
            case TUNE_A:   return part ? 0.00f  : 0.55f;   // B down an octave-ish
            case ATTACK_A: return part ? 0.686f : 0.657f;
            case DECAY_A:  return part ? 0.721f : 0.902f;  // A rings/stacks; B plucks
            case RES_A:    return part ? 0.347f : 0.411f;
            case SUB_A:    return part ? 0.663f : 0.35f;   // weight under the bass
            case DETUNE_A: return part ? 0.10f  : 0.20f;
            case FLUX_A:   return part ? 0.88f  : 0.325f;  // both echo engaged
            case GRIT_A:   return part ? 0.20f  : 0.00f;   // B light grit; A off
            case COMP_A:   return part ? 0.28f  : 0.45f;
            default: break;
        }
        return 0.5f;
    }

    void reinit(float sr) {
        curSr = sr;
        inst.init(sr, fxmem);
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
            // The FX blocks are gated by an explicit on/off (a pad on hardware,
            // a scenario action on the desktop). VCV has no such pad, so the mix
            // knob doubles as the on switch: knob up == engaged. At 0 the block
            // stays idle and the whole chain is skipped (bit-exact bypass).
            inst.set_fx_on(p, spky::FxBlock::Flux, pp(FLUX_A, p) > 1e-4f);
            inst.set_fx_on(p, spky::FxBlock::Grit, pp(GRIT_A, p) > 1e-4f);
            inst.set_comp(p, pp(COMP_A, p));

            inst.set_engine(p, ppb(ENGINE_A, p) ? spky::ENGINE_TEST_TONE
                                                : spky::ENGINE_SYNTH);
            inst.set_grit_mode(p, ppb(GRITMODE_A, p) ? spky::GritMode::Reduce
                                                     : spky::GritMode::Drive);
            inst.set_step(p, ppb(STEP_A, p), (int)std::round(pp(STEPS_A, p)));

            if (principleTrig[p].process(ppb(PRINCIPLE_A, p))) {
                principleIdx[p] = (principleIdx[p] + 1) % 5;   // cycle the 5 principles
                inst.set_principle(p, principleIdx[p]);
            }
            if (newPhraseTrig[p].process(ppb(NEWPHRASE_A, p))) inst.new_phrase(p);
            if (triggerTrig[p].process(ppb(TRIGGER_A, p))) inst.trigger_manual(p);
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
    }

    void onReset() override { reinit(curSr > 0.f ? curSr : 48000.f); }
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
    }
};

// --- panel text ---------------------------------------------------------------
// Rack's SVG loader (NanoSVG) ignores <text>, so the faceplate ships with none
// of its lettering visible. Every label already lives in the generated control
// table (kParamCtls[].label, ...); we draw it here at runtime with nvgText, plus
// the section titles/brand. Positions mirror res/gen_panel.py exactly (label
// baseline = control centre + glyph radius + 2.5 mm), so this matches the SVG
// preview one-to-one. Font is a stock Rack asset, present in every v2 install.
struct PanelText : Widget {
    // glyph radius per kind -- mirror of GLYPH_R in res/gen_panel.py
    static float glyphR(int kind) {
        switch (kind) {
            case WK_BIGKNOB: case WK_KNOBC: case WK_IN: case WK_OUT: return 4.2f;
            case WK_SMKNOB:  case WK_KNOBI: return 3.0f;
            case WK_SW2:     return 3.0f;
            case WK_LATCH:   case WK_SMBTN: return 2.7f;
            default:         return 1.7f;
        }
    }

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
        auto captions = [&](const PanelCtl* t, size_t n) {
            for (size_t i = 0; i < n; ++i)
                if (t[i].label[0])
                    text(t[i].mm.x, t[i].mm.y + glyphR(t[i].kind) + 2.5f, 2.0f,
                         col(kColLabel), t[i].label);
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
        for (const auto& c : kLightCtls)  // warm signal hue for the gate glow
            addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(c.mm.x, c.mm.y)), module, c.id));

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
    }
};

Model* modelSpotymod = createModel<Spotymod, SpotymodWidget>("Spotymod");
