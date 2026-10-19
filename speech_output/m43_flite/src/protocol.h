#ifndef TURBO_M43_FLITE_PROTOCOL_H
#define TURBO_M43_FLITE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* One request per child; host-approved text via stdin, bounded PCM via
 * stdout, no arbitrary SSML, paths or commands in the wire protocol.
 * Fixed-size native-endian-independent headers (big endian u32) and
 * little-endian S16 samples. No file persistence or fallback channel. */
#define M43_REQUEST_MAGIC UINT32_C(0x46343351) /* F43Q */
#define M43_PCM_MAGIC UINT32_C(0x50434d30)     /* PCM0 */
#define M43_DONE_MAGIC UINT32_C(0x444f4e45)    /* DONE */
#define M43_ERROR_MAGIC UINT32_C(0x45525230)   /* ERR0 */
#define M43_WIRE_VERSION 1u
#define M43_REQUEST_HEADER 16u
#define M43_REPLY_HEADER 8u
#define M43_MAX_SAMPLES 480u
#define M43_MAX_FRAME_BYTES (M43_REPLY_HEADER + M43_MAX_SAMPLES * 2u)
#define M43_TOTAL_SAMPLE_LIMIT 720000u /* 30 s at 24 kHz */

static inline void m43_set_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24u);
  p[1] = (uint8_t)(v >> 16u);
  p[2] = (uint8_t)(v >> 8u);
  p[3] = (uint8_t)v;
}
static inline uint32_t m43_get_u32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
         ((uint32_t)p[2] << 8u) | (uint32_t)p[3];
}
static inline void m43_set_s16(uint8_t *p, int16_t x) {
  uint16_t v = (uint16_t)x;
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8u);
}
static inline int16_t m43_get_s16(const uint8_t *p) {
  uint16_t v = (uint16_t)((uint16_t)p[0] |
                          ((uint16_t)p[1] << 8u));
  return (int16_t)v;
}
#endif
