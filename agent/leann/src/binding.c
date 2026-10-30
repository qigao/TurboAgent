#include "turbo_leann_binding.h"
#include "turbo_leann_internal.h"

#include <stdlib.h>
#include <string.h>

/* Canonical node map format v1, little endian.
 * magic[8] "TLMAP001", version[4], header_size[4], generation[8],
 * entry_count[4], reserved[4], model[32], scope[32], total_length[8].
 * Each entry: chunk_label[16], revision_label[16], chunk_len[2],
 * revision_len[2], exact chunk key bytes, exact revision key bytes.
 * Each input key is an opaque, length-delimited byte string from Agent #304.
 * The manifest SHA-256, NOT this format header, authenticates the map. */
static const uint8_t tl_map_magic[8] = {'T','L','M','A','P','0','0','1'};

static void tl_write16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static void tl_write32(uint8_t *p, uint32_t v) {
  for (uint32_t i=0; i<4; ++i) p[i] = (uint8_t)(v >> (i*8));
}
static void tl_write64(uint8_t *p, uint64_t v) {
  for (uint32_t i=0; i<8; ++i) p[i] = (uint8_t)(v >> (i*8));
}
static uint16_t tl_read16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t tl_read32(const uint8_t *p) {
  uint32_t v = 0;
  for (uint32_t i=0; i<4; ++i) v |= (uint32_t)p[i] << (i*8);
  return v;
}
static uint64_t tl_read64(const uint8_t *p) {
  uint64_t v = 0;
  for (uint32_t i=0; i<8; ++i) v |= (uint64_t)p[i] << (i*8);
  return v;
}
static int tl_valid_bytes(turbo_leann_bytes_t b) {
  return b.data && b.size && b.size <= TURBO_LEANN_NODE_KEY_MAX_BYTES;
}
static int tl_id_valid(const uint8_t *id) {
  uint8_t accum = 0;
  for (uint32_t j=0; j<TURBO_LEANN_ID_BYTES; ++j) accum |= id[j];
  return accum != 0;
}

/* Keys are ordered by canonical binary content, not by the label hash. */
typedef struct tl_key_view_s {
  turbo_leann_bytes_t chunk;
} tl_key_view_t;
static int tl_key_compare(const void *va, const void *vb) {
  const tl_key_view_t *a=(const tl_key_view_t *)va;
  const tl_key_view_t *b=(const tl_key_view_t *)vb;
  size_t size=a->chunk.size < b->chunk.size ? a->chunk.size : b->chunk.size;
  int result=memcmp(a->chunk.data,b->chunk.data,size);
  if (result) return result;
  return (a->chunk.size > b->chunk.size) - (a->chunk.size < b->chunk.size);
}

/* This builder is intentionally strict: order must match the segment's
 * canonical sorted labels. No truncation, aliasing or implicit sort. */
