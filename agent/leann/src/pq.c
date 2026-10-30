#include "turbo_leann_compact.h"
#include "turbo_leann_internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PQ_HEADER 176u
static const uint8_t pq_magic[8] = {'T','L','C','1','P','Q','0','2'};

struct turbo_leann_compact_s {
  uint32_t dimensions, count, degree, edge_count, m, k, iterations;
  uint64_t generation;
  uint8_t model[32], scope[32];
  uint8_t *ids;       /* n x (chunk_label[16], revision_label[16]) */
  uint32_t *offsets;  /* (n+1) CSR offsets */
  uint32_t *edges;    /* CSR neighbor node ordinals */
  float *codebook;    /* m * k * (dim/m) normalized-space centroids */
  uint8_t *codes;     /* n * m byte codes; no FP32 vectors in compact */
};

static void pq_release(turbo_leann_compact_t *s) {
  if (!s) return;
  free(s->ids); free(s->offsets); free(s->edges);
  free(s->codebook); free(s->codes); free(s);
}
void turbo_leann_compact_destroy(turbo_leann_compact_t *s) { pq_release(s); }

static void put32(uint8_t *p, uint32_t v) {
  for (uint32_t i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8));
}
static void put64(uint8_t *p, uint64_t v) {
  for (uint32_t i=0;i<8;i++) p[i]=(uint8_t)(v>>(i*8));
}
static uint32_t get32(const uint8_t *p) {
  uint32_t v=0;
  for (uint32_t i=0;i<4;i++) v|=(uint32_t)p[i]<<(i*8);
  return v;
}
static uint64_t get64(const uint8_t *p) {
  uint64_t v=0;
  for (uint32_t i=0;i<8;i++) v|=(uint64_t)p[i]<<(i*8);
  return v;
}
static uint32_t crc32_blob(const uint8_t *data, size_t n) {
  uint32_t crc=UINT32_MAX;
  for(size_t i=0;i<n;i++) {
    uint8_t b=(i>=168u && i<172u)?0:data[i];
    crc^=b;
    for(uint32_t t=0;t<8;t++)
      crc=(crc>>1) ^ ((crc&1u)?UINT32_C(0xedb88320):0u);
  }
  return ~crc;
}

/* Fixed header plus 32-byte labels, CSR offsets/edges, FP32 codebooks,
 * and ONE byte per (node, subquantizer). Each step is checked. */
static int layout(const turbo_leann_compact_t *s, size_t *rows,
                  size_t *edges, size_t *centers, size_t *codes,
                  size_t *total) {
  size_t ids_bytes, row_bytes, edge_bytes, center_cells, center_bytes, code_bytes;
  if (!tl_checked_mul(s->count,32u,&ids_bytes) ||
      !tl_checked_mul((size_t)s->count+1u,4u,&row_bytes) ||
      !tl_checked_mul(s->edge_count,4u,&edge_bytes) ||
      !tl_checked_mul(s->dimensions,s->k,&center_cells) ||
      !tl_checked_mul(center_cells,4u,&center_bytes) ||
      !tl_checked_mul(s->count,s->m,&code_bytes) ||
      !tl_checked_add(PQ_HEADER,ids_bytes,rows) ||
      !tl_checked_add(*rows,row_bytes,edges) ||
      !tl_checked_add(*edges,edge_bytes,centers) ||
      !tl_checked_add(*centers,center_bytes,codes) ||
      !tl_checked_add(*codes,code_bytes,total)) return 0;
  return 1;
}

/* All PQ centroids are derived from the normalized L0 data. This is ordinary
 * squared L2 in each subspace; the searcher's ADC score is NOT cosine. */
static double sq_l2(const float *a, const float *b, uint32_t dim) {
  double sum=0.0;
  for (uint32_t j=0;j<dim;j++) {
    double d=(double)a[j]-(double)b[j]; sum+=d*d;
  }
  return sum;
}
static uint32_t nearest_center(const float *v, const float *centers,
                               uint32_t k, uint32_t len) {
  uint32_t best=0;
  double score=sq_l2(v,centers,len);
  for(uint32_t i=1;i<k;i++) {
    double candidate=sq_l2(v,centers+(size_t)i*len,len);
    if(candidate<score){score=candidate;best=i;}
  }
  return best;
}

