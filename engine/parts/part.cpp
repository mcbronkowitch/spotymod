#include "parts/part.h"

using namespace spky;

// The mod tick must ride the same raster the engine control tick uses --
// Part::_control_tick() reads texture lane outputs the sample they are
// produced (spec 2026-07-19 mod-plane-control-rate).
static_assert(ModLane::kTickInterval == SynthEngine::kCtrlInterval,
              "mod tick interval must equal the engine control raster");

void Part::init(float sample_rate, uint32_t seed_base,
                float* echo_l, float* echo_r,
                SampleBuffer::Frame* sampler_mem, size_t sampler_frames) {
    _sr = sample_rate;
    _mod.init(sample_rate, seed_base);
    _tone.init(sample_rate);
    _synth.set_seed(seed_base ^ 0x5eedC0DEu);   // per-part drift decorrelation
    _synth.init(sample_rate);
    _sampler.set_seed(seed_base ^ 0x5A11E20Du);
    _sampler.set_memory(sampler_mem, sampler_frames);
    _sampler.init(sample_rate);
    _last_gate = false;
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

// PITCH target + TUNE offset (a bipolar +/-18-semi transpose, 0.5 = neutral).
// On a SYNTH part the sum is quantized afterwards, so the final audible pitch
// lands on the scale grid and both parts share one grid. On a SAMPLER part it
// is used as-is -- see the quantizer comment in _control_tick.
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
    // Durch _flatten_for_sampler, genau wie der Fire-Pfad in process()
    // (part.cpp:306). Ohne das landeten bei COLOR > 0 bis zu vier Toene in
    // der SamplerEngine, bis der naechste _control_tick (<= 96 Samples) ueber
    // set_chord korrigiert -- weit genug fuer rund ein Dutzend Spawns mit
    // Oktavspruengen beim TRIG-Druck, auf einem Deck, das ausdruecklich EINE
    // Tonhoehe halten soll. Auf einer Synth-Part gibt der Helper nch
    // unveraendert zurueck, dort aendert sich also nichts.
    _engine->trigger_chord(chord, _flatten_for_sampler(chord, n));
}

float Part::max_voice_env() const {
    float m = 0.f;
    for (int v = 0; v < SynthEngine::kVoices; ++v) {
        const float e = voice_env(v);   // engine-qualified: 0 on test tone
        if (e > m) m = e;
    }
    return m;
}

