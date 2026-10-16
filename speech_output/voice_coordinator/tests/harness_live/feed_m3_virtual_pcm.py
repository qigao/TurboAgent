#!/usr/bin/env python3
"""Bounded original M3-model-FAKE input on a private PulseAudio null monitor.

Synthetic nonzero signed S16 mono 16k samples are fed only into a verified
software null sink, never persisted or sent to physical devices. This is not
actual Whisper acoustic inference or human voice content.
"""
import struct
import sys
import time

FRAME = struct.pack("<320h", *(1200 if (i & 1) == 0 else -1200 for i in range(320)))

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
