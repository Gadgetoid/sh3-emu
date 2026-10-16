#!/usr/bin/env python3
import math
import struct
import sys
import wave

RATE = 44100
SECONDS = 4
SECTOR_FRAMES = 588


def tone(path):
    frames = bytearray()
    for i in range(RATE * SECONDS):
        level = math.sin(2 * math.pi * 441 * i / RATE) * (1 + 0.5 * math.sin(2 * math.pi * 3 * i / RATE)) / 1.5
        frames += struct.pack("<hh", int(12000 * level), int(-9000 * level))
    with wave.open(path, "wb") as audio:
        audio.setnchannels(2)
        audio.setsampwidth(2)
        audio.setframerate(RATE)
        audio.writeframes(bytes(frames))


def sectors_heard(source_path, recording_path):
    with wave.open(source_path) as source:
        pcm = source.readframes(source.getnframes())
    with wave.open(recording_path) as recording:
        heard = recording.readframes(recording.getnframes())
    mono = bytearray()
    for i in range(0, len(pcm), 4):
        left, right = struct.unpack_from("<hh", pcm, i)
        mono += struct.pack("<h", int((left + right) / 2))
    sector_bytes = SECTOR_FRAMES * 2
    return sum(1 for offset in range(0, len(mono) - sector_bytes + 1, sector_bytes) if mono[offset:offset + sector_bytes] in heard)


def main():
    if sys.argv[1:2] == ["tone"] and len(sys.argv) == 3:
        tone(sys.argv[2])
    elif sys.argv[1:2] == ["heard"] and len(sys.argv) == 4:
        print(sectors_heard(sys.argv[2], sys.argv[3]))
    else:
        sys.exit("usage: cd_audio.py tone WAV | heard SOURCE.wav RECORDING.wav")


if __name__ == "__main__":
    main()
