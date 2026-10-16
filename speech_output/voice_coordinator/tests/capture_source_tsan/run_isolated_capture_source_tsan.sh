#!/usr/bin/env bash
set -euo pipefail
umask 077

# Usage: bash run_isolated_capture_source_tsan.sh <ctest-build-directory>.
# Isolated private PulseAudio instance: only a UNIX socket, exactly one
# module-null-sink, and its monitor as the sole Capture source.
if [[ "$#" != 1 ]]; then
  echo "expected CTest build directory" >&2
  exit 2
fi
build_dir="$(realpath "$1")"
root="$(cd "$(dirname "$0")/../../../.." && pwd)"
isolated="$(mktemp -d "${RUNNER_TEMP:-/tmp}/m42-isolated-pulse.XXXXXX")"
export XDG_RUNTIME_DIR="$isolated/xdg"
export PULSE_RUNTIME_PATH="$isolated/pulse"
export PULSE_SERVER="unix:$isolated/virtual-pulse.sock"
export M42_VIRTUAL_PULSE_ONLY=1
mkdir -p "$XDG_RUNTIME_DIR" "$PULSE_RUNTIME_PATH"
chmod 700 "$XDG_RUNTIME_DIR" "$PULSE_RUNTIME_PATH"

feed_pid=""
cleanup() {
  if [[ -n "$feed_pid" ]]; then
    kill "$feed_pid" 2>/dev/null || true
    wait "$feed_pid" 2>/dev/null || true
  fi
  pactl exit >/dev/null 2>&1 || true
  rm -rf "$isolated"
}
trap cleanup EXIT

# Never load the host's default.pa, udev, ALSA, hardware, TCP or network.
pulseaudio --daemonize=yes --exit-idle-time=-1 --disallow-exit \
  --log-target=stderr --log-level=notice -n \
  -L "module-native-protocol-unix socket=$isolated/virtual-pulse.sock auth-anonymous=1" \
  -L "module-null-sink sink_name=ta_ci_sink sink_properties=device.description=TurboAgentVirtualAudioSink"

pactl info
virtual_sources="$(pactl list short sources | awk '{print $2}')"
virtual_sinks="$(pactl list short sinks | awk '{print $2}')"
if [[ "$virtual_sources" != "ta_ci_sink.monitor" ||
      "$virtual_sinks" != "ta_ci_sink" ||
      -n "$(pactl list short cards)" ]]; then
  echo "PulseAudio unexpectedly exposes physical/implicit devices" >&2
  pactl list short sources >&2
  pactl list short sinks >&2
  pactl list short cards >&2
  exit 1
fi
pactl set-default-source ta_ci_sink.monitor
pactl set-default-sink ta_ci_sink
pactl list sources | grep -F 'TurboAgentVirtualAudioSink'
echo "PASS private PulseAudio namespace has one null sink and one monitor only"

# The source monitor receives bounded synthetic silence from its own sink.
# No physical speaker, microphone, ASR model or audio file participates.
(
  python3 -u "$root/speech_output/voice_coordinator/tests/virtual_capture/feed_virtual_null.py" |
    pacat --playback --device=ta_ci_sink --format=s16le \
      --rate=16000 --channels=1 >/dev/null 2>&1
) &
feed_pid="$!"
sleep 1

ctest --test-dir "$build_dir" \
  -R '^m42_original_capture_source_tsan_virtual_start_stop$' \
  --repeat until-fail:6 --verbose --output-on-failure --timeout 35 |
  tee "$isolated/native-pulse-acceptance.log"

for marker in \
    'PASS actual published Salts Capture callback start/stop/join:' \
    'PASS actual Capture stop blocks on held native callback, then joins before destroy' \
    'PASS original M1/M2 native Capture Salts adapter:' \
    'PASS native published Capture cancellation stop/join' \
    'PASS released Capture DSO + ORIGINAL M2 Salts adapter on isolated PulseAudio'; do
  actual="$(grep -Fc "$marker" "$isolated/native-pulse-acceptance.log" || true)"
  if [[ "$actual" != 6 ]]; then
    echo "missing real virtual Capture lifecycle evidence: $marker ($actual/6)" >&2
    exit 1
  fi
done
echo "PASS 6/6 actual original Capture SOURCE-TSan start/stop/join and M1/M2 adapter, virtual only"

exe="$build_dir/test_m42_original_capture_source_tsan"
test -x "$exe"
readelf -d "$exe" | grep -F 'libsalts_capture.so'
nm -D "$exe" | grep -F ' U salts_capture_start'
nm -D "$exe" | grep -F ' U salts_capture_stop'
nm -D "$exe" | grep -F ' U salts_audio_capture_create'
nm --defined-only "$exe" | grep -F ' T turbo_agent_speech_salts_capture_create'
nm --defined-only "$exe" | grep -F ' T turbo_agent_speech_offer_frame'
nm --defined-only "$exe" | grep -F ' T turbo_agent_speech_capture_bridge_offer'
ldd "$exe" | tee "$isolated/capture-elf-closure.txt"
if grep -F 'not found' "$isolated/capture-elf-closure.txt"; then
  echo "source-instrumented original native Capture ELF dependencies unresolved" >&2
  exit 1
fi
capture_library="$build_dir/libsalts_capture.so"
test -f "$capture_library"
nm -D "$capture_library" | grep -F ' U __tsan_init'
nm -D --defined-only "$capture_library" | grep -F ' T salts_capture_start'
nm -D --defined-only "$capture_library" | grep -F ' T salts_capture_stop'
nm -D --defined-only "$capture_library" | grep -F ' T salts_audio_capture_create'
# Fail if the loader took the released library instead of the actual source
# TSan shared image built from exact immutable rc.11 sources.
grep -F "libsalts_capture.so => $capture_library" "$isolated/capture-elf-closure.txt"
echo "PASS source-TSan original full native Capture Linux DSO + unchanged M1/M2 adapter linked"
