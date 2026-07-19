#pragma once
#include "fx/comp.h"
#include "fx/flux.h"
#include "fx/grit.h"
#include "util/onepole.h"

namespace spky {

// The second target row: pad slot in the FX layer == lane index, mirroring the
// engine targets' "lane index == pad slot == target slot" principle (spec).
enum FxTargetId {
    FXT_GRIT_INT  = 0,   // lane 0 (x2)    fast rhythmic texture
    FXT_FLUX_TIME = 1,   // lane 1 (x1/2)  slow tape drift / dub steps
    FXT_FX_MIX    = 2,   // lane 2 (x1)    accents locked to the melody cycle
    FXT_REV_SEND  = 3,   // lane 3 (x3/4)  polyrhythmic breathing
    FXT_FLUX_FB   = 4,   // lane 4 (x3/2)  swells
    FXT_COUNT     = 5
};

enum class FxBlock { Flux, Grit };

// Per-part chain: GRIT -> FLUX -> FX MIX -> COMP, plus the post-COMP reverb
// send tap (M4.6: comp BEFORE the tap — dry and send are compressed and
// auto-gained together, so full-wet profits fully).
// FX MIX is a linear (equal-gain) dry/wet — dry and wet are correlated, and
// bypass must be bit-exact (wet == dry => out == dry). The square-law XFade
// stays inside Drive/Reduce where it belongs. When both blocks are off the
// whole chain is skipped, so "FX off" costs nothing and changes nothing.
class PartFx {
public:
    // `dust_seed`: forwarded to Flux::init -- see its comment for why this is
    // a caller-supplied constant, not derived from echo_l/echo_r's address.
    void init(float sample_rate, float* echo_l, float* echo_r,
              uint32_t dust_seed);

    Grit& grit() { return _grit; }
    Flux& flux() { return _flux; }
    const Grit& grit() const { return _grit; }
    const Flux& flux() const { return _flux; }
    Comp& comp() { return _comp; }
    const Comp& comp() const { return _comp; }
    void set_comp(float n) { _comp.set_amount(n); }

    void set_fx_on(FxBlock b, bool on, bool immediate = false);
    void set_grit_mode(GritMode m) { _grit.set_mode(m); }
    void set_flux_mix(float n) { _flux.set_mix(n); }
    void set_grit_mix(float n) { _grit.set_mix(n); }
    void set_bpm(float bpm)           { _flux.set_bpm(bpm); }
    void set_flux_rate(int slice_idx) { _flux.set_rate(slice_idx); }
    void set_dust(float n) { _flux.set_dust(n); }
    void set_rot(float n)  { _flux.set_rot(n); }
    void sync_beat(float beat_samples) { _flux.sync_beat(beat_samples); }

    // fxv[FXT_COUNT]: already-modulated values from Part::fx_target_value().
    void process(float& l, float& r, float& send_l, float& send_r,
                 const float* fxv);

private:
    Grit _grit;
    Flux _flux;
    Comp _comp;
    OnePole _smooth[FXT_COUNT];
    float _grit_applied = -1.f;   // change guard: Overdrive::SetDrive costs
    bool _primed = false;         // first process() snaps the smoothers
};

} // namespace spky
