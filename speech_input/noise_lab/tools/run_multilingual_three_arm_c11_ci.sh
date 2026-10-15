#!/usr/bin/env bash
# TEST ONLY: pinned licensed voices -> SpeexDSP -12/-18 -> native C11
# Capture/M1/Provider with FAKE vendor, never model weights.
set -euo pipefail
umask 077
test -n "$RUNNER_TEMP" && test -n "$GITHUB_WORKSPACE"
test -n "$SALTS_ROOT" && test -n "$MODEL_FIXTURES"
private="$(mktemp -d "$RUNNER_TEMP/ns-language-XXXXXX")"
chmod 700 "$private"
trap 'rm -rf -- "$private"' EXIT
model="$MODEL_FIXTURES/fake-model.bin"
model_sha="$(cat "$MODEL_FIXTURES/fake-model.sha256")"
cli="$GITHUB_WORKSPACE/build/speech-cli-mock/mock_whisper_cli"
probe="$GITHUB_WORKSPACE/build/three-arm-native-c11/ta_speex_ns_raw_probe"
test -x "$cli" && test -x "$probe" && test -f "$model"
test "$(pkg-config --modversion speexdsp)" = 1.2.1
test "$(dpkg-query -W -f='${Version}' libspeexdsp-dev)" = 1.2.1-1ubuntu3
speex_lib="$(readlink -f "$(pkg-config --variable=libdir speexdsp)/libspeexdsp.so")"
test "$(sha256sum "$speex_lib" | awk '{print $1}')" = \
  ef4e0d1c80fb4fcf832bb98be7df08b44faaed84ee2c003070fa1b1023a1c5fd
export LD_LIBRARY_PATH="$SALTS_ROOT/lib:$GITHUB_WORKSPACE/build/speech-cli-mock"
ffmpeg="$(realpath "$(command -v ffmpeg)")"
ffmpeg_sha="$(sha256sum "$ffmpeg" | awk '{print $1}')"
probe_sha="$(sha256sum "$probe" | awk '{print $1}')"
checked=0
# id|lang|source bytes|SHA1|SHA256|canonical WAV SHA256|20ms frames|URL|gate
while IFS='|' read -r identity lang bytes sha1 sha256 wavsha frames url gate; do
  test -n "$identity" || continue
  case "$identity" in ja|zh|en|quiet) ;; *) exit 2;; esac
  case "$gate" in strict|short-quiet-observation) ;; *) exit 2;; esac
  checked=$((checked+1))
  work="$private/$identity"
  mkdir -m 700 "$work"
  curl --fail --location --silent --show-error --retry 2 \
    --connect-timeout 15 --max-time 60 --max-filesize 500000 \
    --proto '=https' --proto-redir '=https' \
    --user-agent 'TurboAgent-multilingual-C11-NS-evidence/1.0 (qigao/TurboAgent)' \
    "$url" --output "$work/source.ogg"
  test "$(wc -c < "$work/source.ogg" | tr -d '[:space:]')" = "$bytes"
  test "$(sha1sum "$work/source.ogg" | awk '{print $1}')" = "$sha1"
  test "$(sha256sum "$work/source.ogg" | awk '{print $1}')" = "$sha256"
  python3 speech_input/whisper/tools/prepare_stationarity_wav.py \
    --source "$work/source.ogg" --source-sha256 "$sha256" \
    --ffmpeg "$ffmpeg" --ffmpeg-sha256 "$ffmpeg_sha" \
    --output-wav "$work/canonical.wav" \
    --private-provenance "$work/source.private.json" \
    --acknowledge-rights > "$work/source.status.private.json"
  test "$(sha256sum "$work/canonical.wav" | awk '{print $1}')" = "$wavsha"
  mkdir -m 700 "$work/paired"
  python3 speech_input/noise_lab/tools/prepare_paired_asr_wavs.py \
    --source-wav "$work/canonical.wav" --source-sha256 "$wavsha" \
    --ns-probe "$probe" --ns-probe-sha256 "$probe_sha" \
    --private-root "$work/paired" > "$work/paired.private.status"
  PAIRED_DIR="$work/paired" CASE_ID="$identity" FRAME_TOTAL="$frames" \
  python3 - <<'PY'
import hashlib,json,os,struct,sys,wave
from pathlib import Path
sys.path.insert(0,"speech_input/whisper/tools")
from measure_frame_stationarity import inspect_wav_bytes
root=Path(os.environ["PAIRED_DIR"])
name=os.environ["CASE_ID"]; expected=int(os.environ["FRAME_TOTAL"])
data=json.loads((root/"paired-private.json").read_text())
assert data["frame_count"]==expected
assert data["ns_configuration"]=={
    "ns12":-12,"ns18":-18,"VAD":False,"AGC":False,
    "tail_flush":"one_frame_explicit"}
