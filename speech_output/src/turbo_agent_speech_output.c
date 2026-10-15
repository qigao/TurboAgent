#include "turbo_agent_speech_output.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct turbo_agent_speech_output_s {
  turbo_agent_tts_provider_v1_t provider; /* copied callbacks, borrowed context */
  turbo_agent_tts_config_t config;
  turbo_agent_tts_state_t state;
  uint64_t generation_id;
  size_t total_pcm_samples;
  size_t max_pcm_samples;
  size_t pending_samples;
  size_t pending_offset;
  int fail_pending;
  char text[TURBO_AGENT_TTS_MAX_TEXT_BYTES + 1u];
  char voice[TURBO_AGENT_TTS_MAX_VOICE_BYTES + 1u];
  char language[3];
  int16_t frame[TURBO_AGENT_TTS_SAMPLES_PER_FRAME];
};

static void wipe(void *ptr, size_t bytes) {
  volatile unsigned char *p = (volatile unsigned char *)ptr;
  while (bytes) {
    *p++ = 0u;
    --bytes;
  }
}

static void clear_pending(turbo_agent_speech_output_t *output) {
  wipe(output->frame, sizeof(output->frame));
  output->pending_samples = 0u;
  output->pending_offset = 0u;
}

static void finish_generation(turbo_agent_speech_output_t *output,
                              turbo_agent_tts_state_t next) {
  clear_pending(output);
  wipe(output->text, sizeof(output->text));
  wipe(output->voice, sizeof(output->voice));
  wipe(output->language, sizeof(output->language));
  output->fail_pending = 0;
  output->state = next;
}

static int valid_language(const char *s) {
  return s &&
      ((s[0] == 'e' && s[1] == 'n' && s[2] == '\0') ||
       (s[0] == 'j' && s[1] == 'a' && s[2] == '\0') ||
       (s[0] == 'z' && s[1] == 'h' && s[2] == '\0'));
}

static int valid_voice(const char *s) {
  size_t i;
  if (!s || !s[0]) return 0;
  for (i = 0u; i <= TURBO_AGENT_TTS_MAX_VOICE_BYTES; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0u) return i != 0u;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
      return 0; /* no path, URL, control byte or SSML directive */
  }
  return 0;
}

/* RFC3629 scalar UTF-8: unlike a NUL-terminated interface, length is exact.
 * No C0/C1/DEL, BOM, bidi overrides, directional isolates, unpaired Unicode
 * scalar, newlines or angle markup in the initial plain-text-only TTS API.
 * This prevents controls/tool traces from entering a voice-capable Provider.
 * It deliberately does not normalize/rewrite untrusted text. */
static int valid_plain_utf8(const char *input, size_t len) {
  size_t i = 0u;
  const unsigned char *p = (const unsigned char *)input;
  if (!input || len == 0u || len > TURBO_AGENT_TTS_MAX_TEXT_BYTES) return 0;
  while (i < len) {
    unsigned char a = p[i++];
    uint32_t cp;
    unsigned extra, n;
    if (a >= 0x20u && a < 0x7fu) {
      if (a == '<' || a == '>') return 0;
      continue;
    }
    if (a < 0xc2u || a > 0xf4u) return 0;
    if (a <= 0xdfu) {
      extra = 1u;
      cp = (uint32_t)(a & 0x1fu);
    } else if (a <= 0xefu) {
      extra = 2u;
      cp = (uint32_t)(a & 0x0fu);
    } else {
      extra = 3u;
      cp = (uint32_t)(a & 0x07u);
    }
    if (len - i < extra) return 0;
    for (n = 0u; n < extra; n++) {
      unsigned char b = p[i++];
      if ((b & 0xc0u) != 0x80u) return 0;
      cp = (cp << 6u) | (uint32_t)(b & 0x3fu);
    }
    if ((extra == 1u && cp < 0x80u) ||
        (extra == 2u && cp < 0x800u) ||
        (extra == 3u && cp < 0x10000u) ||
        cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu) ||
        cp == 0x7fu || (cp >= 0x80u && cp <= 0x9fu) ||
        cp == 0x061cu || cp == 0xfeffu ||
        (cp >= 0x200eu && cp <= 0x200fu) ||
        (cp >= 0x2028u && cp <= 0x202eu) ||
        (cp >= 0x2066u && cp <= 0x2069u))
      return 0;
  }
  return 1;
}

