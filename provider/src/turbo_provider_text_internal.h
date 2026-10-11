#ifndef TURBO_PROVIDER_TEXT_INTERNAL_H
#define TURBO_PROVIDER_TEXT_INTERNAL_H

#include <tstr.h>
#include <string.h>

/* Keep the old tstr on allocation failure; callers own its final release. */
static inline int turbo_provider_text_append_bytes(char **buffer, size_t *length,
                                                    const char *data, size_t size) {
  tstr next;
  if (!buffer || (!data && size > 0)) return -1;
  next = *buffer ? tstr_cat_len(*buffer, data, size) : tstr_new_len(data, size);
  if (!next) return -1;
  *buffer = next;
  if (length) *length = tstr_len(next);
  return 0;
}

static inline int turbo_provider_text_append(char **buffer, size_t *length,
                                              const char *text) {
  return text ? turbo_provider_text_append_bytes(buffer, length, text, strlen(text)) : -1;
}

#endif