powers={}
for arm in ("original","ns12","ns18"):
    raw=(root/(arm+".wav")).read_bytes()
    assert hashlib.sha256(raw).hexdigest()==data["wavs"][arm]["sha256"]
    metrics=inspect_wav_bytes(raw)
    assert metrics["frame_count"]==expected
    with wave.open(str(root/(arm+".wav")),"rb") as reader:
        assert (reader.getframerate(),reader.getnchannels(),
                reader.getsampwidth(),reader.getnframes())==(16000,1,2,expected*320)
        pcm=struct.unpack("<%dh" % (expected*320),reader.readframes(expected*320))
    powers[arm]=[sum(v*v for v in pcm[i:i+320])
                 for i in range(0,len(pcm),320)]
    print(json.dumps({
      "kind":"licensed_voice_NS_only_model_free",
      "language_sample":name,"arm":arm,
      "frames":expected,
      "stationarity_hypothesis":metrics["stationarity_hypothesis"],
      "nonzero_20ms_frames":sum(x>0 for x in powers[arm]),
      "independently_SHA_pinned":True,"real_ASR_model_used":False,
      "quality_or_intelligibility_certified":False
    },sort_keys=True))
for arm in ("ns12","ns18"):
    print(json.dumps({
      "kind":"aligned_voice_frame_zero_observation",
      "language_sample":name,"arm":arm,
      "newly_zeroed_source_frames":sum(a>0 and b==0 for a,b in
            zip(powers["original"],powers[arm])),
      "voice_quality_certified":False
    },sort_keys=True))
assert {p.name for p in root.iterdir()}=={
    "original.wav","ns12.wav","ns18.wav","paired-private.json"}
PY
  for arm in original ns12 ns18; do
    for guard in 0 1; do
      stdout="$work/$arm-$guard.stdout"
      stderr="$work/$arm-$guard.stderr"
      set +e
      "$cli" --model "$model" --sha256 "$model_sha" \
        --wav "$work/paired/$arm.wav" --language "$lang" \
        --stationarity-guard "$guard" --threads 1 --timeout-ms 30000 \
        > "$stdout" 2> "$stderr"
      status=$?
      set -e
      test "$status" = 0 || exit 3
      if [ "$(cat "$stdout")" = ' utterance' ] &&
         ! grep -Fq 'NO_SPEECH' "$stderr"; then
        result=FINAL
      elif [ ! -s "$stdout" ] &&
           grep -Fq 'NO_SPEECH (no transcript emitted)' "$stderr"; then
        result=NO_SPEECH
      else
        exit 3
      fi
      if [ "$arm" = original ] || [ "$gate" = strict ]; then
        test "$result" = FINAL || exit 4
      fi
      # Short quiet original is only 18 frames and has very low energy.
      # Filtering may drop it; report that NEGATIVE outcome, not a pass.
      printf '{"test_kind":"native_C11_Provider_fake_engine","language_sample":"%s","arm":"%s","guard_enabled":%s,"outcome":"%s","admission":"%s","model_weights_used":false,"real_WER_CER_measured":false}\n' \
        "$identity" "$arm" "$guard" "$result" "$gate"
    done
  done
done <<'PINNED_VOICES'
ja|ja|264858|c7992ec2188ee94102f83e4eff22c4f9b9365d39|cad2f1545a3ac898362590ea636b8c09f1d77391b506790ae909e0e1ff1441f7|ccb16e8ea766bd2f4bf40ed09e5666ea6700edbd79343eb961fb13e9cafd6a03|740|https://upload.wikimedia.org/wikipedia/commons/b/b9/%E6%98%A5%E3%81%AF%E3%81%82%E3%81%91%E3%81%BC%E3%81%AE.ogg|strict
zh|zh|156728|2fb567da2edd9fc18160769b1c803bd9d072c273|304f11fa91ec51b2a2b9e4c11424f9944c6bdf3ee909440923511a86b5585b22|f246dded9eb91796c515f21b37f8e41b3357bdef3f6fe19552568c12653f0421|749|https://upload.wikimedia.org/wikipedia/commons/8/8a/Mandarin_Chinese-%E5%8D%97%E5%9B%BD%E5%B1%B1%E6%B2%B3.ogg|strict
en|en|21562|79b6f88e3db116fab8bc068cd523420406bef1c4|10829789c781d912ccbfdd9a4f5d4b3dbb2dac0a42af34610f7cf4497ad25deb|458ce0d1086ae8a5381ccc09878a630c016ebed42303a859e2030e2efbbe339e|95|https://upload.wikimedia.org/wikipedia/commons/5/58/En-uk-Do_you_speak_English.ogg|strict
quiet|en|12926|9d7986ce0257056b679a6b199c42447b5df82962|3725cf00284943531b0e899e3f7161117397cd3be8f6310a7beca26fbd6e0e3d|6f4f58227166088ba2745d51af05553b01be557b5a2a9d2e04edcbff21d9730a|18|https://upload.wikimedia.org/wikipedia/commons/1/1b/English_male_voice_speech_word_No.ogg|short-quiet-observation
PINNED_VOICES
test "$checked" = 4
printf '{"test_kind":"licensed_multilingual_C11_SpeexDSP","source_files":4,"source_sha_pinned":true,"real_model_weights_used":false}\n'
