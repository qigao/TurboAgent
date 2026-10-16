# M4.2 Harness completed-FINAL gate (strict C11, test-only)

This code belongs solely to the opt-in Host integration experiment:
`ENABLE_TTS_HARNESS_FINAL_GATE_TEST=ON`, nested under
`ENABLE_TTS_VOICE_COORDINATOR_TEST=ON` and the explicit M4.1 Playback
gate. The default TurboAgent graph/Agent/Harness and installed SDK
remain unchanged. **Draft / DO NOT MERGE / DO NOT RELEASE.**

## Authoritative source and policy

The gate compiles with exact unmodified public Harness, App, Session,
RuntimeData and execution headers from upstream SpeechInput M2 commit
`9e51d377bd84ee91ce5a47c94f24f73e4e01bba7`. It links the
official `Salts::JsonParser` target and its QueryVM dependency
closure from SHA-pinned published SaltsUtils.Native v4.3.0-rc.11;
there is no private JSON parser or compatibility header.

The Host must first provide a checked ASR FINAL after the Capture
callbacks are joined and VoiceCoordinator reaches WAIT_APPROVAL.
Even then, SpeechOutput begins **only after** a trusted Host explicitly
approves a committed, user-visible response and all of the following
actual public Harness APIs report consistent facts:

- stable nonempty Harness execution ID;
- execution state exactly COMPLETED;
- underlying operation result SALTS_OK;
- current Harness-owned App last run ID equals the Host's exact expected
  run ID both **before and after** one-shot take_result;
- result summary status exactly `completed`, summary.run_id matches;
- canonical borrowed `turbo_agent_state_final_answer_text(state)` is
  nonempty and <= 4096 bytes **before** malloc-copy;
- `turbo_agent_app_result_text(state)` yields owned user-facing text;
  M4.0 additionally applies its original UTF-8/plain-text/voice/locale
  and selected text-length boundaries before synthesis.

A failed or stale run must never silently fall back to another run,
speculative SSE, tool JSON, raw ASR transcript or alternative Provider.
A taken result stays consumed, even when its summary or final text
fails subsequent checks. The gate never owns Harness execution or
Session, invokes tools, creates a native device, persists PCM, or
introduces another Runtime owner. It always releases copied response
text (zeroed first) and terminal JSON trees after admission.

## What CI actually proves

The dedicated standalone C11 executable supplies **test-only Harness
function doubles**, compiled against the exact real public header
signatures. The JSON objects and `json_get_string` behavior come from
the actual published Salts JsonParser library. The tests exercise
COMPLETED, RUNNING, CANCELLED, INTERRUPTED, TIMED_OUT and FAILED,
operation failures, missing/invalid run IDs, unauthorized approval,
missing/oversized/markup text, one-shot result replay, changed current
run during take_result, Capture not quiesced, and the authorized
bounded PCM/drain positive.

The parent #182 tests original M1/M2 C11 production source, actual
pthread callback stop/join and one-shot checked ASR FINAL. Parent
#178 separately tests real Linux Provider DSO pthread/TSan and safe
`dlclose`. This Harness final policy test **does not run a genuine
Harness worker**; its Harness callbacks are doubles. End-to-end
Harness execution with a real bounded executor and deterministic
approved transport remains OPEN, as do real Whisper recognition,
device permissions, physical playback, TTS model and acoustic AEC.

No CLI/Agent/session dispatch is activated by this branch.
