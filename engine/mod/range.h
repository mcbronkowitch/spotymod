#pragma once
#include "util/math.h"

namespace spky {

// RANGE mapping (spec): monotonic, minimum = off, opening unipolar through the
// first half, widening to full bipolar at maximum.
//   r <= 0      : off (0)
//   r in 0..0.5 : unipolar, amplitude 0..1
//   r in 0.5..1 : blend unipolar -> full bipolar
// `v` is the bipolar lane value [-1,1].
inline float apply_range(float v, float r) {
    r = clampf(r, 0.f, 1.f);
    if (r <= 0.f) return 0.f;
    float uni = v * 0.5f + 0.5f;          // [0,1]
    if (r <= 0.5f) return uni * (r * 2.f); // unipolar 0..amp
    float t = (r - 0.5f) * 2.f;           // 0..1
    return lerpf(uni, v, t);              // unipolar -> bipolar
}

} // namespace spky