// Everything the engine and FX read at their own control rate: the five lane
// targets, the quantized pitch, the chord surface, the set_targets push, and
// the five FX target values. Runs on the SynthEngine::kCtrlInterval-sample
// raster, phase-aligned with SynthEngine's own control tick (see the
// _ctrl_ctr comment in part.h). Not idempotent -- it advances
// Quantizer::process's slew and re-evaluates ChordBuilder::set_color's zone
// hysteresis -- so process()'s raster-tick and fire-refresh branches must
// stay mutually exclusive (else if, not a second if); calling this twice on
// the same sample double-steps the glide.
void Part::_control_tick() {
    for (int i = 0; i < LANE_COUNT; ++i) _tg[i] = target_raw(i);

    const float pitch_raw = pitch_pre_quant();
    // Called unconditionally, even when the sampler discards the result: the
    // quantizer carries a slew counter and a hysteresis note across calls, and
    // skipping it while a part is on the sampler would leave that state frozen
    // and make the first synth tick after an engine switch depend on how long
    // the part spent as a sampler. Cheap, and it keeps the synth's behaviour
    // exactly what it was.
    const float pitch_quantized = _quant.process(pitch_raw);

    // The SAMPLER does not quantize. The quantizer is a melody device: it snaps
    // a lane's pitch onto the scale so composed notes land in key. The sampler
    // has no melody -- the morphagene-controls work switched its PITCH lane off
    // -- and TUNE there means one thing only: transpose this recording as a
    // whole, to match material that may be tonal, atonal, or plainly out of
    // tune. Snapping that to the instrument's scale is meaningless, and at the
    // knob's centre it was actively wrong: 0.5 of a 36-semitone span is exactly
    // 18 semitones, a tritone above the root, which most scales do not contain.
    // Measured at TUNE 0.5 with the PITCH lane off: three of the eight scales
    // snapped the "neutral" detent a semitone flat, one a semitone sharp, and
    // only four left it at unity -- so recorded material played back off-pitch
    // against its own source, and which way depended on the SCALE knob.
    // Unquantized, the centre is exactly 1.0 for every scale and root, and the
    // knob transposes continuously (the author's call: out-of-tune material has
    // to be tunable to the key, which a semitone grid cannot do).
    _pitch_q = _engine_id == ENGINE_SAMPLER ? pitch_raw : pitch_quantized;
    _tg[LANE_PITCH] = clampf(_pitch_q + _detune_cents * (1.f / 3600.f), 0.f, 1.f);

    // MOTION's Scatter startet auf einem Sampler-Deck bei null, nicht bei der
    // Lane-Basis 0.5. Dieselbe Schicht und dieselbe Begruendung wie
    // _flatten_for_sampler und die abgeschaltete PITCH-Lane: die INSTRUMENT-
    // Schicht entscheidet, was ein Sampler-Deck nicht tut.
    //
    // Der Grund ist messbar, nicht aesthetisch. Die Basis 0.5 schreibt
    // niemand -- weder Host noch Instrument -- und SuperModulator::set_range
    // trifft nur LANE_PITCH, die Texturlanes behalten also _range = 1. Bei
    // MOD = 0 stand _targets[LANE_MOTION] damit unabaenderlich auf 0.5, und
    // in SamplerEngine::_spawn_one ist der Positions-Jitter dann
    // gleichverteilt ueber ein Intervall der Breite GENAU content. Damit ist
    // (SOURCE*span + _scan_pos + jitter) mod content exakt gleichverteilt,
    // unabhaengig von beiden Summanden: ORGANIZE und SCAN hatten auf die
    // Spawn-Position nachweislich null Effekt (gemessen: Mittelwert 12036 /
    // 11896 / 11951 bei SOURCE 0 / 0.25 / 0.9 ueber content 24000).
    //
    // Nur der Basisanteil faellt weg, die Lane-Modulation bleibt: bei MOD > 0
    // schiebt sie von 0 nach oben, MOD wird also zum MOTION-Regler des Decks
    // und der Nebel bleibt erreichbar.
    //
    // Bewusst an _tg und nicht an _base: COLOR (cmod) und DENS (omod) unten
    // lesen _mod.lane_output(LANE_MOTION) direkt und bleiben davon unberuehrt.
    //
    // Fuer Szenario-Autoren (review 2026-07-22, F-04-Nachtrag): auf einem
    // Sampler-Deck ist set_target_base(part, LANE_MOTION, …) damit absichtlich
    // wirkungslos, egal welchen Wert man ihm gibt -- der Regler, der MOTION
    // hier tatsaechlich bewegt, ist MOD (set_depth), nicht die Lane-Basis. Ein
    // Szenario, das stattdessen die Basis walkt, rendert an allen Punkten
    // dieselbe Audio (traf host/render/scenarios/sampler_extremes.json genau
    // so, bevor es korrigiert wurde).
    //
    // Die Lane selbst bleibt bipolar: _mod.lane_output(LANE_MOTION) liefert
    // [-1, 1] (_range bleibt 1, siehe oben), und clampf(mmod, 0.f, 1.f) unten
    // kappt die untere Haelfte komplett weg, statt sie -- wie die alte
    // 0.5+mod-Formel -- symmetrisch um einen Mittelpunkt zu falten. Hoerbar
    // heisst das: der Scatter PULSIERT (steht die halbe Modulationsperiode
    // lang exakt bei 0 und schiesst dann in einen positiven Ausschlag),
    // statt gleichmaessig zu ATMEN. Zwei Alternativen, falls das nicht die
    // gewuenschte Form ist: fabsf(mmod) fuer eine kontinuierliche
    // Vollratenversion (beide Halbwellen tragen bei, nie ein Stillstand),
    // oder eine reskalierte bipolare Abbildung (0.5f + 0.5f*mmod), die wieder
    // atmet statt zu pulsen. Der harte Clamp hier ist die aktuell gehoerte
    // und vorlaeufig akzeptierte Fassung (Variante a) -- welche der drei am
    // Ende bleibt, ist eine Hoerentscheidung, keine, die dieser Kommentar
    // trifft.
    if (_engine_id == ENGINE_SAMPLER) {
        const float mmod = _active[LANE_MOTION]
            ? _mod.lane_output(LANE_MOTION) * _depth * _tdepth[LANE_MOTION]
            : 0.f;
        _tg[LANE_MOTION] = clampf(mmod, 0.f, 1.f);
    }

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

    // DENS -> grain overlap, with MOTION's swing on top (spec 2026-07-21
    // morphagene-controls). Pushed straight at _sampler rather than through
    // _engine: it is a sampler-only parameter, and _sampler is a concrete
    // member here just as it is for the voice row (part.h). On a synth part
    // this is one float store into an engine nobody is listening to.
    const float omod = _active[LANE_MOTION]
        ? _mod.lane_output(LANE_MOTION) * _depth * kOverlapMod
        : 0.f;
    _overlap_eff = clampf(_overlap + omod, 0.f, 1.f);
    _sampler.set_overlap(_overlap_eff);

    // Slice-groove side channel (spec 2026-07-22): the step clock rides the
    // same raster as every other engine push. Same idiom as set_overlap --
    // sampler-only, pushed at _sampler directly.
    // FEEL (spec 2026-07-23) rides the same push: COLOR reaches the sampler
    // RAW, not as _color_eff. The MOTION swing on COLOR (kColorMod, computed
    // just above) is right for the chord -- a breathing chord size is the
    // point there -- and wrong for accents: an accent depth that breathes
    // would be exactly the hidden coupling this spec exists to remove.
    if (_engine_id == ENGINE_SAMPLER) {
        _sampler.set_step_clock(_mod.pitch_step_samples());
        _sampler.set_feel(_color);
    }

    float chord[ChordBuilder::kMaxNotes];
    // apply() runs unconditionally even when the sampler discards its result:
    // ChordBuilder carries zone hysteresis and voice-leading state across
    // calls, and skipping it on a sampler part would freeze that state and
    // make the first synth tick after an engine switch depend on how long the
    // part had been a sampler. Same reasoning as the quantizer call above.
    int nch = _chord.apply(_tg[LANE_PITCH], _chord_mask(),
                           _quant.root_semis(), chord);
    nch = _flatten_for_sampler(chord, nch);
    _engine->set_chord(chord, nch);

    // Raster-rate push is safe because both receivers smooth on their own
    // side and neither ever sees the 96-sample staircase raw: SynthEngine::
    // process runs _targets[LANE_LEVEL] through a ~10 ms smoother, and
    // PartFx::process runs each of the five FX values through a 2 ms
    // smoother. Do not "optimize away" that smoothing -- it is what makes
    // this raster hold inaudible.
    _engine->set_targets(_tg, _tune);
    for (int i = 0; i < FXT_COUNT; ++i) _fxv[i] = fx_target_value(i);
}

