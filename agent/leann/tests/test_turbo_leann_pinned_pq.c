#include "turbo_leann_binding.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if(!(x)) { \
  fprintf(stderr,"L1b CHECK failed %s:%d: %s\n",__FILE__,__LINE__,#x); \
  exit(1); } } while(0)
#define N 8u
#define D 4u

typedef struct fake_owner_s {
  uint32_t hash_calls;
  uint32_t admits;
  uint32_t deny_at;
  uint32_t recomputes;
  uint32_t fail_recompute_at;
  uint32_t forged_extra_bytes;
  uint32_t partial_embedding_at;
  float vectors[N][D];
} fake_owner_t;
/* Test-only deterministic fake hash; production requires real provider SHA-256. */
static turbo_leann_status_t fake_digest(void *ctx,const uint8_t *b,size_t n,
                                        uint8_t out[32]) {
  fake_owner_t *o=(fake_owner_t *)ctx;o->hash_calls++;
  uint32_t h=UINT32_C(2166136261);
  for(size_t i=0;i<n;i++)h=(h^b[i])*UINT32_C(16777619);
  for(size_t j=0;j<32;j++) {
    h=(h^(h>>15))*UINT32_C(2246822519);
    out[j]=(uint8_t)(h>>(8*(j%4)));
  }
  return TURBO_LEANN_OK;
}
static turbo_leann_status_t fake_admit(void *ctx,turbo_leann_bytes_t chunk,
                                       turbo_leann_bytes_t revision) {
  fake_owner_t *o=(fake_owner_t *)ctx;o->admits++;
  if(o->deny_at && o->admits==o->deny_at)return TURBO_LEANN_SCOPE_MISMATCH;
  if(chunk.size!=3 || revision.size!=3 ||
     chunk.data[0]!=0xa1 || revision.data[0]!=0xb1 ||
     chunk.data[1]>=N || chunk.data[1]!=revision.data[1] ||
     chunk.data[2]!=0 || revision.data[2]!=0)return TURBO_LEANN_SCOPE_MISMATCH;
  return TURBO_LEANN_OK;
}
static turbo_leann_status_t fake_recompute(void *ctx,
    turbo_leann_bytes_t chunk,turbo_leann_bytes_t revision,
    uint32_t dimensions,uint64_t max_bytes,float *dst,uint64_t *out_consumed) {
  fake_owner_t *o=(fake_owner_t *)ctx;o->recomputes++;
  if(o->fail_recompute_at && o->recomputes==o->fail_recompute_at)
    return TURBO_LEANN_CORRUPT; /* missing immutable source */
  if(o->partial_embedding_at && o->recomputes==o->partial_embedding_at) {
    dst[0]=1.0f; *out_consumed=16u;
    return TURBO_LEANN_OK; /* deliberately does not fill remaining cells */
  }
  if(dimensions!=D || chunk.size!=3 || revision.size!=3 ||
     chunk.data[0]!=0xa1 || revision.data[0]!=0xb1 ||
     chunk.data[1]>=N || chunk.data[1]!=revision.data[1] ||
     max_bytes<16)return TURBO_LEANN_LIMIT;
  memcpy(dst,o->vectors[chunk.data[1]],D*sizeof(float));
  *out_consumed=o->forged_extra_bytes?max_bytes+1u:16u;
  return TURBO_LEANN_OK;
}
int main(void) {
  fake_owner_t owner={0};
  turbo_leann_vector_t v[N]={0};
  turbo_leann_binding_entry_t entries[N]={0};
  uint8_t chunk_keys[N][3],revision_keys[N][3];
  for(uint32_t i=0;i<N;i++) {
    owner.vectors[i][0]=(float)(i+1u);
    owner.vectors[i][1]=(float)(N-i);
    owner.vectors[i][2]=(float)(i%3u+1u);
    owner.vectors[i][3]=(float)(i%2u+1u);
    v[i].chunk_label[15]=(uint8_t)(i+1u);
    v[i].revision_label[15]=(uint8_t)(i+33u);
    v[i].components=owner.vectors[i];
    memcpy(entries[i].chunk_label,v[i].chunk_label,16);
    memcpy(entries[i].revision_label,v[i].revision_label,16);
    chunk_keys[i][0]=0xa1;chunk_keys[i][1]=(uint8_t)i;chunk_keys[i][2]=0;
    revision_keys[i][0]=0xb1;revision_keys[i][1]=(uint8_t)i;revision_keys[i][2]=0;
    entries[i].chunk_key.data=chunk_keys[i];entries[i].chunk_key.size=3;
    entries[i].revision_key.data=revision_keys[i];entries[i].revision_key.size=3;
  }
  turbo_leann_build_t b={0};
  b.struct_size=sizeof(b);b.abi_version=TURBO_LEANN_ABI_VERSION;
  b.dimensions=D;b.graph_degree=4;b.generation=123;
  b.model_fingerprint[0]=0xab;b.scope_fingerprint[0]=0xcd;
  b.max_nodes=N;b.max_pairwise_evaluations=64;b.max_segment_bytes=100000;
  turbo_leann_segment_t *l0=NULL;
  CHECK(turbo_leann_build(&b,v,N,&l0)==TURBO_LEANN_OK);
  turbo_leann_pq_config_t pc={0};
  pc.struct_size=sizeof(pc);pc.abi_version=TURBO_LEANN_ABI_VERSION;
  pc.subquantizers=2;pc.centroids=4;pc.iterations=5;
  pc.max_training_distance_evaluations=10000;
  pc.max_compact_bytes=100000;pc.max_peak_allocation_bytes=100000;
  turbo_leann_compact_t *pq=NULL;
  CHECK(turbo_leann_pq_build(l0,&pc,&pq)==TURBO_LEANN_OK);
  size_t sz=0;
  CHECK(turbo_leann_pq_serialized_size(pq,&sz)==TURBO_LEANN_OK);
  uint8_t *blob=(uint8_t *)malloc(sz);CHECK(blob);
  CHECK(turbo_leann_pq_serialize(pq,blob,sz,&sz)==TURBO_LEANN_OK);
  turbo_leann_compact_destroy(pq);pq=NULL;

  turbo_leann_node_map_config_t mc={0};mc.struct_size=sizeof(mc);
  mc.abi_version=TURBO_LEANN_ABI_VERSION;mc.generation=b.generation;
  memcpy(mc.model_fingerprint,b.model_fingerprint,32);
  memcpy(mc.scope_fingerprint,b.scope_fingerprint,32);
  mc.max_nodes=N;mc.max_node_map_bytes=10000;
  size_t maplen=0;
  CHECK(turbo_leann_node_map_encode(&mc,entries,N,NULL,0,&maplen)==TURBO_LEANN_LIMIT);
  uint8_t *map=(uint8_t *)malloc(maplen);CHECK(map);
  CHECK(turbo_leann_node_map_encode(&mc,entries,N,map,maplen,&maplen)==TURBO_LEANN_OK);

  turbo_leann_manifest_t m={0};m.struct_size=sizeof(m);
  m.abi_version=TURBO_LEANN_ABI_VERSION;m.generation=b.generation;
  m.commit_seq=987;m.node_count=N;m.dimensions=D;
  memcpy(m.model_fingerprint,b.model_fingerprint,32);
  memcpy(m.scope_fingerprint,b.scope_fingerprint,32);
  CHECK(fake_digest(&owner,blob,sz,m.segment_sha256)==TURBO_LEANN_OK);
  CHECK(fake_digest(&owner,map,maplen,m.node_map_sha256)==TURBO_LEANN_OK);
  owner.hash_calls=0;
  turbo_leann_pinned_read_t read={0};read.struct_size=sizeof(read);
  read.abi_version=TURBO_LEANN_ABI_VERSION;read.pinned_owner=&owner;
  read.generation=m.generation;read.commit_seq=m.commit_seq;
  memcpy(read.model_fingerprint,m.model_fingerprint,32);
  memcpy(read.scope_fingerprint,m.scope_fingerprint,32);
  read.sha256=fake_digest;read.admit_node=fake_admit;
  turbo_leann_limits_t limits={0};limits.struct_size=sizeof(limits);
  limits.abi_version=TURBO_LEANN_ABI_VERSION;limits.max_nodes=N;
  limits.max_dimensions=D;limits.max_segment_bytes=100000;
  turbo_leann_query_t q={0};q.struct_size=sizeof(q);
  q.abi_version=TURBO_LEANN_ABI_VERSION;q.generation=b.generation;
  memcpy(q.model_fingerprint,b.model_fingerprint,32);
  memcpy(q.scope_fingerprint,b.scope_fingerprint,32);
  q.components=owner.vectors[5];q.dimensions=D;
  q.top_k=3;q.max_distance_evaluations=N;

  turbo_leann_verified_adc_hit_t hits[3]={0};size_t found=22;
#define ADC() turbo_leann_search_pinned_adc(&m,&read,blob,sz,map,maplen, \
    &limits,10000,&q,hits,3,&found)
  CHECK(ADC()==TURBO_LEANN_OK);
  CHECK(found==3 && owner.admits==N && owner.hash_calls==2);
  for(size_t i=0;i<found;i++) {
    CHECK(hits[i].chunk_key.size==3);
    CHECK(hits[i].revision_key.size==3);
    CHECK(hits[i].scored.chunk_label[15]==hits[i].chunk_key.data[1]+1u);
  }

  turbo_leann_verified_hit_t reranked[3]={0};
