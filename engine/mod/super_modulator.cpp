#include "mod/super_modulator.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

namespace {
constexpr float kLaneRatio[LANE_COUNT] = { 2.f, 0.5f, 1.f, 0.75f, 1.5f };
} // namespace

void SuperModulator::init(float sample_rate, uint32_t seed_base) {
    _sr = sample_rate;
    for (int i = 0; i < LANE_COUNT; ++i) {
        _lanes[i].set_melodic(i == LANE_PITCH);
        _lanes[i].init(sample_rate, seed_base + static_cast<uint32_t>(i) * 2654435761u);
        _out[i] = 0.f;
    }
    _tick_ctr = 0;
    _pitch_scale = 1.f; _mod_scale = 1.f;
    _since_onset = 0;
    _onsets = 0;
    _gap[0] = _gap[1] = 0;
    _rhythm = RhythmView{};
    _update_rate();
}

void SuperModulator::_update_rate() {
    _base_hz = _synced ? division_hz(division_index(_rate_norm), _bpm)
                       : free_hz(_rate_norm);
    _apply_rate();
}

void SuperModulator::_apply_rate() {
    _master_hz = _base_hz * _pitch_scale;
    for (int i = 0; i < LANE_COUNT; ++i) {
        const float s = (i == LANE_PITCH) ? _pitch_scale
                                          : _mod_scale * _tide_mult;
        _lanes[i].set_rate_hz(_base_hz * s * kLaneRatio[i]);
    }
}

void SuperModulator::set_tide(float norm) {
    _tide_norm = clampf(norm, 0.f, 1.f);
    _update_tide();
}

void SuperModulator::_update_tide() {
    _tide_mult = _synced ? kTideRatios[tide_index(_tide_norm)]
                         : tide_free(_tide_norm);
    _apply_rate();
}

void SuperModulator::set_shape(float s)       { for (auto& l : _lanes) l.set_shape(s); }
void SuperModulator::set_smooth(float s)      { for (auto& l : _lanes) l.set_smooth(s); }
// RANGE is the melody ambitus: PITCH lane only. The texture lanes stay at
// full range (lane.h default 1.f); their amplitude is MOD's job at the
// target combine (spec 2026-07-17 mod-tide).
void SuperModulator::set_range(float r)       { _lanes[LANE_PITCH].set_range(r); }
void SuperModulator::set_variation(float v)   { for (auto& l : _lanes) l.set_variation(v); }
void SuperModulator::set_step(bool on, int n) { for (auto& l : _lanes) l.set_step(on, n); }
void SuperModulator::set_fixed_slew(bool on)  { for (auto& l : _lanes) l.set_fixed_slew(on); }

void SuperModulator::process() {
    // The PITCH lane is the anchor: per-sample, fires bit-identical to the
    // pre-rework engine. The four texture lanes advance on the 96-sample
    // raster (spec 2026-07-19 mod-plane-control-rate); the counter boots at
    // 0 so the first call ticks, which lands the mod tick on the same
    // samples as Part::_control_tick() -- the sole audio-path consumer reads
    // values that are 0 samples old. That claim holds on the shared boot
    // grid; after an off-grid engine swap Part re-arms its own raster
    // (_ctrl_ctr) while this counter does not follow, so the two de-phase --
    // up to 95 samples (~2 ms) of texture staleness at the read until the
    // grids happen to coincide again. Documented as the spec's "Accepted
    // asymmetry"; see also part.h.
    _out[LANE_PITCH] = _lanes[LANE_PITCH].process();

    // Onset-gap ring, moved up from ModLane (see the field comments in
    // super_modulator.h): fed only by LANE_PITCH's per-sample process()
    // above, since that is the only path that ever drives it.
    if (_since_onset < kSinceOnsetMax) ++_since_onset;
    // Latch BEFORE record, exactly mirroring ModLane's old internal order
    // (_wrap_events() ran before that same wrap's _on_boundary() in both
    // process() and tick() -- lane.cpp). Latching first publishes the ring
    // as it stood BEFORE this sample's own onset (if any) is folded in, so
    // a wrap that also happens to be a fresh onset still publishes the
    // PREVIOUS cycle's gaps, not one that already includes itself.
    if (_lanes[LANE_PITCH].wrapped()) {
        _rhythm.gap[0] = _gap[0];
        _rhythm.gap[1] = _gap[1];
        _rhythm.valid  = _onsets >= 3;
    }
    if (_lanes[LANE_PITCH].fired()) {
        _gap[1] = _gap[0];
        // Clamp to 1: a gap of 0 is not a duration -- see rhythm_view.h.
        // Currently unreachable: the increment a few lines up (this
        // function) runs unconditionally before this read, on every call,
        // and this line is the only place `_since_onset` is reset to 0 -- so
        // `_since_onset >= 1` always holds by the time it lands here, and
        // the `: 1` branch never executes. Kept as a guard for a
        // hypothetical future `tick()`-fed accumulator, not because this one
        // needs it -- do not read this clamp as license to drop the
        // increment above.
        _gap[0] = _since_onset > 0 ? _since_onset : 1;
        _since_onset = 0;
        if (_onsets < 3) ++_onsets;
    }

    if (_tick_ctr == 0) {
        _tick_ctr = ModLane::kTickInterval;
        for (int i = 0; i < LANE_COUNT; ++i)
            if (i != LANE_PITCH) _out[i] = _lanes[i].tick();
    }
    --_tick_ctr;
}

void SuperModulator::spot(Rng& rng) {
    // SPOT stumbles every lane EXCEPT the PITCH master lane: the melody is the
    // anchor everything else stumbles around, so pitch is never a target of the
    // stumble. Draw a kick for every lane so the RNG stream stays independent
    // of which lanes are skipped.
    for (int i = 0; i < LANE_COUNT; ++i) {
        float dphase = rng.next_bipolar() * 0.5f;    // uniform +/- 1/2 cycle
        float dshape = rng.next_bipolar() * 0.35f;   // uniform +/- 0.35
        if (i != LANE_PITCH) _lanes[i].kick(dphase, dshape);
    }
}
