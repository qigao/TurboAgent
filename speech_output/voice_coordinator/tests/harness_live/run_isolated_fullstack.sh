#!/usr/bin/env bash
set -euo pipefail
umask 077

# bash run_isolated_fullstack.sh <original-Harness-source-dir>
# Dedicated Linux opt-in test ONLY. It NEVER talks to a physical device.
if [[ "$#" != 1 ]]; then
  echo "expected original Harness source root" >&2
  exit 2
fi
harness_root="$(realpath "$1")"
root="$(cd "$(dirname "$0")/../../../.." && pwd)"
isolated="$(mktemp -d "${RUNNER_TEMP:-/tmp}/m42-fullstack-virtual.XXXXXX")"
export XDG_RUNTIME_DIR="$isolated/xdg"
export PULSE_RUNTIME_PATH="$isolated/pulse"
export PULSE_SERVER="unix:$isolated/fullstack-virtual.sock"
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

# Every audio route here is a software null sink/monitor. The daemon does
# not read default.pa and cannot load hardware discovery or a network port.
pulseaudio --daemonize=yes --exit-idle-time=-1 --disallow-exit \
  --log-target=stderr --log-level=notice -n \
  -L "module-native-protocol-unix socket=$isolated/fullstack-virtual.sock auth-anonymous=1" \
  -L "module-null-sink sink_name=ta_ci_sink sink_properties=device.description=TurboAgentVirtualAudioSink"

sources="$(pactl list short sources | awk '{print $2}')"
sinks="$(pactl list short sinks | awk '{print $2}')"
if [[ "$sources" != "ta_ci_sink.monitor" || "$sinks" != "ta_ci_sink" ||
      -n "$(pactl list short cards)" ]]; then
  echo "refusing unknown, physical, or extra audio devices" >&2
  pactl list short sources >&2
  pactl list short sinks >&2
  pactl list short cards >&2
  exit 1
fi
pactl set-default-source ta_ci_sink.monitor
pactl set-default-sink ta_ci_sink
pactl list sources | grep -F 'TurboAgentVirtualAudioSink'
echo 'PASS isolated fullstack OS Pulse source and sink audited (zero hardware cards)'

(
  python3 -u "$root/speech_output/voice_coordinator/tests/harness_live/feed_m3_virtual_pcm.py" |
    pacat --playback --device=ta_ci_sink --format=s16le \
      --rate=16000 --channels=1 >/dev/null 2>&1
) &
feed_pid="$!"
sleep 1
# The isolated software sink MUST have exactly one active PCM producer.
# Without an attached stream the monitor can yield startup silence, which
# M3 must correctly refuse rather than accidentally authorizing a turn.
sink_inputs="$(pactl list short sink-inputs | awk 'NF { n++ } END { print n+0 }')"
test "$sink_inputs" = 1 || {
  echo "refusing absent/extra virtual PCM feeder: sink_inputs=$sink_inputs" >&2
  pactl list short sink-inputs >&2
  exit 1
}
echo 'PASS one attached isolated null-sink PCM producer, zero physical devices'

# Genuine production Harness, original native M2, original M3 Provider and
# currently built M4 + the true loaded pthread TTS Provider image.
cd "$harness_root"
# Exact 8 iterations remain the acceptance default. Source TSan runs a
# smaller but bounded separate original-binary matrix, never a skipped case.
repeats="${M42_FULLSTACK_REPEATS:-8}"
if [[ ! "$repeats" =~ ^[1-9][0-9]?$ ]] || (( repeats > 20 )); then
  echo "unsupported fullstack repeat count: $repeats" >&2
  exit 1
fi
ctest --preset ci-linux-release \
  -R '^tts_m42_real_virtual_fullstack
  tee "$isolated/fullstack-live.log"
for marker in \
  'PASS shipped Capture callback stop/join -> original M1/M2/M3 one-shot checked FINAL' \
  'PASS full published Capture+Playback + actual Harness + true TTS DSO: 480-byte backpressure/native drain/join/unload' \
  'PASS full published Capture+Playback + actual Harness + true TTS DSO: cancel/clear/late-reject/join/unload' \
  'PASS same-process actual M1/M2/M3 -> Harness -> loaded TTS worker -> BOTH published Salts audio DSOs'; do
  count="$(grep -Fc "$marker" "$isolated/fullstack-live.log" || true)"
  expected="$repeats"
  # Exactly TWO complete Capture/M3 checked FINAL passages per CTest:
  # normal-drain then cancellation. The other markers occur once per case.
  if [[ "$marker" == 'PASS shipped Capture callback stop/join -> original M1/M2/M3 one-shot checked FINAL' ]]; then
    expected="$((repeats * 2))"
  fi
  if [[ "$count" != "$expected" ]]; then
    echo "missing fullstack exact native lifecycle evidence: $marker ($count/$expected)" >&2
    exit 1
  fi