void turbo_agent_tts_config_init(turbo_agent_tts_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_AGENT_SPEECH_OUTPUT_ABI_VERSION;
  config->output.sample_rate_hz = TURBO_AGENT_TTS_RATE_HZ;
  config->output.channels = TURBO_AGENT_TTS_CHANNELS;
  config->output.bits_per_sample = TURBO_AGENT_TTS_BITS_PER_SAMPLE;
  config->output.frame_ms = TURBO_AGENT_TTS_FRAME_MS;
  config->max_text_bytes = TURBO_AGENT_TTS_MAX_TEXT_BYTES;
  config->max_audio_ms = TURBO_AGENT_TTS_MAX_AUDIO_MS;
}

void turbo_agent_tts_provider_v1_init(turbo_agent_tts_provider_v1_t *provider) {
  if (!provider) return;
  memset(provider, 0, sizeof(*provider));
  provider->struct_size = sizeof(*provider);
  provider->abi_version = TURBO_AGENT_SPEECH_OUTPUT_ABI_VERSION;
}

int turbo_agent_speech_output_create(
    const turbo_agent_tts_config_t *config,
    const turbo_agent_tts_provider_v1_t *provider,
    turbo_agent_speech_output_t **out_output) {
  turbo_agent_speech_output_t *output;
  if (!out_output) return TURBO_TTS_EINVAL;
  *out_output = NULL;
  if (!config || config->struct_size != sizeof(*config) ||
      config->abi_version != TURBO_AGENT_SPEECH_OUTPUT_ABI_VERSION ||
      config->output.sample_rate_hz != TURBO_AGENT_TTS_RATE_HZ ||
      config->output.channels != TURBO_AGENT_TTS_CHANNELS ||
      config->output.bits_per_sample != TURBO_AGENT_TTS_BITS_PER_SAMPLE ||
      config->output.frame_ms != TURBO_AGENT_TTS_FRAME_MS ||
      config->max_text_bytes < 1u ||
      config->max_text_bytes > TURBO_AGENT_TTS_MAX_TEXT_BYTES ||
      config->max_audio_ms < TURBO_AGENT_TTS_FRAME_MS ||
      config->max_audio_ms > TURBO_AGENT_TTS_MAX_AUDIO_MS ||
      config->max_audio_ms % TURBO_AGENT_TTS_FRAME_MS != 0u)
    return TURBO_TTS_EINVAL;
  if (!provider || provider->struct_size != sizeof(*provider) ||
      provider->abi_version != TURBO_AGENT_SPEECH_OUTPUT_ABI_VERSION ||
      !provider->begin || !provider->poll_pcm || !provider->request_cancel)
    return TURBO_TTS_EINVAL;
  output = (turbo_agent_speech_output_t *)calloc(1u, sizeof(*output));
  if (!output) return TURBO_TTS_ENOMEM;
  output->config = *config;
  output->provider = *provider;
  output->max_pcm_samples =
      ((size_t)config->max_audio_ms * TURBO_AGENT_TTS_RATE_HZ) / 1000u;
  output->state = TURBO_TTS_IDLE;
  *out_output = output;
  return TURBO_TTS_OK;
}

static int check_generation(const turbo_agent_speech_output_t *output,
                            uint64_t generation_id) {
  if (!output || generation_id == 0u) return TURBO_TTS_EINVAL;
  return generation_id == output->generation_id ?
      TURBO_TTS_OK : TURBO_TTS_ESTALE;
}

