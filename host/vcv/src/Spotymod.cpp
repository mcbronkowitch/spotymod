#include <cmath>
#include <algorithm>
#include "plugin.hpp"
#include "generated_panel.hpp"   // enums + control table (generated from res/gen_panel.py)

// The portable engine core -- exactly the same headers the desktop render host
// and (later) the Daisy firmware use. No hardware type crosses this boundary.
#include "instrument.h"
#include "fx/flux.h"

using namespace spkyvcv;

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
    dsp::SchmittTrigger clockTrig;
    dsp::BooleanTrigger triggerTrig[2], spotTrig, settleTrig;
    dsp::BooleanTrigger principleTrig[2], newPhraseTrig[2];
    int principleIdx[2] = {0, 0};   // current principle per part (0=TwoMotif)
    float clkSamples = 0.f;                 // samples since last external clock edge
    float gateFilt[2] = {0.f, 0.f};

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
                case WK_SMKNOB: configParam(c.id, 0.f, 1.f, defaultFor(c.id), lbl); break;
                case WK_KNOBC:  configParam(c.id, -1.f, 1.f, 0.f, lbl); break;
                case WK_KNOBI:
                    if (c.id == SCALE)  // init patch is Dorian (holds every min9 tone)
                        configParam(c.id, 0.f, (float)(spky::SCALE_LIST_COUNT - 1),
                                    (float)spky::SCALE_DORIAN, "Scale");
                    else  // STEPS_A / STEPS_B
                        configParam(c.id, 2.f, 16.f, c.id == STEPS_A ? 6.f : 8.f, "Steps");
                    getParamQuantity(c.id)->snapEnabled = true;
                    break;
                case WK_SW3:  // init patch runs both parts tempo-Synced
                    configSwitch(c.id, 0.f, 2.f, 1.f, "Sync", {"Free", "Sync", "Triplet"});
                    break;
                case WK_LATCH:
                    if (c.id == ENGINE_A || c.id == ENGINE_B)
                        configSwitch(c.id, 0.f, 1.f, 0.f, "Engine", {"Synth", "Test tone"});
                    else if (c.id == GRITMODE_A || c.id == GRITMODE_B)
                        configSwitch(c.id, 0.f, 1.f, 0.f, "Grit mode", {"Drive", "Reduce"});
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
    // A C E G B). Values were tuned by rendering the equivalent scenario through
    // the desktop host and reading back pitch/voice content (see
    // res/../scenarios). Only knob params (WK_BIGKNOB/WK_SMKNOB) come through
    // here; STEP/SYNC/STEPS/SCALE defaults live in configControls().
    static float defaultFor(int id) {
        switch (id) {                       // global knobs
            case MORPH:        return 0.50f;   // center neutral
            case COUPLE:       return 0.00f;
            case DRIFT:        return 0.00f;
            case MASTER_DRIVE: return 0.50f;
            case REV_SIZE:     return 0.70f;   // long, roomy tail under the drone
            case REV_DECAY:    return 0.80f;
            case REV_TONE:     return 0.55f;
            case REV_DEPTH:    return 0.25f;
            case REV_MIX:      return 0.25f;   // by-ear default (0.5 ~= old balance)
            case TEMPO:        return 0.35f;   // ~110 BPM on the 40..240 map
            default: break;
        }
        const int part = id / PART_STRIDE;  // 0 = A (drone), 1 = B (bass)
        switch (id % PART_STRIDE) {         // fold part B onto the *_A enum
            case RATE_A:   return part ? 0.20f : 0.10f;
            case SHAPE_A:  return part ? 0.70f : 0.40f;
            case DENSITY_A: return 1.0f;
            case SMOOTH_A: return part ? 0.20f : 0.50f;
            case RANGE_A:  return part ? 0.30f : 0.45f;
            case DEPTH_A:  return part ? 0.60f : 0.50f;
            case TUNE_A:   return part ? 0.00f : 0.50f;   // B down an octave-ish
            case ATTACK_A: return part ? 0.05f : 0.20f;
            case DECAY_A:  return part ? 0.16f : 0.90f;   // A rings/stacks; B plucks
            case RES_A:    return part ? 0.25f : 0.20f;
            case SUB_A:    return part ? 0.50f : 0.30f;   // weight under the bass
            case DETUNE_A: return part ? 0.10f : 0.20f;
            case FLUX_A:   return 0.00f;
            case GRIT_A:   return 0.00f;
            case COMP_A:   return part ? 0.20f : 0.30f;
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
            inst.set_depth(p, pp(DEPTH_A, p));
            inst.set_tune(p, pp(TUNE_A, p));

            inst.set_voice_attack(p, pp(ATTACK_A, p));
            inst.set_voice_decay(p, pp(DECAY_A, p));
            inst.set_voice_resonance(p, pp(RES_A, p));
            inst.set_voice_sub(p, pp(SUB_A, p));
            inst.set_voice_detune(p, pp(DETUNE_A, p));

            inst.set_flux_mix(p, pp(FLUX_A, p));
            inst.set_grit_mix(p, pp(GRIT_A, p));
            // The FX blocks are gated by an explicit on/off (a pad on hardware,
            // a scenario action on the desktop). VCV has no such pad, so the mix
            // knob doubles as the on switch: knob up == engaged. At 0 the block
            // stays idle and the whole chain is skipped (bit-exact bypass).
            inst.set_fx_on(p, spky::FxBlock::Flux, pp(FLUX_A, p) > 1e-4f);
            inst.set_fx_on(p, spky::FxBlock::Grit, pp(GRIT_A, p) > 1e-4f);
            inst.set_comp(p, pp(COMP_A, p));

            // 3-pos switch: 0=Free, 1=Sync, 2=Triplet.
            int sm = (int)std::round(pp(SYNC_A, p));
            inst.set_sync_mode(p, sm == 1 ? spky::SyncMode::Sync
                                : sm == 2 ? spky::SyncMode::SyncTriplet
                                          : spky::SyncMode::Free);
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
        inst.set_reverb_size(params[REV_SIZE].getValue());
        inst.set_reverb_decay(params[REV_DECAY].getValue());
        inst.set_reverb_tone(params[REV_TONE].getValue());
        inst.set_reverb_diffusion(params[REV_DEPTH].getValue());
        inst.set_reverb_mix(params[REV_MIX].getValue());
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

        // external clock edge -> remember the period
        if (inputs[CLOCK].isConnected()) {
            clkSamples += 1.f;
            if (clockTrig.process(inputs[CLOCK].getVoltage(), 0.1f, 1.f)) {
                clkSamples = 0.f;
            }
        }

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
        for (int i = 0; i < kRingDots; ++i) {
            float b = bright[i];
            if (b <= 0.02f) continue;
            float a = TWO_PI * i / kRingDots;
            Vec p = c.plus(Vec(std::sin(a), -std::cos(a)).mult(R));
            // soft glow + core, mint (#6DE0C8)
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, p.x, p.y, dr * 2.6f);
            nvgFillColor(args.vg, nvgRGBAf(0.43f, 0.88f, 0.78f, 0.25f * b));
            nvgFill(args.vg);
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, p.x, p.y, dr);
            nvgFillColor(args.vg, nvgRGBAf(0.43f, 0.88f, 0.78f, b));
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
            case WK_SW3:     return 2.2f;
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

        const NVGcolor lbl   = nvgRGB(0xc9, 0xcc, 0xd8);   // control captions
        const NVGcolor mint  = nvgRGB(0x6d, 0xe0, 0xc8);   // titles + brand
        const NVGcolor mintD = nvgRGB(0x3a, 0x5d, 0x55);   // dim A/B under the rings

        auto text = [&](float xmm, float ymm, float szmm, NVGcolor col, const char* s) {
            nvgFontSize(args.vg, mm2px(szmm));
            nvgFillColor(args.vg, col);
            Vec p = mm2px(Vec(xmm, ymm));
            nvgText(args.vg, p.x, p.y, s, NULL);
        };
        auto captions = [&](const PanelCtl* t, size_t n) {
            for (size_t i = 0; i < n; ++i)
                if (t[i].label[0])
                    text(t[i].mm.x, t[i].mm.y + glyphR(t[i].kind) + 2.5f, 2.0f, lbl, t[i].label);
        };
        captions(kParamCtls,  sizeof(kParamCtls)  / sizeof(kParamCtls[0]));
        captions(kInputCtls,  sizeof(kInputCtls)  / sizeof(kInputCtls[0]));
        captions(kOutputCtls, sizeof(kOutputCtls) / sizeof(kOutputCtls[0]));

        // section titles + brand (mirror res/gen_panel.py svg())
        const float CX = 106.680f, Hh = 128.5f;
        text(kLightCtls[0].mm.x, kLightCtls[0].mm.y + 1.f, 5.0f, mintD, "A");
        text(kLightCtls[1].mm.x, kLightCtls[1].mm.y + 1.f, 5.0f, mintD, "B");
        text(CX, 57.0f, 2.4f, mint, "ROOM");
        text(CX, 85.0f, 2.4f, mint, "CLOCK / MIX");
        text(CX, Hh - 2.5f, 4.5f, mint, "spotymod");
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
                case WK_SW3:
                    addParam(createParamCentered<CKSSThree>(pos, module, c.id)); break;
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
};

Model* modelSpotymod = createModel<Spotymod, SpotymodWidget>("Spotymod");