turbo_leann_status_t turbo_leann_node_map_encode(
    const turbo_leann_node_map_config_t *config,
    const turbo_leann_binding_entry_t *entries, size_t count,
    uint8_t *dst, size_t dst_capacity, size_t *out_size) {
  if (!out_size) return TURBO_LEANN_INVALID;
  *out_size = 0;
  if (!config || !entries || config->struct_size != sizeof(*config) ||
      config->abi_version != TURBO_LEANN_ABI_VERSION || config->generation == 0 ||
      !tl_valid_fingerprint(config->model_fingerprint) ||
      !tl_valid_fingerprint(config->scope_fingerprint)) return TURBO_LEANN_INVALID;
  if (count == 0 || count > config->max_nodes || count > 65535u ||
      !config->max_node_map_bytes) return TURBO_LEANN_LIMIT;
  size_t need=TURBO_LEANN_NODE_MAP_HEADER_BYTES;
  for (size_t i=0; i<count; ++i) {
    const turbo_leann_binding_entry_t *e=&entries[i];
    if (!tl_valid_bytes(e->chunk_key) || !tl_valid_bytes(e->revision_key) ||
        !tl_id_valid(e->chunk_label) || !tl_id_valid(e->revision_label) ||
        (i && memcmp(entries[i-1].chunk_label,e->chunk_label,
                     TURBO_LEANN_ID_BYTES) >= 0)) return TURBO_LEANN_INVALID;
    if (!tl_checked_add(need,36u,&need) ||
        !tl_checked_add(need,e->chunk_key.size,&need) ||
        !tl_checked_add(need,e->revision_key.size,&need) ||
        need > config->max_node_map_bytes) return TURBO_LEANN_LIMIT;
  }
  /* Refuse two labels that alias the same full canonical Chunk key.
   * Use bounded O(N log N) sort rather than quadratic identity checks. */
  tl_key_view_t *unique=(tl_key_view_t *)calloc(count,sizeof(*unique));
  if (!unique) return TURBO_LEANN_NO_MEMORY;
  for (size_t i=0; i<count; ++i) unique[i].chunk=entries[i].chunk_key;
  qsort(unique,count,sizeof(*unique),tl_key_compare);
  for (size_t i=1; i<count; ++i) {
    if (!tl_key_compare(&unique[i-1],&unique[i])) {
      free(unique); return TURBO_LEANN_INVALID;
    }
  }
  free(unique);
  *out_size=need;
  if (!dst || dst_capacity < need) return TURBO_LEANN_LIMIT;
  /* Build after validating the entire sequence; never partially encode a bad
   * entry. Output-size semantics match the existing segment serializer. */
  memset(dst,0,TURBO_LEANN_NODE_MAP_HEADER_BYTES);
  memcpy(dst,tl_map_magic,8);
  tl_write32(dst+8,1u);
  tl_write32(dst+12,TURBO_LEANN_NODE_MAP_HEADER_BYTES);
  tl_write64(dst+16,config->generation);
  tl_write32(dst+24,(uint32_t)count);
  memcpy(dst+32,config->model_fingerprint,32);
  memcpy(dst+64,config->scope_fingerprint,32);
  tl_write64(dst+96,(uint64_t)need);
  size_t cursor=TURBO_LEANN_NODE_MAP_HEADER_BYTES;
  for (size_t i=0; i<count; ++i) {
    const turbo_leann_binding_entry_t *e=&entries[i];
    memcpy(dst+cursor,e->chunk_label,16);
    memcpy(dst+cursor+16,e->revision_label,16);
    tl_write16(dst+cursor+32,(uint16_t)e->chunk_key.size);
    tl_write16(dst+cursor+34,(uint16_t)e->revision_key.size);
    cursor+=36;
    memcpy(dst+cursor,e->chunk_key.data,e->chunk_key.size);
    cursor+=e->chunk_key.size;
    memcpy(dst+cursor,e->revision_key.data,e->revision_key.size);
    cursor+=e->revision_key.size;
  }
  return TURBO_LEANN_OK;
}

static int tl_manifest_matches(const turbo_leann_manifest_t *m,
                               const turbo_leann_pinned_read_t *r,
                               const turbo_leann_query_t *q) {
  if (m->generation == 0 || m->commit_seq == 0 || m->node_count == 0 ||
      m->dimensions == 0 || m->generation != r->generation ||
      m->commit_seq != r->commit_seq || m->generation != q->generation ||
      m->dimensions != q->dimensions ||
      memcmp(m->model_fingerprint,r->model_fingerprint,32) ||
      memcmp(m->scope_fingerprint,r->scope_fingerprint,32) ||
      memcmp(m->model_fingerprint,q->model_fingerprint,32) ||
      memcmp(m->scope_fingerprint,q->scope_fingerprint,32)) return 0;
  return 1;
}

static turbo_leann_status_t tl_hash_blob(turbo_leann_sha256_fn fn, void *owner,
     const uint8_t *blob, size_t length, const uint8_t expected[32]) {
  uint8_t digest[32]={0};
  turbo_leann_status_t s=fn(owner,blob,length,digest);
  if (s!=TURBO_LEANN_OK) return s;
  if (memcmp(digest,expected,32)) return TURBO_LEANN_CORRUPT;
  return TURBO_LEANN_OK;
}

