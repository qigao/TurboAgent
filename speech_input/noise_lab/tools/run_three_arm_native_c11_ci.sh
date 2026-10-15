#!/usr/bin/env bash
# TEST ONLY: exact licensed whisper -> private SpeexDSP three-arm WAVs ->
# production C11 M2/M1/Whisper Provider with FAKE vendor; NO model weights.
set -euo pipefail
umask 077
test -n "$RUNNER_TEMP"
test -n "$GITHUB_WORKSPACE"
test -n "$SALTS_ROOT"
test -n "$MODEL_FIXTURES"
private="$(mktemp -d "$RUNNER_TEMP/speex-native-three-arm-XXXXXX")"
chmod 700 "$private"
trap 'rm -rf -- "$private"' EXIT

test "$(pkg-config --modversion speexdsp)" = 1.2.1
test "$(dpkg-query -W -f='${Version}' libspeexdsp-dev)" = '1.2.1-1ubuntu3'
speex_bin="$(readlink -f "$(pkg-config --variable=libdir speexdsp)/libspeexdsp.so")"
test -f "$speex_bin"
test "$(sha256sum "$speex_bin" | awk '{print $1}')" = \
  ef4e0d1c80fb4fcf832bb98be7df08b44faaed84ee2c003070fa1b1023a1c5fd
cmake -S speech_input/noise_lab \
  -B build/three-arm-native-c11 -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/three-arm-native-c11 \
  --target ta_speex_ns_raw_probe --parallel 2

ns_probe="$GITHUB_WORKSPACE/build/three-arm-native-c11/ta_speex_ns_raw_probe"
cli="$GITHUB_WORKSPACE/build/speech-cli-mock/mock_whisper_cli"
model="$MODEL_FIXTURES/fake-model.bin"
model_sha="$(cat "$MODEL_FIXTURES/fake-model.sha256")"
test -x "$ns_probe"
test -x "$cli"
test -f "$model"

# CC0 English whisper by Mx. Granger; verified license/source in #158.
source_url='https://upload.wikimedia.org/wikipedia/commons/e/e8/Whispering-example.ogg'
source_sha='bbdbe95dfe4742e27845c03fba3a8caf9f0f5ad65e34e512fd5609d155e6304e'
curl --fail --location --silent --show-error --retry 2 \
  --connect-timeout 15 --max-time 60 --max-filesize 100000 \
  --proto '=https' --proto-redir '=https' \
  --user-agent 'TurboAgent-three-arm-C11-study/1.0 (qigao/TurboAgent)' \
  "$source_url" --output "$private/source.ogg"
test "$(wc -c < "$private/source.ogg" | tr -d '[:space:]')" = 30283
test "$(sha1sum "$private/source.ogg" | awk '{print $1}')" = \
  cc3b98f93353cf6ed8bb453669987afe7dd748bd
test "$(sha256sum "$private/source.ogg" | awk '{print $1}')" = "$source_sha"

ffmpeg_bin="$(realpath "$(command -v ffmpeg)")"
ffmpeg_sha="$(sha256sum "$ffmpeg_bin" | awk '{print $1}')"
python3 speech_input/whisper/tools/prepare_stationarity_wav.py \
  --source "$private/source.ogg" \
  --source-sha256 "$source_sha" \
  --ffmpeg "$ffmpeg_bin" --ffmpeg-sha256 "$ffmpeg_sha" \
  --output-wav "$private/canonical.wav" \
  --private-provenance "$private/original.provenance.json" \
  --acknowledge-rights > "$private/preflight.status.json"
test "$(sha256sum "$private/canonical.wav" | awk '{print $1}')" = \
  c81cf6b75800daf1070d3001f094877581bc68100f3c64cb43638491f89988d0

mkdir -m 700 "$private/paired"
python3 speech_input/noise_lab/tools/prepare_paired_asr_wavs.py \
  --source-wav "$private/canonical.wav" \
  --source-sha256 "$(sha256sum "$private/canonical.wav" | awk '{print $1}')" \
  --ns-probe "$ns_probe" \
  --ns-probe-sha256 "$(sha256sum "$ns_probe" | awk '{print $1}')" \
  --private-root "$private/paired" > "$private/paired.status.json"

PRIVATE_WAVS="$private/paired" python3 - <<'PY'
import hashlib,json,os,sys
from pathlib import Path
sys.path.insert(0,"speech_input/whisper/tools")
from measure_frame_stationarity import inspect_wav_bytes
root=Path(os.environ["PRIVATE_WAVS"])
report=json.loads((root/"paired-private.json").read_text())
assert report["frame_count"]==128
assert (report["frame_samples"],report["sample_rate"])==(320,16000)
assert report["model_weights_or_inference_used"] is False
assert report["ns_configuration"]["VAD"] is False
assert report["ns_configuration"]["AGC"] is False
assert report["ns_configuration"]["tail_flush"]=="one_frame_explicit"
digests=[]
for arm in ("original","ns12","ns18"):
    path=root/(arm+".wav")
    raw=path.read_bytes()
    assert report["wavs"][arm]["sha256"]==hashlib.sha256(raw).hexdigest()
    assert report["wavs"][arm]["frames"]==128
    metrics=inspect_wav_bytes(raw)
    assert metrics["frame_count"]==128
    assert metrics["stationarity_hypothesis"]=="dynamic_or_unknown"
    digests.append(hashlib.sha256(raw).hexdigest())
