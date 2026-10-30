#include "turbo_leann_binding.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr,"L0.5 check failed %s:%d: %s\n",__FILE__,__LINE__,#expr); \
  exit(1); } } while(0)

/* The test digest is only a deterministic FAKE callback. Production MUST
 * pass canonical provider SHA-256; these tests prove callback bindings,
 * scope/manifest checks and memory safety, not cryptographic strength. */
typedef struct fake_pinned_owner_s {
  uint32_t hash_calls;
  uint32_t admitted;
  uint32_t deny_at;
  uint32_t corrupt_digest;
} fake_pinned_owner_t;
static turbo_leann_status_t fake_sha256(void *user,const uint8_t *p,size_t n,
                                        uint8_t out[32]) {
  fake_pinned_owner_t *o=(fake_pinned_owner_t *)user;
  o->hash_calls++;
  if (!p || !n) return TURBO_LEANN_INVALID;
  uint32_t acc=UINT32_C(0x811c9dc5);
  for (size_t i=0; i<n; ++i) acc=(acc^p[i])*UINT32_C(16777619);
  for (size_t i=0; i<32; ++i) {
    acc^=acc>>13;
    acc*=UINT32_C(0x5bd1e995);
    out[i]=(uint8_t)(acc>>(8*(i%4)));
  }
  if (o->corrupt_digest) out[0]^=0x80u;
  return TURBO_LEANN_OK;
}
static turbo_leann_status_t fake_admit(void *user,turbo_leann_bytes_t chunk,
                                      turbo_leann_bytes_t revision) {
  fake_pinned_owner_t *o=(fake_pinned_owner_t *)user;
  o->admitted++;
  if (o->deny_at && o->admitted==o->deny_at)
    return TURBO_LEANN_SCOPE_MISMATCH;
  if (chunk.size!=4 || revision.size!=3 || chunk.data[0]!=0xa7 ||
      chunk.data[1]>=3 || chunk.data[2]!=0xff || chunk.data[3]!=0 ||
      revision.data[0]!=0xb8 || revision.data[1]!=chunk.data[1] ||
      revision.data[2]!=0) return TURBO_LEANN_SCOPE_MISMATCH;
  return TURBO_LEANN_OK;
}

