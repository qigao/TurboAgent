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

## Read-only blocker snapshot

The host may call turbo_voice_snapshot_init() then
turbo_voice_get_snapshot() from its serialized control Owner. ABI/version
and exact struct_size are checked; no handles, TTS text, PCM or device
pointers escape. The flags are observations of existing obligations,
not a second ownership authority:

- INPUT_QUIESCE: Capture stopped/callback join not yet attested.
- DEVICE_CLEAR: M4.1 still owes native ring clear after cancel.
- PROVIDER_SETTLE: the TTS Provider has not returned a terminal settle.
- PLAYBACK_DRAIN: SOURCE_DONE but device ring consumption not confirmed.
- HOST_APPROVAL: ready for committed approved user-visible text.
- FAULT: the generation has observed a device/Provider error.

A failed clear and outstanding Provider settlement can be reported
simultaneously. After ring clear succeeds, the Provider barrier remains
until explicit terminal cancellation; no timeout grants DSO unload.
A permanent device error and a permanently noncooperative Provider
remain fail-closed indefinitely: the Host must retain their borrowed
lifetime or use a separately reviewed isolated-process kill boundary.
The deterministic test runs 32 unsuccessful clear retries and 64
noncooperative Provider ticks, rejects premature destroy/new input and
verifies recovery only after explicit clear and settle.

Snapshot state does NOT independently verify an external mic callback
join, a trusted ASR FINAL or committed Harness approval. Acoustic
echo/AEC is not tested by rejecting in-process ASR events.

The additional fatal-drain conformance first reaches SOURCE_DONE, then
injects a native drain device failure plus an unsuccessful clear. It
requires FAULT + DEVICE_CLEAR, blocks destruction and new generations,
and only allows FAILED (never COMPLETED) after the clear obligation is
actually discharged. The public release consumer also executes the real
pinned Playback library's invalid PCM-rate rejection (12345 Hz) before
any device-context initialization, for C11 and C++17. This is a
**real-library fail-closed runtime negative**, not a null-device or
physical speaker playback positive.
