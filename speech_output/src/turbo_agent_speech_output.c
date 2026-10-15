#include "turbo_agent_speech_output.h"

#include <stdlib.h>
#include <string.h>

struct turbo_tts_output {
  turbo_tts_provider_t provider;
  turbo_tts_format_t format;
  char *text;
  char voice[TURBO_TTS_MAX_VOICE_BYTES + 1u];
  char language[TURBO_TTS_MAX_LANGUAGE_BYTES + 1u];
  uint32_t max_text_bytes;
  uint32_t max_frames;
  uint32_t emitted_frames;
  uint64_t generation;
  turbo_tts_state_t state;
  int in_callback;
  int cancel_requested;
  int error_pending;
};

static void wipe_memory(void *ptr, size_t bytes) {
  volatile unsigned char *cursor = (volatile unsigned char *)ptr;
  while (bytes-- != 0u) *cursor++ = 0u;
}

static void clear_text(turbo_tts_output_t *o) {
  wipe_memory(o->text, (size_t)o->max_text_bytes + 1u);
  wipe_memory(o->voice, sizeof(o->voice));
  wipe_memory(o->language, sizeof(o->language));
}

/* RFC 3629 scalar UTF-8; refuse control/bidi/markup in the plain-text MVP. */
static int valid_text(const char *s, size_t length) {
  size_t i = 0u;
  if (!s || !length) return 0;
  while (i < length) {
    unsigned char first = (unsigned char)s[i++];
    unsigned extra = 0u;
    uint32_t cp = 0u;
    if (first < 0x20u || first == 0x7fu || first == '<' || first == '>')
      return 0;
    if (first < 0x80u) continue;
    if (first >= 0xc2u && first <= 0xdfu) {
      extra = 1u; cp = first & 0x1fu;
    } else if (first >= 0xe0u && first <= 0xefu) {
      extra = 2u; cp = first & 0x0fu;
    } else if (first >= 0xf0u && first <= 0xf4u) {
      extra = 3u; cp = first & 0x07u;
    } else {
      return 0;
    }
    if (length - i < extra) return 0;
    for (unsigned j = 0u; j < extra; j++) {
      unsigned char next = (unsigned char)s[i++];
      if ((next & 0xc0u) != 0x80u) return 0;
      cp = (cp << 6u) | (next & 0x3fu);
    }
    if ((extra == 1u && cp < 0x80u) ||
        (extra == 2u && cp < 0x800u) ||
        (extra == 3u && cp < 0x10000u) ||
        (cp >= 0xd800u && cp <= 0xdfffu) || cp > 0x10ffffu ||
        (cp >= 0x80u && cp <= 0x9fu) ||
        cp == 0x061cu || cp == 0x200eu || cp == 0x200fu ||
        cp == 0x2028u || cp == 0x2029u ||
        (cp >= 0x202au && cp <= 0x202eu) ||
        (cp >= 0x2066u && cp <= 0x2069u) ||
        cp == 0xfeffu) return 0;
  }
  return 1;
}

static int copy_ascii_label(char *dst, size_t max_len,
                            const char *src, int language) {
  size_t i = 0u;
  if (!src) return 0;
  for (; i <= max_len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == 0u) {
      if (!i) return 0;
      dst[i] = '\0';
      return 1;
    }
    if (i == max_len) return 0;
    if (!((c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == '-' || (!language && (c == '_' || c == '.'))))
      return 0;
    dst[i] = (char)c;
  }
  return 0;
}

void turbo_tts_config_init(turbo_tts_config_t *c) {
  if (!c) return;
  memset(c, 0, sizeof(*c));
  c->struct_size = sizeof(*c);
  c->abi_version = TURBO_TTS_ABI_VERSION;
  c->format.sample_rate_hz = TURBO_TTS_SAMPLE_RATE_HZ;
  c->format.channels = 1u;
  c->format.bits_per_sample = 16u;
  c->format.frame_samples = TURBO_TTS_SAMPLES_PER_FRAME;
  c->max_text_bytes = 1024u;
  c->max_frames = TURBO_TTS_MAX_FRAMES;
}

void turbo_tts_provider_init(turbo_tts_provider_t *p) {
  if (!p) return;
  memset(p, 0, sizeof(*p));
  p->struct_size = sizeof(*p);
  p->abi_version = TURBO_TTS_ABI_VERSION;
}