#define RECOMPUTE() turbo_leann_search_pinned_recompute(&m,&read,blob,sz, \
    map,maplen,&limits,10000,&q,N,128,fake_recompute,reranked,3,&found)
  owner.admits=0;owner.recomputes=0;
  CHECK(RECOMPUTE()==TURBO_LEANN_OK);
  CHECK(found==3 && owner.admits==N && owner.recomputes==N);
  turbo_leann_hit_t exact[3]={0};size_t exact_found=0;
  CHECK(turbo_leann_search_exact(l0,&q,exact,3,&exact_found)==TURBO_LEANN_OK);
  CHECK(found==exact_found);
  for(size_t i=0;i<found;i++) {
    CHECK(!memcmp(reranked[i].scored.chunk_label,exact[i].chunk_label,16));
    CHECK(fabsf(reranked[i].scored.cosine_similarity-exact[i].cosine_similarity)<0.00001f);
  }
  /* Incomplete Host validation fails before any ADC or recompute. */
  owner.deny_at=5;owner.admits=0;owner.recomputes=0;
  CHECK(ADC()==TURBO_LEANN_SCOPE_MISMATCH && found==0 && owner.admits==5);
  CHECK(owner.recomputes==0);
  owner.admits=0;
  CHECK(RECOMPUTE()==TURBO_LEANN_SCOPE_MISMATCH && found==0);
  CHECK(owner.recomputes==0);
  owner.deny_at=0;

  /* Missing historical source: no approximate fallback or partial result. */
  owner.fail_recompute_at=3;owner.admits=0;owner.recomputes=0;
  memset(reranked,0xc7,sizeof(reranked));
  turbo_leann_verified_hit_t unchanged[3];memcpy(unchanged,reranked,sizeof(unchanged));
  CHECK(RECOMPUTE()==TURBO_LEANN_CORRUPT && found==0);
  CHECK(owner.recomputes==3);
  CHECK(memcmp(unchanged,reranked,sizeof(unchanged))==0);
  owner.fail_recompute_at=0;
  owner.partial_embedding_at=2;owner.admits=0;owner.recomputes=0;
  CHECK(RECOMPUTE()==TURBO_LEANN_INVALID && found==0);
  CHECK(memcmp(unchanged,reranked,sizeof(unchanged))==0);
  owner.partial_embedding_at=0;
  /* Reported source bytes beyond bounded Host budget are rejected. */
  owner.forged_extra_bytes=1;owner.admits=0;owner.recomputes=0;
  CHECK(RECOMPUTE()==TURBO_LEANN_LIMIT && found==0);
  owner.forged_extra_bytes=0;

  /* Current Host ACL revoked after an earlier valid generation. */
  read.scope_fingerprint[0]^=1u;owner.admits=0;owner.recomputes=0;
  CHECK(RECOMPUTE()==TURBO_LEANN_SCOPE_MISMATCH && found==0);
  CHECK(owner.admits==0 && owner.recomputes==0);
  read.scope_fingerprint[0]^=1u;
  read.generation++;
  CHECK(ADC()==TURBO_LEANN_SCOPE_MISMATCH && found==0);
  read.generation--;

  /* Tampering blocks entry before Host callbacks. */
  map[maplen-1]^=1u;owner.admits=0;owner.recomputes=0;
  CHECK(RECOMPUTE()==TURBO_LEANN_CORRUPT && found==0);
  CHECK(owner.admits==0 && owner.recomputes==0);
  map[maplen-1]^=1u;
  blob[sz-1]^=1u;
  CHECK(ADC()==TURBO_LEANN_CORRUPT && found==0);
  blob[sz-1]^=1u;
  owner.admits=0;owner.recomputes=0;
  CHECK(RECOMPUTE()==TURBO_LEANN_OK && found==3);

  turbo_leann_segment_destroy(l0);free(blob);free(map);
  puts("turbo_leann L1b pinned PQ + selective recompute: PASS");
  return 0;
}