/* Exact predeclared memory budget applies to all resident allocations owned
 * by compact build, including scratch and the struct itself. */
static int check_build_memory(const turbo_leann_segment_t *src,
                              const turbo_leann_pq_config_t *cfg,
                              size_t total_blob) {
  size_t bytes=sizeof(turbo_leann_compact_t), val;
  if (!tl_checked_mul(src->count,32u,&val) || !tl_checked_add(bytes,val,&bytes) ||
      !tl_checked_mul((size_t)src->count+1u,4u,&val) || !tl_checked_add(bytes,val,&bytes) ||
      !tl_checked_mul(src->edge_count,4u,&val) || !tl_checked_add(bytes,val,&bytes) ||
      !tl_checked_mul(src->dimensions,cfg->centroids,&val) ||
      !tl_checked_mul(val,4u,&val) || !tl_checked_add(bytes,val,&bytes) ||
      !tl_checked_mul(src->count,cfg->subquantizers,&val) || !tl_checked_add(bytes,val,&bytes) ||
      !tl_checked_mul(src->dimensions,cfg->centroids,&val) ||
      !tl_checked_mul(val,sizeof(double),&val) || !tl_checked_add(bytes,val,&bytes) ||
      !tl_checked_mul(cfg->centroids,sizeof(uint32_t),&val) || !tl_checked_add(bytes,val,&bytes))
    return 0;
  return (uint64_t)bytes<=cfg->max_peak_allocation_bytes &&
         (uint64_t)total_blob<=cfg->max_compact_bytes;
}

