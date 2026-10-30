#include "turbo_leann_internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* L0 format: little-endian with fixed 144-byte header.
 * 0..7 magic, 8 format, 12 header_size, 16 dimensions, 20 count,
 * 24 degree, 28 edge_count, 32 generation, 40 model SHA256 identity,
 * 72 security-scope identity, 104 total length, 112 IDs offset,
 * 120 CSR row-offsets position, 128 neighbors position, 136 CRC32,
 * 140 reserved. The vector section follows the neighbor section.
 * CRC is an accidental-corruption check, not authentication. The owner must
 * verify a cryptographic manifest digest in its pinned database transaction.
 */
#define TL_HEADER_SIZE 144u
#define TL_FORMAT_VERSION 1u
static const uint8_t tl_magic[8] = {'T','L','C','1','1','S','0','1'};

static void tl_put32(uint8_t *p, uint32_t v) {
  for (uint32_t i=0; i<4; ++i) p[i] = (uint8_t)(v >> (i*8));
}
static void tl_put64(uint8_t *p, uint64_t v) {
  for (uint32_t i=0; i<8; ++i) p[i] = (uint8_t)(v >> (i*8));
}
static uint32_t tl_get32(const uint8_t *p) {
  uint32_t v=0;
  for (uint32_t i=0; i<4; ++i) v |= (uint32_t)p[i] << (i*8);
  return v;
}
static uint64_t tl_get64(const uint8_t *p) {
  uint64_t v=0;
  for (uint32_t i=0; i<8; ++i) v |= (uint64_t)p[i] << (i*8);
  return v;
}

static uint32_t tl_crc32(const uint8_t *bytes, size_t len) {
  uint32_t crc = UINT32_MAX;
  for (size_t i=0; i<len; ++i) {
    uint8_t byte = (i >= 136u && i < 140u) ? 0 : bytes[i];
    crc ^= byte;
    for (uint32_t b=0; b<8; ++b)
      crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
  }
  return ~crc;
}

static int tl_layout(const turbo_leann_segment_t *s,
                     size_t *rows, size_t *neighbors, size_t *vectors,
                     size_t *total) {
  size_t ids_bytes, row_bytes, edge_bytes, vector_count, vector_bytes;
  if (!tl_checked_mul(s->count,32u,&ids_bytes) ||
      !tl_checked_mul((size_t)s->count+1u,4u,&row_bytes) ||
      !tl_checked_mul(s->edge_count,4u,&edge_bytes) ||
      !tl_checked_mul(s->count,s->dimensions,&vector_count) ||
      !tl_checked_mul(vector_count,4u,&vector_bytes) ||
      !tl_checked_add(TL_HEADER_SIZE,ids_bytes,rows) ||
      !tl_checked_add(*rows,row_bytes,neighbors) ||
      !tl_checked_add(*neighbors,edge_bytes,vectors) ||
      !tl_checked_add(*vectors,vector_bytes,total)) return 0;
  return 1;
}

turbo_leann_status_t turbo_leann_serialized_size(
    const turbo_leann_segment_t *segment, size_t *out_size) {
  if (!out_size) return TURBO_LEANN_INVALID;
  *out_size = 0;
  if (!segment) return TURBO_LEANN_INVALID;
  size_t a,b,c,size;
  if (!tl_layout(segment,&a,&b,&c,&size)) return TURBO_LEANN_LIMIT;
  *out_size = size;
  return TURBO_LEANN_OK;
}

turbo_leann_status_t turbo_leann_serialize(
    const turbo_leann_segment_t *segment, uint8_t *dst, size_t dst_capacity,
    size_t *out_size) {
  if (!out_size) return TURBO_LEANN_INVALID;
  *out_size = 0;
  if (!segment) return TURBO_LEANN_INVALID;
  size_t rows,edges,vectors,size;
  if (!tl_layout(segment,&rows,&edges,&vectors,&size)) return TURBO_LEANN_LIMIT;
  *out_size = size;
  if (!dst || dst_capacity < size) return TURBO_LEANN_LIMIT;
  memset(dst,0,TL_HEADER_SIZE);
  memcpy(dst,tl_magic,8);
  tl_put32(dst+8,TL_FORMAT_VERSION);
  tl_put32(dst+12,TL_HEADER_SIZE);
  tl_put32(dst+16,segment->dimensions);
  tl_put32(dst+20,segment->count);
  tl_put32(dst+24,segment->graph_degree);
  tl_put32(dst+28,segment->edge_count);
  tl_put64(dst+32,segment->generation);
  memcpy(dst+40,segment->model,32);
  memcpy(dst+72,segment->scope,32);
  tl_put64(dst+104,(uint64_t)size);
  tl_put64(dst+112,TL_HEADER_SIZE);
  tl_put64(dst+120,(uint64_t)rows);
  tl_put64(dst+128,(uint64_t)edges);
  memcpy(dst+TL_HEADER_SIZE,segment->ids,(size_t)segment->count*32u);
  for (uint32_t i=0; i<=segment->count; ++i)
    tl_put32(dst+rows+(size_t)i*4,segment->offsets[i]);
  for (uint32_t i=0; i<segment->edge_count; ++i)
    tl_put32(dst+edges+(size_t)i*4,segment->edges[i]);
  size_t cells = (size_t)segment->count*segment->dimensions;
  for (size_t i=0; i<cells; ++i) {
    uint32_t bits=0;
    memcpy(&bits,&segment->vectors[i],4);
    tl_put32(dst+vectors+i*4,bits);
  }
  tl_put32(dst+136,tl_crc32(dst,size));
  return TURBO_LEANN_OK;
}

