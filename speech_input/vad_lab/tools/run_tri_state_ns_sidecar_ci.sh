#!/usr/bin/env bash
# Test-only: evaluate exactly the same private C11 VAD on licensed
# human voice, recorded road noise, SpeexDSP NS and sound-overlay pairs.
# No model weights, production admission, ASR transcript or raw uploads.
set -euo pipefail
umask 077
test -n "$RUNNER_TEMP" && test -n "$GITHUB_WORKSPACE"
private="$(mktemp -d "$RUNNER_TEMP/vad-speex-sidecar-XXXXXX")"
chmod 700 "$private"
trap 'rm -rf -- "$private"' EXIT

test "$(pkg-config --modversion speexdsp)" = 1.2.1
test "$(dpkg-query -W -f='${Version}' libspeexdsp-dev)" = 1.2.1-1ubuntu3
so="$(readlink -f "$(pkg-config --variable=libdir speexdsp)/libspeexdsp.so")"
test "$(sha256sum "$so" | awk '{print $1}')" = \
  ef4e0d1c80fb4fcf832bb98be7df08b44faaed84ee2c003070fa1b1023a1c5fd

cmake -S speech_input/vad_lab -B build/vad-lab-sidecar -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/vad-lab-sidecar --parallel 2
ctest --test-dir build/vad-lab-sidecar --output-on-failure

cmake -S speech_input/noise_lab -B build/vad-sidecar-speex -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/vad-sidecar-speex \
  --target ta_speex_ns_raw_probe --parallel 2

vad="$GITHUB_WORKSPACE/build/vad-lab-sidecar/ta_speech_vad_lab_raw_probe"
ns="$GITHUB_WORKSPACE/build/vad-sidecar-speex/ta_speex_ns_raw_probe"
test -x "$vad" && test -x "$ns"
ffmpeg="$(realpath "$(command -v ffmpeg)")"
ffmpeg_sha="$(sha256sum "$ffmpeg" | awk '{print $1}')"

# Strict sources and rights verified in #158 and #155.
while IFS='|' read -r label bytes sha1 original_sha converted_sha url; do
  test -n "$label" || continue
  case "$label" in whisper|road) ;; *) exit 2 ;; esac
  curl --fail --location --silent --show-error --retry 2 \
    --connect-timeout 15 --max-time 60 --max-filesize 300000 \
    --proto '=https' --proto-redir '=https' \
    --user-agent 'TurboAgent-C11-VAD-NS-sidecar/1.0 (qigao/TurboAgent)' \
    "$url" --output "$private/$label.ogg"
  test "$(wc -c < "$private/$label.ogg" | tr -d '[:space:]')" = "$bytes"
  test "$(sha1sum "$private/$label.ogg" | awk '{print $1}')" = "$sha1"
  test "$(sha256sum "$private/$label.ogg" | awk '{print $1}')" = "$original_sha"
  python3 speech_input/whisper/tools/prepare_stationarity_wav.py \
    --source "$private/$label.ogg" --source-sha256 "$original_sha" \
    --ffmpeg "$ffmpeg" --ffmpeg-sha256 "$ffmpeg_sha" \
    --output-wav "$private/$label.wav" \
    --private-provenance "$private/$label.private.provenance.json" \
    --acknowledge-rights > "$private/$label.private.status.json"
  test "$(sha256sum "$private/$label.wav" | awk '{print $1}')" = "$converted_sha"
done <<'PINNED_LICENSED_ORIGINALS'
whisper|30283|cc3b98f93353cf6ed8bb453669987afe7dd748bd|bbdbe95dfe4742e27845c03fba3a8caf9f0f5ad65e34e512fd5609d155e6304e|c81cf6b75800daf1070d3001f094877581bc68100f3c64cb43638491f89988d0|https://upload.wikimedia.org/wikipedia/commons/e/e8/Whispering-example.ogg
road|120796|71500b016280d285ac6854bd46eef6373847247e|7d70594ef7ba1bd7a34136ab7140420d016fd730acc6c124a1a02fe6c75337d0|cd9023210066ea6ad618fe386d86c6e9469d4b9a19ba170d5032eb76fb30c808|https://upload.wikimedia.org/wikipedia/commons/e/e2/Back-up_white_noise.ogg
PINNED_LICENSED_ORIGINALS

PRIVATE_INPUT="$private" python3 - <<'PY'
import os,stat,wave
from pathlib import Path
root=Path(os.environ["PRIVATE_INPUT"])
for name, frames in (("whisper",128),("road",375)):
    with wave.open(str(root/(name+".wav")),"rb") as input:
        assert (input.getframerate(),input.getnchannels(),
                input.getsampwidth(),input.getnframes())==(16000,1,2,frames*320)
        raw=input.readframes(input.getnframes())
    dest=root/(name+".raw")
    with dest.open("xb") as out: out.write(raw)
    dest.chmod(0o600)
    assert stat.S_IMODE(dest.stat().st_mode)==0o600
PY