assert digests[0]=="c81cf6b75800daf1070d3001f094877581bc68100f3c64cb43638491f89988d0"
assert len(set(digests))==3
assert {p.name for p in root.iterdir()}=={
    "original.wav","ns12.wav","ns18.wav","paired-private.json"}
print(json.dumps({
    "test_kind":"three_arm_pinned_real_whisper_pcm",
    "arms":3,"frames_each":128,
    "independent_native_c11_ns_modes":True,
    "all_guard_energy_hypotheses_dynamic":True,
    "all_exact_pcm_and_model_inputs_pinned":True,
    "real_model_used":False,"audio_artifact_uploaded":False
},sort_keys=True))
PY

# The C11 Provider and M2/M1 are actual implementation sources. Only
# the Whisper vendor inference engine is a deterministic TEST STUB.
export LD_LIBRARY_PATH="$SALTS_ROOT/lib:$GITHUB_WORKSPACE/build/speech-cli-mock"
for arm in original ns12 ns18; do
  for guard in 0 1; do
    "$cli" --model "$model" --sha256 "$model_sha" \
      --wav "$private/paired/$arm.wav" \
      --language en --stationarity-guard "$guard" \
      --threads 1 --timeout-ms 30000 \
      > "$private/$arm-guard$guard.stdout" \
      2> "$private/$arm-guard$guard.stderr"
    test "$(cat "$private/$arm-guard$guard.stdout")" = ' utterance'
    ! grep -Fq NO_SPEECH "$private/$arm-guard$guard.stderr"
  done
  printf '{"type":"real_C11_capture_provider_fake_vendor","arm":"%s","guard_off":"FINAL","guard_on":"FINAL","real_model_quality_measured":false}\n' "$arm"
done

# Score through a REAL separately compiled C11 CLI process and paired
# evaluator but with a FAKE transcript: 0 WER here is plumbing only.
printf ' utterance\n' > "$private/reference.txt"
python3 speech_input/noise_lab/tools/evaluate_whisper_paired.py \
  --cli "$cli" --cli-sha256 "$(sha256sum "$cli" | awk '{print $1}')" \
  --model "$model" --model-sha256 "$model_sha" --model-max-mib 128 \
  --original-wav "$private/paired/original.wav" \
  --original-sha256 "$(sha256sum "$private/paired/original.wav" | awk '{print $1}')" \
  --ns12-wav "$private/paired/ns12.wav" \
  --ns12-sha256 "$(sha256sum "$private/paired/ns12.wav" | awk '{print $1}')" \
  --ns18-wav "$private/paired/ns18.wav" \
  --ns18-sha256 "$(sha256sum "$private/paired/ns18.wav" | awk '{print $1}')" \
  --reference-file "$private/reference.txt" \
  --reference-sha256 "$(sha256sum "$private/reference.txt" | awk '{print $1}')" \
  --language en --critical-word utterance \
  --report "$private/paired-metric.json" > "$private/metric-private.stdout"

PRIVATE_METRIC="$private/paired-metric.json" python3 - <<'PY'
import json,os
from pathlib import Path
data=json.loads(Path(os.environ["PRIVATE_METRIC"]).read_text())
assert data["frame_count"]==128
assert data["critical_words_requested"]==1
assert data["stationarity_guard_enabled"] is False
assert data["model_or_audio_downloaded_by_evaluator"] is False
for key in ("original","ns12","ns18"):
    arm=data["conditions"][key]
    assert arm["status"]=="final"
    assert arm["wer"]==0 and arm["cer"]==0
    assert arm["critical_words_retained"]==1
print(json.dumps({
    "test_kind":"mocked_ASR_three_arm_score_pipeline",
    "all_three_fake_transcripts_scored":True,
    "real_ASR_WER_CER_measured":False,
    "real_model_weights_used":False,
    "no_private_audio_or_transcript_artifact":True
},sort_keys=True))
PY
# Broaden the exact same real C11-only smoke to independently licensed
# Japanese, Mandarin, ordinary English and explicitly short quiet speech.
# The short quiet clip is exploratory: any lost human speech is logged as
# a negative diagnostic, never counted as a quality success.
bash speech_input/noise_lab/tools/run_multilingual_three_arm_c11_ci.sh
# Everything remains under private runner temp until trap deletes it.
