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
    dsp::BooleanTrigger captureTrig[2], triggerTrig[2], spotTrig, settleTrig;
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
                    if (c.id == SCALE)
                        configParam(c.id, 0.f, (float)(spky::SCALE_LIST_COUNT - 1), 0.f, "Scale");
                    else  // STEPS_A / STEPS_B
                        configParam(c.id, 2.f, 16.f, 8.f, "Steps");
                    getParamQuantity(c.id)->snapEnabled = true;
                    break;
                case WK_SW3:
                    configSwitch(c.id, 0.f, 2.f, 0.f, "Sync", {"Free", "Sync", "Triplet"});
                    break;
                case WK_LATCH:
                    if (c.id == ENGINE_A || c.id == ENGINE_B)
                        configSwitch(c.id, 0.f, 1.f, 0.f, "Engine", {"Synth", "Test tone"});
                    else if (c.id == GRITMODE_A || c.id == GRITMODE_B)
                        configSwitch(c.id, 0.f, 1.f, 0.f, "Grit mode", {"Drive", "Reduce"});
                    else  // STEP / REPLAY
                        configSwitch(c.id, 0.f, 1.f, 0.f, lbl, {"Off", "On"});
                    break;
                case WK_SMBTN: configButton(c.id, lbl); break;
                default: break;
            }
        }
        for (const auto& c : kInputCtls)  configInput(c.id, c.label);
        for (const auto& c : kOutputCtls) configOutput(c.id, c.label);
    }

    static float defaultFor(int id) {
        if (id == TEMPO)  return 0.4f;   // ~120 BPM on the 40..240 map
        if (id == DEPTH_A || id == DEPTH_B) return 0.6f;
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
            inst.set_probability(p, pp(PROB_A, p));
            inst.set_smooth(p, pp(SMOOTH_A, p));
            inst.set_range(p, pp(RANGE_A, p));
            inst.set_entropy(p, pp(ENTROPY_A, p));            // -1..+1
            inst.set_depth(p, pp(DEPTH_A, p));
            inst.set_tune(p, pp(TUNE_A, p));

            inst.set_voice_attack(p, pp(ATTACK_A, p));
            inst.set_voice_decay(p, pp(DECAY_A, p));
            inst.set_voice_resonance(p, pp(RES_A, p));
            inst.set_voice_sub(p, pp(SUB_A, p));
            inst.set_voice_detune(p, pp(DETUNE_A, p));

            inst.set_flux_mix(p, pp(FLUX_A, p));
            inst.set_grit_mix(p, pp(GRIT_A, p));
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
            inst.set_replay(p, ppb(REPLAY_A, p));

            if (captureTrig[p].process(ppb(CAPTURE_A, p))) inst.capture_now(p);
            if (triggerTrig[p].process(ppb(TRIGGER_A, p))) inst.trigger_manual(p);
        }

        inst.set_morph(params[MORPH].getValue());
        inst.set_couple(params[COUPLE].getValue());
        inst.set_drift(params[DRIFT].getValue());
        inst.set_reverb_size(params[REV_SIZE].getValue());
        inst.set_reverb_decay(params[REV_DECAY].getValue());
        inst.set_reverb_tone(params[REV_TONE].getValue());
        inst.set_reverb_depth(params[REV_DEPTH].getValue());
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

// --- widget -------------------------------------------------------------------
struct SpotymodWidget : ModuleWidget {
    SpotymodWidget(Spotymod* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Spotymod.svg")));

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
