#include "workload.h"
#include "mem.h"
#include <cmath>

namespace bench {
namespace {

using namespace spky;

// --- grain-read proxy -------------------------------------------------------
// Eight scattered linear-interpolated stereo reads per sample. This is the
// access pattern a granular sampler has, without a sampler existing yet: the
// M5 texture deck's SDRAM exposure, measured in advance.
//
// The SRAM and SDRAM rows run the IDENTICAL pattern over identically-sized
// windows -- only the region changes, so the ratio is the region's factor and
// nothing else.
constexpr int    kGrains     = 8;
constexpr size_t kWindowFrames = kSramFloats / 2;   // stereo frames; same for both

float*   g_grain_buf = nullptr;
uint32_t g_grain_rng = 0;

void setup_grain(float* buf)
{
    g_grain_buf = buf;
    g_grain_rng = 99991u;
    for (size_t i = 0; i < kWindowFrames * 2; ++i)
        buf[i] = sinf(static_cast<float>(i) * 0.001f);
}
void setup_grain_sram()  { setup_grain(sram_arena()); }
void setup_grain_sdram() { setup_grain(sdram_arena()); }

float proc_grain()
{
    float acc = 0.f;
    for (size_t s = 0; s < kBlock; ++s) {
        for (int g = 0; g < kGrains; ++g) {
            // Scattered on purpose: a sequential walk would be prefetched and
            // would measure the prefetcher, not the memory.
            g_grain_rng = g_grain_rng * 1664525u + 1013904223u;
            const uint32_t idx = (g_grain_rng >> 8) % (kWindowFrames - 2);
            const float frac = static_cast<float>(g_grain_rng & 0xFFu) / 256.f;
            const float* p = g_grain_buf + idx * 2;
            acc += p[0] + (p[2] - p[0]) * frac;      // L, lerp
            acc += p[1] + (p[3] - p[1]) * frac;      // R, lerp
        }
    }
    return acc;
}

// --- Oliverb placement ------------------------------------------------------
// The SRAM side is family 1's oliverb_solo_sram row -- one 128 KB object, not
// two (plan deviation 3). This row is only the SDRAM twin; the M6 placement
// decision is the ratio between the two.
void setup_verb(AmbientReverb& v)
{
    v.init(kSampleRate);
    v.clear();
    v.set_size(0.9f);
    v.set_decay(0.95f);
    v.set_tone(0.8f);
    v.set_diffusion(0.9f);
    v.set_diffuser_mod_depth(1.f);
    v.set_mod_depth(1.f);
}
void setup_verb_sdram() { setup_verb(reverb_sdram()); }

float proc_verb_sdram()
{
    const float* in = test_input();
    AmbientReverb& v = reverb_sdram();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l, r;
        v.process(in[i], in[i] * 0.9f, l, r);
        acc += l + r;
    }
    return acc;
}

// --- echo-style streaming access -------------------------------------------
// FLUX's real buffer is 240 000 floats (937 KB per channel) and does not fit
// SRAM at any size, so this is a shortened window in BOTH regions: what is
// measured is the access-pattern factor, not the full delay length.
constexpr size_t kEchoFrames = kSramFloats;   // same window in both regions

float*   g_echo_buf = nullptr;
size_t   g_echo_w = 0;

void setup_echo(float* buf)
{
    g_echo_buf = buf;
    g_echo_w = 0;
    for (size_t i = 0; i < kEchoFrames; ++i) buf[i] = 0.f;
}
void setup_echo_sram()  { setup_echo(sram_arena()); }
void setup_echo_sdram() { setup_echo(sdram_arena()); }

float proc_echo()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        // One read a long way behind the write head, one write at it: the
        // streaming pattern a delay line has.
        const size_t rd = (g_echo_w + kEchoFrames - (kEchoFrames * 3 / 4)) % kEchoFrames;
        const float out = g_echo_buf[rd];
        g_echo_buf[g_echo_w] = in[i] + out * 0.6f;
        g_echo_w = (g_echo_w + 1) % kEchoFrames;
        acc += out;
    }
    return acc;
}

} // namespace

const Workload kMemWorkloads[] = {
    { "mem", "grain_read_sram",  setup_grain_sram,  proc_grain      },
    { "mem", "grain_read_sdram", setup_grain_sdram, proc_grain      },
    { "mem", "oliverb_sdram",    setup_verb_sdram,  proc_verb_sdram },
    { "mem", "echo_walk_sram",   setup_echo_sram,   proc_echo       },
    { "mem", "echo_walk_sdram",  setup_echo_sdram,  proc_echo       },
};
const int kMemCount = sizeof(kMemWorkloads) / sizeof(kMemWorkloads[0]);

} // namespace bench