void Part::process(float inL, float inR, float& outL, float& outR,
                   float& sendL, float& sendR) {
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
        _engine->set_gate(_last_gate);   // the freshly swapped-in engine
        if (_last_master_hz > 0.f) _engine->set_cycle(1.f / _last_master_hz);
        _switching = false;
        _engine_fade.set_on(true);
        // A freshly swapped-in engine holds none of the previous engine's
        // pushed state -- set_targets()/set_chord() never reached it while
        // it was inactive -- so re-arm the raster to run _control_tick()
        // later in THIS SAME process() call, rather than up to
        // SynthEngine::kCtrlInterval - 1 samples from now on its power-on
        // defaults (e.g. TestToneEngine's 220 Hz _freq, test_tone_engine.h).
        _ctrl_ctr = 0;
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
    //   a raster tick. Quantizer::process's slew counts *calls*, and each
    //   call now spans SynthEngine::kCtrlInterval samples, so that refresh
    //   advances the glide by a full tick's worth. Bounded at one extra step
    //   per note and probably desirable, but not something the next reader
    //   should have to rediscover.
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

    if (fired && !_note_suppressed) {
        if (_engine_id == ENGINE_SAMPLER) {
            const int slot = _mod.pitch_cur_step();
            _sampler.set_phrase_pos(slot, _mod.pitch_steps(),
                                    pg_metric_weight(slot));
        }
        float chord[ChordBuilder::kMaxNotes];
        // build() unconditionally, for the same state reason as apply() in
        // _control_tick.
        int nch = _chord.build(_tg[LANE_PITCH], _chord_mask(),
                               _quant.root_semis(), chord);
        nch = _flatten_for_sampler(chord, nch);
        _engine->trigger_chord(chord, nch);
    }

    // Composed gate, forwarded on edges only (see engine_iface.h). Computed
    // after _gate_ctr has been advanced, so it reflects THIS sample.
    const bool g = gate();
    if (g != _last_gate) {
        _last_gate = g;
        _engine->set_gate(g);
    }

    _engine->process_in(inL, inR);
    _engine->process(outL, outR);
    outL *= fade;
    outR *= fade;

    _fx.process(outL, outR, sendL, sendR, _fxv);
}
