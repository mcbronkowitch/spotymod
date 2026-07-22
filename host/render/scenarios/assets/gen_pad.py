"""Writes in_pad.wav: 10 s of a sustained, transientless pad -- material for
the slice-groove GRID-FALLBACK render scenario.

The point of sampler_slice_field.json is that fallback: material whose
SliceMap stays under kMinSlices (4), so STEP walks the tempo grid instead of
transient markers. The asset that scenario used before this file existed,
in_drone.wav, does not do that -- run this branch's detector over it and it
finds 179 onsets across the file, 24 of them inside the scenario's 1-9 s
record window. The scenario ran in marker mode, identically to the drums one,
and the fallback had no listening coverage at all.

This writes a NEW asset instead of replacing in_drone.wav, which four other
scenarios (sampler_solo, sampler_scan, sampler_overlap, sampler_texture_deck)
also feed on. Those are FLOW listening aids with nothing to do with the slice
detector; quietly changing what they granulate to fix a STEP scenario would be
a change nobody asked for.

WHAT ACTUALLY KEEPS MATERIAL UNDER THE DETECTOR, because it is not what one
would guess. SliceMap::_detect fires when env_fast > kOnsetThresh (2.0) *
env_slow, with time constants of 1 ms and 80 ms. "Sustained and slow" is not
enough on its own: what matters is the CREST FACTOR of the waveform itself.
The 1 ms envelope partly tracks the waveform's own ripple at pad frequencies,
the 80 ms one sits on its mean, and their ratio is roughly that crest factor --
so any sum of partials whose peaks stack past ~2.5x its mean rectified level
marks onsets in the middle of a held chord, LFOs or no LFOs. Measured on the
way to these numbers, same 8 s window, same everything else:

  crest 2.76 (six roughly equal partials) -> 14 onsets
  crest 2.22 (the stack below)            ->  0 onsets in the record window

Hence the shape below: one dominant fundamental with a fast-decaying partial
series, phases spread so their peaks do not coincide, and every frequency an
exact harmonic (no detuning) so nothing beats down into a null the envelopes
would then climb back out of steeply. Two further details that each cost
onsets when missing:

  * The 0.9 s fade-in ends BEFORE the scenario starts recording at 1.0 s. A
    rising ramp is a genuine onset by the detector's rule -- env_fast leads
    env_slow the whole way up -- and a 2 s fade put five markers inside the
    window on its own.
  * The noise bed is one-poled with a 300 ms time constant before it is mixed,
    longer than the SLOW envelope. Raw noise spikes env_fast by itself and
    marks onsets in the middle of a pad.

Normalised to 0.45 rather than something hotter: at 0.7 the field
scenario's own render peaked at 0.98, which is not clipping but leaves a
listening aid one edit away from it.

MEASURED on the file this script writes (SliceMap at 48 kHz):
  offline scan, whole file        : 7 markers, all inside the fade regions
  offline scan, inside [1 s, 9 s) : 0 markers
  LIVE record path, 1 s -> 9 s    : 2 markers, both at 0.000 / 0.068 s -- the
                                    detector booting from zeroed envelopes at
                                    the punch-in, not the material
2 < kMinSlices (4), so the scenario falls back to the grid. Re-measure with the
SliceMap after any edit here; "it sounds like a pad" is not the test.

Regenerate with:  python host/render/scenarios/assets/gen_pad.py
"""
import math, random, struct, wave

SR = 48000
DUR = 10.0
FADE_IN = 0.9      # must finish before the scenario's record punch-in at 1.0 s
FADE_OUT = 0.5     # ...and start after it stops, at 9.0 s
LFO_DEPTH = 0.06
random.seed(20260723)
n = int(SR * DUR)

# (frequency, amplitude, LFO rate Hz, LFO phase, partial phase).
# Exact harmonics of 220 Hz; the phases are spread so the peaks do not stack.
PARTIALS = [
    (220.0, 0.60, 0.021, 0.0, 0.0),
    (330.0, 0.18, 0.017, 1.1, 1.6),
    (440.0, 0.10, 0.029, 2.3, 3.1),
    (660.0, 0.05, 0.013, 3.7, 4.7),
    (880.0, 0.03, 0.023, 5.1, 0.9),
]

noise_coef = 1.0 - math.exp(-1.0 / (0.300 * SR))
noise = 0.0

buf = [0.0] * n
for i in range(n):
    t = i / SR
    s = 0.0
    for f, a, lr, lp, ph in PARTIALS:
        lfo = 1.0 + LFO_DEPTH * math.sin(2 * math.pi * lr * t + lp)
        s += a * lfo * math.sin(2 * math.pi * f * t + ph)
    noise += noise_coef * ((random.random() * 2 - 1) - noise)
    s += 3.0 * noise                      # the one-pole eats most of the level
    if t < FADE_IN:
        s *= 0.5 - 0.5 * math.cos(math.pi * t / FADE_IN)
    elif t > DUR - FADE_OUT:
        s *= 0.5 - 0.5 * math.cos(math.pi * (DUR - t) / FADE_OUT)
    buf[i] = s

peak = max(abs(x) for x in buf)
gain = 0.45 / peak if peak > 0 else 1.0

with wave.open("host/render/scenarios/assets/in_pad.wav", "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    frames = bytearray()
    for s in buf:
        v = max(-1.0, min(1.0, s * gain))
        frames += struct.pack("<hh", int(v * 32767), int(v * 32767))
    w.writeframes(bytes(frames))
print("wrote in_pad.wav (%.1f s, peak %.3f normalised to 0.450)" % (DUR, peak))