int turbo_agent_speech_output_begin(turbo_agent_speech_output_t *output,
                                    uint64_t generation_id,
                                    const char *approved_utf8,
                                    size_t text_bytes,
                                    const char *voice_id,
                                    const char *language) {
  int rc;
  if (!output || !generation_id || !approved_utf8 ||
      !valid_voice(voice_id) || !valid_language(language))
    return TURBO_TTS_EINVAL;
  if (output->state == TURBO_TTS_SYNTHESIZING ||
      output->state == TURBO_TTS_STAGING_DRAINING ||
      output->state == TURBO_TTS_CANCEL_PENDING)
    return TURBO_TTS_EBUSY;
  if (generation_id <= output->generation_id) return TURBO_TTS_ESTALE;
  if (text_bytes == 0u || text_bytes > output->config.max_text_bytes)
    return TURBO_TTS_ECAPACITY;
  if (!valid_plain_utf8(approved_utf8, text_bytes))
    return TURBO_TTS_ETEXT;

  clear_pending(output);
  wipe(output->text, sizeof(output->text));
  wipe(output->voice, sizeof(output->voice));
  wipe(output->language, sizeof(output->language));
  memcpy(output->text, approved_utf8, text_bytes);
  memcpy(output->voice, voice_id, strlen(voice_id));
  memcpy(output->language, language, 2u);
  output->generation_id = generation_id;
  output->total_pcm_samples = 0u;
  output->fail_pending = 0;

  rc = output->provider.begin(output->provider.context, generation_id,
                              output->text, text_bytes, output->voice,
                              output->language, &output->config.output);
  if (rc != 0) {
    /* ABI contract: failed begin guarantees no asynchronous work started. */
    finish_generation(output, TURBO_TTS_FAILED);
    return TURBO_TTS_EPROVIDER;
  }
  output->state = TURBO_TTS_SYNTHESIZING;
  return TURBO_TTS_OK;
}

static int cancel_failed(turbo_agent_speech_output_t *output, int status) {
  clear_pending(output);
  if (output->state != TURBO_TTS_CANCEL_PENDING) {
    output->state = TURBO_TTS_CANCEL_PENDING;
    output->provider.request_cancel(output->provider.context,
                                    output->generation_id);
  }
  output->fail_pending = 1;
  return status;
}

int turbo_agent_speech_output_pump(turbo_agent_speech_output_t *output,
                                   uint64_t generation_id) {
  size_t samples = 0u;
  turbo_agent_tts_poll_kind_t kind = TURBO_TTS_WOULD_BLOCK;
  int canceling, rc;
  rc = check_generation(output, generation_id);
  if (rc) return rc;
  if (output->state != TURBO_TTS_SYNTHESIZING &&
      output->state != TURBO_TTS_CANCEL_PENDING)
    return output->state == TURBO_TTS_STAGING_DRAINING ?
        TURBO_TTS_EBUSY : TURBO_TTS_ESTATE;
  canceling = output->state == TURBO_TTS_CANCEL_PENDING;
  if (output->pending_samples != 0u) return TURBO_TTS_EBUSY;
  rc = output->provider.poll_pcm(
      output->provider.context, generation_id, output->frame,
      TURBO_AGENT_TTS_SAMPLES_PER_FRAME, &samples, &kind);
  if (rc != 0) return cancel_failed(output, TURBO_TTS_EPROVIDER);
  if (samples > TURBO_AGENT_TTS_SAMPLES_PER_FRAME)
    return cancel_failed(output, TURBO_TTS_EPROVIDER);
  if (kind == TURBO_TTS_WOULD_BLOCK && samples == 0u) {
    clear_pending(output);
    return TURBO_TTS_EAGAIN;
  }
  if (canceling) {
    /* The vendor might produce late frames while cancellation settles.
     * Discard them and never expose them to the previous/new generation. */
    if (kind == TURBO_TTS_PCM || kind == TURBO_TTS_PCM_FINAL) {
      clear_pending(output);
      return TURBO_TTS_EAGAIN;
    }
    if (samples == 0u &&
        (kind == TURBO_TTS_PROVIDER_CANCELED ||
         kind == TURBO_TTS_SOURCE_DONE ||
         kind == TURBO_TTS_PROVIDER_FAILED)) {
      finish_generation(output, output->fail_pending ||
          kind == TURBO_TTS_PROVIDER_FAILED ? TURBO_TTS_FAILED :
          TURBO_TTS_CANCELED);
      return TURBO_TTS_OK;
    }
    return cancel_failed(output, TURBO_TTS_EPROVIDER);
  }
  if (kind == TURBO_TTS_PCM || kind == TURBO_TTS_PCM_FINAL) {
    if ((kind == TURBO_TTS_PCM &&
         samples != TURBO_AGENT_TTS_SAMPLES_PER_FRAME) ||
        (kind == TURBO_TTS_PCM_FINAL && samples == 0u))
      return cancel_failed(output, TURBO_TTS_EPROVIDER);
    if (samples > output->max_pcm_samples - output->total_pcm_samples)
      return cancel_failed(output, TURBO_TTS_ECAPACITY);
    output->total_pcm_samples += samples;
    output->pending_samples = samples;
    output->pending_offset = 0u;
    if (kind == TURBO_TTS_PCM_FINAL)
      output->state = TURBO_TTS_STAGING_DRAINING;
    return TURBO_TTS_OK;
  }
  if (samples != 0u)
    return cancel_failed(output, TURBO_TTS_EPROVIDER);
  if (kind == TURBO_TTS_SOURCE_DONE) {
    finish_generation(output, TURBO_TTS_SETTLED);
    return TURBO_TTS_OK;
  }
  if (kind == TURBO_TTS_PROVIDER_FAILED ||
      kind == TURBO_TTS_PROVIDER_CANCELED) {
    finish_generation(output, TURBO_TTS_FAILED);
    return TURBO_TTS_EPROVIDER;
  }
  return cancel_failed(output, TURBO_TTS_EPROVIDER);
}

