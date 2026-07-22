#pragma once
#include "instrument.h"
#include "fx/reverb.h"
#include "sampler/sampler_config.h"
#include "workload.h"   // kSampleRate, for kSamplerFrames below

namespace bench {

// 64 KB. Comfortably past the H7's 32 KB D-cache, so an access pattern over it
// is honestly cache-hostile, while leaving room next to the 128 KB SRAM reverb
// inside alt_sram.lds's 256 KB SRAM region (plan deviation 3).
//
// It no longer lives in that half, though -- see BENCH_SRAM_EXEC_BSS below.
constexpr size_t kSramFloats  = 16 * 1024;
constexpr size_t kSdramFloats = 2 * 1024 * 1024;   // 8 MB

// The sampler's record buffer at full capacity: kSizeCeilS seconds of stereo
// frames, the same figure both hosts allocate (host/render/main.cpp,
// host/vcv/src/Spotymod.cpp compute 42.0 * sample_rate). 2 016 000 frames =
// 16.1 MB per part, two parts, so the sampler rows alone put 32.3 MB in
// SDRAM. That is the point of the family: MOTION 1 scatters grain reads over
// the whole of it, and no cache on this part can hold that.
constexpr size_t kSamplerFrames =
    static_cast<size_t>(spky::sampler_cfg::kSizeCeilS * kSampleRate);

// The bench's 64 KB arena sits in .sram_exec_bss (alt_sram.lds), i.e. in the
// SRAM_EXEC half of the AXI SRAM rather than the SRAM half. Same physical
// memory, same latency -- the split is a linker bookkeeping line, not a
// hardware one -- so grain_read_sram and sampler_win_sram still measure AXI
// SRAM exactly as before. The move happened because .bss overflowed the SRAM
// half by 6 168 bytes the moment Part started carrying a SamplerEngine
// (1 392 B each, four bare Parts and two Instruments in the bench's globals),
// and the free 95 KB happened to be on the other side of that line.
#define BENCH_SRAM_EXEC_BSS __attribute__((section(".sram_exec_bss")))

float* sram_arena();
float* sdram_arena();

// Per-part record buffers at kSamplerFrames, SDRAM. Also what fx_mem() hands
// the Instrument, so instrument-level sampler rows and the solo rows read the
// same memory. Solo rows use part 0.
spky::SampleBuffer::Frame* sampler_arena(int part);

spky::AmbientReverb& reverb_sram();
spky::AmbientReverb& reverb_sdram();

const spky::FxMem& fx_mem();
const float*       test_input();   // kBlock seeded-noise samples

} // namespace bench
