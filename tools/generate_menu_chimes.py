#!/usr/bin/env python3
"""Generate original, short synthesized menu chimes as 16-bit mono WAV files."""

import math
import struct
import wave
from pathlib import Path

RATE = 44100
OUT = Path(__file__).resolve().parent.parent / "godot" / "assets" / "audio"


def chime(name, notes, duration):
    """Mix softly decaying bell tones; each note is (start, frequency, gain)."""
    frames = bytearray()
    for i in range(round(duration * RATE)):
        t = i / RATE
        value = 0.0
        for start, frequency, gain in notes:
            age = t - start
            if age < 0:
                continue
            attack = min(1.0, age / 0.008)
            decay = math.exp(-age * 10.0)
            tone = math.sin(2 * math.pi * frequency * age)
            tone += 0.18 * math.sin(2 * math.pi * frequency * 2.01 * age)
            value += gain * attack * decay * tone
        # End at zero to avoid a click if the tail is cut short.
        value *= min(1.0, (duration - t) / 0.025)
        frames.extend(struct.pack("<h", round(max(-1.0, min(1.0, value)) * 32767)))
    with wave.open(str(OUT / (name + ".wav")), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(frames)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    chime("cursor", [(0.0, 880.0, 0.17)], 0.22)
    chime("confirm", [(0.0, 659.25, 0.18), (0.075, 987.77, 0.17)], 0.40)
    chime("cancel", [(0.0, 659.25, 0.17), (0.065, 493.88, 0.16)], 0.38)


if __name__ == "__main__":
    main()
