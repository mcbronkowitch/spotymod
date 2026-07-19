#include "parts/part.h"

using namespace spky;

void Part::init(float sample_rate, uint32_t seed_base,
                float* echo_l, float* echo_r) {
    _sr = sample_rate;
    _mod.init(sample_rate, seed_base);
    _tone.init(sample_rate);
    _synth.set_seed(seed_base ^ 0x5eedC0DEu);   // per-part drift decorrelation
    _synth.init(sample_rate);
    _engine_id = ENGINE_SYNTH;                  // boot default (M2 spec)
    _pending_engine = _engine_id;
    _switching = false;
    _engine = _engine_for(_engine_id);
    _engine_fade.init(sample_rate);
    _engine_fade.set_on(true, true);            // boot: engine fully on
    _step_on = false;
    _engine->set_flow(true);                    // lanes boot in FLOW -> drone
    _last_master_hz = -1.f;                     // force a cycle forward on
                                                // the first process()
    _fx.init(sample_rate, echo_l, echo_r);
    _gate_len = static_cast<int>(sample_rate * 0.005f);
    _gate_ctr = 0;
    _inhibit = false;
    _note_suppressed = false;
    _ctrl_ctr = 0;                              // first process() runs a tick
    _quant.init(sample_rate, SynthEngine::kCtrlInterval);   // slew in ticks
    _pitch_q = _quant.process(pitch_pre_quant());
    _chord.init();
}

float Part::target_raw(int slot) const {
    // Master MOD (ex-DEPTH) shapes the texture only; the PITCH lane is the
    // anchor and keeps its per-slot depth alone (spec 2026-07-17 mod-tide).
    const float d = (slot == LANE_PITCH) ? 1.f : _depth;
    float mod = _active[slot] ? _mod.lane_output(slot) * d * _tdepth[slot] : 0.f;
    float v = clampf(_base[slot] + mod, 0.f, 1.f);
    // LEVEL floor (play-test rev 2026-07-17): modulation may duck the part to
    // at most 40% of its set level, never into silence. Relative to the base,
    // so a hand-muted part (base 0) stays silent; FILT's deliberate fade and
    // the other slots are untouched.
    const float floor_v = kLevelFloor * _base[slot];
    if (slot == LANE_LEVEL && v < floor_v) v = floor_v;
    return v;
}

// PITCH target + TUNE offset, summed BEFORE quantization so the final audible
// pitch always lands on the scale grid (tune is a bipolar +/-18-semi transpose,
// 0.5 = neutral). Quantizing the sum keeps both parts on one shared grid.
float Part::pitch_pre_quant() const {
    return clampf(target_raw(LANE_PITCH) + (_tune - 0.5f), 0.f, 1.f);
}

float Part::target_value(int slot) const {
    return slot == LANE_PITCH ? _pitch_q : target_raw(slot);
}

// Same combine rule as target_raw, tapped from the SAME lanes — the FX breathe
// in the part's own character. Never quantized (that is a PITCH-lane concern).
float Part::fx_target_value(int slot) const {
    float mod = _fx_active[slot]
        ? _mod.lane_output(slot) * _depth * _fx_depth[slot] : 0.f;
    return clampf(_fx_base[slot] + mod, 0.f, 1.f);
}

void Part::set_engine(EngineId e) {
    if (_switching ? e == _pending_engine : e == _engine_id) return;
    _pending_engine = e;
    _switching = true;
    _engine_fade.set_on(false);   // fade out; process() swaps at the idle point
}

void Part::set_step(bool on, int steps) {
    _step_on = on;
    _mod.set_step(on, steps);
    _engine->set_flow(!on);
}

void Part::trigger_manual() {
    _gate_ctr = _gate_len;
    float chord[ChordBuilder::kMaxNotes];
    const int n = _chord.build(target_value(LANE_PITCH), _chord_mask(),
                               _quant.root_semis(), chord);
    _engine->trigger_chord(chord, n);
}

float Part::max_voice_env() const {
    float m = 0.f;
    for (int v = 0; v < SynthEngine::kVoices; ++v) {
        const float e = voice_env(v);   // engine-qualified: 0 on test tone
        if (e > m) m = e;
    }
    return m;
}

