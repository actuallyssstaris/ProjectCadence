#!/usr/bin/env python3
"""Writes a small self-made test map (120 bpm click track + circles, sliders,
a spinner) so the console build has something to load without real beatmaps."""
import math
import os
import random
import struct
import sys
import wave

out = sys.argv[1] if len(sys.argv) > 1 else "osu5 test"
os.makedirs(out, exist_ok=True)

rate, bpm, secs = 44100, 120, 48
beat = 60.0 / bpm
frames = bytearray()
for i in range(int(rate * secs)):
    t = i / rate
    b = t / beat
    phase = b - math.floor(b)
    dt = phase * beat
    accent = 1.0 if int(b) % 4 == 0 else 0.6
    v = math.sin(2 * math.pi * (880 if accent == 1.0 else 660) * dt) * math.exp(-dt * 40) * 0.5 * accent
    v += math.sin(2 * math.pi * 110 * t) * 0.08  # quiet drone so silence isn't confused with broken audio
    s = int(max(-1, min(1, v)) * 30000)
    frames += struct.pack("<hh", s, s)

with wave.open(os.path.join(out, "audio.wav"), "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(bytes(frames))

ms = beat * 1000
objs = []
random.seed(7)
t = 4 * ms
x, y = 256, 192
combo = True
while t < (secs - 10) * 1000:
    kind = random.random()
    nc = 4 if combo else 0
    combo = False
    if kind < 0.6:
        x = max(40, min(472, x + random.randint(-140, 140)))
        y = max(40, min(344, y + random.randint(-110, 110)))
        objs.append(f"{x},{y},{int(t)},{1 | nc},0,0:0:0:0:")
        t += ms
    else:
        x2 = max(40, min(472, x + random.randint(-160, 160)))
        y2 = max(40, min(344, y + random.randint(-120, 120)))
        mx, my = (x + x2) // 2 + random.randint(-60, 60), (y + y2) // 2 + random.randint(-60, 60)
        length = math.hypot(x2 - x, y2 - y) * 1.1
        objs.append(f"{x},{y},{int(t)},{2 | nc},0,B|{mx}:{my}|{x2}:{y2},1,{length:.2f}")
        t += ms * 2
        x, y = x2, y2
    if len(objs) % 8 == 0:
        combo = True
objs.append(f"256,192,{int(t + ms)},12,0,{int(t + ms * 9)},0:0:0:0:")

osu = f"""osu file format v14

[General]
AudioFilename: audio.wav
AudioLeadIn: 0
PreviewTime: 2000
Mode: 0

[Metadata]
Title:osu5 test
Artist:osu5
Creator:osu5
Version:Normal

[Difficulty]
HPDrainRate:3
CircleSize:4
OverallDifficulty:5
ApproachRate:6
SliderMultiplier:1.4
SliderTickRate:1

[Events]

[TimingPoints]
0,{ms},4,1,0,60,1,0

[HitObjects]
""" + "\n".join(objs) + "\n"

with open(os.path.join(out, "test.osu"), "w") as f:
    f.write(osu)
print(f"{len(objs)} objects -> {out}")
