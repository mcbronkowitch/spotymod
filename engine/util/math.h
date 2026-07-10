#pragma once

namespace spky {

static constexpr float TWO_PI = 6.28318530717958647692f;

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

} // namespace spky
