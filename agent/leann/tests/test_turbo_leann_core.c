#include "turbo_leann.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
  exit(1); } } while (0)

static turbo_leann_build_t test_config(void) {
  turbo_leann_build_t c = {0};
  c.struct_size = sizeof(c);
  c.abi_version = TURBO_LEANN_ABI_VERSION;
  c.dimensions = 3;
  c.graph_degree = 3;
  c.generation = 108;
  c.model_fingerprint[0] = 0x31;
  c.scope_fingerprint[0] = 0x42;
  c.max_nodes = 256;
  c.max_pairwise_evaluations = 65536;
  c.max_segment_bytes = 1024*1024;
  return c;
}
static turbo_leann_query_t test_query(const turbo_leann_build_t *c,
                                      const float *v) {
  turbo_leann_query_t q = {0};
  q.struct_size = sizeof(q);
  q.abi_version = TURBO_LEANN_ABI_VERSION;
  q.generation = c->generation;
  memcpy(q.model_fingerprint,c->model_fingerprint,32);
  memcpy(q.scope_fingerprint,c->scope_fingerprint,32);
  q.components = v;
  q.dimensions = c->dimensions;
  q.top_k = 2;
  q.max_distance_evaluations = 20;
  return q;
}

int main(void) {
  const float x[3] = {1,0,0};
  const float y[3] = {0,2,0};
  const float z[3] = {0,0,3};
  const float diag[3] = {1,1,0};
  const float *data[] = {x,y,z,diag};
  turbo_leann_vector_t v[4] = {0};
  for (size_t i=0;i<4;++i) {
    v[i].chunk_id[15] = (uint8_t)(i+1);
    v[i].revision_id[0] = (uint8_t)(200+i);
    v[i].components = data[i];
  }
  turbo_leann_build_t c = test_config();
  turbo_leann_segment_t *s = NULL;
  CHECK(turbo_leann_build(&c,v,4,&s)==TURBO_LEANN_OK && s);
  size_t size=0;
  CHECK(turbo_leann_serialized_size(s,&size)==TURBO_LEANN_OK);
  CHECK(size > 144);
  uint8_t *bytes = (uint8_t *)malloc(size);
  CHECK(bytes);
  size_t written=0;
  CHECK(turbo_leann_serialize(s,bytes,size,&written)==TURBO_LEANN_OK);
  CHECK(size==written);

  /* Input permutation must produce identical bytes and graph ordinals. */
  turbo_leann_vector_t reversed[4];
  for (size_t i=0;i<4;++i) reversed[i]=v[3-i];
  turbo_leann_segment_t *again=NULL;
  CHECK(turbo_leann_build(&c,reversed,4,&again)==TURBO_LEANN_OK);
  uint8_t *canonical = (uint8_t *)malloc(size);
  CHECK(canonical);
  CHECK(turbo_leann_serialize(again,canonical,size,&written)==TURBO_LEANN_OK);
  CHECK(memcmp(bytes,canonical,size)==0);
  turbo_leann_segment_destroy(again);

  turbo_leann_limits_t limits = {0};
  limits.struct_size = sizeof(limits);
  limits.abi_version = TURBO_LEANN_ABI_VERSION;
  limits.max_nodes=100;
  limits.max_dimensions=128;
  limits.max_segment_bytes=1024*1024;
  turbo_leann_segment_t *restored=NULL;
  CHECK(turbo_leann_deserialize(bytes,size,&limits,&restored)==TURBO_LEANN_OK);
  turbo_leann_query_t q = test_query(&c,x);
  turbo_leann_hit_t hits[2] = {0};
  size_t count=99;
  CHECK(turbo_leann_search_exact(restored,&q,hits,2,&count)==TURBO_LEANN_OK);
  CHECK(count==2 && hits[0].chunk_id[15]==1 && hits[0].revision_id[0]==200);
  CHECK(fabs(hits[0].cosine_similarity-1.0f)<0.00001f);
  CHECK(hits[1].chunk_id[15]==4);

  /* Scope/generation/model mismatches are never recovered by a fallback. */
  q.generation++;
  CHECK(turbo_leann_search_exact(restored,&q,hits,2,&count)==TURBO_LEANN_SCOPE_MISMATCH && !count);
  q.generation--;
  q.scope_fingerprint[0]++;
  CHECK(turbo_leann_search_exact(restored,&q,hits,2,&count)==TURBO_LEANN_SCOPE_MISMATCH && !count);
  q.scope_fingerprint[0]--;
  q.model_fingerprint[0]++;
  CHECK(turbo_leann_search_exact(restored,&q,hits,2,&count)==TURBO_LEANN_SCOPE_MISMATCH && !count);
  q.model_fingerprint[0]--;
  q.max_distance_evaluations=2;
  CHECK(turbo_leann_search_exact(restored,&q,hits,2,&count)==TURBO_LEANN_LIMIT && !count);
  q.max_distance_evaluations=20;

  /* The identity + compacted graph are immutable; CRC catches mutation. */
  bytes[size-1] ^= 0x80;
  turbo_leann_segment_t *invalid=(turbo_leann_segment_t *)(uintptr_t)1;
  CHECK(turbo_leann_deserialize(bytes,size,&limits,&invalid)==TURBO_LEANN_CORRUPT);
  CHECK(invalid==NULL);
  bytes[size-1] ^= 0x80;
  limits.max_segment_bytes = size-1;
  CHECK(turbo_leann_deserialize(bytes,size,&limits,&invalid)==TURBO_LEANN_LIMIT);
  limits.max_segment_bytes = size;
  CHECK(turbo_leann_deserialize(bytes,size,&limits,&invalid)==TURBO_LEANN_OK);
  turbo_leann_segment_destroy(invalid);

  /* Never cross an invalid chunk ID, zero vector or nonfinite source. */
  c.max_pairwise_evaluations=1;
  CHECK(turbo_leann_build(&c,v,4,&invalid)==TURBO_LEANN_LIMIT && !invalid);
  c.max_pairwise_evaluations=65536;
  v[1].chunk_id[15] = v[0].chunk_id[15];
  CHECK(turbo_leann_build(&c,v,4,&invalid)==TURBO_LEANN_INVALID && !invalid);
  v[1].chunk_id[15] = 2;
  const float zero[3]={0,0,0};
  v[0].components=zero;
  CHECK(turbo_leann_build(&c,v,4,&invalid)==TURBO_LEANN_INVALID && !invalid);
  const float nan_vec[3]={NAN,1,1};
  v[0].components=nan_vec;
  CHECK(turbo_leann_build(&c,v,4,&invalid)==TURBO_LEANN_INVALID && !invalid);
  v[0].components=x;
  c.max_segment_bytes=100;
  CHECK(turbo_leann_build(&c,v,4,&invalid)==TURBO_LEANN_LIMIT && !invalid);

  /* All single-node and two-node CSR rows remain loadable. */
  c.max_segment_bytes=1024*1024;
  for (size_t n=1;n<=2;++n) {
    CHECK(turbo_leann_build(&c,v,n,&invalid)==TURBO_LEANN_OK);
    size_t needed=0;
    CHECK(turbo_leann_serialized_size(invalid,&needed)==TURBO_LEANN_OK);
    uint8_t *buf=(uint8_t *)malloc(needed);
    CHECK(buf);
    CHECK(turbo_leann_serialize(invalid,buf,needed,&written)==TURBO_LEANN_OK);
    turbo_leann_segment_t *single=NULL;
    CHECK(turbo_leann_deserialize(buf,needed,&limits,&single)==TURBO_LEANN_OK);
    turbo_leann_segment_destroy(single);
    turbo_leann_segment_destroy(invalid);
    free(buf);
  }
  turbo_leann_segment_destroy(s);
  turbo_leann_segment_destroy(restored);
  free(canonical);
  free(bytes);
  puts("turbo_leann L0 C11: PASS");
  return 0;
}
