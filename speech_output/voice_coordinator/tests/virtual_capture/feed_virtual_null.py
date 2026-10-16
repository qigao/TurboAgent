#!/usr/bin/env python3
"""CI-only bounded PCM generator feeding an isolated PulseAudio null sink.

No physical device, microphone, model input, network or raw audio persistence.
All generated samples are digital zero; this does not assess ASR quality.
"""
import sys
import time

FRAME_BYTES = 640  # 20ms / 16kHz / mono S16LE
OUTPUT_PERIOD_SECONDS = 0.010

def main() -> int:
    chunk = bytes(FRAME_BYTES)
    try:
        for _ in range(12000):
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            time.sleep(OUTPUT_PERIOD_SECONDS)
    except BrokenPipeError:
        return 0
    return 0

if __name__ == "__main__":
    sys.exit(main())
