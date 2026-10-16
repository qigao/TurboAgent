#!/usr/bin/env bash
# TEST ONLY: pinned real whispered voice + field warning noise, native C11
# SpeexDSP and actual C11 Capture/M1/Provider with FAKE Whisper inference.
set -euo pipefail
umask 077
test -n "$RUNNER_TEMP" && test -n "$GITHUB_WORKSPACE"
test -n "$SALTS_ROOT" && test -n "$MODEL_FIXTURES"
private="$(mktemp -d "$RUNNER_TEMP/road-noise-c11-XXXXXX")"
chmod 700 "$private"
trap 'rm -rf -- "$private"' EXIT
cli="$GITHUB_WORKSPACE/build/speech-cli-mock/mock_whisper_cli"
probe="$GITHUB_WORKSPACE/build/three-arm-native-c11/ta_speex_ns_raw_probe"
model="$MODEL_FIXTURES/fake-model.bin"
model_sha="$(cat "$MODEL_FIXTURES/fake-model.sha256")"
# Independent branch does not rely on any other Draft-only CI step.
cmake -S speech_input/noise_lab -B build/three-arm-native-c11 \
  -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/three-arm-native-c11 \
  --target ta_speex_ns_raw_probe --parallel 2
test -x "$cli" && test -x "$probe"
test "$(cat "$model")" = MOCK_SIGNED_SPEECH_MODEL_ONLY
test "$(sha256sum "$model" | awk '{print $1}')" = "$model_sha"
test "$(pkg-config --modversion speexdsp)" = 1.2.1
test "$(dpkg-query -W -f='${Version}' libspeexdsp-dev)" = 1.2.1-1ubuntu3
speex_lib="$(readlink -f "$(pkg-config --variable=libdir speexdsp)/libspeexdsp.so")"
test "$(sha256sum "$speex_lib" | awk '{print $1}')" = \
  ef4e0d1c80fb4fcf832bb98be7df08b44faaed84ee2c003070fa1b1023a1c5fd
export LD_LIBRARY_PATH="$SALTS_ROOT/lib:$GITHUB_WORKSPACE/build/speech-cli-mock"
decoder="$(realpath "$(command -v ffmpeg)")"
decoder_sha="$(sha256sum "$decoder" | awk '{print $1}')"
while IFS='|' read -r id bytes sha1 sha256 canonical url; do
  test -n "$id" || continue
  case "$id" in whisper|road) ;; *) exit 2 ;; esac
  curl --fail --location --silent --show-error --retry 2 \
    --connect-timeout 15 --max-time 60 --max-filesize 300000 \
    --proto '=https' --proto-redir '=https' \
    --user-agent 'TurboAgent-licensed-road-noise-C11/1.0 (qigao/TurboAgent)' \
    "$url" --output "$private/$id.ogg"
  test "$(wc -c < "$private/$id.ogg" | tr -d '[:space:]')" = "$bytes"
  test "$(sha1sum "$private/$id.ogg" | awk '{print $1}')" = "$sha1"
  test "$(sha256sum "$private/$id.ogg" | awk '{print $1}')" = "$sha256"
  python3 speech_input/whisper/tools/prepare_stationarity_wav.py \
    --source "$private/$id.ogg" --source-sha256 "$sha256" \
    --ffmpeg "$decoder" --ffmpeg-sha256 "$decoder_sha" \
    --output-wav "$private/$id.wav" \
    --private-provenance "$private/$id.private.provenance.json" \
    --acknowledge-rights > "$private/$id.private.status.json"
  test "$(sha256sum "$private/$id.wav" | awk '{print $1}')" = "$canonical"
done <<'SOURCE_PINS'
whisper|30283|cc3b98f93353cf6ed8bb453669987afe7dd748bd|bbdbe95dfe4742e27845c03fba3a8caf9f0f5ad65e34e512fd5609d155e6304e|c81cf6b75800daf1070d3001f094877581bc68100f3c64cb43638491f89988d0|https://upload.wikimedia.org/wikipedia/commons/e/e8/Whispering-example.ogg
road|120796|71500b016280d285ac6854bd46eef6373847247e|7d70594ef7ba1bd7a34136ab7140420d016fd730acc6c124a1a02fe6c75337d0|cd9023210066ea6ad618fe386d86c6e9469d4b9a19ba170d5032eb76fb30c808|https://upload.wikimedia.org/wikipedia/commons/e/e2/Back-up_white_noise.ogg
SOURCE_PINS
PRIVATE_AUDIO="$private" python3 - <<'PY'
import json,os,stat,wave
from pathlib import Path
root=Path(os.environ["PRIVATE_AUDIO"])
for name,frames in (("whisper",128),("road",375)):
    with wave.open(str(root/(name+".wav")),"rb") as f:
        assert (f.getframerate(),f.getnchannels(),f.getsampwidth(),
                f.getnframes())==(16000,1,2,frames*320)
        pcm=f.readframes(f.getnframes())
    path=root/(name+".raw")
    with path.open("xb") as out: out.write(pcm)
    path.chmod(0o600)
print(json.dumps({"verified_licensed_originals":2,"real_inference":False,
  "source_pcm_private":True},sort_keys=True))
