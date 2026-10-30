#include "turbo_agent_legacy_knowledge_batch.h"
#include "turbo_agent_legacy_provenance.h"
#include <stdlib.h>
#include <string.h>

typedef struct ta_owned_legacy_source_s {
  ta_legacy_migration_id_t source, revision, extraction;
  ta_source_revision_record_t record;
  unsigned char *document;
  size_t document_size;
  ta_revision_write_chunk_t *chunks;
  ta_revision_blob_t *chunk_metadata;
  size_t chunk_count;
} ta_owned_legacy_source_t;
typedef struct ta_owned_legacy_batch_s {
  unsigned char authority[TA_REVISION_ID_MAX_BYTES];
  unsigned char collection[TA_REVISION_ID_MAX_BYTES];
  size_t authority_size, collection_size;
  ta_revision_write_budget_t budget;
  uint64_t bytes;
  size_t total_chunks, count;
  ta_owned_legacy_source_t sources[TA_REVISION_GENERATION_MAX_SOURCES];
  ta_revision_generation_member_t members[TA_REVISION_GENERATION_MAX_SOURCES];
  ta_revision_write_source_t staged[TA_REVISION_GENERATION_MAX_SOURCES];
  ta_revision_write_legacy_source_t legacy[TA_REVISION_GENERATION_MAX_SOURCES];
} ta_owned_legacy_batch_t;
typedef struct ta_capture_ctx_s {
  ta_owned_legacy_batch_t *batch;
  const ta_legacy_migration_sqlite_limits_t *limits;
  ta_revision_scope_t scope;
  ta_legacy_batch_host_admit_fn admit;
  void *host;
  ta_legacy_batch_status_t error;
} ta_capture_ctx_t;

