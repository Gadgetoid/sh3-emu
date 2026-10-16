#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import wave

RATE = 44100
CHANNELS = 2
SAMPLE_BYTES = 2
SECTOR_BYTES = 2352
FRAMES_PER_SECOND = 75
PREGAP_SECTORS = 2 * FRAMES_PER_SECOND


def wav_pcm(path):
    with wave.open(path) as audio:
        if audio.getframerate() == RATE and audio.getnchannels() == CHANNELS and audio.getsampwidth() == SAMPLE_BYTES:
            return audio.readframes(audio.getnframes())
    return None


def decoded_pcm(path):
    if path.lower().endswith(".wav"):
        try:
            pcm = wav_pcm(path)
        except wave.Error:
            pcm = None
        if pcm is not None:
            return pcm
    if not shutil.which("ffmpeg"):
        sys.exit("mkaudiocd.py: %s isn't 44.1 kHz 16-bit stereo WAV, and converting it needs ffmpeg" % path)
    command = ["ffmpeg", "-loglevel", "error", "-i", path, "-f", "s16le", "-acodec", "pcm_s16le", "-ar", str(RATE), "-ac", str(CHANNELS), "-"]
    return subprocess.run(command, check=True, capture_output=True).stdout


def msf(sectors):
    return "%02d:%02d:%02d" % (sectors // (60 * FRAMES_PER_SECOND), sectors // FRAMES_PER_SECOND % 60, sectors % FRAMES_PER_SECOND)


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: mkaudiocd.py IMAGE.cue AUDIO...")
    cue_path = sys.argv[1]
    if not cue_path.lower().endswith(".cue"):
        sys.exit("mkaudiocd.py: the image name must end in .cue")
    for path in sys.argv[2:]:
        if not os.path.isfile(path):
            sys.exit("mkaudiocd.py: no such file %s" % path)
    bin_path = cue_path[:-4] + ".bin"
    lines = ['FILE "%s" BINARY' % os.path.basename(bin_path)]
    sector = 0
    with open(bin_path, "wb") as image:
        for number, path in enumerate(sys.argv[2:], 1):
            if number > 1:
                image.write(b"\0" * SECTOR_BYTES * PREGAP_SECTORS)
                sector += PREGAP_SECTORS
            pcm = decoded_pcm(path)
            pcm += b"\0" * (-len(pcm) % SECTOR_BYTES)
            lines += ["  TRACK %02d AUDIO" % number, "    INDEX 01 %s" % msf(sector)]
            image.write(pcm)
            sector += len(pcm) // SECTOR_BYTES
    if sector > 80 * 60 * FRAMES_PER_SECOND:
        print("mkaudiocd.py: %s is longer than an 80-minute CD" % msf(sector), file=sys.stderr)
    with open(cue_path, "w") as cue:
        cue.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
