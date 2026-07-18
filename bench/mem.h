#pragma once
#include "instrument.h"
#include "fx/reverb.h"

namespace bench {

// 64 KB. Comfortably past the H7's 32 KB D-cache, so an access pattern over it
// is honestly cache-hostile, while leaving room next to the 128 KB SRAM reverb
// inside alt_sram.lds's 256 KB SRAM region (plan deviation 3).
constexpr size_t kSramFloats  = 16 * 1024;
constexpr size_t kSdramFloats = 2 * 1024 * 1024;   // 8 MB

float* sram_arena();
float* sdram_arena();

spky::AmbientReverb& reverb_sram();
spky::AmbientReverb& reverb_sdram();

const spky::FxMem& fx_mem();
const float*       test_input();   // kBlock seeded-noise samples

} // namespace bench