PY
checks=0
for divisor in 8 16; do
  work="$private/case$divisor"
  mkdir -m 700 "$work"
  python3 speech_input/noise_lab/tools/mix_recorded_field_noise.py prepare \
    --clean-raw "$private/whisper.raw" \
    --clean-sha256 "$(sha256sum "$private/whisper.raw" | awk '{print $1}')" \
    --recorded-noise-raw "$private/road.raw" \
    --recorded-noise-sha256 "$(sha256sum "$private/road.raw" | awk '{print $1}')" \
    --scale-divisor "$divisor" --private-root "$work" \
    > "$private/mixture-$divisor.private.json"

  # Same first 178 frames of recorded-noise PCM, no real voice included.
  CASE_DIR="$work" ROAD_FILE="$private/road.raw" DIVISOR="$divisor" \
    python3 - <<'PY'
import os,struct,sys
from pathlib import Path
sys.path.insert(0,"speech_input/noise_lab/tools")
from known_noise_reference import new_private
from mix_recorded_field_noise import _toward_zero_divide
path=Path(os.environ["ROAD_FILE"])
root=Path(os.environ["CASE_DIR"])
raw=path.read_bytes()
values=struct.unpack("<%dh"%(len(raw)//2),raw)
factor=int(os.environ["DIVISOR"])
assert factor in (8,16)
field=[_toward_zero_divide(x,factor) for x in values[:178*320]]
assert len(field)==178*320 and any(field)
new_private(root/"noiseonly.raw",struct.pack("<%dh"%len(field),*field))
PY

  for source in mixed-recorded-road noiseonly; do
    for db in -12 -18; do
      "$probe" "$work/$source.raw" "$work/$source-ns$db.raw" \
        "$db" --align-one-frame \
        > "$private/processed-$divisor-$source-$db.private.json"
    done
  done

  # Generate only exact 178*320 sample WAVs, private 0600.
  CASE_DIR="$work" DIVISOR="$divisor" python3 - <<'PY'
import json,os,sys
from pathlib import Path
sys.path.insert(0,"speech_input/noise_lab/tools")
from prepare_paired_asr_wavs import wrapped_raw,write_new
root=Path(os.environ["CASE_DIR"])
arm_map={
    "clean":"clean-reference.raw",
    "mixed":"mixed-recorded-road.raw",
    "ns12":"mixed-recorded-road-ns-12.raw",
    "ns18":"mixed-recorded-road-ns-18.raw",
    "noiseonly":"noiseonly.raw",
    "noiseonly-ns12":"noiseonly-ns-12.raw",
    "noiseonly-ns18":"noiseonly-ns-18.raw",
}
for name,rawname in arm_map.items():
    raw=(root/rawname).read_bytes()
    assert len(raw)==178*640
    write_new(root/(name+".wav"),wrapped_raw(raw,178))
print(json.dumps({"noise_divisor":int(os.environ["DIVISOR"]),
  "arms":len(arm_map),"frames_per_arm":178,
  "noise_only_controls":3,"human_speech_conditions":4,
  "recorded_noise_was_software_overlaid":True,
  "real_ASR_hallucination_measured":False},sort_keys=True))
PY

  # All six voices-with-noise must reach fake vendor (Guard not falsely
  # rejecting actual speech). For noise-only, only report OFF/ON outcome;
  # fake vendor text on noise is NOT genuine Whisper hallucination evidence.
  for arm in clean mixed ns12 ns18 noiseonly noiseonly-ns12 noiseonly-ns18; do
    for guard in 0 1; do
      out="$work/$arm-$guard.stdout"
      err="$work/$arm-$guard.stderr"
      set +e
      "$cli" --model "$model" --sha256 "$model_sha" \
        --wav "$work/$arm.wav" --language en \
        --stationarity-guard "$guard" \
        --threads 1 --timeout-ms 30000 > "$out" 2> "$err"
      code=$?
      set -e
      if [ "$code" != 0 ]; then
        echo '{"error":"native_C11_decoding_failed_not_NO_SPEECH"}' >&2
        exit 3
      fi
      if [ "$(cat "$out")" = ' utterance' ] &&
         ! grep -Fq NO_SPEECH "$err"; then
        terminal=FINAL
      elif [ ! -s "$out" ] &&
           grep -Fq 'NO_SPEECH (no transcript emitted)' "$err"; then
        terminal=NO_SPEECH
      else
        echo '{"error":"malformed_native_C11_terminal"}' >&2
        exit 3
      fi
      case "$arm" in
        clean|mixed|ns12|ns18)
          if [ "$terminal" != FINAL ]; then
            echo '{"error":"real_speech_rejected_before_fake_vendor"}' >&2
            exit 4
          fi ;;
        *)
          if [ "$guard" = 0 ] && [ "$terminal" != FINAL ]; then
            echo '{"error":"noise_control_failed_with_guard_off"}' >&2
            exit 4
          fi ;;
      esac
      checks=$((checks+1))
      printf '{"kind":"native_C11_NoiseGuard_fake_Whisper","recorded_noise_scale_divisor":%s,"arm":"%s","guard_on":%s,"terminal":"%s","fake_vendor_would_run":%s,"real_hallucination_measured":false}\n' \
        "$divisor" "$arm" "$guard" "$terminal" \
        "$([ "$terminal" = FINAL ] && echo true || echo false)"
    done
  done
done
test "$checks" = 28
echo '{"native_C11_terminal_checks":28,"real_Whisper_model_used":false,"no_audio_uploaded":true}'