turbo_leann_status_t turbo_leann_pq_build(
    const turbo_leann_segment_t *src, const turbo_leann_pq_config_t *cfg,
    turbo_leann_compact_t **out) {
  if (!out) return TURBO_LEANN_INVALID;
  *out=NULL;
  if(!src || !cfg || cfg->struct_size!=sizeof(*cfg) ||
     cfg->abi_version!=TURBO_LEANN_ABI_VERSION ||
     cfg->subquantizers<1 || cfg->subquantizers>128 ||
     src->dimensions%cfg->subquantizers ||
     cfg->centroids<2 || cfg->centroids>256 || cfg->centroids>src->count ||
     cfg->iterations<1 || cfg->iterations>64 ||
     !cfg->max_training_distance_evaluations ||
     !cfg->max_compact_bytes || !cfg->max_peak_allocation_bytes ||
     !src->count || !src->vectors || !src->ids || !src->offsets ||
     (src->edge_count && !src->edges) || src->dimensions>4096 ||
     !src->generation) return TURBO_LEANN_INVALID;
  if(sizeof(float)!=4 || FLT_RADIX!=2 || FLT_MANT_DIG!=24)
    return TURBO_LEANN_VERSION_UNSUPPORTED;
  /* Count one centroid distance evaluation per training assignment (includes
   * final code assignment). Require multiplication to fit in u64. */
  uint64_t trials=src->count;
  if(trials>UINT64_MAX/cfg->centroids) return TURBO_LEANN_LIMIT;
  trials*=cfg->centroids;
  if(trials>UINT64_MAX/cfg->subquantizers) return TURBO_LEANN_LIMIT;
  trials*=cfg->subquantizers;
  if(trials>UINT64_MAX/(cfg->iterations+1u)) return TURBO_LEANN_LIMIT;
  trials*=cfg->iterations+1u;
  if(trials>cfg->max_training_distance_evaluations) return TURBO_LEANN_LIMIT;

  turbo_leann_compact_t s0={0};
  s0.dimensions=src->dimensions;s0.count=src->count;s0.degree=src->graph_degree;
  s0.edge_count=src->edge_count;s0.m=cfg->subquantizers;
  s0.k=cfg->centroids;s0.iterations=cfg->iterations;
  size_t ro,ed,ce,co,total;
  if(!layout(&s0,&ro,&ed,&ce,&co,&total) ||
     !check_build_memory(src,cfg,total)) return TURBO_LEANN_LIMIT;

  turbo_leann_compact_t *s=(turbo_leann_compact_t *)calloc(1,sizeof(*s));
  if(!s) return TURBO_LEANN_NO_MEMORY;
  *s=s0; s->generation=src->generation;
  memcpy(s->model,src->model,32);memcpy(s->scope,src->scope,32);
  const size_t centers=(size_t)s->dimensions*s->k;
  s->ids=(uint8_t *)malloc((size_t)s->count*32u);
  s->offsets=(uint32_t *)malloc(((size_t)s->count+1u)*4u);
  s->edges=s->edge_count?(uint32_t *)malloc((size_t)s->edge_count*4u):NULL;
  s->codebook=(float *)malloc(centers*4u);
  s->codes=(uint8_t *)malloc((size_t)s->count*s->m);
  double *sums=(double *)calloc(centers,sizeof(double));
  uint32_t *counts=(uint32_t *)calloc(s->k,sizeof(uint32_t));
  if(!s->ids || !s->offsets || (s->edge_count && !s->edges) ||
     !s->codebook || !s->codes || !sums || !counts) {
    free(sums); free(counts); pq_release(s); return TURBO_LEANN_NO_MEMORY;
  }
  memcpy(s->ids,src->ids,(size_t)s->count*32u);
  memcpy(s->offsets,src->offsets,((size_t)s->count+1u)*4u);
  if(s->edge_count)memcpy(s->edges,src->edges,(size_t)s->edge_count*4u);

  uint32_t width=s->dimensions/s->m;
  for(uint32_t sub=0;sub<s->m;sub++) {
    float *book=s->codebook+(size_t)sub*s->k*width;
    for(uint32_t c=0;c<s->k;c++) {
      size_t seed=((size_t)c*s->count)/s->k;
      memcpy(book+(size_t)c*width,
             src->vectors+seed*s->dimensions+(size_t)sub*width,
             (size_t)width*4u);
    }
    for(uint32_t pass=0;pass<s->iterations;pass++) {
      memset(sums,0,(size_t)s->k*width*sizeof(double));
      memset(counts,0,(size_t)s->k*sizeof(uint32_t));
      for(uint32_t i=0;i<s->count;i++) {
        const float *v=src->vectors+(size_t)i*s->dimensions+(size_t)sub*width;
        uint32_t chosen=nearest_center(v,book,s->k,width);
        counts[chosen]++;
        for(uint32_t j=0;j<width;j++)
          sums[(size_t)chosen*width+j]+=v[j];
      }
      for(uint32_t c=0;c<s->k;c++) if(counts[c])
        for(uint32_t j=0;j<width;j++)
          book[(size_t)c*width+j]=(float)(sums[(size_t)c*width+j]/counts[c]);
    }
    for(uint32_t i=0;i<s->count;i++) {
      const float *v=src->vectors+(size_t)i*s->dimensions+(size_t)sub*width;
      s->codes[(size_t)i*s->m+sub]=(uint8_t)nearest_center(v,book,s->k,width);
    }
  }
  free(sums);free(counts);
  *out=s;
  return TURBO_LEANN_OK;
}

turbo_leann_status_t turbo_leann_pq_serialized_size(
    const turbo_leann_compact_t *s, size_t *out_size) {
  if(!out_size) return TURBO_LEANN_INVALID;
  *out_size=0;
  if(!s) return TURBO_LEANN_INVALID;
  size_t ro,ed,ce,co,total;
  if(!layout(s,&ro,&ed,&ce,&co,&total)) return TURBO_LEANN_LIMIT;
  *out_size=total;return TURBO_LEANN_OK;
}