int turbo_tts_create(const turbo_tts_config_t *c,
                     const turbo_tts_provider_t *p,
                     turbo_tts_output_t **out) {
  if (!out) return TURBO_TTS_EINVAL;
  *out = NULL;
  if (!c || c->struct_size != sizeof(*c) ||
      c->abi_version != TURBO_TTS_ABI_VERSION ||
      c->format.sample_rate_hz != TURBO_TTS_SAMPLE_RATE_HZ ||
      c->format.channels != 1u || c->format.bits_per_sample != 16u ||
      c->format.frame_samples != TURBO_TTS_SAMPLES_PER_FRAME ||
      c->max_text_bytes == 0u ||
      c->max_text_bytes > TURBO_TTS_MAX_TEXT_BYTES ||
      c->max_frames == 0u || c->max_frames > TURBO_TTS_MAX_FRAMES ||
      !p || p->struct_size != sizeof(*p) ||
      p->abi_version != TURBO_TTS_ABI_VERSION ||
      !p->begin || !p->poll_pcm || !p->request_cancel)
    return TURBO_TTS_EINVAL;
  turbo_tts_output_t *o = (turbo_tts_output_t *)calloc(1u, sizeof(*o));
  if (!o) return TURBO_TTS_ENOMEM;
  o->text = (char *)calloc((size_t)c->max_text_bytes + 1u, 1u);
  if (!o->text) {
    free(o);
    return TURBO_TTS_ENOMEM;
  }
  o->provider = *p;
  o->format = c->format;
  o->max_text_bytes = c->max_text_bytes;
  o->max_frames = c->max_frames;
  o->state = TURBO_TTS_IDLE;
  *out = o;
  return TURBO_TTS_OK;
}

int turbo_tts_begin(turbo_tts_output_t *o, uint64_t generation,
                    const char *text, size_t length,
                    const char *voice_id, const char *language) {
  char voice[TURBO_TTS_MAX_VOICE_BYTES + 1u] = {0};
  char lang[TURBO_TTS_MAX_LANGUAGE_BYTES + 1u] = {0};
  if (!o) return TURBO_TTS_EINVAL;
  if (o->in_callback) return TURBO_TTS_EBUSY;
  if (!generation || generation <= o->generation) return TURBO_TTS_ESTALE;
  if (o->state == TURBO_TTS_ACTIVE ||
      o->state == TURBO_TTS_CANCEL_PENDING) return TURBO_TTS_EBUSY;
  if (!length || length > o->max_text_bytes || !valid_text(text, length) ||
      !copy_ascii_label(voice, TURBO_TTS_MAX_VOICE_BYTES, voice_id, 0) ||
      !copy_ascii_label(lang, TURBO_TTS_MAX_LANGUAGE_BYTES, language, 1))
    return TURBO_TTS_EINVAL;
  clear_text(o);
  memcpy(o->text, text, length);
  memcpy(o->voice, voice, sizeof(o->voice));
  memcpy(o->language, lang, sizeof(o->language));
  o->generation = generation;
  o->emitted_frames = 0u;
  o->cancel_requested = 0;
  o->error_pending = 0;
  /* Host text and strings are now owned until provider settles. Reentrancy
   * must be refused to keep one caller/one outstanding Provider callback. */
  o->in_callback = 1;
  int rc = o->provider.begin(o->provider.context, generation,
                             o->text, length, o->voice, o->language,
                             &o->format);
  o->in_callback = 0;
  if (rc != 0) {
    o->state = TURBO_TTS_CANCEL_PENDING;
    o->error_pending = 1;
    o->cancel_requested = 1;
    o->in_callback = 1;
    o->provider.request_cancel(o->provider.context, generation);
    o->in_callback = 0;
    return TURBO_TTS_EPROVIDER;
  }
  o->state = TURBO_TTS_ACTIVE;
  return TURBO_TTS_OK;
}

static void request_cancel_once(turbo_tts_output_t *o, int is_error) {
  o->state = TURBO_TTS_CANCEL_PENDING;
  if (is_error) o->error_pending = 1;
  if (!o->cancel_requested) {
    o->cancel_requested = 1;
    o->in_callback = 1;
    o->provider.request_cancel(o->provider.context, o->generation);
    o->in_callback = 0;
  }
}

