# Dual released Salts Native DSOs under isolated virtual PulseAudio

This is an **independent, opt-in, default-OFF, C11 acceptance test** stacked
on #203. It adds a **second executable** to the previously accepted single
private PulseAudio null-sink/monitor server. The first executable retains all
original released Capture 20× lifecycle and held-callback/stop barriers.

The second executable **dynamically imports both** published SaltsUtils.Native
v4.3.0-rc.11 `Salts::Capture` and `Salts::Playback` instead of linking
the original test-null Playback static implementation from #202. Both
preinstalled SDK archives are SHA-256/size checked, pinned to Salts.Native
v2.3.0-rc.16 / SaltsUtils.Native v4.3.0-rc.11. The CI service exposes only
one `ta_ci_sink` software null sink and its `ta_ci_sink.monitor`
source, with zero physical audio cards. The test refuses to run unless both
the returned native input and the native playback device names contain the
exact `TurboAgentVirtual` identity; it passes the original enumerated
input ID and **unchanged output opaque device ID** to each published DSO.
No default-device fallback or real microphone/speaker is ever used.

## Same-process production native device evidence

- Original immutable M1 SpeechInput + M2 Native Salts Capture adapter
  (C11 SHA `9e51d377bd84ee91ce5a47c94f24f73e4e01bba7`): true
  miniaudio/PulseAudio virtual input callback, one M1 bounded PCM queue,
  callback stop/join before original one-shot FINAL, duplicate FINAL and
  stale generation refusal.
- Current-head M4.0 SpeechOutput, M4.1 Playback Bridge and M4.2 Voice
  Coordinator **production C11 source**, compiled directly in the same
  executable, connected to the *published* `Salts::Playback` DSO.
  A deterministic **TEST-only** TTS Provider emits one bounded 480-sample
  24k mono S16 PCM frame. With 10ms native playback buffer capacity, M4.1
  accepts exactly 480 of 960 bytes and RETAINS the rest. It cannot poll
  a new TTS frame until native Callback is started and space released.
  The real released Playback DSO's miniaudio callback drains its own ring;
  M4.2 reports COMPLETED **only after successful original native drain**,
  never by manually setting fake ring sizes.
- Independently, a currently playing official Playback DSO must safely
  `clear` its genuine native ring and stop/restart its native callback
  during M4.2 interruption; no late PCM, reported false success or double
  owned queue is permitted.

The CI runs both executables **20×** under the SAME isolated Pulse daemon and
asserts each semantic marker and ELF provenance: the combined executable must
have `NEEDED libsalts_capture.so` **and** `NEEDED libsalts_playback.so`,
import actual Capture/Playback start/write/drain/clear, and define the
original M1/M2 and current M4 symbols. No symbols are replaced with a
test implementation. The software Pulse null sink is an **external OS virtual
device**, unlike Salts' `SALTS_PLAYBACK_TEST_NULL_BACKEND` code path.

**Excluded:** physical mic/speaker permissions, human acoustics, real
Whisper/TTS weights and sound, genuine Harness invocation in this *specific*
second executable, acoustic echo/AEC, full production native DSOs TSan and
cross-platform physical-device validation. The previous #199 true original
Harness + loaded TTS Provider DSO path continues as its own opt-in matrix.
Production defaults OFF. DRAFT / DO NOT MERGE / DO NOT RELEASE.