static int reserve_bytes(ta_owned_legacy_batch_t *b, size_t n) {
  if ((uint64_t)n > b->budget.max_payload_bytes - b->bytes) return 0;
  b->bytes += (uint64_t)n;
  return 1;
}
static unsigned char *copy_bytes(const unsigned char *p, size_t n) {
  unsigned char *v;
  if (!p && n) return NULL;
  v = (unsigned char *)malloc(n ? n : 1u);
  if (!v) return NULL;
  if (n) memcpy(v,p,n);
  else v[0]=0;
  return v;
}
static void release_owned(ta_owned_legacy_batch_t *b) {
  size_t i, j;
  if (!b) return;
  for (i=0;i<b->count;++i) {
    ta_owned_legacy_source_t *s=&b->sources[i];
    for (j=0;j<s->chunk_count;++j) {
      if (s->chunks) free((void *)s->chunks[j].payload.data);
      if (s->chunk_metadata) free((void *)s->chunk_metadata[j].data);
    }
    free(s->chunks); free(s->chunk_metadata); free(s->document);
  }
  free(b);
}
static int id_compare(ta_revision_id_t a, ta_revision_id_t b) {
  size_t n=a.size<b.size?a.size:b.size;
  int rc=memcmp(a.data,b.data,n);
  return rc?rc:(a.size>b.size)-(a.size<b.size);
}
static int source_compare(const void *a, const void *b) {
  const ta_owned_legacy_source_t *x=(const ta_owned_legacy_source_t *)a;
  const ta_owned_legacy_source_t *y=(const ta_owned_legacy_source_t *)b;
  return id_compare((ta_revision_id_t){x->source.data,x->source.size},
                    (ta_revision_id_t){y->source.data,y->source.size});
}
static int capture_one(void *context, const ta_legacy_migration_plan_t *p) {
  ta_capture_ctx_t *c=(ta_capture_ctx_t *)context;
  ta_owned_legacy_batch_t *b=c->batch;
  ta_owned_legacy_source_t *s;
  ta_legacy_provenance_document_payload_t d;
  size_t i, array_bytes;
  if (!c->admit(c->host,&c->scope,p)) {
    c->error=TA_LEGACY_BATCH_DENIED; return 1;
  }
  if (b->count>=c->limits->max_documents ||
      b->count>=TA_REVISION_GENERATION_MAX_SOURCES ||
      b->total_chunks>b->budget.max_chunks ||
      p->chunk_count>(size_t)(b->budget.max_chunks-b->total_chunks)) {
    c->error=TA_LEGACY_BATCH_LIMIT; return 1;
  }
  if (ta_legacy_provenance_document_encode(p->document,&d)!=TA_LEGACY_PROVENANCE_OK ||
      p->chunk_count>SIZE_MAX/(sizeof(ta_revision_write_chunk_t)+sizeof(ta_revision_blob_t))) {
    c->error=TA_LEGACY_BATCH_SOURCE_ERROR; return 1;
  }
  array_bytes=p->chunk_count*(sizeof(ta_revision_write_chunk_t)+sizeof(ta_revision_blob_t));
  if (!reserve_bytes(b,d.size) || !reserve_bytes(b,array_bytes)) {
    c->error=TA_LEGACY_BATCH_LIMIT; return 1;
  }
  s=&b->sources[b->count++];
  s->source=p->source_id; s->revision=p->revision_id;
  s->extraction=p->extraction_id; s->record=p->revision_record;
  s->document_size=d.size; s->chunk_count=p->chunk_count;
  s->document=copy_bytes(d.data,d.size);
  s->chunks=(ta_revision_write_chunk_t *)calloc(p->chunk_count?p->chunk_count:1u,sizeof(*s->chunks));
  s->chunk_metadata=(ta_revision_blob_t *)calloc(p->chunk_count?p->chunk_count:1u,sizeof(*s->chunk_metadata));
  if (!s->document || !s->chunks || !s->chunk_metadata) {
    c->error=TA_LEGACY_BATCH_NO_MEMORY; return 1;
  }
  for (i=0;i<p->chunk_count;++i) {
    ta_legacy_provenance_chunk_payload_t meta;
    const ta_legacy_migration_chunk_t *row=&p->chunks[i];
    unsigned char *text, *metadata;
    if (ta_legacy_provenance_chunk_encode(row,&meta)!=TA_LEGACY_PROVENANCE_OK) {
      c->error=TA_LEGACY_BATCH_SOURCE_ERROR; return 1;
    }
    if (!reserve_bytes(b,row->text.size) || !reserve_bytes(b,meta.size)) {
      c->error=TA_LEGACY_BATCH_LIMIT; return 1;
    }
    text=copy_bytes(row->text.data,row->text.size);
    if (!text) { c->error=TA_LEGACY_BATCH_NO_MEMORY; return 1; }
    s->chunks[i].payload=(ta_revision_blob_t){text,row->text.size};
    s->chunks[i].text=s->chunks[i].payload;
    metadata=copy_bytes(meta.data,meta.size);
    if (!metadata) { c->error=TA_LEGACY_BATCH_NO_MEMORY; return 1; }
    s->chunk_metadata[i]=(ta_revision_blob_t){metadata,meta.size};
  }
  b->total_chunks+=p->chunk_count;
  return 0;
}
ta_legacy_batch_status_t ta_legacy_knowledge_batch_capture(
    sqlite3 *db, const ta_legacy_migration_sqlite_limits_t *limits,
    const ta_revision_write_budget_t *budget, const ta_revision_scope_t *scope,
    ta_legacy_batch_host_admit_fn admit, void *host,
    ta_legacy_knowledge_batch_t *out) {
  ta_owned_legacy_batch_t *b;
  ta_capture_ctx_t c;
  ta_revision_key_t key;
  ta_legacy_migration_sqlite_status_t rc;
  uint32_t emitted=0;
  size_t i;
  if (!out || out->private_state || !db || !limits || !budget || !scope || !admit ||
      !budget->max_chunks || !budget->max_edges || !budget->max_payload_bytes ||
      limits->max_documents>TA_REVISION_GENERATION_MAX_SOURCES ||
      ta_revision_generation_key(scope,&key)!=TA_REVISION_KEY_OK)
    return TA_LEGACY_BATCH_INVALID;
  if (budget->max_payload_bytes<(uint64_t)sizeof(*b))
    return TA_LEGACY_BATCH_LIMIT;
  b=(ta_owned_legacy_batch_t *)calloc(1u,sizeof(*b));
  if (!b) return TA_LEGACY_BATCH_NO_MEMORY;
  b->budget=*budget; b->bytes=sizeof(*b);
  memcpy(b->authority,scope->authority.data,scope->authority.size);
  memcpy(b->collection,scope->collection.data,scope->collection.size);
  b->authority_size=scope->authority.size; b->collection_size=scope->collection.size;
  c=(ta_capture_ctx_t){b,limits,*scope,admit,host,TA_LEGACY_BATCH_OK};
  rc=ta_legacy_knowledge_sqlite_plan_all(db,limits,capture_one,&c,&emitted);
  if (rc!=TA_LEGACY_SQLITE_OK || emitted!=b->count) {
    ta_legacy_batch_status_t status=c.error;
    release_owned(b);
    if (status!=TA_LEGACY_BATCH_OK) return status;
    if (rc==TA_LEGACY_SQLITE_LIMIT) return TA_LEGACY_BATCH_LIMIT;
    if (rc==TA_LEGACY_SQLITE_INVALID) return TA_LEGACY_BATCH_INVALID;
    return TA_LEGACY_BATCH_SOURCE_ERROR;
  }
  qsort(b->sources,b->count,sizeof(b->sources[0]),source_compare);
  for (i=0;i<b->count;++i) {
    ta_owned_legacy_source_t *s=&b->sources[i];
    ta_revision_id_t source={s->source.data,s->source.size};
    ta_revision_id_t revision={s->revision.data,s->revision.size};
    ta_revision_id_t extraction={s->extraction.data,s->extraction.size};
    if (i && id_compare(b->members[i-1u].source,source)>=0) {
      release_owned(b); return TA_LEGACY_BATCH_SOURCE_ERROR;
    }
    b->members[i]=(ta_revision_generation_member_t){
      source,revision,extraction,(uint64_t)s->chunk_count};
    b->staged[i]=(ta_revision_write_source_t){
      source,revision,extraction,s->record,s->chunks,s->chunk_count,0};
    b->legacy[i]=(ta_revision_write_legacy_source_t){
      source,revision,{s->document,s->document_size},s->chunk_metadata,s->chunk_count};
  }
  out->private_state=b;
  return TA_LEGACY_BATCH_OK;
}
ta_legacy_batch_status_t ta_legacy_knowledge_batch_initial_request(
    const ta_legacy_knowledge_batch_t *batch, uint64_t generation_id,
    ta_revision_write_request_t *out) {
  ta_owned_legacy_batch_t *b;
  if (!out) return TA_LEGACY_BATCH_INVALID;
  memset(out,0,sizeof(*out));
  if (!batch || !batch->private_state || !generation_id)
    return TA_LEGACY_BATCH_INVALID;
  b=(ta_owned_legacy_batch_t *)batch->private_state;
  out->scope.authority=(ta_revision_id_t){b->authority,b->authority_size};
  out->scope.collection=(ta_revision_id_t){b->collection,b->collection_size};
  out->generation_id=generation_id;
  out->members=b->members; out->member_count=b->count;
  out->staged_sources=b->staged; out->staged_source_count=b->count;
  out->legacy_sources=b->legacy; out->legacy_source_count=b->count;
  out->budget=b->budget;
  return TA_LEGACY_BATCH_OK;
}
void ta_legacy_knowledge_batch_release(ta_legacy_knowledge_batch_t *batch) {
  if (!batch) return;
  release_owned((ta_owned_legacy_batch_t *)batch->private_state);
  batch->private_state=NULL;
}
