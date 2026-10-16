# Original Salts Capture Linux source TSan (virtual-only)

This **independent opt-in Linux Draft** continues #206. Its source
test originally compiled the **unchanged SaltsUtils v4.3.0-rc.11 Capture
Linux C11 production closure** with `-fsanitize=thread` and exposed a
real concurrent read/write data race at
`capture/src/capture_audio_miniaudio.c:50`: the audio callback read the
plain public `capture->state` while Linux start/stop wrote it.
The unsuppressed original-source [negative CI #38865018002](https://github.com/qigao/TurboAgent/actions/runs/38865018002)
failed as required, preserving the baseline evidence.

The positive acceptance retains **every other original rc.11 Capture,
video, screen and miniaudio source file** and substitutes ONLY
`capture_audio_miniaudio.c` from exact
[SaltsUtils Draft #653 commit `4e1c77f`](https://github.com/qigao/salts-utils/pull/653).
That patch introduces a **private C11 atomic callback admission gate**
instead of racing a public plain state enum. The original miniaudio
0.11.22 native backend remains unchanged. It runs actual original M1
SpeechInput and M2 `SpeechCaptureSalts` adapter from immutable SHA
`9e51d377bd84ee91ce5a47c94f24f73e4e01bba7`.

The source build deliberately includes original video/screen dispatchers:
`capture_video.c`, `capture_linux.c`, `capture_audio_miniaudio.c`,
`capture_video_linux.c`, `capture_screen_linux.c` and
`miniaudio_impl.c`. All are genuine production implementations, with
only the exact explicitly pinned **private atomic Capture source fix**
different from the tag; **no test stand-ins for `salts_capture_*`**. They link
original Linux PipeWire/X11 runtime libraries; only the audio path is
opened in this test.

## Strict input isolation

A separate private PulseAudio daemon with **no default modules** exposes
one local UNIX socket, one software `ta_ci_sink` null sink, **only one**
`ta_ci_sink.monitor` source, and **zero** hardware cards. Before any
`salts_capture_start`, the original #203 test refuses a missing,
multiple, physical or mismatched native source and passes the **exact
enumerated device ID**. Only bounded digital-zero PCM is fed into the
software sink. No user microphone/speaker, external model, recorded bytes,
network service or implicit device fallback is allowed.

The test validates at least six consecutive full sessions: actual
`salts_audio_capture_create/start`, the miniaudio callback's borrowed
PCM lifetime, `salts_capture_stop` and native callback thread join,
no post-stop callback; an intentionally **held in-flight callback**
must keep a competing `stop` call blocked until explicitly released,
then stop and destroy settle safely. The original M2 native adapter must
refuse early destroy, copy precisely-sized S16LE frames into the single
M1 bounded queue, stop/join before original M1 one-shot FINAL, and
ensure cancellation is terminal with no FINAL.

The candidate's unsuppressed [positive CI #38865851955](https://github.com/qigao/TurboAgent/actions/runs/38865851955)
passed **6/6** full native callback stop/join and M1/M2 one-shot
FINAL/cancellation cases with no race reports.

The executable has `DT_NEEDED libsalts_capture.so` whose loader path
must be **the newly source-built TSan library**, never the installed
uninstrumented published DSO. CI asserts `__tsan_init` inside this
library and its genuine `salts_capture_start/stop/create` definitions.
The original M1/M2 functions are defined locally, without a substitute
Capture API. `TSAN_OPTIONS=halt_on_error=1:exitcode=66` and **no
suppressions**; a race or deadlock fails the job.

## Explicit non-claims

- This is source-level TSan **of the original Capture and miniaudio code**,
  **not** a new officially published binary nor a physical microphone test.
- The upstream Salts.Native Core/Platform SDK and OS PipeWire/PulseAudio
  dependencies are installed binaries, not rebuilt with TSan here.
- The test-only ASR Provider emits deterministic text and never performs
  Whisper/model-backed inference. It cannot establish acoustic accuracy,
  noise/AEC/barge-in, licensed TTS or a real voice interaction.
- Production defaults remain OFF, no new PCM queues, raw recording
  persistence, suppression, fallback, model download, merge or release.
- #206 original Harness/Agent/TTS worker and native Playback source TSan
  results remain separate required gates. Main full SDK Linux hidden
  `hash_bytes` and Windows CLI storage timeout remain separate.

**DRAFT / DO NOT MERGE / DO NOT RELEASE**; parent M4 issue #164 remains OPEN.
