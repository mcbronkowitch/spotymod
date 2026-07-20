#include "workload.h"
#include "report.h"
#include <daisy_seed.h>

namespace bench {
namespace {

using namespace daisy;

// The three rows worth cross-checking. ORDER MATTERS: the two that fit inside
// the block budget run first and sound clean; instrument_worst runs LAST
// because it does not fit and will sound broken -- see the note below.
const char* kAnchorNames[] = {
    "oliverb_solo_sram",
    "tap_read_sdram",
    "instrument_worst",
};
constexpr int kAnchorCount = 3;
constexpr int kAnchorSeconds = 2;

CpuLoadMeter    g_meter;
const Workload* g_current = nullptr;

// THE CALLBACK LIMITS ITSELF. Do not go back to bounding the segment with a
// foreground hw.DelayMs().
//
// instrument_worst costs ~160 % of the block budget, so inside a real callback
// the ISR never finishes before the next block is due and the CPU is saturated
// in interrupt context. A foreground delay is then starved by the very
// workload it is meant to time out: a "4 second" segment ran for minutes,
// emitting continuous DMA garbage at the outputs. (Observed on hardware
// 2026-07-18. The noise IS the 160 % result, made audible.)
//
// Counting blocks in the callback and early-returning silence once the limit
// is hit frees the CPU immediately, so the foreground can proceed even after
// total starvation.
volatile uint32_t g_blocks = 0;
volatile bool     g_done   = false;
uint32_t          g_block_limit = 0;

void AnchorCallback(AudioHandle::InputBuffer,
                    AudioHandle::OutputBuffer out, size_t size)
{
    if (g_done) {                       // limit reached: cheap, silent, done
        for (size_t i = 0; i < size; ++i) { out[0][i] = 0.f; out[1][i] = 0.f; }
        return;
    }

    g_meter.OnBlockStart();

    const float v = g_current ? g_current->process() : 0.f;

    // Audible proof that the workload really computes: the block's checksum
    // value, heavily attenuated, as a click/tone on both outputs. Not music --
    // it is there so a dead workload can be HEARD as silence.
    const float mon = v * 0.0005f;
    for (size_t i = 0; i < size; ++i) {
        out[0][i] = mon;
        out[1][i] = mon;
    }

    g_meter.OnBlockEnd();

    if (++g_blocks >= g_block_limit) g_done = true;
}

} // namespace

void run_anchors(DaisySeed& hw)
{
    g_meter.Init(kSampleRate, kBlock);

    for (int a = 0; a < kAnchorCount; ++a) {
        const Workload* w = find_workload(kAnchorNames[a]);
        if (!w) continue;

        w->setup();
        g_current = w;
        g_meter.Reset();

        // Blocks, not milliseconds. An over-budget workload stretches wall
        // clock, so a fixed block count is the only bound it cannot starve.
        g_block_limit = (uint32_t)((kAnchorSeconds * kSampleRate) / kBlock);
        g_blocks = 0;
        g_done   = false;

        hw.StartAudio(AnchorCallback);
        while (!g_done) { }             // callback self-limits; see the note
        hw.StopAudio();

        // Print only AFTER the audio has stopped. This is not a nicety: a
        // semihosting write HALTS THE CORE while openocd services it, so a
        // print inside or alongside a running callback would not merely skew
        // the measurement, it would break the audio outright.
        report_anchor(kAnchorNames[a],
                      (uint32_t)(g_meter.GetAvgCpuLoad() * 10000.f),
                      (uint32_t)(g_meter.GetMaxCpuLoad() * 10000.f));
    }
    g_current = nullptr;
}

} // namespace bench
