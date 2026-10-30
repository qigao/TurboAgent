#include "turbo_leann_compact.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if(!(x)) { \
  fprintf(stderr,"CHECK FAILED %s:%d: %s\n",__FILE__,__LINE__,#x); \
  exit(1); } } while(0)
#define N 192u
#define D 8u

static uint32_t rng=UINT32_C(0x7d54ce01);
static uint32_t random_u32(void){
  rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;
}
static void data_fixture(turbo_leann_vector_t v[N],float raw[N][D]) {
  for(uint32_t i=0;i<N;i++) {
    memset(&v[i],0,sizeof(v[i]));
    v[i].chunk_label[0]=1u;
    v[i].chunk_label[14]=(uint8_t)(i>>8);
    v[i].chunk_label[15]=(uint8_t)i;
    v[i].revision_label[0]=2u;
    v[i].revision_label[15]=(uint8_t)i;
    for(uint32_t d=0;d<D;d++) {
      float f=(float)((int32_t)(random_u32()%2001u)-1000)/1000.0f;
      raw[i][d]=f+(d==i%8u?0.25f:0.0f);
    }
    v[i].components=raw[i];
  }
}
static turbo_leann_build_t build_cfg(void){
  turbo_leann_build_t x={0};x.struct_size=sizeof(x);x.abi_version=TURBO_LEANN_ABI_VERSION;
  x.dimensions=D;x.graph_degree=8;x.generation=109;
  x.model_fingerprint[0]=0x31;x.scope_fingerprint[0]=0x42;
  x.max_nodes=N;x.max_pairwise_evaluations=UINT64_C(40000);
  x.max_segment_bytes=UINT64_C(1000000);return x;
}
static turbo_leann_pq_config_t pq_cfg(void){
  turbo_leann_pq_config_t x={0};x.struct_size=sizeof(x);
  x.abi_version=TURBO_LEANN_ABI_VERSION;
  x.subquantizers=2;x.centroids=16;x.iterations=8;
  x.max_training_distance_evaluations=UINT64_C(1000000);
  x.max_compact_bytes=UINT64_C(1000000);
  x.max_peak_allocation_bytes=UINT64_C(1000000);return x;
}
static turbo_leann_limits_t limits(void){
  turbo_leann_limits_t x={0};x.struct_size=sizeof(x);
  x.abi_version=TURBO_LEANN_ABI_VERSION;
  x.max_nodes=N;x.max_dimensions=D;x.max_segment_bytes=UINT64_C(1000000);
  return x;
}
static turbo_leann_query_t query_cfg(turbo_leann_build_t cfg, const float *v){
  turbo_leann_query_t q={0};q.struct_size=sizeof(q);q.abi_version=TURBO_LEANN_ABI_VERSION;
  q.generation=cfg.generation;memcpy(q.model_fingerprint,cfg.model_fingerprint,32);
  memcpy(q.scope_fingerprint,cfg.scope_fingerprint,32);
  q.components=v;q.dimensions=cfg.dimensions;
  q.top_k=10;q.max_distance_evaluations=N;return q;
}
static uint32_t crc32_segment(const uint8_t *data,size_t n) {
  uint32_t crc=UINT32_MAX;
  for(size_t i=0;i<n;i++) {
    uint8_t b=(i>=168u && i<172u)?0:data[i];
    crc^=b;
    for(uint32_t j=0;j<8;j++)
      crc=(crc>>1)^((crc&1u)?UINT32_C(0xedb88320):0u);
  }
  return ~crc;
}
static void write32(uint8_t *p,uint32_t v){
  for(uint32_t j=0;j<4;j++)p[j]=(uint8_t)(v>>(8*j));
}
static uint64_t read64(const uint8_t *p) {
  uint64_t v=0;
  for(uint32_t j=0;j<8;j++)v|=(uint64_t)p[j]<<(8*j);
  return v;
}
int main(void){
  turbo_leann_vector_t v[N];float raw[N][D];data_fixture(v,raw);
  turbo_leann_build_t b=build_cfg();
  turbo_leann_segment_t *reference=NULL;
  CHECK(turbo_leann_build(&b,v,N,&reference)==TURBO_LEANN_OK);
  CHECK(reference!=NULL);
  size_t fp32_bytes=0;
  CHECK(turbo_leann_serialized_size(reference,&fp32_bytes)==TURBO_LEANN_OK);
  turbo_leann_pq_config_t cfg=pq_cfg();
  turbo_leann_compact_t *compact=NULL;
  CHECK(turbo_leann_pq_build(reference,&cfg,&compact)==TURBO_LEANN_OK);
  CHECK(compact!=NULL);
  size_t sz=0;
  CHECK(turbo_leann_pq_serialized_size(compact,&sz)==TURBO_LEANN_OK);
  CHECK(sz<fp32_bytes);
  uint8_t *buf=(uint8_t *)malloc(sz),*again=(uint8_t *)malloc(sz);
  CHECK(buf && again);
  size_t written=0;
  CHECK(turbo_leann_pq_serialize(compact,buf,sz,&written)==TURBO_LEANN_OK);
  CHECK(written==sz);
  CHECK(memcmp(buf,"TLC1PQ02",8)==0);
  CHECK(read64(buf+104)==sz);
  turbo_leann_limits_t l=limits();
  turbo_leann_compact_t *opened=NULL;
  CHECK(turbo_leann_pq_deserialize(buf,sz,&l,&opened)==TURBO_LEANN_OK);
  CHECK(opened!=NULL);
  CHECK(turbo_leann_pq_serialize(opened,again,sz,&written)==TURBO_LEANN_OK);
  CHECK(memcmp(buf,again,sz)==0);
  /* Deterministic training and canonical binary serialization. */
  turbo_leann_compact_t *retrained=NULL;
  CHECK(turbo_leann_pq_build(reference,&cfg,&retrained)==TURBO_LEANN_OK);
  CHECK(turbo_leann_pq_serialize(retrained,again,sz,&written)==TURBO_LEANN_OK);
  CHECK(memcmp(buf,again,sz)==0);
  turbo_leann_compact_destroy(retrained);

  turbo_leann_adc_hit_t adc[10];turbo_leann_hit_t exact[10];
  unsigned total_recall=0;
  for(unsigned trial=0;trial<24;trial++) {
    turbo_leann_query_t query=query_cfg(b,v[(trial*7u)%N].components);
    size_t n_adc=0,n_exact=0;
    CHECK(turbo_leann_search_adc(opened,&query,adc,10,&n_adc)==TURBO_LEANN_OK);
    CHECK(turbo_leann_search_exact(reference,&query,exact,10,&n_exact)==TURBO_LEANN_OK);
    CHECK(n_adc==10 && n_exact==10);
    for(size_t i=0;i<n_adc;i++) {
      CHECK(isfinite(adc[i].adc_squared_distance) && adc[i].adc_squared_distance>=0);
      if(i)CHECK(adc[i-1].adc_squared_distance <= adc[i].adc_squared_distance);
      for(size_t k=0;k<n_exact;k++)
        if(!memcmp(adc[i].chunk_label,exact[k].chunk_label,16)) {
          total_recall++;break;
        }
    }
  }
  printf("PQ bytes=%zu FP32 bytes=%zu saving=%.1f%% recall@10=%.3f\n",sz,fp32_bytes,
      100.0*(1.0-(double)sz/(double)fp32_bytes),(double)total_recall/(24*10));
  CHECK(total_recall >= 24u*5u); /* >= 0.5 fixed deterministic fixture */

  turbo_leann_query_t q=query_cfg(b,v[9].components);
  size_t count=99;
  q.max_distance_evaluations=9;
  CHECK(turbo_leann_search_adc(opened,&q,adc,10,&count)==TURBO_LEANN_LIMIT);
  CHECK(count==0);
  q.max_distance_evaluations=10;
  CHECK(turbo_leann_search_adc(opened,&q,adc,10,&count)==TURBO_LEANN_OK);
  CHECK(count==10);
  q.max_distance_evaluations=N;
  q.generation++;
  CHECK(turbo_leann_search_adc(opened,&q,adc,10,&count)==TURBO_LEANN_SCOPE_MISMATCH);
  CHECK(count==0);
  q.generation=b.generation;q.model_fingerprint[0]^=1;
  CHECK(turbo_leann_search_adc(opened,&q,adc,10,&count)==TURBO_LEANN_SCOPE_MISMATCH);
  CHECK(count==0);
  q.model_fingerprint[0]^=1;q.scope_fingerprint[0]^=1;
  CHECK(turbo_leann_search_adc(opened,&q,adc,10,&count)==TURBO_LEANN_SCOPE_MISMATCH);
  q.scope_fingerprint[0]^=1;
  float bad[D]={1,2,NAN,4,5,6,7,8};q.components=bad;
  CHECK(turbo_leann_search_adc(opened,&q,adc,10,&count)==TURBO_LEANN_INVALID);
  q.components=v[9].components;

  cfg.max_training_distance_evaluations=1;
  turbo_leann_compact_t *rejected=(turbo_leann_compact_t *)1;
  CHECK(turbo_leann_pq_build(reference,&cfg,&rejected)==TURBO_LEANN_LIMIT);
  CHECK(rejected==NULL);
  cfg=pq_cfg();cfg.max_peak_allocation_bytes=1;
  CHECK(turbo_leann_pq_build(reference,&cfg,&rejected)==TURBO_LEANN_LIMIT);
  cfg=pq_cfg();cfg.max_compact_bytes=1;
  CHECK(turbo_leann_pq_build(reference,&cfg,&rejected)==TURBO_LEANN_LIMIT);
  cfg=pq_cfg();cfg.subquantizers=3;
  CHECK(turbo_leann_pq_build(reference,&cfg,&rejected)==TURBO_LEANN_INVALID);
  cfg=pq_cfg();cfg.centroids=N+1;
  CHECK(turbo_leann_pq_build(reference,&cfg,&rejected)==TURBO_LEANN_INVALID);

  for(size_t n=0;n<sz;n++) {
    turbo_leann_compact_t *badseg=(turbo_leann_compact_t *)1;
    CHECK(turbo_leann_pq_deserialize(buf,n,&l,&badseg)!=TURBO_LEANN_OK);
    CHECK(badseg==NULL);
  }
  uint8_t saved=buf[sz-1];buf[sz-1]^=0x1u;
  CHECK(turbo_leann_pq_deserialize(buf,sz,&l,&rejected)==TURBO_LEANN_CORRUPT);
  CHECK(rejected==NULL);buf[sz-1]=saved;
  saved=buf[0];buf[0]^=1u;
  CHECK(turbo_leann_pq_deserialize(buf,sz,&l,&rejected)==TURBO_LEANN_CORRUPT);
  buf[0]=saved;
  /* A valid CRC is not enough: reject an out-of-range PQ code. */
  size_t codes_offset=(size_t)read64(buf+144);
  saved=buf[codes_offset];buf[codes_offset]=255u;
  write32(buf+168,crc32_segment(buf,sz));
  CHECK(turbo_leann_pq_deserialize(buf,sz,&l,&rejected)==TURBO_LEANN_CORRUPT);
  buf[codes_offset]=saved;write32(buf+168,crc32_segment(buf,sz));
  CHECK(turbo_leann_pq_deserialize(buf,sz,&l,&rejected)==TURBO_LEANN_OK);
  turbo_leann_compact_destroy(rejected);
  rejected=NULL;
  /* Regression: L0 remains byte-for-byte deterministic when PQ was trained. */
  turbo_leann_segment_t *reference2=NULL;
  CHECK(turbo_leann_build(&b,v,N,&reference2)==TURBO_LEANN_OK);
  size_t raw_size=0;
  CHECK(turbo_leann_serialized_size(reference2,&raw_size)==TURBO_LEANN_OK);
  CHECK(raw_size==fp32_bytes);
  uint8_t *a=(uint8_t *)malloc(raw_size),*c=(uint8_t *)malloc(raw_size);
  CHECK(a && c);
  CHECK(turbo_leann_serialize(reference,a,raw_size,&written)==TURBO_LEANN_OK);
  CHECK(turbo_leann_serialize(reference2,c,raw_size,&written)==TURBO_LEANN_OK);
  CHECK(memcmp(a,c,raw_size)==0);
  free(a);free(c);free(buf);free(again);
  turbo_leann_compact_destroy(compact);
  turbo_leann_compact_destroy(opened);
  turbo_leann_segment_destroy(reference);
  turbo_leann_segment_destroy(reference2);
  puts("LEANN-C11 L1a PQ/ADC PASS");
  return 0;
}
