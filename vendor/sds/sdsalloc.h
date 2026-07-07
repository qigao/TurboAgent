/* SDSLib 2.0 -- A C dynamic strings library
 *
 * Copyright (c) 2006-2015, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2015, Oran Agra
 * Copyright (c) 2015, Redis Labs, Inc
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* SDS allocator selection.
 *
 * This file is used in order to change the SDS allocator at compile time.
 * Just define the following defines to what you want to use. Also add
 * the include of your alternate allocator if needed (not needed in order
 * to use the default libc allocator). */

#include <stdlib.h>
#include <string.h>

#undef SDS_ALLOC_POOL
#define SDS_ALLOC_POOL 0

#ifndef SDS_POOL_MAX
#define SDS_POOL_MAX (64 * 1024)
#endif

#if defined(_MSC_VER)
  #define SDS_TLS __declspec(thread)
  #define SDS_TLS_AVAILABLE 1
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define SDS_TLS _Thread_local
  #define SDS_TLS_AVAILABLE 1
#elif defined(__GNUC__)
  #define SDS_TLS __thread
  #define SDS_TLS_AVAILABLE 1
#else
  #define SDS_TLS
  #define SDS_TLS_AVAILABLE 0
#endif

#if SDS_ALLOC_POOL && !SDS_TLS_AVAILABLE
  #undef SDS_ALLOC_POOL
  #define SDS_ALLOC_POOL 0
#endif

#if SDS_ALLOC_POOL

typedef struct sds_pool_hdr {
  size_t size;
  struct sds_pool_hdr *next;
} sds_pool_hdr;

enum {
  SDS_POOL_CLASS_COUNT = 13
};

static inline size_t sds_pool_class_max(int cls) {
  static const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
  if (cls >= 0 && cls < SDS_POOL_CLASS_COUNT) return sizes[cls];
  return 0;
}

static inline int sds_pool_class(size_t size) {
  if (size <= 16) return 0;
  if (size <= 32) return 1;
  if (size <= 64) return 2;
  if (size <= 128) return 3;
  if (size <= 256) return 4;
  if (size <= 512) return 5;
  if (size <= 1024) return 6;
  if (size <= 2048) return 7;
  if (size <= 4096) return 8;
  if (size <= 8192) return 9;
  if (size <= 16384) return 10;
  if (size <= 32768) return 11;
  if (size <= 65536) return 12;
  return -1;
}

static SDS_TLS sds_pool_hdr *sds_pool_bins[SDS_POOL_CLASS_COUNT];

static inline void *sds_pool_malloc(size_t size) {
  int cls = sds_pool_class(size);
  if (cls < 0 || size > SDS_POOL_MAX) {
    sds_pool_hdr *h = (sds_pool_hdr *)malloc(sizeof(*h) + size);
    if (!h) return NULL;
    h->size = size;
    return (void *)(h + 1);
  }
  sds_pool_hdr *h = sds_pool_bins[cls];
  if (h) {
    sds_pool_bins[cls] = h->next;
    h->next = NULL;
    return (void *)(h + 1);
  }
  size_t actual_size = sds_pool_class_max(cls);
  h = (sds_pool_hdr *)malloc(sizeof(*h) + actual_size);
  if (!h) return NULL;
  h->size = actual_size;
  return (void *)(h + 1);
}

static inline void *sds_pool_realloc(void *ptr, size_t size) {
  if (!ptr) return sds_pool_malloc(size);
  sds_pool_hdr *h = ((sds_pool_hdr *)ptr) - 1;
  size_t old_size = h->size;
  if (size <= old_size) {
    return ptr;
  }
  void *n = sds_pool_malloc(size);
  if (!n) return NULL;
  memcpy(n, ptr, old_size);
  /* free old */
  int cls = sds_pool_class(old_size);
  if (cls >= 0) {
    h->next = sds_pool_bins[cls];
    sds_pool_bins[cls] = h;
  } else {
    free(h);
  }
  return n;
}

static inline void sds_pool_free(void *ptr) {
  if (!ptr) return;
  sds_pool_hdr *h = ((sds_pool_hdr *)ptr) - 1;
  int cls = sds_pool_class(h->size);
  if (h->size > SDS_POOL_MAX) cls = -1;
  if (cls >= 0) {
    h->next = sds_pool_bins[cls];
    sds_pool_bins[cls] = h;
  } else {
    free(h);
  }
}

#define s_malloc sds_pool_malloc
#define s_realloc sds_pool_realloc
#define s_free sds_pool_free

#else

#define s_malloc malloc
#define s_realloc realloc
#define s_free free

#endif
