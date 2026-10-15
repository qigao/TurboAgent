# M4.2 Voice Coordinator — standalone Draft, default OFF

Stacked on experimental M4.1 Draft #173; tracked by #164. This module
only builds with ENABLE_TTS_PLAYBACK_BRIDGE_TEST=ON and
ENABLE_TTS_VOICE_COORDINATOR_TEST=ON. It does not enter the ordinary
TurboAgent Agent/SDK graph.

## Sole owner / host approval

Host owns Capture/ASR stop+join, checked ASR FINAL, Harness/Session tool
authority, approval of committed user-visible Agent FINAL, SpeechOutput,
and the existing Salts Playback. Coordinator borrows SpeechOutput and
Playback and does not invoke ASR, Agent, tools, native audio callbacks
or any new device/DSO owner. A single serialized host owner calls every API.

ASR PARTIAL is never admitted for TTS. A checked FINAL alone is also
insufficient. The host must separately confirm that microphone capture
was stopped and all callbacks joined before approved user-visible
response text can be sent to SpeechOutput. Coordinator cannot prove the
truth of those external host assertions: real Session integration is
an independent acceptance gate.

One bridge-owned 480-sample/960-byte PCM stage, no second PCM queue or
fallback device. A single pump call allows up to eight Provider polls;
a single drain call has an explicit 1000ms maximum. Device ring drain
does NOT prove user-heard playback.

## State transitions

READY / COMPLETED / INTERRUPTED -> INPUT only for a newer generation.
INPUT -> WAIT_APPROVAL only after checked FINAL plus input quiescence.
WAIT_APPROVAL -> SPEAKING only on explicitly approved committed plain text
that passes M4.0 UTF-8, voice/locale and length validation.
SPEAKING -> DRAIN_PENDING only after SOURCE_DONE; a successful native
ring drain -> COMPLETED. No new input generation or ASR event is
admitted while output/drain/cancel is active.

Explicit UI/push-to-talk interruption (not acoustic/VAD barge-in) of
INPUT requires microphone quiescence before returning INTERRUPTED.
Interrupting output forbids writes, cancels Provider, clears the native
ring and waits for explicit Provider settlement. A failed clear keeps
CANCEL_PENDING; subsequent pump retries clear BEFORE polling Provider.
A noncooperative Provider can remain pending indefinitely: destroy
returns EBUSY and host must retain its borrowed DSO. Device or Provider
failures become FAILED, never COMPLETED.

## Qualification and remaining work

Branch-only sanitizer CI uses SHA-verified published SaltsUtils.Native
4.3.0-rc.11 and Salts.Native 2.3.0-rc.16. Strict C11 fake tests cover
checked FINAL/PARTIAL isolation, generations, FIFO/short writes, bounded
drain, device/clear failure, late PCM and stalled Provider settlement.
A separate link witness pulls current-head coordinator + bridge +
SpeechOutput archives against the actually published Salts::Playback
imported target. The test-only fake does not open a speaker.

This is NOT actual released null-device or physical speaker runtime,
NOT independently TSan-verified concurrent inference/DSO unload,
NOT real Harness/ASR integration, and NOT acoustic echo/AEC validation.
M4.3 real TTS model licensing, downloads, worker/DSO and hardware all
remain separately authorized acceptance work. DO NOT MERGE / RELEASE.