turbo_leann_status_t turbo_leann_pq_serialize(
    const turbo_leann_compact_t *s, uint8_t *dst,size_t cap,size_t *out_size) {
  if(!out_size) return TURBO_LEANN_INVALID;
  *out_size=0;
  if(!s) return TURBO_LEANN_INVALID;
  size_t ro,ed,ce,co,total;
  if(!layout(s,&ro,&ed,&ce,&co,&total))return TURBO_LEANN_LIMIT;
  *out_size=total;
  if(!dst || cap<total) return TURBO_LEANN_LIMIT;
  memset(dst,0,PQ_HEADER);
  memcpy(dst,pq_magic,8);put32(dst+8,TURBO_LEANN_PQ_FORMAT_VERSION);
  put32(dst+12,PQ_HEADER);put32(dst+16,s->dimensions);put32(dst+20,s->count);
  put32(dst+24,s->degree);put32(dst+28,s->edge_count);
  put64(dst+32,s->generation);memcpy(dst+40,s->model,32);
  memcpy(dst+72,s->scope,32);put64(dst+104,(uint64_t)total);
  put64(dst+112,PQ_HEADER);put64(dst+120,(uint64_t)ro);
  put64(dst+128,(uint64_t)ed);put64(dst+136,(uint64_t)ce);
  put64(dst+144,(uint64_t)co);
  put32(dst+152,s->m);put32(dst+156,s->k);put32(dst+160,s->iterations);
  memcpy(dst+PQ_HEADER,s->ids,(size_t)s->count*32u);
  for(uint32_t i=0;i<=s->count;i++)put32(dst+ro+(size_t)i*4u,s->offsets[i]);
  for(uint32_t i=0;i<s->edge_count;i++)put32(dst+ed+(size_t)i*4u,s->edges[i]);
  size_t nc=(size_t)s->dimensions*s->k;
  for(size_t i=0;i<nc;i++) {
    uint32_t bits;memcpy(&bits,&s->codebook[i],4);
    put32(dst+ce+i*4u,bits);
  }
  memcpy(dst+co,s->codes,(size_t)s->count*s->m);
  put32(dst+168,crc32_blob(dst,total));
  return TURBO_LEANN_OK;
}

turbo_leann_status_t turbo_leann_pq_deserialize(
    const uint8_t *data,size_t size,const turbo_leann_limits_t *limits,
    turbo_leann_compact_t **out) {
  if(!out) return TURBO_LEANN_INVALID;
  *out=NULL;
  if(!data || !limits || limits->struct_size!=sizeof(*limits) ||
     limits->abi_version!=TURBO_LEANN_ABI_VERSION) return TURBO_LEANN_INVALID;
  if(size<PQ_HEADER || size>limits->max_segment_bytes)return TURBO_LEANN_LIMIT;
  if(memcmp(data,pq_magic,8))return TURBO_LEANN_CORRUPT;
  if(get32(data+8)!=TURBO_LEANN_PQ_FORMAT_VERSION)
    return TURBO_LEANN_VERSION_UNSUPPORTED;
  if(get32(data+12)!=PQ_HEADER || get32(data+164)!=0 || get32(data+172)!=0)
    return TURBO_LEANN_CORRUPT;
  turbo_leann_compact_t proto={0};
  proto.dimensions=get32(data+16);proto.count=get32(data+20);
  proto.degree=get32(data+24);proto.edge_count=get32(data+28);
  proto.generation=get64(data+32);
  proto.m=get32(data+152);proto.k=get32(data+156);
  proto.iterations=get32(data+160);
  if(!proto.count || proto.count>65535u || proto.count>limits->max_nodes ||
     !proto.dimensions || proto.dimensions>4096u ||
     proto.dimensions>limits->max_dimensions ||
     !proto.m || proto.m>128u || proto.dimensions%proto.m ||
     proto.k<2 || proto.k>256 || proto.k>proto.count ||
     !proto.iterations || proto.iterations>64 ||
     proto.degree>64 || proto.degree>proto.count-1 ||
     (proto.count>2 && proto.degree<2) ||
     (uint64_t)proto.edge_count>(uint64_t)proto.count*proto.degree ||
     !proto.generation || !tl_valid_fingerprint(data+40) ||
     !tl_valid_fingerprint(data+72))return TURBO_LEANN_CORRUPT;
  if(sizeof(float)!=4 || FLT_RADIX!=2 || FLT_MANT_DIG!=24)
    return TURBO_LEANN_VERSION_UNSUPPORTED;
  size_t ro,ed,ce,co,total;
  if(!layout(&proto,&ro,&ed,&ce,&co,&total) || total!=size ||
     get64(data+104)!=(uint64_t)size || get64(data+112)!=PQ_HEADER ||
     get64(data+120)!=(uint64_t)ro || get64(data+128)!=(uint64_t)ed ||
     get64(data+136)!=(uint64_t)ce || get64(data+144)!=(uint64_t)co ||
     get32(data+168)!=crc32_blob(data,size))return TURBO_LEANN_CORRUPT;
  turbo_leann_compact_t *s=(turbo_leann_compact_t *)calloc(1,sizeof(*s));
  if(!s)return TURBO_LEANN_NO_MEMORY;
  *s=proto;memcpy(s->model,data+40,32);memcpy(s->scope,data+72,32);
  s->ids=(uint8_t *)malloc((size_t)s->count*32u);
  s->offsets=(uint32_t *)malloc(((size_t)s->count+1u)*4u);
  s->edges=s->edge_count?(uint32_t *)malloc((size_t)s->edge_count*4u):NULL;
  s->codebook=(float *)malloc((size_t)s->dimensions*s->k*4u);
  s->codes=(uint8_t *)malloc((size_t)s->count*s->m);
  if(!s->ids || !s->offsets || (s->edge_count && !s->edges) ||
     !s->codebook || !s->codes) {pq_release(s);return TURBO_LEANN_NO_MEMORY;}
  memcpy(s->ids,data+PQ_HEADER,(size_t)s->count*32u);
  for(uint32_t i=1;i<s->count;i++)
    if(memcmp(s->ids+(size_t)(i-1)*32u,s->ids+(size_t)i*32u,16)>=0) {
      pq_release(s);return TURBO_LEANN_CORRUPT;
    }
  for(uint32_t i=0;i<=s->count;i++)s->offsets[i]=get32(data+ro+(size_t)i*4u);
  if(s->offsets[0] || s->offsets[s->count]!=s->edge_count) {
    pq_release(s);return TURBO_LEANN_CORRUPT;
  }
  for(uint32_t i=0;i<s->edge_count;i++)s->edges[i]=get32(data+ed+(size_t)i*4u);
  for(uint32_t i=0;i<s->count;i++) {
    uint32_t begin=s->offsets[i],end=s->offsets[i+1];
    if(end<begin || end>s->edge_count || end-begin>s->degree) {
      pq_release(s);return TURBO_LEANN_CORRUPT;
    }
    for(uint32_t j=begin;j<end;j++)
      if(s->edges[j]>=s->count || s->edges[j]==i ||
         (j>begin && s->edges[j]<=s->edges[j-1])) {
        pq_release(s);return TURBO_LEANN_CORRUPT;
      }
  }
  for(size_t i=0;i<(size_t)s->dimensions*s->k;i++) {
    uint32_t bits=get32(data+ce+i*4u);
    memcpy(&s->codebook[i],&bits,4);
    if(!isfinite(s->codebook[i])) {pq_release(s);return TURBO_LEANN_CORRUPT;}
  }
  memcpy(s->codes,data+co,(size_t)s->count*s->m);
  for(size_t i=0;i<(size_t)s->count*s->m;i++)
    if(s->codes[i]>=s->k) {pq_release(s);return TURBO_LEANN_CORRUPT;}
  *out=s;return TURBO_LEANN_OK;
}

