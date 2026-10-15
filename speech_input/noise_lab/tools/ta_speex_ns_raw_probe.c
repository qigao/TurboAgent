#define _POSIX_C_SOURCE 200809L
#include "ta_speex_ns_candidate.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Linux-only diagnostic. Input and output must be absolute private local
 * RAW 16kHz mono S16LE paths. Exclusive output mode 0600; no stdout PCM. */
static int read_exact(int fd, void *out, size_t count) {
  uint8_t *p = (uint8_t *)out;
  while (count != 0u) {
    const ssize_t n = read(fd, p, count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += (size_t)n;
    count -= (size_t)n;
  }
  return 0;
}
static int write_all(int fd, const void *data, size_t count) {
  const uint8_t *p = (const uint8_t *)data;
  while (count != 0u) {
    const ssize_t n = write(fd, p, count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += (size_t)n;
    count -= (size_t)n;
  }
  return 0;
}
int main(int argc, char **argv) {
  const uint16_t endian_probe = 1u;
  const int16_t *probe = NULL;
  ta_speex_ns_owner_t *owner = NULL;
  struct stat st;
  int src = -1, dst = -1, rc = 2, db, align = 0;
  size_t frames, i, j;
  int16_t input[TA_SPEEX_NS_FRAME_SAMPLES];
  int16_t output[TA_SPEEX_NS_FRAME_SAMPLES];
  uint64_t energy_before = 0u, energy_after = 0u;
  if ((argc != 4 && argc != 5) || argv[1][0] != '/' ||
      argv[2][0] != '/' || strcmp(argv[1], argv[2]) == 0) goto done;
  if (argc == 5) {
    if (strcmp(argv[4], "--align-one-frame") != 0) goto done;
    align = 1;
  }
  if (*(const uint8_t *)&endian_probe != 1u) goto done;
  if (strcmp(argv[3], "-12") == 0) db = -12;
  else if (strcmp(argv[3], "-18") == 0) db = -18;
  else goto done;
  src = open(argv[1], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (src < 0 || fstat(src, &st) != 0 || !S_ISREG(st.st_mode))
    goto done;
  if (st.st_size < 640 || st.st_size > 1920000 ||
      st.st_size % 640 != 0) goto done;
  frames = (size_t)(st.st_size / 640);
  if (ta_speex_ns_create(db, &owner) != TA_SPEEX_NS_OK) goto done;
  dst = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
             0600);
  if (dst < 0) goto done;
  for (i = 0u; i < frames; ++i) {
    if (read_exact(src, input, sizeof(input)) != 0) goto done;
    if (ta_speex_ns_process(owner, input, 320u, output) !=
        TA_SPEEX_NS_OK) goto done;
    for (j = 0u; j < TA_SPEEX_NS_FRAME_SAMPLES; ++j) {
      const int64_t a = input[j], b = output[j];
      energy_before += (uint64_t)(a*a);
      energy_after += (uint64_t)(b*b);
    }
    if ((!align || i != 0u) &&
        write_all(dst, output, sizeof(output)) != 0) goto done;
  }
  if (ta_speex_ns_processed_frames(owner) != frames) goto done;
  if (align) {
    if (ta_speex_ns_finish(owner, output, 320u) != TA_SPEEX_NS_OK) goto done;
    if (write_all(dst, output, sizeof(output)) != 0) goto done;
  }
  {
    uint8_t extra;
    const ssize_t n = read(src, &extra, 1u);
    if (n != 0) goto done;
  }
  if (fsync(dst) != 0) goto done;
  rc = 0;
  printf("{\"library\":\"SpeexDSP-1.2.1\","
         "\"frame_samples\":320,\"rate\":16000,"
         "\"frames\":%zu,\"noise_suppress_db\":%d,"
         "\"input_power\":%" PRIu64 ",\"output_power\":%" PRIu64 ","
         "\"vad_enabled\":false,\"agc_enabled\":false,"
         "\"voice_quality_certified\":false,"
         "\"no_model_download\":true}\n",
         frames, db, energy_before, energy_after);
done:
  (void)probe;
  if (dst >= 0) (void)close(dst);
  if (src >= 0) (void)close(src);
  if (owner) (void)ta_speex_ns_destroy(&owner);
  if (rc != 0 && argc >= 3 && argv[2][0] == '/' && dst >= 0)
    (void)unlink(argv[2]);
  if (rc != 0) fputs("SpeexDSP private raw probe refused\n", stderr);
  return rc;
}