int main(void) {
  static const float x[]={1,0,0}, y[]={0,1,0}, z[]={0,0,1};
  const float *vec[]={x,y,z};
  turbo_leann_vector_t v[3]={0};
  turbo_leann_binding_entry_t e[3]={0};
  uint8_t chunk_key[3][4], revision_key[3][3];
  for (uint32_t i=0; i<3; ++i) {
    v[i].chunk_label[15]=(uint8_t)(i+1u);
    v[i].revision_label[0]=(uint8_t)(i+33u);
    v[i].components=vec[i];
    memcpy(e[i].chunk_label,v[i].chunk_label,16);
    memcpy(e[i].revision_label,v[i].revision_label,16);
    chunk_key[i][0]=0xa7;
    chunk_key[i][1]=(uint8_t)i;
    chunk_key[i][2]=0xff;
    chunk_key[i][3]=0;
    revision_key[i][0]=0xb8;
    revision_key[i][1]=(uint8_t)i;
    revision_key[i][2]=0;
    e[i].chunk_key.data=chunk_key[i];
    e[i].chunk_key.size=sizeof(chunk_key[i]);
    e[i].revision_key.data=revision_key[i];
    e[i].revision_key.size=sizeof(revision_key[i]);
  }
  turbo_leann_build_t b={0};
  b.struct_size=sizeof(b);
  b.abi_version=TURBO_LEANN_ABI_VERSION;
  b.dimensions=3;
  b.graph_degree=2;
  b.generation=108;
  b.model_fingerprint[0]=11;
  b.scope_fingerprint[0]=19;
  b.max_nodes=10;
  b.max_pairwise_evaluations=100;
  b.max_segment_bytes=32768;
  turbo_leann_segment_t *segment=NULL;
  CHECK(turbo_leann_build(&b,v,3,&segment)==TURBO_LEANN_OK);
  size_t segment_len=0;
  CHECK(turbo_leann_serialized_size(segment,&segment_len)==TURBO_LEANN_OK);
  uint8_t *segment_blob=(uint8_t *)malloc(segment_len);
  CHECK(segment_blob);
  CHECK(turbo_leann_serialize(segment,segment_blob,segment_len,&segment_len)
        ==TURBO_LEANN_OK);
  turbo_leann_segment_destroy(segment);
  segment=NULL;

  turbo_leann_node_map_config_t cfg={0};
  cfg.struct_size=sizeof(cfg);
  cfg.abi_version=TURBO_LEANN_ABI_VERSION;
  cfg.generation=b.generation;
  memcpy(cfg.model_fingerprint,b.model_fingerprint,32);
  memcpy(cfg.scope_fingerprint,b.scope_fingerprint,32);
  cfg.max_nodes=10;
  cfg.max_node_map_bytes=10000;
  size_t map_len=0;
  CHECK(turbo_leann_node_map_encode(&cfg,e,3,NULL,0,&map_len)
        ==TURBO_LEANN_LIMIT);
  CHECK(map_len>104);
  uint8_t *map_blob=(uint8_t *)malloc(map_len);
  CHECK(map_blob);
  CHECK(turbo_leann_node_map_encode(&cfg,e,3,map_blob,map_len,&map_len)
        ==TURBO_LEANN_OK);

  fake_pinned_owner_t owner={0};
  turbo_leann_manifest_t m={0};
  m.struct_size=sizeof(m);
  m.abi_version=TURBO_LEANN_ABI_VERSION;
  m.generation=108;
  m.commit_seq=352;
  m.node_count=3;
  m.dimensions=3;
  memcpy(m.model_fingerprint,b.model_fingerprint,32);
  memcpy(m.scope_fingerprint,b.scope_fingerprint,32);
  CHECK(fake_sha256(&owner,segment_blob,segment_len,m.segment_sha256)==TURBO_LEANN_OK);
  CHECK(fake_sha256(&owner,map_blob,map_len,m.node_map_sha256)==TURBO_LEANN_OK);
  owner.hash_calls=0;
  turbo_leann_pinned_read_t read={0};
  read.struct_size=sizeof(read);
  read.abi_version=TURBO_LEANN_ABI_VERSION;
  read.pinned_owner=&owner;
  read.generation=m.generation;
  read.commit_seq=m.commit_seq;
  memcpy(read.model_fingerprint,m.model_fingerprint,32);
  memcpy(read.scope_fingerprint,m.scope_fingerprint,32);
  read.sha256=fake_sha256;
  read.admit_node=fake_admit;
  turbo_leann_limits_t limits={0};
  limits.struct_size=sizeof(limits);
  limits.abi_version=TURBO_LEANN_ABI_VERSION;
  limits.max_nodes=10;
  limits.max_dimensions=4;
  limits.max_segment_bytes=32768;
  turbo_leann_query_t query={0};
  query.struct_size=sizeof(query);
  query.abi_version=TURBO_LEANN_ABI_VERSION;
  query.generation=108;
  memcpy(query.model_fingerprint,b.model_fingerprint,32);
  memcpy(query.scope_fingerprint,b.scope_fingerprint,32);
  query.components=x;
  query.dimensions=3;
  query.top_k=2;
  query.max_distance_evaluations=5;
  turbo_leann_verified_hit_t hits[2]={0};
  size_t found=77;
#define SEARCH() turbo_leann_search_pinned_exact(&m,&read,segment_blob,segment_len, \
    map_blob,map_len,&limits,10000,&query,hits,2,&found)
  CHECK(SEARCH()==TURBO_LEANN_OK);
  CHECK(found==2 && owner.admitted==3 && owner.hash_calls==2);
  CHECK(hits[0].scored.chunk_label[15]==1 &&
        hits[0].chunk_key.data==map_blob+104+36 &&
        hits[0].chunk_key.size==4 &&
        hits[0].chunk_key.data[0]==0xa7 &&
        hits[0].revision_key.size==3);
  CHECK(fabsf(hits[0].scored.cosine_similarity-1.0f)<0.001f);

  /* Manifest/snapshot/generation mismatch must reject BEFORE accessing data. */
  owner.hash_calls=0; owner.admitted=0;
  read.commit_seq++;
  CHECK(SEARCH()==TURBO_LEANN_SCOPE_MISMATCH && found==0);
  CHECK(owner.hash_calls==0 && owner.admitted==0);
  read.commit_seq--;
  read.scope_fingerprint[1]^=1;
  CHECK(SEARCH()==TURBO_LEANN_SCOPE_MISMATCH && found==0);
  read.scope_fingerprint[1]^=1;

  /* No cross-generation or wrong-model index may be silently substituted. */
  query.generation++;
  CHECK(SEARCH()==TURBO_LEANN_SCOPE_MISMATCH && found==0);
  query.generation--;
  query.model_fingerprint[0]++;
  CHECK(SEARCH()==TURBO_LEANN_SCOPE_MISMATCH && found==0);
  query.model_fingerprint[0]--;

  /* Any denial during COMPLETE graph-node admission suppresses all hits. */
  owner.hash_calls=0; owner.admitted=0; owner.deny_at=3;
  CHECK(SEARCH()==TURBO_LEANN_SCOPE_MISMATCH && found==0);
  CHECK(owner.admitted==3 && owner.hash_calls==2);
  owner.deny_at=0;

  /* A modified blob with unchanged published SHA-256 fails before decoding. */
  map_blob[map_len-1]^=0x01;
  owner.hash_calls=0; owner.admitted=0;
  CHECK(SEARCH()==TURBO_LEANN_CORRUPT && found==0);
  CHECK(owner.admitted==0);
  map_blob[map_len-1]^=0x01;
  segment_blob[segment_len-1]^=0x01;
  CHECK(SEARCH()==TURBO_LEANN_CORRUPT && found==0);
  segment_blob[segment_len-1]^=0x01;

  /* Even a correctly re-hashed (but forged) map cannot change node labels. */
  uint8_t original=map_blob[104];
  map_blob[104]^=0x80;
  CHECK(fake_sha256(&owner,map_blob,map_len,m.node_map_sha256)==TURBO_LEANN_OK);
  owner.admitted=0;
  CHECK(SEARCH()==TURBO_LEANN_CORRUPT && found==0);
  CHECK(owner.admitted==0);
  map_blob[104]=original;

  /* Duplicate full canonical keys under different labels are prohibited. */
  e[1].chunk_key=e[0].chunk_key;
  size_t rejected_size=99;
  CHECK(turbo_leann_node_map_encode(&cfg,e,3,map_blob,map_len,&rejected_size)
        ==TURBO_LEANN_INVALID && rejected_size==0);
  e[1].chunk_key.data=chunk_key[1];
  CHECK(turbo_leann_node_map_encode(&cfg,e,3,map_blob,map_len,&map_len)==TURBO_LEANN_OK);
  /* Forge an alias in the signed on-disk blob: both labels point to key #0.
   * The verifier rejects this BEFORE calling Host node admission. */
  size_t second_chunk_at=104u+36u+4u+3u+36u;
  CHECK(map_blob[second_chunk_at+1]==1u);
  map_blob[second_chunk_at+1]=0u;
  CHECK(fake_sha256(&owner,map_blob,map_len,m.node_map_sha256)==TURBO_LEANN_OK);
  owner.admitted=0;
  CHECK(SEARCH()==TURBO_LEANN_CORRUPT && found==0);
  CHECK(owner.admitted==0);
  map_blob[second_chunk_at+1]=1u;
  CHECK(fake_sha256(&owner,map_blob,map_len,m.node_map_sha256)==TURBO_LEANN_OK);

  /* Valid earlier vs later generation must require separate manifests. */
  read.generation=109;
  CHECK(SEARCH()==TURBO_LEANN_SCOPE_MISMATCH && found==0);
  read.generation=108;
  query.max_distance_evaluations=2;
  CHECK(SEARCH()==TURBO_LEANN_LIMIT && found==0);
  query.max_distance_evaluations=5;
  CHECK(SEARCH()==TURBO_LEANN_OK && found==2);

  /* Wrong sorted order, short buffer, stale map generation and output reset. */
  cfg.max_node_map_bytes=map_len-1;
  size_t required=0;
  CHECK(turbo_leann_node_map_encode(&cfg,e,3,map_blob,map_len,&required)==TURBO_LEANN_LIMIT);
  cfg.max_node_map_bytes=10000;
  turbo_leann_binding_entry_t tmp=e[1];
  e[1]=e[2]; e[2]=tmp;
  CHECK(turbo_leann_node_map_encode(&cfg,e,3,map_blob,map_len,&required)==TURBO_LEANN_INVALID);
  e[2]=e[1]; e[1]=tmp;
  CHECK(turbo_leann_node_map_encode(&cfg,e,3,map_blob,map_len,&map_len)==TURBO_LEANN_OK);
  CHECK(fake_sha256(&owner,map_blob,map_len,m.node_map_sha256)==TURBO_LEANN_OK);
  cfg.max_node_map_bytes=10000;
  CHECK(turbo_leann_search_pinned_exact(&m,&read,segment_blob,segment_len,
        map_blob,map_len,&limits,map_len-1,&query,hits,2,&found)
        ==TURBO_LEANN_LIMIT && found==0);

  /* Truncated and one-bit damaged map blobs, even when resigned by the test
   * digest, never leak a hit. The canonical key validator is host-owned. */
  for (size_t len=0; len<map_len; ++len) {
    CHECK(fake_sha256(&owner,map_blob,len ? len : 1,m.node_map_sha256)
          ==TURBO_LEANN_OK);
    owner.admitted=0;
    CHECK(turbo_leann_search_pinned_exact(&m,&read,segment_blob,segment_len,
          map_blob,len,&limits,10000,&query,hits,2,&found)!=TURBO_LEANN_OK);
    CHECK(found==0);
  }
  for (size_t i=0; i<map_len; ++i) {
    map_blob[i]^=1u;
    CHECK(fake_sha256(&owner,map_blob,map_len,m.node_map_sha256)
          ==TURBO_LEANN_OK);
    owner.admitted=0;
    CHECK(SEARCH()!=TURBO_LEANN_OK && found==0);
    map_blob[i]^=1u;
  }
  CHECK(fake_sha256(&owner,map_blob,map_len,m.node_map_sha256)
        ==TURBO_LEANN_OK);
  CHECK(SEARCH()==TURBO_LEANN_OK && found==2);

  free(map_blob);
  free(segment_blob);
  puts("turbo_leann L0.5 pinned binding: PASS");
  return 0;
}
