#pragma once
#include <cmath>
#include "util/math.h"

namespace spky {

// The musical rate ladder for SYNC mode: 17 detents, strictly speed-sorted,
// dotted/straight/triplet interleaved. cpb = cycles per beat; the bar-length
// entries assume 4/4. Names appear verbatim in the VCV RATE tooltip.
struct Division { float cpb; const char* name; };

inline constexpr Division kDivisions[] = {
    {1.f/32.f, "8 bars"}, {1.f/16.f, "4 bars"}, {1.f/8.f, "2 bars"},
    {1.f/4.f,  "1 bar"},  {1.f/3.f,  "1/2."},   {1.f/2.f, "1/2"},
    {2.f/3.f,  "1/4."},   {3.f/4.f,  "1/2T"},   {1.f,     "1/4"},
    {4.f/3.f,  "1/8."},   {3.f/2.f,  "1/4T"},   {2.f,     "1/8"},
    {8.f/3.f,  "1/16."},  {3.f,      "1/8T"},   {4.f,     "1/16"},
    {6.f,      "1/16T"},  {8.f,      "1/32"},
};
inline constexpr int kDivisionCount = 17;

inline int division_index(float norm) {
    return static_cast<int>(clampf(norm, 0.f, 1.f) * (kDivisionCount - 1) + 0.5f);
}

inline float division_hz(int idx, float bpm) {
    return (bpm / 60.f) * kDivisions[idx].cpb;
}

// nearest ladder rung to a free rate, compared in log space (ratio symmetry)
inline int nearest_division(float hz, float bpm) {
    int best = 0;
    float best_d = 1e30f;
    for (int i = 0; i < kDivisionCount; ++i) {
        const float d = std::fabs(std::log(hz / division_hz(i, bpm)));
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

// Free-mode rate curve. Lives here (not in super_modulator.cpp) so the VCV
// tooltip shows exactly the Hz the engine runs.
inline constexpr float kRateFreeMin = 0.02f;
inline constexpr float kRateFreeMax = 30.f;
inline float free_hz(float norm) {
    return kRateFreeMin * std::pow(kRateFreeMax / kRateFreeMin, clampf(norm, 0.f, 1.f));
}

} // namespace spky