typedef struct pq_candidate_s { uint32_t node; double distance; } pq_candidate_t;
static int before(pq_candidate_t a,pq_candidate_t b) {
  return a.distance<b.distance || (a.distance==b.distance && a.node<b.node);
}
static void heap_push(pq_candidate_t *heap,size_t *size,pq_candidate_t item) {
  size_t i=(*size)++;
  while(i) {
    size_t parent=(i-1u)/2u;
    if(!before(item,heap[parent]))break;
    heap[i]=heap[parent];i=parent;
  }
  heap[i]=item;
}
static pq_candidate_t heap_pop(pq_candidate_t *heap,size_t *size) {
  pq_candidate_t first=heap[0],last=heap[--(*size)];
  if(*size) {
    size_t i=0;
    for(;;) {
      size_t child=2u*i+1u;
      if(child>=*size)break;
      if(child+1u<*size && before(heap[child+1u],heap[child]))child++;
      if(!before(heap[child],last))break;
      heap[i]=heap[child];i=child;
    }
    heap[i]=last;
  }
  return first;
}
static double adc_distance(const turbo_leann_compact_t *s,uint32_t node,
                           const double *table) {
  double sum=0.0;
  for(uint32_t sub=0;sub<s->m;sub++)
    sum+=table[(size_t)sub*s->k+s->codes[(size_t)node*s->m+sub]];
  return sum;
}
static void best_insert(pq_candidate_t item,pq_candidate_t *best,
                        size_t *used,size_t cap) {
  size_t pos=0;
  while(pos<*used && !before(item,best[pos]))pos++;
  if(pos>=cap)return;
  if(*used<cap)(*used)++;
  for(size_t i=*used-1u;i>pos;i--)best[i]=best[i-1u];
  best[pos]=item;
}

