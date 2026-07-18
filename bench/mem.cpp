#include "mem.h"
#include "workload.h"
#include <daisy_seed.h>
#include <new>

namespace bench {
namespace {

float  g_sram[kSramFloats];
float  DSY_SDRAM_BSS g_sdram[kSdramFloats];

spky::AmbientReverb g_rev_sram;

// g_rev_sdram is NOT a plain global object here (deviation from the brief's
// literal text -- see report). AmbientReverb has a non-trivial default
// constructor (member initializers on _sr/_ctrl, plus the embedded
// clouds::Oliverb), so a plain `AmbientReverb DSY_SDRAM_BSS g_rev_sdram;`
// gets a C++ static initializer that runs in .init_array, before main() and
// therefore before hw.Init() brings the FMC/SDRAM controller up. That
// constructor's write into SDRAM address space bus-faults (CFSR
// IMPRECISERR) on real hardware -- confirmed 2026-07-18 via
// arm-none-eabi-addr2line on the halted PC inside libDaisy's
// HardFault_Handler. Raw storage + placement-new on first call (after
// hw.Init() has run) sidesteps it; the public accessor signature is
// unchanged.
alignas(alignof(spky::AmbientReverb))
    unsigned char DSY_SDRAM_BSS g_rev_sdram_storage[sizeof(spky::AmbientReverb)];
spky::AmbientReverb* g_rev_sdram_ptr = nullptr;

// FLUX echo storage: 4 x 240 000 floats = 3.75 MB. SDRAM in the shipping
// firmware too -- 937 KB per channel does not fit anywhere else.
float DSY_SDRAM_BSS g_echo[2][2][spky::Flux::kMaxSamples];

float g_input[kBlock];
bool  g_input_ready = false;

spky::FxMem g_mem;
bool        g_mem_ready = false;

} // namespace

float* sram_arena()  { return g_sram; }
float* sdram_arena() { return g_sdram; }

spky::AmbientReverb& reverb_sram()  { return g_rev_sram; }
spky::AmbientReverb& reverb_sdram()
{
    if (!g_rev_sdram_ptr)
        g_rev_sdram_ptr = new (g_rev_sdram_storage) spky::AmbientReverb();
    return *g_rev_sdram_ptr;
}

const spky::FxMem& fx_mem()
{
    if (!g_mem_ready) {
        for (int p = 0; p < 2; ++p) {
            g_mem.echo[p][0] = g_echo[p][0];
            g_mem.echo[p][1] = g_echo[p][1];
        }
        g_mem.reverb = &g_rev_sram;
        g_mem_ready = true;
    }
    return g_mem;
}

const float* test_input()
{
    if (!g_input_ready) {
        // Seeded LCG, not rand() -- the checksum is a determinism check and
        // must not depend on library state.
        uint32_t s = 22222u;
        for (size_t i = 0; i < kBlock; ++i) {
            s = s * 1664525u + 1013904223u;
            g_input[i] = (static_cast<float>(s >> 8) / 8388608.f) - 1.f;
        }
        g_input_ready = true;
    }
    return g_input;
}

} // namespace bench