turbo_leann_status_t turbo_leann_search_pinned_exact(
    const turbo_leann_manifest_t *manifest,
    const turbo_leann_pinned_read_t *read,
    const uint8_t *segment_blob, size_t segment_bytes,
    const uint8_t *node_map_blob, size_t node_map_bytes,
    const turbo_leann_limits_t *limits, uint64_t max_node_map_bytes,
    const turbo_leann_query_t *query,
    turbo_leann_verified_hit_t *out_hits, size_t hits_capacity,
    size_t *out_count) {
  if (!out_count) return TURBO_LEANN_INVALID;
  *out_count=0;
  if (!manifest || !read || !segment_blob || !segment_bytes || !node_map_blob ||
      !node_map_bytes || !limits || !query || !out_hits ||
      manifest->struct_size != sizeof(*manifest) ||
      read->struct_size != sizeof(*read) ||
      manifest->abi_version != TURBO_LEANN_ABI_VERSION ||
      read->abi_version != TURBO_LEANN_ABI_VERSION ||
      limits->struct_size != sizeof(*limits) ||
      limits->abi_version != TURBO_LEANN_ABI_VERSION ||
      query->struct_size != sizeof(*query) ||
      query->abi_version != TURBO_LEANN_ABI_VERSION ||
      !read->pinned_owner || !read->sha256 || !read->admit_node ||
      !query->top_k || query->top_k > hits_capacity || query->top_k > 1024)
    return TURBO_LEANN_INVALID;
  if (!tl_manifest_matches(manifest,read,query))
    return TURBO_LEANN_SCOPE_MISMATCH;
  if (manifest->node_count > limits->max_nodes ||
      manifest->dimensions > limits->max_dimensions ||
      query->max_distance_evaluations < manifest->node_count ||
      segment_bytes > limits->max_segment_bytes ||
      node_map_bytes > max_node_map_bytes)
    return TURBO_LEANN_LIMIT;
  if (!tl_valid_fingerprint(manifest->segment_sha256) ||
      !tl_valid_fingerprint(manifest->node_map_sha256)) return TURBO_LEANN_INVALID;
  turbo_leann_status_t status=tl_hash_blob(read->sha256,read->pinned_owner,
      segment_blob,segment_bytes,manifest->segment_sha256);
  if (status!=TURBO_LEANN_OK) return status;
  status=tl_hash_blob(read->sha256,read->pinned_owner,
      node_map_blob,node_map_bytes,manifest->node_map_sha256);
  if (status!=TURBO_LEANN_OK) return status;

  turbo_leann_segment_t *segment=NULL;
  status=turbo_leann_deserialize(segment_blob,segment_bytes,limits,&segment);
  if (status!=TURBO_LEANN_OK) return status;
  tl_key_view_t *keys=NULL;
  turbo_leann_binding_entry_t *bindings=NULL;
  turbo_leann_hit_t *scores=NULL;
  turbo_leann_verified_hit_t *verified=NULL;
  if (segment->count!=manifest->node_count ||
      segment->dimensions!=manifest->dimensions ||
      segment->generation!=manifest->generation ||
      memcmp(segment->model,manifest->model_fingerprint,32) ||
      memcmp(segment->scope,manifest->scope_fingerprint,32)) {
    status=TURBO_LEANN_CORRUPT;
    goto cleanup;
  }
  if (node_map_bytes < TURBO_LEANN_NODE_MAP_HEADER_BYTES ||
      memcmp(node_map_blob,tl_map_magic,8) ||
      tl_read32(node_map_blob+8)!=1u ||
      tl_read32(node_map_blob+12)!=TURBO_LEANN_NODE_MAP_HEADER_BYTES ||
      tl_read64(node_map_blob+16)!=manifest->generation ||
      tl_read32(node_map_blob+24)!=manifest->node_count ||
      tl_read32(node_map_blob+28)!=0 ||
      memcmp(node_map_blob+32,manifest->model_fingerprint,32) ||
      memcmp(node_map_blob+64,manifest->scope_fingerprint,32) ||
      tl_read64(node_map_blob+96)!=(uint64_t)node_map_bytes) {
    status=TURBO_LEANN_CORRUPT;
    goto cleanup;
  }

  bindings=(turbo_leann_binding_entry_t *)calloc(segment->count,sizeof(*bindings));
  keys=(tl_key_view_t *)calloc(segment->count,sizeof(*keys));
  if (!bindings || !keys) { status=TURBO_LEANN_NO_MEMORY; goto cleanup; }
  size_t cursor=TURBO_LEANN_NODE_MAP_HEADER_BYTES;
  for (uint32_t i=0; i<segment->count; ++i) {
    if (node_map_bytes-cursor < 36u) { status=TURBO_LEANN_CORRUPT; goto cleanup; }
    const uint8_t *entry=node_map_blob+cursor;
    uint16_t clen=tl_read16(entry+32), rlen=tl_read16(entry+34);
    if (!clen || !rlen || clen>TURBO_LEANN_NODE_KEY_MAX_BYTES ||
        rlen>TURBO_LEANN_NODE_KEY_MAX_BYTES ||
        memcmp(entry,segment->ids+(size_t)i*32u,32) ||
        (size_t)clen+(size_t)rlen > node_map_bytes-cursor-36u) {
      status=TURBO_LEANN_CORRUPT;
      goto cleanup;
    }
    memcpy(bindings[i].chunk_label,entry,16);
    memcpy(bindings[i].revision_label,entry+16,16);
    cursor+=36u;
    bindings[i].chunk_key.data=node_map_blob+cursor;
    bindings[i].chunk_key.size=clen;
    cursor+=clen;
    bindings[i].revision_key.data=node_map_blob+cursor;
    bindings[i].revision_key.size=rlen;
    cursor+=rlen;
    keys[i].chunk=bindings[i].chunk_key;
  }
  if (cursor!=node_map_bytes) { status=TURBO_LEANN_CORRUPT; goto cleanup; }
  /* A label cannot alias two canonical chunks: map uniqueness must be
   * checked before any Host callback or vector access. */
  qsort(keys,segment->count,sizeof(*keys),tl_key_compare);
  for (uint32_t i=1; i<segment->count; ++i)
    if (!tl_key_compare(&keys[i-1],&keys[i])) {
      status=TURBO_LEANN_CORRUPT; goto cleanup;
    }
  for (uint32_t i=0; i<segment->count; ++i) {
    status=read->admit_node(read->pinned_owner,bindings[i].chunk_key,
                            bindings[i].revision_key);
    if (status!=TURBO_LEANN_OK) goto cleanup;
  }

  scores=(turbo_leann_hit_t *)calloc(query->top_k,sizeof(*scores));
  verified=(turbo_leann_verified_hit_t *)calloc(query->top_k,sizeof(*verified));
  if (!scores || !verified) { status=TURBO_LEANN_NO_MEMORY; goto cleanup; }
  size_t count=0;
  status=turbo_leann_search_exact(segment,query,scores,query->top_k,&count);
  if (status!=TURBO_LEANN_OK) goto cleanup;
  /* Each score is resolved only to the verified same-generation map entry.
   * The caller must retain BOTH blob and pinned read until it consumes hits. */
  for (size_t k=0; k<count; ++k) {
    size_t l=0,r=segment->count;
    while (l<r) {
      size_t mid=l+(r-l)/2;
      if (memcmp(bindings[mid].chunk_label,scores[k].chunk_label,16)<0)
        l=mid+1;
      else r=mid;
    }
    if (l>=segment->count ||
        memcmp(bindings[l].chunk_label,scores[k].chunk_label,16) ||
        memcmp(bindings[l].revision_label,scores[k].revision_label,16)) {
      status=TURBO_LEANN_CORRUPT; goto cleanup;
    }
    verified[k].scored=scores[k];
    verified[k].chunk_key=bindings[l].chunk_key;
    verified[k].revision_key=bindings[l].revision_key;
  }
  memcpy(out_hits,verified,count*sizeof(*out_hits));
  *out_count=count;
cleanup:
  free(verified);
  free(scores);
  free(keys);
  free(bindings);
  turbo_leann_segment_destroy(segment);
  return status;
}
