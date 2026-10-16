#!/usr/bin/env python3
"""Bounded original M3-model-FAKE input on a private PulseAudio null monitor.

Synthetic nonzero signed S16 mono 16k samples are fed only into a verified
software null sink, never persisted or sent to physical devices. This is not
actual Whisper acoustic inference or human voice content.
"""
import struct
import sys
import time

# 16kHz / 40 samples = 400Hz square wave: comfortably below Nyquist.
# Unlike alternating +/- each sample (8kHz), this survives native
# PulseAudio rate conversion to and from the null-monitor without filtering
# the input to digital zero. It is NOT speech or a learned ASR example.
FRAME = struct.pack("<320h", *(8000 if ((i // 20) & 1) == 0 else -8000 for i in range(320)))

def main() -> int:
    try:
        for _ in range(12000):
            sys.stdout.buffer.write(FRAME)
            sys.stdout.buffer.flush()
            time.sleep(0.010)
    except BrokenPipeError:
        return 0
    return 0

if __name__ == "__main__":
    sys.exit(main())