observed=0
record() {
  local kind="$1" label="$2" file="$3" frames="$4"
  local receipt="$private/observation-$observed.json"
  "$vad" "$file" > "$receipt"
  VAD_RECEIPT="$receipt" VAD_KIND="$kind" VAD_LABEL="$label" \
    VAD_FRAMES="$frames" python3 - <<'PY'
import json,os
from pathlib import Path
r=json.loads(Path(os.environ["VAD_RECEIPT"]).read_text())
label=os.environ["VAD_LABEL"]
known=os.environ["VAD_KIND"]=="human_voice"
assert r["frames"]==int(os.environ["VAD_FRAMES"])
assert r["diagnostic_only"] is True
assert r["real_model_or_language_certified"] is False
assert r["non_speech_only_when_all_digital_zero"] is True
if known:
    assert r["nonzero_frames"]>0
    assert r["decision"]!="NON_SPEECH_CONFIDENT"
print(json.dumps({
  "source_class":"real_human_or_overlay" if known else "recorded_noise_only",
  "sample":label,
  "frames":r["frames"],
  "nonzero_frames":r["nonzero_frames"],
  "decision":r["decision"],
  "voicing_cue_frames":r["voicing_cue_frames"],
  "tonal_cue_frames":r["tonal_cue_frames"],
  "power_modulation_events":r["power_modulation_events"],
  "peak_autocorrelation":r["mean_peak_autocorrelation"],
  "spectral_concentration":r["mean_spectral_concentration"],
  "current_production_guard_changed":False,
  "real_ASR_quality_or_noise_hallucination_measured":False
},sort_keys=True))
PY
  observed=$((observed+1))
}

record human_voice original-whisper "$private/whisper.raw" 128
record noise_only original-truck "$private/road.raw" 375

mkdir -m 700 "$private/voice-ns"
python3 speech_input/noise_lab/tools/prepare_paired_asr_wavs.py \
  --source-wav "$private/whisper.wav" \
  --source-sha256 "$(sha256sum "$private/whisper.wav" | awk '{print $1}')" \
  --ns-probe "$ns" --ns-probe-sha256 "$(sha256sum "$ns" | awk '{print $1}')" \
  --private-root "$private/voice-ns" \
  > "$private/voice-ns.private.status.json"

for setting in 12 18; do
  RAW_WAV="$private/voice-ns/ns$setting.wav" \
  RAW_OUT="$private/whisper-ns$setting.raw" \
    python3 - <<'PY'
import os,stat,wave
from pathlib import Path
src=Path(os.environ["RAW_WAV"]); dest=Path(os.environ["RAW_OUT"])
with wave.open(str(src),"rb") as f:
    assert (f.getframerate(),f.getnchannels(),f.getsampwidth(),
            f.getnframes())==(16000,1,2,128*320)
    raw=f.readframes(f.getnframes())
with dest.open("xb") as out: out.write(raw)
dest.chmod(0o600)
assert stat.S_IMODE(dest.stat().st_mode)==0o600
PY
  record human_voice "whisper-ns$setting" \
    "$private/whisper-ns$setting.raw" 128
done

# Use the first contiguous 178 20ms frames from genuine road field noise
# scaled ÷8 or ÷16, with 50 silent frames + 128 original voice frames.
# This is a software overlay, NOT a simultaneous real microphone recording.
for divisor in 8 16; do
  batch="$private/mixed-$divisor"
  mkdir -m 700 "$batch"
  python3 speech_input/noise_lab/tools/mix_recorded_field_noise.py prepare \
    --clean-raw "$private/whisper.raw" \
    --clean-sha256 "$(sha256sum "$private/whisper.raw" | awk '{print $1}')" \
    --recorded-noise-raw "$private/road.raw" \
    --recorded-noise-sha256 "$(sha256sum "$private/road.raw" | awk '{print $1}')" \
    --private-root "$batch" --scale-divisor "$divisor" \
    > "$private/overlay-$divisor.private.status.json"

  # Exactly the same recorded-noise segment used in the overlay, but
  # with absolutely no human voice in the source.
  NOISE_DIVISOR="$divisor" NOISE_INPUT="$private/road.raw" \
  NOISE_OUTPUT="$batch/noiseonly.raw" python3 - <<'PY'
import os,struct,sys
from pathlib import Path
sys.path.insert(0,"speech_input/noise_lab/tools")
from mix_recorded_field_noise import _toward_zero_divide
from known_noise_reference import new_private
path=Path(os.environ["NOISE_INPUT"])
raw=path.read_bytes()
pcm=struct.unpack("<%dh"%(len(raw)//2),raw)
divisor=int(os.environ["NOISE_DIVISOR"])
assert divisor in (8,16)
noise=[_toward_zero_divide(x,divisor) for x in pcm[:178*320]]
assert len(noise)==178*320 and any(noise)
new_private(Path(os.environ["NOISE_OUTPUT"]),
            struct.pack("<%dh"%len(noise),*noise))
PY

  record human_voice "clean-$divisor" "$batch/clean-reference.raw" 178
  record human_voice "recorded-overlay-$divisor" \
    "$batch/mixed-recorded-road.raw" 178
  record noise_only "noiseonly-$divisor" "$batch/noiseonly.raw" 178

  for db in -12 -18; do
    "$ns" "$batch/mixed-recorded-road.raw" "$batch/mixed-ns$db.raw" \
      "$db" --align-one-frame > "$private/ns-overlay-$divisor-$db.private.json"
    "$ns" "$batch/noiseonly.raw" "$batch/noise-ns$db.raw" \
      "$db" --align-one-frame > "$private/ns-noise-$divisor-$db.private.json"
    record human_voice "overlay-$divisor-ns$db" "$batch/mixed-ns$db.raw" 178
    record noise_only "noise-$divisor-ns$db" "$batch/noise-ns$db.raw" 178
  done
done

test "$observed" = 19
printf '{"lab":"strict_C11_VAD_with_isolated_SpeexDSP","input_cases":19,"original_human_cases":3,"overlay_human_cases":8,"road_noise_only_cases":8,"model_weights_used":false,"artifacts_include_audio":false}\n'
