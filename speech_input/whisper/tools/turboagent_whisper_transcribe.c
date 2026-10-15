/* Explicit offline WAV -> M2 bounded PCM -> M1 -> Whisper Provider.
 * Preview-only: NEVER starts Harness/Session, executes tools, downloads
 * models, uses a microphone, uploads audio or persists raw samples. */
#include "turbo_agent_speech_capture.h"
#include "turbo_agent_speech_whisper.h"
#include "turbo_agent_speech_input.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CLI_MAX_WAVE_FILE (4u * 1024u * 1024u)
#define CLI_MAX_PCM_BYTES (TURBO_AGENT_SPEECH_MAX_UTTERANCE_FRAMES * 640u)

typedef struct cli_options_s {
  const char *model_path, *wav_path;
  uint8_t sha256[32];
  uint32_t threads, decode_ms, model_max_mib;
  turbo_agent_speech_whisper_language_t language;
} cli_options_t;

static void usage(void) {
  fputs("usage: turboagent-whisper-transcribe --model ABS_PATH "
        "--sha256 SHA256_64_HEX --wav ABS_PATH --language en|zh|ja|auto "
        "[--threads 1..16] [--timeout-ms 1..120000] "
        "[--model-max-mib 1..1024]\n"
        "Offline WAV only: mono 16kHz signed 16-bit little-endian PCM; "
        "exact 20ms (640-byte) frames, max 60 seconds. "
        "Transcript is preview-only, never a tool request.\n", stderr);
}

static int path_absolute(const char *s) {
  if (!s || !*s) return 0;
#if defined(_WIN32)
  return strlen(s) >= 4u &&
         ((s[0] >= 'A' && s[0] <= 'Z') ||
          (s[0] >= 'a' && s[0] <= 'z')) &&
         s[1] == ':' && (s[2] == '/' || s[2] == '\\');
#else
  return s[0] == '/';
#endif
}
static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static int decode_sha256(const char *hex, uint8_t out[32]) {
  uint8_t nonzero = 0;
  if (!hex || strlen(hex) != 64u) return -1;
  for (unsigned i=0; i<32; ++i) {
    const int a = hex_digit(hex[i*2u]);
    const int b = hex_digit(hex[i*2u+1u]);
    if (a < 0 || b < 0) return -1;
    out[i] = (uint8_t)((a << 4) | b);
    nonzero |= out[i];
  }
  return nonzero ? 0 : -1;
}
static int uint_option(const char *text, uint32_t min, uint32_t max,
                       uint32_t *out) {
  uint64_t value = 0;
  if (!text || !*text) return -1;
  for (const char *p=text; *p; p++) {
    if (*p < '0' || *p > '9') return -1;
    value = value*10u+(uint64_t)(*p-'0');
    if (value > max) return -1;
  }
  if (value < min) return -1;
  *out = (uint32_t)value;
  return 0;
}
static int parse_args(int argc, char **argv, cli_options_t *opts) {
  uint32_t seen=0;
  memset(opts, 0, sizeof(*opts));
  opts->threads=2u;
  opts->decode_ms=30000u;
  opts->model_max_mib=128u;
  if ((argc & 1)==0 || argc < 9 || argc > 15) return -1;
  for (int i=1; i<argc; i+=2) {
    const char *key=argv[i], *value=argv[i+1];
    unsigned bit=0;
    if (strcmp(key,"--model")==0) {
      bit=1u; opts->model_path=value;
    } else if (strcmp(key,"--sha256")==0) {
      bit=2u; if (decode_sha256(value, opts->sha256)!=0) return -1;
    } else if (strcmp(key,"--wav")==0) {
      bit=4u; opts->wav_path=value;
    } else if (strcmp(key,"--language")==0) {
      bit=8u;
      if (strcmp(value,"en")==0) opts->language=TURBO_AGENT_WHISPER_LANGUAGE_EN;
      else if (strcmp(value,"zh")==0) opts->language=TURBO_AGENT_WHISPER_LANGUAGE_ZH;
      else if (strcmp(value,"ja")==0) opts->language=TURBO_AGENT_WHISPER_LANGUAGE_JA;
      else if (strcmp(value,"auto")==0) opts->language=TURBO_AGENT_WHISPER_LANGUAGE_AUTO;
      else return -1;
    } else if (strcmp(key,"--threads")==0) {
      bit=16u; if (uint_option(value,1,16,&opts->threads)!=0) return -1;
    } else if (strcmp(key,"--timeout-ms")==0) {
      bit=32u; if (uint_option(value,1,120000,&opts->decode_ms)!=0) return -1;
    } else if (strcmp(key,"--model-max-mib")==0) {
      bit=64u; if (uint_option(value,1,1024,&opts->model_max_mib)!=0) return -1;
    } else return -1;
    if (seen & bit) return -1;
    seen |= bit;
  }
  return (seen & 15u)==15u && path_absolute(opts->model_path) &&
         path_absolute(opts->wav_path) ? 0 : -1;
}
/* A valid UTF-8 transcript is not necessarily safe to emit verbatim to an
 * operator's terminal. Reject control sequences and Unicode bidi overrides
 * rather than truncating, rewriting or displaying a misleading transcript.
 * M1 remains the sole transcript owner; the CLI adds display admission. */
