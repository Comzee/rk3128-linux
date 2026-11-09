#!/usr/bin/env python3
"""Write the test tone for the board's Linux audio check (development record #303,
docs/RK3128-LINUX.md "Audio"): a 16-bit stereo WAV at 44.1 kHz (the rate Android's mixer runs the
speaker at) with a known pattern, so that a listener can tell a working path from a wrong clock,
a swapped channel or a distorted output:

  0.0-1.5 s   1 kHz, left channel only
  1.5-3.0 s   1 kHz, right channel only
  3.0-6.0 s   440 Hz, both channels
  6.0-7.5 s   silence (amplifier and codec idle)
  7.5-9.0 s   440 Hz sweeping up to 4.4 kHz, both channels (distortion and aliasing show here)

Level is -12 dBFS. Standard library only; the output is deterministic.

  audio_tone.py [OUT.wav]               default: build/linux/audio-test.wav
  audio_tone.py --channels OUT.wav      which DAC channel reaches the board's one (mono) speaker (#357): three
                                        parts, each announced by a count of short pips on both channels (1, 2, 3),
                                        then 2 s of 1 kHz on the left only (1), the right only (2) and both (3),
                                        with 1.5 s of silence after each part: 15.9 s
"""
import math
import os
import struct
import sys
import wave

RATE = 44100
LEVEL = 10 ** (-12 / 20) * 32767


def seg(seconds, left, right, freq0, freq1=None):
    n = int(seconds * RATE)
    out = bytearray()
    phase = 0.0
    for i in range(n):
        f = freq0 if freq1 is None else freq0 * (freq1 / freq0) ** (i / n)
        phase += 2 * math.pi * f / RATE
        s = math.sin(phase) * LEVEL
        out += struct.pack("<hh", int(s) if left else 0, int(s) if right else 0)
    return bytes(out)


def channels():
    pcm = b""
    for count, left, right in ((1, True, False), (2, False, True), (3, True, True)):
        for _ in range(count):
            pcm += seg(0.15, True, True, 2000) + seg(0.25, False, False, 2000)
        pcm += seg(0.6 + 0.4 * (3 - count), False, False, 1000)  # every part's tone starts 1.8 s after its first pip
        pcm += seg(2.0, left, right, 1000) + seg(1.5, False, False, 1000)
    return pcm


def main():
    args = sys.argv[1:]
    mode = "pattern"
    if args and args[0] == "--channels":
        mode = "channels"
        args = args[1:]
        if not args:
            sys.exit("audio_tone.py --channels needs OUT.wav")
    out = args[0] if args else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "build", "linux", "audio-test.wav")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    if mode == "channels":
        pcm = channels()
    else:
        pcm = (seg(1.5, True, False, 1000) + seg(1.5, False, True, 1000) + seg(3.0, True, True, 440)
               + seg(1.5, False, False, 440) + seg(1.5, True, True, 440, 4400))
    with wave.open(out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)
    print("%s: %d bytes, %.1f s, 44100 Hz S16_LE stereo" % (out, os.path.getsize(out), len(pcm) / 4 / RATE))


if __name__ == "__main__":
    main()
