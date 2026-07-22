#include "sampler/sample_buffer.h"
#include "sampler/sampler_config.h"
#include "util/math.h"
#include "util/fast_tanh.h"
#include <cmath>
#include <cstring>

namespace spky {

namespace {
constexpr float kFadeKof = 1.f / static_cast<float>(sampler_cfg::kRecordFade);
}

void SampleBuffer::init(Frame* buf, size_t length, float sample_rate) {
    _buffer      = buf;
    _buffer_size = buf ? length : 0;
    // Deliberately NOT src/core/buffer.cpp:13's `_feedback = 0.95` (a linear
    // factor, ~-0.45 dB, despite that line's own "-3dB" comment). Here
    // kDefaultFeedback is a knob position (sampler_config.h) run through the
    // same -60..0 dB curve as set_feedback(), giving a linear factor of
    // ~0.708 (-3 dB) -- the self-consistent reading, but an intentional
    // behavioural change from the original's inconsistent one.
    _feedback    = std::pow(10.f, (60.f * (sampler_cfg::kDefaultFeedback - 1.f)) * 0.05f);
    // _cut is a SoftSwitch and its 4 ms fade timing (_kof) is meaningless
    // until init()'d with a sample rate. Left uninitialized, _kof defaults to
    // 1.0 (fx_util.h), so process()'s hann_value_at() call is fed integer
    // positions instead of a 0..1 ramp and reads past the end of the static
    // 192-entry Hann table -- undefined behavior, reproduced as an
    // out-of-bounds assertion under the MSVC STL. This line is not one of
    // the plan's six listed changes; it was a silent omission in the
    // brief's draft, restored here rather than left as UB in the audio path.
    _cut.init(sample_rate);   // REQUIRED: see plan Task 1, change 7
    clear();
}

void SampleBuffer::set_feedback(float knob) {
    // Control rate only -- std::pow must never reach the per-sample path.
    knob = clampf(knob, 0.f, 1.f);
    float dbfs;
    if (knob <= sampler_cfg::kFbKnee) {
        // Unchanged from M5a: -60..-6 dB over the first 90% of travel, so
        // every setting already listened to stays where it was.
        dbfs = 60.f * (knob - 1.f);
    } else {
        // Above the knee: -6 dB on to kFbMaxDb, crossing unity around 0.971.
        const float t = (knob - sampler_cfg::kFbKnee) / (1.f - sampler_cfg::kFbKnee);
        dbfs = lerpf(60.f * (sampler_cfg::kFbKnee - 1.f), sampler_cfg::kFbMaxDb, t);
    }
    _feedback = std::pow(10.f, dbfs * 0.05f);
}

void SampleBuffer::set_recording(bool on) {
    if (!valid()) return;
    switch (_state) {
        case State::idle:
            if (on) {
                // No playhead in a cloud, so a record punches in at frame 0
                // rather than at a read cursor (plan Task 1, change 6).
                // Unconditional: the branch this replaced evaluated to 0 in
                // both arms for every reachable state, while its comment
                // claimed frame 0 and its code said _size.
                _write_head = 0;
                _state     = State::fadein;
                _fade_ctr  = 0;
            }
            break;
        case State::fadeout:
            // Punch-in mitten im Fade-out: zurueck in den Fade-in, mit dem
            // Zaehler, der gerade steht. Das ist symmetrisch zu dem, was der
            // umgekehrte Weg unten schon tut (Stopp im Fade-in uebernimmt den
            // Teilzaehler), und die Hann-Kurve blendet einfach von dem Pegel
            // wieder auf, den sie erreicht hat -- kein Sprung.
            //
            // Vorher wurde der Wunsch verworfen, der Fade-out lief zu Ende
            // und cut() nagelte die Loop-Laenge fest. Der VCV-Host vergleicht
            // level-basiert gegen is_recording(), das im Fade-out noch true
            // liefert, sodass der Re-Arm hier gar nicht mehr ankam und
            // stattdessen ein Overdub des gekuerzten Loops begann: ein
            // REC-Doppelklick innerhalb von 4 ms kuerzte die Aufnahme
            // dauerhaft.
            //
            // _write_head zusaetzlich zurueck auf _fadeout_resume_head: ohne
            // das blieb er auf der 0, auf die der Stopp-Zweig unten ihn fuer
            // die Kopf-Ueberblendung gerade erst gezogen hatte, und die neue
            // Aufnahme schrieb von vorn -- eine Splice statt einer Fortsetzung.
            // Mit fb = 1 (Feedback-Uebergang steht noch auf aus, _cut ist
            // nicht an) summiert write() dort obendrein, statt zu ersetzen, so
            // dass der Anfang der ersten Aufnahme mit dem Anfang der zweiten
            // verschmolz (review 2026-07-22, F-08 Nachtrag). Der gestashte
            // Wert ist unabhaengig davon immer korrekt: lief der Stopp-Zweig
            // NICHT zurueck (schon im Loop, _cut.is_on()), ist er identisch
            // mit dem aktuellen _write_head, die Zeile also ein No-op.
            if (on) {
                _state      = State::fadein;
                _write_head = _fadeout_resume_head;
            }
            break;
        default:
            if (!on) {
                _state = State::fadeout;
                // Stash BEFORE the rewind below can zero it -- the only
                // record of where this take actually was, needed if a
                // punch-in interrupts the fade-out (see the fadeout case
                // above). Unconditional: when the rewind does not fire
                // (_cut.is_on()) this just captures the same value again.
                _fadeout_resume_head = _write_head;
                // Wrap around and fade out OVER the fade-in, so the loop
                // point is a real crossfade rather than two dips with a
                // seam between them. This matters MORE here than in the
                // original: read_linear folds, so every grain that wanders
                // across the seam runs through it (src/core/buffer.cpp:45).
                if (!_cut.is_on()) _write_head = 0;
            }
            break;
    }
}

void SampleBuffer::write(float in0, float in1) {
    if (!valid()) return;

    float fade = 1.f;
    switch (_state) {
        case State::idle:
            return;
        case State::sustain:
            break;
        case State::fadein:
            fade = hann_value_at(static_cast<float>(_fade_ctr) * kFadeKof);
            if (++_fade_ctr >= sampler_cfg::kRecordFade - 1) _state = State::sustain;
            break;
        case State::fadeout:
            fade = hann_value_at(static_cast<float>(_fade_ctr) * kFadeKof);
            if (_fade_ctr == 0) {
                // Stopped with nothing written -- two REC toggles inside one
                // audio block. Reachable here though not in the original:
                // the render host applies all scenario events sharing a
                // timestamp back-to-back before the next process() call, and
                // a plugin host can toggle REC twice within one block.
                //
                // Return to idle WITHOUT cutting. Two bugs are guarded at
                // once: _fade_ctr is size_t, so `--_fade_ctr` here would wrap
                // to SIZE_MAX and strand the buffer in fadeout forever; and
                // cut() at _size == 0 would lock the loop at zero length,
                // after which the _cut.is_on() branch below pins _write_head
                // to 0 and _size can never grow -- the part could never
                // record again until clear(). Nothing was recorded, so there
                // is nothing to lock.
                _state = State::idle;
                return;
            }
            if (--_fade_ctr == 0) { cut(); _state = State::idle; return; }
            break;
    }

    // Feedback only bites where the fade is open, so a fading edge never
    // scrubs content it is not yet writing to. (Original: buffer.cpp:142-143.)
    const float fb = lerpf(1.f, _feedback, _cut.process());
    // The ceiling follows the commanded coefficient rather than sitting at
    // 1: above unity this term interpolates from 1 (fade shut) to _feedback
    // (fade open), and a hard [0,1] clamp here would silently cancel the
    // bloom while looking like a safety guard. The floor of 0 is the real
    // guard and stays.
    const float fb_ceil = _feedback > 1.f ? _feedback : 1.f;
    const float fb_fade = clampf(1.f - fade * (1.f - fb), 0.f, fb_ceil);

    Frame f = _buffer[_write_head];
    // Saturate what was read back BEFORE the feedback multiply -- the order
    // that keeps EchoDelay::Process stable at its 1.2 coefficient
    // (engine/fx/flux.h:129-141). Saturating after the multiply would not
    // bound the write. Nicht unbedingt: fast_tanh komprimiert ab etwa halber
    // Aussteuerung hoerbar, und jeder Overdub bekaeme sonst einen
    // Tape-Charakter, den er heute nicht hat.
    //
    // Die Schwelle liegt bei kFbSatKnee und NICHT bei 1.0. An 1.0 gebunden
    // war der Bereich knapp darunter voellig unbegrenzt -- ein Integrator
    // mit Fixpunkt in/(1-fb) -- und lauter als jede Einstellung darueber.
    if (_feedback > sampler_cfg::kFbSatKnee) {
        f.l = fast_tanh(f.l);
        f.r = fast_tanh(f.r);
    }
    f.l = in0 * fade + f.l * fb_fade;
    f.r = in1 * fade + f.r * fb_fade;
    // Ein einziges nicht-endliches Sample bliebe sonst fuer immer: der
    // Overdub liest es zurueck, multipliziert und schreibt es wieder, und nur
    // clear() heilt das. In VCV kann es vom Nachbarmodul kommen. Zwei
    // Vergleiche pro Sample, und sie fangen NaN wie Inf (jeder Vergleich mit
    // NaN ist falsch, also greift die Negation).
    if (!(f.l > -1e6f && f.l < 1e6f)) f.l = 0.f;
    if (!(f.r > -1e6f && f.r < 1e6f)) f.r = 0.f;
    _buffer[_write_head] = f;

    ++_write_head;
    if (_cut.is_on()) {
        if (_write_head >= _size) _write_head = 0;        // locked loop
    } else if (_write_head >= _buffer_size) {             // capacity reached
        _size = _buffer_size;
        cut();
    } else {
        if (_write_head > _size) _size = _write_head;     // free-run growth
    }
}

void SampleBuffer::cut() {
    if (_cut.is_on()) return;
    _cut.set_on(true);
    _write_head = 0;
}

void SampleBuffer::set_rec_size(size_t frames) {
    if (!valid()) return;
    if (frames == 0) {
        // Never cut() at zero length: that locks the loop at zero, after
        // which write()'s locked-loop branch pins _write_head to 0 and
        // _size can never grow -- the part is permanently unable to record
        // until clear(). Reachable through the load path (an empty or
        // zero-length WAV), which is why this is not merely defensive.
        clear();
        return;
    }
    _size = frames > _buffer_size ? _buffer_size : frames;
    cut();
}

void SampleBuffer::clear() {
    // Skip the memset when the buffer is already empty (_size == 0): init()
    // always ends in clear() before content can exist, so an empty buffer
    // is already zeroed (or was never written), and _size == 0 means
    // read_linear() returns silence unconditionally regardless of what the
    // memory holds -- nothing past the write head is reachable either way.
    // This is the fix for I-3: on a 42 s buffer the unconditional memset was
    // ~1.5-3 ms inside one process() call, and the factory-drone autoload
    // (Spotymod.cpp pushParams) hits exactly this empty-buffer case on the
    // audio thread. Every other line below stays unconditional -- a buffer
    // that DID hold content (_size != 0) still gets the full memset.
    if (_buffer && _buffer_size && _size != 0)
        std::memset(_buffer, 0, sizeof(Frame) * _buffer_size);
    _write_head          = 0;
    _size                = 0;
    _fade_ctr            = 0;
    _fadeout_resume_head = 0;
    _state               = State::idle;
    _cut.set_on(false, true);
}

void SampleBuffer::read_linear(float frame, float& out0, float& out1) const {
    // Empty or un-init'ed: silence. The original divides by zero on the
    // first branch and spins forever on the second (plan Task 1, change 5).
    if (_size == 0 || _buffer == nullptr) { out0 = 0.f; out1 = 0.f; return; }

    // NaN or an absurd magnitude: silence, same contract. Two compares, and
    // NaN fails both (every NaN comparison is false), so this closes the
    // static_cast<size_t>(NaN) undefined behaviour below AND bounds the fold
    // loops for any finite input. Deliberately NOT fmodf: eight grains x two
    // parts x 48 kHz is ~768k fmodf/s, which is real CPU on the M6 hardware
    // for a case that cannot currently occur.
    if (!(frame > -1e9f && frame < 1e9f)) { out0 = 0.f; out1 = 0.f; return; }

    const float fsz = static_cast<float>(_size);
    // O(1) fold, replacing the original's `frame %= _size` without an
    // integer division. NOT a subtract loop: that would be O(frame/_size),
    // and a grain holding a position latched against a longer previous
    // content length (set_rec_size shrinks it, or a load lands mid-cloud)
    // would then spin hundreds of thousands of times on the audio thread.
    // std::floor is a single instruction on both targets -- roundsd on x86,
    // VRINTM on the Daisy's Cortex-M7 FPv5 -- not a libm call, and cheaper
    // than the loop even in the common case.
    frame -= fsz * std::floor(frame / fsz);
    // BEIDE Kanten hier, vor i0 und frac, und nicht nur i0 spaeter: bei
    // grossem fsz rundet fsz - epsilon in float32 auf exakt fsz, was ein
    // knapp negatives frame genau hierher bringt. Wurde nur i0 korrigiert,
    // blieb frac = fsz stehen und die Interpolation lief mit dem Faktor der
    // Puffergroesse -- an einem Puffer mit nur +-0.3 Inhalt gemessene
    // Ausgaenge von 14 400 (24 000 Frames) bis 1 209 600 (2 016 000 Frames).
    // Erreichbar ueber REVERSE-Grains, sobald _start + _off unter 0 laeuft
    // (grain.h:111). frame und i0 muessen dieselbe Zahl beschreiben.
    if (!(frame >= 0.f) || frame >= fsz) frame = 0.f;

    size_t i0 = static_cast<size_t>(frame);
    if (i0 >= _size) i0 = 0;                     // Guertel zum Hosentraeger
    size_t i1 = i0 + 1;
    if (i1 >= _size) i1 = 0;

    const float frac = frame - static_cast<float>(i0);
    const Frame a = _buffer[i0];
    const Frame b = _buffer[i1];
    out0 = a.l + frac * (b.l - a.l);
    out1 = a.r + frac * (b.r - a.r);
}

}  // namespace spky
