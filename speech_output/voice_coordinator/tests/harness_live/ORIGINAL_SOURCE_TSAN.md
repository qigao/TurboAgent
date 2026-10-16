# Original-source ThreadSanitizer acceptance — test-only

This additional **independent Linux Draft** qualifies races in original
TurboAgent Harness/Agent/Session, original M1/M2/M3 C11 and M4.0/M4.1/M4.2,
plus a second, separate original SaltsUtils Playback/miniaudio callback
source build. It is **not** equivalent to a production release containing
source-instrumented published Salts DSO binaries.

## Original Harness + Voice source instrumentation

The original exact immutable TurboAgent source
`9e51d377bd84ee91ce5a47c94f24f73e4e01bba7` is configured through
its original `ENABLE_SANITIZER_THREAD=ON` CMake option and Debug profile.
That option globally adds `-fsanitize=thread -fno-omit-frame-pointer` to
source compilation/linking, rather than merely sanitizing an external
consumer. The original C11 M1/M2 and CMeta bounded executor,
actual production Harness/Agent/Session .so targets, original
TTS Provider DSO C11 pthread fixture and all their TurboAgent source
dependencies must be compiled under TSan. The separate current-head
experimental M4.0/1/2 C11 static libraries and Host policy gate are
also rebuilt from source with TSan before linking into this binary.
The CI checks `__tsan_init` symbol references in both actual Harness
and Agent DSOs and M4 static library, not just a linked sanitizer executable.

It exercises the same original true Harness terminal/checked-final,
active worker TTS cancellation, late PCM rejection, source terminal,
`pthread_join` and real DSO `dlclose` lifetime paths, and the explicit
single-process virtual PulseAudio fullstack with both released Salts
Capture/Playback audio DSOs. The default original 20x/8x Release
acceptance remains unchanged and GREEN from #205; **this independent
source-TSan job** uses 4x genuine Harness repetitions and 2x
virtual Pulse fullstack as a bounded race-detection witness.

`TSAN_OPTIONS=halt_on_error=1:exitcode=66:history_size=7` is a
diagnostic mode, **NOT** a suppression. Any detected race must fail CI.
A source-level race should be fixed at the correct original ownership
boundary and independently retested; never replace the actual Provider
with a mock to make a reported race disappear.

## Original Playback/miniaudio source instrumentation

Original untouched SaltsUtils tag `v4.3.0-rc.11` playback
`miniaudio_impl.c`, `playback_buffer.c`, `salts_playback.c` and
the current M4 C11 source are compiled together with
`SALTS_PLAYBACK_TEST_NULL_BACKEND=1`, original upstream miniaudio
0.11.22, and TSan. The test exercises actual upstream
`ma_context` / `ma_device`, the real backend callback, ring
short-write/suffix/drain and original stop/pause/resume/clear/restart
and unexpected-stop negative paths 4x. Binary source `__tsan_init`
witnesses are required from both original Salts Playback .a and
current M4 Coordinator .a.

## Explicit limitations / non-claims

The installed **released** SaltsUtils Native 4.3.0-rc.11
`libsalts_capture.so` and `libsalts_playback.so`, Salts.Native,
CHttp.Native, original third-party SQLite and actual Whisper.cpp
v1.9.5 vendor CPU library are **prebuilt or independently linked**
and not rebuilt under TSan in this job. The M3 Whisper Provider C11
source is instrumented but uses a test-only v1.9.5 vendor ABI for
the model-free positive. The original Salts native Playback source
null-backend test is source-instrumented, but its published binary is
not. The virtual Capture OS callback in the fullstack test is real
but the released Capture DSO **is not itself** source TSan-instrumented.

No physical microphone/speaker, real ASR model, licensed TTS voice,
human speech accuracy, acoustic AEC/barge-in, user permissions or
cross-OS actual audio claim. No model download, production/default
enable, C11 fallback, second PCM queue, merge or publication.

All ancestor speech PRs including this one remain
**DRAFT / DO NOT MERGE / DO NOT RELEASE**. Main SDK Praktor `hash_bytes`
and Windows CLI storage timeout remain separately tracked; no CI
disablement or suppression has been authorized.
