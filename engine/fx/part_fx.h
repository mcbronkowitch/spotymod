#pragma once
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

// Per-part chain: GRIT -> FLUX -> FX MIX, plus the post-FX reverb send tap.
// FX MIX is a linear (equal-gain) dry/wet — dry and wet are correlated, and
// bypass must be bit-exact (wet == dry => out == dry). The square-law XFade
// stays inside Drive/Reduce where it belongs. When both blocks are off the
// whole chain is skipped, so "FX off" costs nothing and changes nothing.
class PartFx {
public:
    void init(float sample_rate, float* echo_l, float* echo_r);

    Grit& grit() { return _grit; }
    Flux& flux() { return _flux; }
    const Grit& grit() const { return _grit; }
    const Flux& flux() const { return _flux; }

    void set_fx_on(FxBlock b, bool on, bool immediate = false);
    void set_grit_mode(GritMode m) { _grit.set_mode(m); }
    void set_flux_mix(float n) { _flux.set_mix(n); }
    void set_grit_mix(float n) { _grit.set_mix(n); }

    // fxv[FXT_COUNT]: already-modulated values from Part::fx_target_value().
    void process(float& l, float& r, float& send_l, float& send_r,
                 const float* fxv);

private:
    Grit _grit;
    Flux _flux;
    OnePole _smooth[FXT_COUNT];
    float _grit_applied = -1.f;   // change guard: Overdrive::SetDrive costs
    bool _primed = false;         // first process() snaps the smoothers
};

} // namespace spky