done
exe="$harness_root/build/ci-linux-release/bin/test_tts_m42_real_virtual_fullstack"
test -x "$exe"
for soname in libsalts_capture.so libsalts_playback.so libturbo_agent_harness.so; do
  readelf -d "$exe" | grep -F "$soname"
done
for symbol in salts_capture_start salts_capture_stop salts_playback_create \
              salts_playback_start salts_playback_drain salts_playback_clear \
              turbo_agent_harness_start_text; do
  nm -D "$exe" | grep -F " U $symbol"
done
for symbol in turbo_agent_speech_salts_capture_create \
              turbo_agent_speech_whisper_create \
              turbo_voice_admit_harness_completed_final; do
  nm --defined-only "$exe" | grep -F " T $symbol"
done

image="$harness_root/build/ci-linux-release/bin/libtts_m42_combined_provider_dso.so"
test -f "$image"
nm -D --defined-only "$image" | grep -F ' T turbo_tts_test_dso_entry'
if readelf -d "$exe" | grep -F 'libtts_m42_combined_provider_dso.so'; then
  echo "TTS shared image is linked, not actually dlopened by Host" >&2
  exit 1
fi
ldd "$exe" | tee "$isolated/fullstack-needed.txt"
if grep -F 'not found' "$isolated/fullstack-needed.txt"; then
  echo "one or more real published SDK dependencies unresolved" >&2
  exit 1
fi
echo "PASS $repeats/$repeats fullstack original M1/M2/M3 + true Harness + loaded TTS worker + both published Salts virtual audio DSOs"
 \
  --repeat until-fail:"$repeats" --verbose --output-on-failure --timeout 60 |
  tee "$isolated/fullstack-live.log"
for marker in \
  'PASS shipped Capture callback stop/join -> original M1/M2/M3 one-shot checked FINAL' \
  'PASS full published Capture+Playback + actual Harness + true TTS DSO: 480-byte backpressure/native drain/join/unload' \
  'PASS full published Capture+Playback + actual Harness + true TTS DSO: cancel/clear/late-reject/join/unload' \
  'PASS same-process actual M1/M2/M3 -> Harness -> loaded TTS worker -> BOTH published Salts audio DSOs'; do
  count="$(grep -Fc "$marker" "$isolated/fullstack-live.log" || true)"
  expected=8
  # Exactly TWO complete Capture/M3 checked FINAL passages per CTest:
  # normal-drain then cancellation. The other markers occur once per case.
  if [[ "$marker" == 'PASS shipped Capture callback stop/join -> original M1/M2/M3 one-shot checked FINAL' ]]; then
    expected=16
  fi
  if [[ "$count" != "$expected" ]]; then
    echo "missing fullstack exact native lifecycle evidence: $marker ($count/$expected)" >&2
    exit 1
  fi
done
exe="$harness_root/build/ci-linux-release/bin/test_tts_m42_real_virtual_fullstack"
test -x "$exe"
for soname in libsalts_capture.so libsalts_playback.so libturbo_agent_harness.so; do
  readelf -d "$exe" | grep -F "$soname"
done
for symbol in salts_capture_start salts_capture_stop salts_playback_create \
              salts_playback_start salts_playback_drain salts_playback_clear \
              turbo_agent_harness_start_text; do
  nm -D "$exe" | grep -F " U $symbol"
done
for symbol in turbo_agent_speech_salts_capture_create \
              turbo_agent_speech_whisper_create \
              turbo_voice_admit_harness_completed_final; do
  nm --defined-only "$exe" | grep -F " T $symbol"
done

image="$harness_root/build/ci-linux-release/bin/libtts_m42_combined_provider_dso.so"
test -f "$image"
nm -D --defined-only "$image" | grep -F ' T turbo_tts_test_dso_entry'
if readelf -d "$exe" | grep -F 'libtts_m42_combined_provider_dso.so'; then
  echo "TTS shared image is linked, not actually dlopened by Host" >&2
  exit 1
fi
ldd "$exe" | tee "$isolated/fullstack-needed.txt"
if grep -F 'not found' "$isolated/fullstack-needed.txt"; then
  echo "one or more real published SDK dependencies unresolved" >&2
  exit 1
fi
echo 'PASS 8/8 fullstack original M1/M2/M3 + true Harness + loaded TTS worker + both published Salts virtual audio DSOs'