static int safe_preview_text(const char *utf8, size_t length) {
  const unsigned char *p = (const unsigned char *)utf8;
  for (size_t i=0; i<length; ++i) {
    const unsigned char b = p[i];
    if ((b < 0x20u && b != '\n' && b != '\t') || b == 0x7fu)
      return 0; /* reject ANSI ESC, CR rewrites and other C0/C1 terminals */
    if (i+2u < length && b == 0xe2u) {
      if (p[i+1u] == 0x80u &&
          ((p[i+2u] >= 0xaau && p[i+2u] <= 0xaeu) ||
           p[i+2u] == 0x8eu || p[i+2u] == 0x8fu))
        return 0; /* U+202A..202E / U+200E,U+200F bidi controls */
      if (p[i+1u] == 0x81u &&
          p[i+2u] >= 0xa6u && p[i+2u] <= 0xa9u)
        return 0; /* U+2066..U+2069 isolate overrides */
    }
  }
  return 1;
}

static uint16_t u16le(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8u));
}
static uint32_t u32le(const uint8_t *p) {
  return ((uint32_t)p[0]) | ((uint32_t)p[1]<<8u) |
         ((uint32_t)p[2]<<16u) | ((uint32_t)p[3]<<24u);
}
typedef struct audio_span_s {
  long start;
  uint32_t bytes;
} audio_span_t;

/* Deliberately accepts ONLY complete lossless PCM samples and declared
 * exact metadata; never resamples or silently drops tail frames. */
static int verify_wav(FILE *f, audio_span_t *audio) {
  uint8_t riff[12], header[8], fmt[16];
  long end, cursor=12;
  int found_fmt=0, found_data=0;
  if (fseek(f,0,SEEK_END)!=0) return -1;
  end=ftell(f);
  if (end < 44 || end > (long)CLI_MAX_WAVE_FILE ||
      fseek(f,0,SEEK_SET)!=0 || fread(riff,1,sizeof(riff),f)!=sizeof(riff))
    return -1;
  if (memcmp(riff,"RIFF",4)!=0 || memcmp(riff+8,"WAVE",4)!=0 ||
      (uint64_t)u32le(riff+4)+8u != (uint64_t)end) return -1;

  while (cursor < end) {
    uint32_t bytes, padding;
    if (end-cursor < 8 ||
        fread(header,1,sizeof(header),f)!=sizeof(header)) return -1;
    cursor+=8;
    bytes=u32le(header+4);
    padding=bytes & 1u;
    if ((uint64_t)bytes+(uint64_t)padding > (uint64_t)(end-cursor))
      return -1;
    if (memcmp(header,"fmt ",4)==0) {
      if (found_fmt || found_data || bytes < 16u || bytes > 256u ||
          fread(fmt,1,sizeof(fmt),f)!=sizeof(fmt)) return -1;
      if (u16le(fmt)!=1u || u16le(fmt+2)!=1u ||
          u32le(fmt+4)!=16000u || u32le(fmt+8)!=32000u ||
          u16le(fmt+12)!=2u || u16le(fmt+14)!=16u) return -1;
      found_fmt=1;
      if (fseek(f,(long)(bytes-16u+padding),SEEK_CUR)!=0) return -1;
    } else if (memcmp(header,"data",4)==0) {
      if (!found_fmt || found_data || bytes==0u ||
          bytes>CLI_MAX_PCM_BYTES ||
          bytes % TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES != 0u ||
          /* Strict M3a: data must be last (no trailing metadata). */
          (uint64_t)cursor+bytes+padding!=(uint64_t)end) return -1;
      audio->start=cursor;
      audio->bytes=bytes;
      found_data=1;
      break;
    } else if (fseek(f,(long)(bytes+padding),SEEK_CUR)!=0) return -1;
    cursor += (long)bytes + (long)padding;
  }
  return found_fmt && found_data ? 0 : -1;
}