int turbo_agent_speech_output_peek_pcm(
    turbo_agent_speech_output_t *output, uint64_t generation_id,
    const int16_t **out_samples, size_t *out_count) {
  int rc;
  if (!out_samples || !out_count) return TURBO_TTS_EINVAL;
  *out_samples = NULL;
  *out_count = 0u;
  rc = check_generation(output, generation_id);
  if (rc) return rc;
  if (output->state != TURBO_TTS_SYNTHESIZING &&
      output->state != TURBO_TTS_STAGING_DRAINING)
    return TURBO_TTS_ESTATE;
  if (!output->pending_samples) return TURBO_TTS_EAGAIN;
  *out_samples = output->frame + output->pending_offset;
  *out_count = output->pending_samples - output->pending_offset;
  return TURBO_TTS_OK;
}

int turbo_agent_speech_output_consume_pcm(
    turbo_agent_speech_output_t *output, uint64_t generation_id,
    size_t accepted_samples) {
  int rc = check_generation(output, generation_id);
  if (rc) return rc;
  if (output->state != TURBO_TTS_SYNTHESIZING &&
      output->state != TURBO_TTS_STAGING_DRAINING)
    return TURBO_TTS_ESTATE;
  if (output->pending_samples == 0u) return TURBO_TTS_EAGAIN;
  if (accepted_samples == 0u ||
      accepted_samples > output->pending_samples - output->pending_offset)
    return TURBO_TTS_EBUFFER;
  output->pending_offset += accepted_samples;
  if (output->pending_offset == output->pending_samples) {
    int last = output->state == TURBO_TTS_STAGING_DRAINING;
    clear_pending(output);
    if (last) {
      /* Source PCM is accepted, not necessarily yet drained by a device. */
      finish_generation(output, TURBO_TTS_SETTLED);
    }
  }
  return TURBO_TTS_OK;
}

int turbo_agent_speech_output_cancel(turbo_agent_speech_output_t *output,
                                     uint64_t generation_id) {
  int rc = check_generation(output, generation_id);
  if (rc) return rc;
  if (output->state == TURBO_TTS_CANCEL_PENDING ||
      output->state == TURBO_TTS_CANCELED) return TURBO_TTS_OK;
  if (output->state != TURBO_TTS_SYNTHESIZING &&
      output->state != TURBO_TTS_STAGING_DRAINING)
    return TURBO_TTS_ESTATE;
  clear_pending(output);
  output->state = TURBO_TTS_CANCEL_PENDING;
  output->provider.request_cancel(output->provider.context, generation_id);
  return TURBO_TTS_OK;
}

turbo_agent_tts_state_t turbo_agent_speech_output_get_state(
    const turbo_agent_speech_output_t *output) {
  return output ? output->state : TURBO_TTS_FAILED;
}

int turbo_agent_speech_output_destroy(turbo_agent_speech_output_t **output) {
  if (!output || !*output) return TURBO_TTS_EINVAL;
  if ((*output)->state == TURBO_TTS_SYNTHESIZING ||
      (*output)->state == TURBO_TTS_STAGING_DRAINING ||
      (*output)->state == TURBO_TTS_CANCEL_PENDING)
    return TURBO_TTS_EBUSY;
  wipe(*output, sizeof(**output));
  free(*output);
  *output = NULL;
  return TURBO_TTS_OK;
}