turbo_leann_status_t turbo_leann_deserialize(const uint8_t *data, size_t size,
    const turbo_leann_limits_t *limits, turbo_leann_segment_t **out_segment) {
  if (!out_segment) return TURBO_LEANN_INVALID;
  *out_segment = NULL;
  if (!limits || !data || limits->struct_size != sizeof(*limits) ||
      limits->abi_version != TURBO_LEANN_ABI_VERSION)
    return TURBO_LEANN_INVALID;
  if (size < TL_HEADER_SIZE || size > limits->max_segment_bytes)
    return TURBO_LEANN_LIMIT;
  if (memcmp(data,tl_magic,8)) return TURBO_LEANN_CORRUPT;
  if (tl_get32(data+8) != TL_FORMAT_VERSION)
    return TURBO_LEANN_VERSION_UNSUPPORTED;
  if (tl_get32(data+12) != TL_HEADER_SIZE || tl_get32(data+140) != 0)
    return TURBO_LEANN_CORRUPT;
  uint32_t dim=tl_get32(data+16), n=tl_get32(data+20),
           degree=tl_get32(data+24), edge_count=tl_get32(data+28);
  if (!n || n > limits->max_nodes || n > 65535u || !dim ||
      dim > limits->max_dimensions || dim > 4096u ||
      degree > 64u || degree > n-1 ||
      (n > 2 && degree < 2) ||
      !tl_get64(data+32) || !tl_valid_fingerprint(data+40) ||
      !tl_valid_fingerprint(data+72) ||
      (uint64_t)edge_count > (uint64_t)n*degree) return TURBO_LEANN_CORRUPT;
  if (sizeof(float)!=4 || FLT_RADIX!=2 || FLT_MANT_DIG!=24)
    return TURBO_LEANN_VERSION_UNSUPPORTED;
  turbo_leann_segment_t sample = {0};
  sample.dimensions=dim; sample.count=n;
  sample.graph_degree=degree; sample.edge_count=edge_count;
  size_t rows,edges,vectors,required;
  if (!tl_layout(&sample,&rows,&edges,&vectors,&required) ||
      required!=size || tl_get64(data+104)!=(uint64_t)size ||
      tl_get64(data+112)!=TL_HEADER_SIZE ||
      tl_get64(data+120)!=(uint64_t)rows ||
      tl_get64(data+128)!=(uint64_t)edges)
    return TURBO_LEANN_CORRUPT;
  if (tl_get32(data+136)!=tl_crc32(data,size)) return TURBO_LEANN_CORRUPT;
  turbo_leann_segment_t *s=(turbo_leann_segment_t *)calloc(1,sizeof(*s));
  if (!s) return TURBO_LEANN_NO_MEMORY;
  *s=sample;
  s->generation=tl_get64(data+32);
  memcpy(s->model,data+40,32);
  memcpy(s->scope,data+72,32);
  s->ids=(uint8_t *)malloc((size_t)n*32u);
  s->offsets=(uint32_t *)malloc(((size_t)n+1u)*4u);
  s->edges=edge_count ? (uint32_t *)malloc((size_t)edge_count*4u) : NULL;
  s->vectors=(float *)malloc((size_t)n*dim*4u);
  if (!s->ids || !s->offsets || (edge_count && !s->edges) || !s->vectors) {
    tl_release(s); return TURBO_LEANN_NO_MEMORY;
  }
  memcpy(s->ids,data+TL_HEADER_SIZE,(size_t)n*32u);
  for (uint32_t i=1; i<n; ++i) {
    if (memcmp(s->ids+(size_t)(i-1)*32u,s->ids+(size_t)i*32u,16) >= 0) {
      tl_release(s); return TURBO_LEANN_CORRUPT;
    }
  }
  for (uint32_t i=0; i<=n; ++i)
    s->offsets[i]=tl_get32(data+rows+(size_t)i*4u);
  if (s->offsets[0]!=0 || s->offsets[n]!=edge_count) {
    tl_release(s); return TURBO_LEANN_CORRUPT;
  }
  for (uint32_t i=0; i<edge_count; ++i)
    s->edges[i]=tl_get32(data+edges+(size_t)i*4u);
  for (uint32_t i=0; i<n; ++i) {
    uint32_t begin=s->offsets[i],end=s->offsets[i+1];
    if (end<begin || end>edge_count || end-begin>degree) {
      tl_release(s); return TURBO_LEANN_CORRUPT;
    }
    for (uint32_t j=begin; j<end; ++j) {
      if (s->edges[j]>=n || s->edges[j]==i ||
          (j>begin && s->edges[j]<=s->edges[j-1])) {
        tl_release(s); return TURBO_LEANN_CORRUPT;
      }
    }
  }
  for (size_t i=0; i<(size_t)n*dim; ++i) {
    uint32_t bits=tl_get32(data+vectors+i*4u);
    memcpy(&s->vectors[i],&bits,4);
  }
  for (uint32_t i=0; i<n; ++i) {
    double norm=0.0;
    if (!tl_valid_vector(s->vectors+(size_t)i*dim,dim,&norm) ||
        fabs(norm-1.0)>0.002) {
      tl_release(s); return TURBO_LEANN_CORRUPT;
    }
  }
  *out_segment=s;
  return TURBO_LEANN_OK;
}
