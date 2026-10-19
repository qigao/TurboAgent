#include "turbo_agent_speech_capture.h"

#include <salts_capture.h>
#include <stdlib.h>
#include <string.h>

struct turbo_agent_speech_salts_capture_s {
  salts_capture_t *native; /* unique Salts::Capture device owner */
  turbo_agent_speech_capture_bridge_t *bridge; /* no duplicate PCM queue */
  int started;
  int terminal;
};

/* Called only by Salts::Capture's borrowed native audio callback. Never
 * allocate, log, request stop/destroy, call a Provider or enter Agent. */
static void salts_pcm_callback(salts_capture_t *capture,
                               const uint8_t *borrowed_pcm,
                               size_t bytes, uint64_t timestamp_us,
                               void *user_data) {
  turbo_agent_speech_capture_bridge_t *bridge =
      (turbo_agent_speech_capture_bridge_t *)user_data;
  (void)capture;
  (void)turbo_agent_speech_capture_bridge_offer(
      bridge, borrowed_pcm, bytes, timestamp_us);
}

int turbo_agent_speech_salts_capture_create(
    turbo_agent_speech_input_t *input, uint64_t utterance_id,
    const char *device_id, turbo_agent_speech_salts_capture_t **out_capture) {
  turbo_agent_speech_salts_capture_t *capture;
  salts_audio_capture_config_t config;
  int rc;
  if (!out_capture) return TURBO_AGENT_SPEECH_EINVAL;
  *out_capture = NULL;
  if (!input || utterance_id == 0u ||
      (device_id && strlen(device_id) >= sizeof(((salts_capture_device_t *)0)->id)))
    return TURBO_AGENT_SPEECH_EINVAL;
  capture = (turbo_agent_speech_salts_capture_t *)calloc(1, sizeof(*capture));
  if (!capture) return TURBO_AGENT_SPEECH_ENOMEM;
  rc = turbo_agent_speech_capture_bridge_create(
      input, utterance_id, &capture->bridge);
  if (rc != TURBO_AGENT_SPEECH_OK) {
    free(capture);
    return rc;
  }
  /* No implicit native/backend format changes. Unsupported exact device
   * or sample-rate/period permission errors fail at create/start. */
  config.sample_rate = (int)TURBO_AGENT_SPEECH_SAMPLE_RATE_HZ;
  config.channels = (int)TURBO_AGENT_SPEECH_CHANNELS;
  config.bits_per_sample = 16;
  config.frame_size_ms = 20;
  capture->native = salts_audio_capture_create(
      device_id && device_id[0] ? device_id : NULL, &config);
  if (!capture->native) {
    (void)turbo_agent_speech_capture_bridge_destroy(&capture->bridge);
    free(capture);
    return TURBO_AGENT_SPEECH_CAPTURE_EDEVICE;
  }
  salts_audio_capture_set_callback(
      capture->native, salts_pcm_callback, capture->bridge);
  *out_capture = capture;
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_salts_capture_start(
    turbo_agent_speech_salts_capture_t *capture) {
  int rc;
  if (!capture || !capture->native || !capture->bridge)
    return TURBO_AGENT_SPEECH_EINVAL;
  if (capture->started || capture->terminal) return TURBO_AGENT_SPEECH_ESTATE;
  rc = turbo_agent_speech_capture_bridge_begin(capture->bridge);
  if (rc != TURBO_AGENT_SPEECH_OK) {
    capture->terminal = 1;
    return rc;
  }
  if (salts_capture_start(capture->native) != 0) {
    /* A partially started backend must be quiesced BEFORE canceling Provider
     * or destroying Capture. No implicit device or cloud fallback. */
    (void)turbo_agent_speech_capture_bridge_stop_admission(capture->bridge);
    salts_capture_stop(capture->native);
    (void)turbo_agent_speech_capture_bridge_cancel(capture->bridge);
    capture->terminal = 1;
    return TURBO_AGENT_SPEECH_CAPTURE_EDEVICE;
  }
  capture->started = 1;
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_salts_capture_first_error(
    const turbo_agent_speech_salts_capture_t *capture) {
  if (!capture || !capture->bridge) return TURBO_AGENT_SPEECH_EINVAL;
  return turbo_agent_speech_capture_bridge_first_error(capture->bridge);
}

int turbo_agent_speech_salts_capture_finish(
    turbo_agent_speech_salts_capture_t *capture) {
  int rc;
  if (!capture || !capture->started || capture->terminal)
    return TURBO_AGENT_SPEECH_ESTATE;
  (void)turbo_agent_speech_capture_bridge_stop_admission(capture->bridge);
  salts_capture_stop(capture->native); /* joins/settles native callbacks */
  capture->started = 0;
  capture->terminal = 1;
  rc = turbo_agent_speech_capture_bridge_finish(capture->bridge);
  return rc;
}

int turbo_agent_speech_salts_capture_cancel(
    turbo_agent_speech_salts_capture_t *capture) {
  int rc;
  if (!capture || !capture->started || capture->terminal)
    return TURBO_AGENT_SPEECH_ESTATE;
  (void)turbo_agent_speech_capture_bridge_stop_admission(capture->bridge);
  salts_capture_stop(capture->native); /* never called on its own callback */
  capture->started = 0;
  capture->terminal = 1;
  rc = turbo_agent_speech_capture_bridge_cancel(capture->bridge);
  return rc;
}

int turbo_agent_speech_salts_capture_destroy(
    turbo_agent_speech_salts_capture_t **handle) {
  turbo_agent_speech_salts_capture_t *capture;
  int rc;
  if (!handle || !*handle) return TURBO_AGENT_SPEECH_EINVAL;
  capture = *handle;
  if (capture->started) return TURBO_AGENT_SPEECH_EBUSY;
  rc = turbo_agent_speech_capture_bridge_destroy(&capture->bridge);
  if (rc != TURBO_AGENT_SPEECH_OK) return rc;
  /* Native stop has already settled callbacks, so user_data is no longer
   * accessed. Salts remains the only owner of the native device. */
  salts_capture_destroy(capture->native);
  capture->native = NULL;
  free(capture);
  *handle = NULL;
  return TURBO_AGENT_SPEECH_OK;
}