int turbo_tts_poll(turbo_tts_output_t *o, int16_t *dst,
                   size_t capacity, size_t *out_samples,
                   turbo_tts_poll_t *out_kind) {
  if (out_samples) *out_samples = 0u;
  if (out_kind) *out_kind = TURBO_TTS_WOULD_BLOCK;
  if (!o) return TURBO_TTS_EINVAL;
  if (o->in_callback) return TURBO_TTS_EBUSY;
  if (!dst || !out_samples || !out_kind ||
      capacity != TURBO_TTS_SAMPLES_PER_FRAME) return TURBO_TTS_EINVAL;
  if (o->state != TURBO_TTS_ACTIVE &&
      o->state != TURBO_TTS_CANCEL_PENDING) return TURBO_TTS_ESTATE;

  /* Destination is caller-owned; PCM from canceled/stale requests is never
   * published, and no vendor bytes leak on failure/empty polls. */
  memset(dst, 0, capacity * sizeof(*dst));
  size_t produced = 0u;
  turbo_tts_poll_t kind = TURBO_TTS_WOULD_BLOCK;
  o->in_callback = 1;
  int rc = o->provider.poll_pcm(o->provider.context, o->generation,
                                dst, capacity, &produced, &kind);
  o->in_callback = 0;
  if (rc != 0 || produced > capacity ||
      (kind == TURBO_TTS_PCM && produced == 0u) ||
      (kind != TURBO_TTS_PCM && produced != 0u) ||
      kind < TURBO_TTS_WOULD_BLOCK || kind > TURBO_TTS_POLL_CANCELED ||
      (o->state == TURBO_TTS_ACTIVE &&
       kind == TURBO_TTS_PCM && o->emitted_frames >= o->max_frames)) {
    wipe_memory(dst, capacity * sizeof(*dst));
    request_cancel_once(o, 1);
    return TURBO_TTS_EPROVIDER;
  }

  if (o->state == TURBO_TTS_CANCEL_PENDING) {
    wipe_memory(dst, capacity * sizeof(*dst));
    if (kind == TURBO_TTS_TERMINAL || kind == TURBO_TTS_POLL_CANCELED) {
      o->state = o->error_pending ? TURBO_TTS_FAILED : TURBO_TTS_CANCELED;
      clear_text(o);
      *out_kind = TURBO_TTS_POLL_CANCELED;
    }
    return TURBO_TTS_OK;
  }

  if (kind == TURBO_TTS_PCM) {
    o->emitted_frames++;
    *out_samples = produced;
  } else if (kind == TURBO_TTS_TERMINAL) {
    o->state = TURBO_TTS_SOURCE_DONE;
    clear_text(o);
    wipe_memory(dst, capacity * sizeof(*dst));
  } else if (kind == TURBO_TTS_POLL_CANCELED) {
    o->state = TURBO_TTS_FAILED;
    clear_text(o);
    wipe_memory(dst, capacity * sizeof(*dst));
    return TURBO_TTS_EPROVIDER; /* Unrequested vendor abort */
  } else {
    wipe_memory(dst, capacity * sizeof(*dst));
  }
  *out_kind = kind;
  return TURBO_TTS_OK;
}

int turbo_tts_cancel(turbo_tts_output_t *o) {
  if (!o) return TURBO_TTS_EINVAL;
  if (o->in_callback) return TURBO_TTS_EBUSY;
  if (o->state == TURBO_TTS_CANCEL_PENDING ||
      o->state == TURBO_TTS_CANCELED) return TURBO_TTS_OK;
  if (o->state != TURBO_TTS_ACTIVE) return TURBO_TTS_ESTATE;
  request_cancel_once(o, 0);
  return TURBO_TTS_OK;
}

turbo_tts_state_t turbo_tts_state(const turbo_tts_output_t *o) {
  return o ? o->state : TURBO_TTS_FAILED;
}

int turbo_tts_destroy(turbo_tts_output_t **handle) {
  if (!handle || !*handle) return TURBO_TTS_EINVAL;
  turbo_tts_output_t *o = *handle;
  if (o->in_callback || o->state == TURBO_TTS_ACTIVE ||
      o->state == TURBO_TTS_CANCEL_PENDING) return TURBO_TTS_EBUSY;
  clear_text(o);
  free(o->text);
  wipe_memory(o, sizeof(*o));
  free(o);
  *handle = NULL;
  return TURBO_TTS_OK;
}