static int transcribe(FILE *wav, const audio_span_t *audio,
                      const cli_options_t *options) {
  turbo_agent_speech_whisper_config_t whisper_config;
  turbo_agent_speech_whisper_t *owner=NULL;
  turbo_agent_speech_provider_v1_t provider;
  turbo_agent_speech_config_t input_config;
  turbo_agent_speech_input_t *input=NULL;
  turbo_agent_speech_capture_bridge_t *bridge=NULL;
  uint8_t raw[TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES];
  char output[TURBO_AGENT_SPEECH_MAX_TEXT_BYTES+1u];
  size_t pumped, length=0;
  turbo_agent_speech_transcript_kind_t kind;
  int rc=-1;

  turbo_agent_speech_whisper_config_init(&whisper_config);
  whisper_config.model_path=options->model_path;
  memcpy(whisper_config.expected_sha256,options->sha256,
         sizeof(whisper_config.expected_sha256));
  whisper_config.max_model_bytes =
      (uint64_t)options->model_max_mib*1024u*1024u;
  whisper_config.max_utterance_frames=
      audio->bytes/TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES;
  whisper_config.max_text_bytes=TURBO_AGENT_SPEECH_MAX_TEXT_BYTES;
  whisper_config.cpu_threads=options->threads;
  whisper_config.max_decode_ms=options->decode_ms;
  whisper_config.language=options->language;
  if (turbo_agent_speech_whisper_create(&whisper_config,&owner)!=0)
    goto cleanup;
  if (turbo_agent_speech_whisper_provider(owner,&provider)!=0)
    goto cleanup;
  turbo_agent_speech_config_init(&input_config);
  input_config.max_utterance_frames=whisper_config.max_utterance_frames;
  input_config.max_text_bytes=TURBO_AGENT_SPEECH_MAX_TEXT_BYTES;
  if (turbo_agent_speech_create(&input_config,&provider,&input)!=0)
    goto cleanup;
  if (turbo_agent_speech_capture_bridge_create(input,1u,&bridge)!=0 ||
      turbo_agent_speech_capture_bridge_begin(bridge)!=0 ||
      fseek(wav,audio->start,SEEK_SET)!=0) goto cleanup;

  for (uint32_t position=0; position<audio->bytes;
       position+=TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES) {
    const uint64_t timestamp =
        ((uint64_t)position/TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES+1u)*20000u;
    if (fread(raw,1,sizeof(raw),wav)!=sizeof(raw)) goto cleanup;
    if (turbo_agent_speech_capture_bridge_offer(
          bridge,raw,sizeof(raw),timestamp)!=0) goto cleanup;
    memset(raw,0,sizeof(raw));
    if (turbo_agent_speech_pump(input,1u,&pumped)!=0 || pumped!=1u)
      goto cleanup;
  }
  if (turbo_agent_speech_capture_bridge_stop_admission(bridge)!=0 ||
      turbo_agent_speech_capture_bridge_finish(bridge)!=0 ||
      turbo_agent_speech_pump(input,1u,&pumped)!=0) goto cleanup;
  if (turbo_agent_speech_take_final(input,output,sizeof(output),
                                    &length,&kind)!=0) goto cleanup;
  if (kind==TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH) {
    fputs("NO_SPEECH (no transcript emitted)\n",stderr);
    rc=0;
  } else if (kind==TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL &&
             length>0u && length<=TURBO_AGENT_SPEECH_MAX_TEXT_BYTES) {
    if (!safe_preview_text(output, length)) goto cleanup;
    if (fwrite(output,1,length,stdout)!=length ||
        fputc('\n',stdout)==EOF) goto cleanup;
    rc=0;
  }
cleanup:
  memset(raw,0,sizeof(raw));
  memset(output,0,sizeof(output));
  if (bridge && input) {
    const turbo_agent_speech_state_t state =
        turbo_agent_speech_get_state(input);
    if (state==TURBO_AGENT_SPEECH_ACTIVE ||
        state==TURBO_AGENT_SPEECH_DRAINING) {
      (void)turbo_agent_speech_capture_bridge_stop_admission(bridge);
      (void)turbo_agent_speech_capture_bridge_cancel(bridge);
      (void)turbo_agent_speech_cancel(input);
    }
    (void)turbo_agent_speech_capture_bridge_destroy(&bridge);
  }
  if (input) (void)turbo_agent_speech_destroy(&input);
  if (owner) (void)turbo_agent_speech_whisper_destroy(&owner);
  return rc;
}

int main(int argc, char **argv) {
  cli_options_t options;
  audio_span_t audio={0};
  FILE *file;
  int rc;
  if (parse_args(argc,argv,&options)!=0) {
    usage();
    return 2;
  }
  file=fopen(options.wav_path,"rb");
  if (!file || verify_wav(file,&audio)!=0) {
    fputs("invalid WAV: require complete 16k mono S16LE, 20ms frames, <=60s\n",
          stderr);
    if (file) fclose(file);
    return 3;
  }
  rc=transcribe(file,&audio,&options);
  if (fclose(file)!=0) rc=-1;
  if (rc!=0) {
    fputs("Whisper local model validation/decode failed; no transcript admitted\n",
          stderr);
    return 4;
  }
  return 0;
}