// Everything the engine reads at its own control tick: the five lane targets,
// the quantized pitch, the chord surface. Task 4 gates this behind a
// 96-sample raster; until then it runs per sample and the output is
// bit-identical to the pre-extraction code.
void Part::_control_tick() {
    for (int i = 0; i < LANE_COUNT; ++i) _tg[i] = target_raw(i);
    _tg[LANE_PITCH] = _quant.process(pitch_pre_quant());
    _pitch_q = _tg[LANE_PITCH];                              // clean, drives pitch_cv()
    _tg[LANE_PITCH] = clampf(_pitch_q + _detune_cents * (1.f / 3600.f), 0.f, 1.f);

    // chord layer: refresh the surface every tick (cheap interval apply);
    // full voice-leading build only on a fire
    // COLOR is MOTION's third destination, alongside pan fan and drift (spec
    // 2026-07-18 color-motion-target). Bipolar additive: the knob is the
    // centre, MOTION swings +/-kColorMod around it at MOD = 1. The gate makes
    // COLOR = 0 exactly silent by construction.
    const float cgate = clampf(_color / kColorGate, 0.f, 1.f);
    const float cmod  = _active[LANE_MOTION]
        ? _mod.lane_output(LANE_MOTION) * _depth * kColorMod * cgate
        : 0.f;
    _color_eff = clampf(_color + cmod, 0.f, 1.f);
    _chord.set_color(_color_eff);
    float chord[ChordBuilder::kMaxNotes];
    const int nch = _chord.apply(_tg[LANE_PITCH], _chord_mask(),
                                 _quant.root_semis(), chord);
    _engine->set_chord(chord, nch);
}

void Part::process(float& outL, float& outR, float& sendL, float& sendR) {
    _mod.process();

    // click-free engine switch: fade out (4 ms) -> swap -> fade in (4 ms).
    // At hold the multiplier is exactly 1.0, so unswitched runs stay
    // bit-identical (M1.6 bypass invariant).
    const float fade = _engine_fade.process();
    if (_switching && _engine_fade.is_idle()) {
        _engine_id = _pending_engine;
        _engine = _engine_for(_engine_id);
        _engine->set_flow(!_step_on);                          // re-sync state
        _engine->set_hold(_inhibit);
        if (_last_master_hz > 0.f) _engine->set_cycle(1.f / _last_master_hz);
        _switching = false;
        _engine_fade.set_on(true);
    }

    // forward the master-lane cycle length on change, not per sample
    const float hz = _mod.master_hz();
    if (hz != _last_master_hz && hz > 0.f) {
        _last_master_hz = hz;
        _engine->set_cycle(1.f / hz);
    }

    const bool fired = _mod.lane_fired(LANE_PITCH);
    if (fired) {
        _note_suppressed = _inhibit;
        if (!_inhibit) _gate_ctr = _gate_len;
    }
    if (_gate_ctr > 0) --_gate_ctr;

    // Raster, plus an event refresh: a PITCH fire samples the lane at that
    // exact sample, so a tick-stale pitch is not "late", it is the wrong
    // note. The refresh deliberately does not re-phase _ctrl_ctr -- the
    // alignment with the engine's own tick is the point. The two branches
    // are mutually exclusive (else if, not a second if) because
    // _control_tick() is not idempotent -- it advances Quantizer::process's
    // slew and re-evaluates ChordBuilder::set_color's zone hysteresis -- so
    // a sample that is both a raster tick and a fire must call it once, not
    // twice, or the glide double-steps.
    //
    // Two consequences worth knowing about, neither a bug:
    // - A fire refresh is an extra Quantizer::process call one sample after
    //   a raster tick. Since Task 3 recalibrated the slew to count *calls*
    //   (a call now spans 96 samples), that refresh advances the glide by a
    //   full tick's worth. Bounded at one extra step per note and probably
    //   desirable, but not something the next reader should have to
    //   rediscover.
    // - The fire refresh only covers lane_fired(LANE_PITCH). SynthEngine's
    //   _auto_pending drone promise (synth_engine.cpp:243-245) also reads
    //   the chord surface, and a set_flow/set_hold transition landing
    //   mid-interval triggers against a surface up to 95 samples (~2 ms)
    //   stale. Musically negligible for rare knob transitions -- this is an
    //   accepted asymmetry, not something to fix here.
    if (_ctrl_ctr == 0) {
        _ctrl_ctr = SynthEngine::kCtrlInterval;
        _control_tick();
    } else if (fired) {
        _control_tick();
    }
    --_ctrl_ctr;

    _tg[LANE_LEVEL] = target_raw(LANE_LEVEL);   // per-sample engine consumer

    _engine->set_targets(_tg, _tune);

    if (fired && !_note_suppressed) {
        float chord[ChordBuilder::kMaxNotes];
        const int nch = _chord.build(_tg[LANE_PITCH], _chord_mask(),
                                     _quant.root_semis(), chord);
        _engine->trigger_chord(chord, nch);
    }
    _engine->process(outL, outR);
    outL *= fade;
    outR *= fade;

    float fxv[FXT_COUNT];
    for (int i = 0; i < FXT_COUNT; ++i) fxv[i] = fx_target_value(i);
    _fx.process(outL, outR, sendL, sendR, fxv);
}