turbo_leann_status_t turbo_leann_search_adc(
    const turbo_leann_compact_t *s,const turbo_leann_query_t *q,
    turbo_leann_adc_hit_t *out,size_t capacity,size_t *out_count) {
  if(!out_count)return TURBO_LEANN_INVALID;
  *out_count=0;
  if(!s || !q || !out || q->struct_size!=sizeof(*q) ||
     q->abi_version!=TURBO_LEANN_ABI_VERSION || !q->components ||
     q->dimensions!=s->dimensions || !q->top_k || q->top_k>capacity ||
     q->top_k>1024u)return TURBO_LEANN_INVALID;
  if(q->generation!=s->generation ||
     memcmp(q->model_fingerprint,s->model,32) ||
     memcmp(q->scope_fingerprint,s->scope,32))return TURBO_LEANN_SCOPE_MISMATCH;
  if(q->max_distance_evaluations<q->top_k)return TURBO_LEANN_LIMIT;
  double norm=0.0;
  if(!tl_valid_vector(q->components,q->dimensions,&norm))
    return TURBO_LEANN_INVALID;
  uint32_t budget=q->max_distance_evaluations;
  if(budget>s->count)budget=s->count;
  size_t table_cells=(size_t)s->m*s->k;
  double *table=(double *)malloc(table_cells*sizeof(double));
  pq_candidate_t *heap=(pq_candidate_t *)malloc((size_t)s->count*sizeof(*heap));
  pq_candidate_t *best=(pq_candidate_t *)malloc((size_t)q->top_k*sizeof(*best));
  uint8_t *visited=(uint8_t *)calloc(s->count,1);
  if(!table || !heap || !best || !visited) {
    free(table);free(heap);free(best);free(visited);return TURBO_LEANN_NO_MEMORY;
  }
  uint32_t width=s->dimensions/s->m;
  for(uint32_t sub=0;sub<s->m;sub++)
    for(uint32_t c=0;c<s->k;c++) {
      const float *center=s->codebook+((size_t)sub*s->k+c)*width;
      double d=0.0;
      for(uint32_t j=0;j<width;j++) {
        double delta=(double)q->components[(size_t)sub*width+j]/norm-center[j];
        d+=delta*delta;
      }
      table[(size_t)sub*s->k+c]=d;
    }
  size_t queue_size=0,best_count=0;
  uint32_t evaluations=0;
  /* Deterministic fixed entry points, then best-first CSR exploration. */
  const uint32_t seeds[3]={0,s->count/2u,s->count-1u};
  for(uint32_t j=0;j<3 && evaluations<budget;j++) {
    uint32_t seed=seeds[j];
    if(visited[seed])continue;
    visited[seed]=1;
    double d=adc_distance(s,seed,table);
    pq_candidate_t item={seed,d};
    heap_push(heap,&queue_size,item);best_insert(item,best,&best_count,q->top_k);
    evaluations++;
  }
  while(queue_size && evaluations<budget) {
    pq_candidate_t node=heap_pop(heap,&queue_size);
    for(uint32_t e=s->offsets[node.node];e<s->offsets[node.node+1u] &&
        evaluations<budget;e++) {
      uint32_t next=s->edges[e];
      if(visited[next])continue;
      visited[next]=1;
      pq_candidate_t item={next,adc_distance(s,next,table)};
      heap_push(heap,&queue_size,item);best_insert(item,best,&best_count,q->top_k);
      evaluations++;
    }
  }
  for(size_t i=0;i<best_count;i++) {
    const uint8_t *id=s->ids+(size_t)best[i].node*32u;
    memcpy(out[i].chunk_label,id,16u);
    memcpy(out[i].revision_label,id+16u,16u);
    out[i].adc_squared_distance=(float)best[i].distance;
  }
  *out_count=best_count;
  free(table);free(heap);free(best);free(visited);
  return TURBO_LEANN_OK;
}
