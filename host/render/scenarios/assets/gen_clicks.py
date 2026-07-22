"""Writes in_clicks.wav: 8 s of a dry click pattern at 120 bpm -- kick-ish
sine thumps on the beat, noise ticks on the offs. Material for the
slice-groove render scenarios."""
import math, random, struct, wave

SR = 48000
random.seed(7)
n = SR * 8
buf = [0.0] * n
for beat in range(16):                      # 8th grid at 120 bpm = 250 ms
    at = int(beat * 0.25 * SR)
    if beat % 2 == 0:                       # thump: 60 Hz sine, 80 ms decay
        for i in range(int(0.12 * SR)):
            buf[at + i] += 0.8 * math.sin(2 * math.pi * 60 * i / SR) \
                           * math.exp(-i / (0.03 * SR))
    else:                                   # tick: noise burst, 15 ms decay
        for i in range(int(0.03 * SR)):
            buf[at + i] += 0.5 * (random.random() * 2 - 1) \
                           * math.exp(-i / (0.005 * SR))
with wave.open("host/render/scenarios/assets/in_clicks.wav", "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    frames = bytearray()
    for s in buf:
        v = max(-1.0, min(1.0, s))
        frames += struct.pack("<hh", int(v * 32767), int(v * 32767))
    w.writeframes(bytes(frames))
print("wrote in_clicks.wav")
