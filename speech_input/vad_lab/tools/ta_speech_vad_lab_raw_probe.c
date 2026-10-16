#define _POSIX_C_SOURCE 200809L
#include "ta_speech_vad_lab.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Linux-only private RAW PCM probe, NOT a runtime VAD/admission endpoint.
 * Caller must supply an absolute, nonsymlink, mode-0600 16k mono S16LE
 * source in a private 0700 workspace. No audio bytes/path are logged or
 * written, no model invocation or additional DSP provider. */
static int read_complete(int fd, int16_t *out, size_t bytes) {
  uint8_t *p = (uint8_t *)(void *)out;
  while (bytes != 0u) {
    const ssize_t n = read(fd, p, bytes);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += (size_t)n;
    bytes -= (size_t)n;
  }
  return 0;
}

int main(int argc, char **argv) {
  const uint16_t endian_check = 1u;
  ta_vad_lab_owner_t *owner = NULL;
  ta_vad_lab_result_t result;
  struct stat info;
  size_t i, input_frames;
  int fd = -1, rc = 2;
  int16_t pcm[TA_VAD_LAB_FRAME_SAMPLES];

  if (argc != 2 || !argv[1] || argv[1][0] != '/' ||
      *(const uint8_t *)(const void *)&endian_check != 1u)
    goto end;
  fd = open(argv[1], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 || fstat(fd, &info) != 0 ||
      !S_ISREG(info.st_mode) || (info.st_mode & 0077) != 0 ||
      info.st_size < (off_t)sizeof(pcm) ||
      info.st_size > (off_t)(TA_VAD_LAB_MAX_FRAMES * sizeof(pcm)) ||
      info.st_size % (off_t)sizeof(pcm) != 0)
    goto end;
  input_frames = (size_t)(info.st_size / (off_t)sizeof(pcm));
  if (ta_vad_lab_create(&owner) != TA_VAD_LAB_OK)
    goto end;
  for (i = 0u; i < input_frames; ++i) {
    if (read_complete(fd, pcm, sizeof(pcm)) != 0 ||
        ta_vad_lab_accept(owner, pcm, TA_VAD_LAB_FRAME_SAMPLES) !=
            TA_VAD_LAB_OK)
      goto end;
    memset(pcm, 0, sizeof(pcm));
  }
  {
    uint8_t extra = 0u;
    if (read(fd, &extra, 1u) != 0)
      goto end; /* Input changed during bounded snapshot. */
  }
  if (ta_vad_lab_settle(owner, &result) != TA_VAD_LAB_OK)
    goto end;
  if (result.input_frames != input_frames)
    goto end;

  /* Only aggregate metrics, never PCM, raw paths or speaker metadata.
   * high confidence on digital ZERO is a lab-only classification.
   * Do not forward "non speech" result into production ASR admission. */
  printf("{\"schema\":1,\"lab\":\"strict_C11_tri_state_no_weights\","
         "\"sample_rate\":16000,\"samples_per_frame\":320,"
         "\"frames\":%" PRIu32 ","
         "\"nonzero_frames\":%" PRIu32 ","
         "\"above_floor_frames\":%" PRIu32 ","
         "\"voicing_cue_frames\":%" PRIu32 ","
         "\"tonal_cue_frames\":%" PRIu32 ","
         "\"power_modulation_events\":%" PRIu32 ","
         "\"maximum_frame_power\":%" PRIu64 ","
         "\"minimum_nonzero_frame_power\":%" PRIu64 ","
         "\"mean_spectral_concentration\":%.6f,"
         "\"mean_peak_autocorrelation\":%.6f,"
         "\"mean_sparse_band_share\":[%.6f,%.6f,%.6f,%.6f],"
         "\"mean_sparse_band_entropy\":%.6f,"
         "\"mean_sparse_band_flux\":%.6f,"
         "\"sparse_band_observed_frames\":%" PRIu32 ","
         "\"sparse_band_flux_transitions\":%" PRIu32 ","
         "\"sparse_band_is_calibrated_classifier\":false,"
         "\"decision\":\"%s\","
         "\"non_speech_only_when_all_digital_zero\":true,"
         "\"diagnostic_only\":true,"
         "\"real_model_or_language_certified\":false,"
         "\"audio_or_transcript_persisted\":false}\n",
         result.input_frames,
         result.nonzero_frames,
         result.above_floor_frames,
         result.voicing_cue_frames,
         result.tonal_cue_frames,
         result.power_modulation_events,
         result.maximum_frame_power,
         result.minimum_nonzero_frame_power,
         result.mean_spectral_concentration,
         result.mean_peak_autocorrelation,
         result.mean_sparse_band_share[0],
         result.mean_sparse_band_share[1],
         result.mean_sparse_band_share[2],
         result.mean_sparse_band_share[3],
         result.mean_sparse_band_entropy,
         result.mean_sparse_band_flux,
         result.sparse_band_observed_frames,
         result.sparse_band_flux_transitions,
         result.decision == TA_VAD_LAB_NON_SPEECH_CONFIDENT ?
            "NON_SPEECH_CONFIDENT" :
            result.decision == TA_VAD_LAB_SPEECH_PRESENT ?
            "SPEECH_PRESENT" : "UNCERTAIN");
  rc = 0;

end:
  memset(pcm, 0, sizeof(pcm));
  if (fd >= 0) (void)close(fd);
  if (owner) (void)ta_vad_lab_destroy(&owner);
  if (rc != 0)
    fputs("private bounded C11 VAD feature probe refused\n", stderr);
  return rc;
}
