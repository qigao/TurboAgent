#include "turbo_agent_legacy_provenance.h"
#include "turbo_agent_revision_write.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "legacy provenance failed at %d: %s\n", __LINE__, #x); return 1; } } while (0)
static ta_legacy_migration_text_t t(const char *s) {
  ta_legacy_migration_text_t x = {(const unsigned char *)s, strlen(s)};
  return x;
}
static ta_revision_id_t id(const ta_legacy_migration_id_t *v) {
  ta_revision_id_t x = {v->data, v->size};
  return x;
}
static ta_revision_id_t text(const char *s) {
  ta_revision_id_t x = {(const unsigned char *)s, strlen(s)};
  return x;
}
static ta_revision_blob_t blob(const char *s) {
  ta_revision_blob_t x = {(const unsigned char *)s, strlen(s)};
  return x;
}
typedef struct writer_s {
  const char *names[12];
  size_t count, fail_at;
  unsigned publishes, aborts;
} writer_t;
static ta_revision_write_backend_status_t put(
    void *owner, const char *ns, ta_revision_id_t key, ta_revision_blob_t val,
    uint64_t expected) {
  writer_t *w = owner;
  (void)expected;
  if (!key.data || !key.size || !val.data || !val.size || w->count >= 12u)
    return TA_REVISION_WRITE_BACKEND_ERROR;
  if (w->fail_at && w->count + 1u == w->fail_at)
    return TA_REVISION_WRITE_BACKEND_BUSY;
  w->names[w->count++] = ns;
  return TA_REVISION_WRITE_BACKEND_OK;
}
static ta_revision_write_backend_status_t project(
    void *owner, const char *ns, ta_revision_id_t key, ta_revision_blob_t val) {
  writer_t *w = owner;
  if (!key.data || !key.size || !val.data || !val.size || w->count >= 12u)
    return TA_REVISION_WRITE_BACKEND_ERROR;
  if (w->fail_at && w->count + 1u == w->fail_at)
    return TA_REVISION_WRITE_BACKEND_BUSY;
  w->names[w->count++] = ns;
  return TA_REVISION_WRITE_BACKEND_OK;
}
static ta_revision_write_backend_status_t publish(void *owner, uint64_t *seq) {
  writer_t *w = owner;
  ++w->publishes;
  *seq = 200u;
  return TA_REVISION_WRITE_BACKEND_OK;
}
static void abort_write(void *owner) { ++((writer_t *)owner)->aborts; }
static const ta_revision_write_backend_ops_t ops = {
  put, project, publish, abort_write
};
static ta_revision_write_backend_t backend(writer_t *w) {
  ta_revision_write_backend_t b = {w, &ops};
  return b;
}
static int roundtrip_and_publish(void) {
  ta_legacy_migration_document_t d = {0}, d2 = {0};
  ta_legacy_migration_chunk_t c = {0}, c2 = {0};
  ta_legacy_migration_plan_t p;
  ta_legacy_provenance_document_payload_t dp;
  ta_legacy_provenance_chunk_payload_t cp;
  ta_revision_write_chunk_t chunk;
  ta_revision_write_source_t source;
  ta_revision_generation_member_t member;
  ta_revision_write_legacy_source_t legacy;
  ta_revision_write_request_t request;
  ta_revision_write_result_t result;
  ta_revision_blob_t chunk_provenance;
  writer_t w = {0};
  ta_revision_write_backend_t b;
  static const unsigned char empty[] = "";
  d.id = t("legacy/a"); d.uri = t("memory://a?q=1");
  d.kind = t("note"); d.title = t("title A");
  d.mtime = 23; d.size = 37; d.updated_at = 91;
  d.content_hash = t("0123456789abcdef");
  d.metadata_json = t("{\"key\":\"a/b\",\"nested\":{\"v\":1}}");
  c.id = t("legacy/a#0"); c.document_id = d.id;
  c.ordinal = 0; c.text = t("original extracted text");
  c.metadata_json = t("{\"offset\":0,\"start\":1}");
  CHECK(ta_legacy_knowledge_plan_document(&d, &c, 1, &p) ==
        TA_LEGACY_MIGRATION_OK);
  CHECK(ta_legacy_provenance_document_encode(&d, &dp) == TA_LEGACY_PROVENANCE_OK);
  CHECK(ta_legacy_provenance_chunk_encode(&c, &cp) == TA_LEGACY_PROVENANCE_OK);
  CHECK(ta_legacy_provenance_document_decode(dp.data, dp.size, &d2) ==
        TA_LEGACY_PROVENANCE_OK);
  CHECK(ta_legacy_provenance_chunk_decode(cp.data, cp.size, &c2) ==
        TA_LEGACY_PROVENANCE_OK);
  CHECK(d2.uri.size == d.uri.size && !memcmp(d2.uri.data,d.uri.data,d.uri.size));
  CHECK(d2.kind.size == d.kind.size && !memcmp(d2.kind.data,d.kind.data,d.kind.size));
  CHECK(d2.title.size == d.title.size && !memcmp(d2.title.data,d.title.data,d.title.size));
  CHECK(d2.metadata_json.size == d.metadata_json.size &&
        !memcmp(d2.metadata_json.data,d.metadata_json.data,d.metadata_json.size));
  CHECK(d2.mtime == d.mtime && d2.size == d.size &&
        d2.updated_at == d.updated_at);
  CHECK(c2.metadata_json.size == c.metadata_json.size &&
        !memcmp(c2.metadata_json.data,c.metadata_json.data,c.metadata_json.size));
  CHECK(c2.ordinal == 0 && c2.text.data == NULL && c2.text.size == 0);
  /* Decode views borrow dp.data; never encode back into their own buffer. */
  d.title = (ta_legacy_migration_text_t){NULL,0};
  d.metadata_json = (ta_legacy_migration_text_t){NULL,0};
  CHECK(ta_legacy_provenance_document_encode(&d,&dp)==TA_LEGACY_PROVENANCE_OK);
  CHECK(ta_legacy_provenance_document_decode(dp.data,dp.size,&d2)==TA_LEGACY_PROVENANCE_OK);
  CHECK(d2.title.data == NULL && d2.metadata_json.data == NULL);
  d.title = (ta_legacy_migration_text_t){empty,0};
  d.metadata_json = (ta_legacy_migration_text_t){empty,0};
  CHECK(ta_legacy_provenance_document_encode(&d,&dp)==TA_LEGACY_PROVENANCE_OK);
  CHECK(ta_legacy_provenance_document_decode(dp.data,dp.size,&d2)==TA_LEGACY_PROVENANCE_OK);
  CHECK(d2.title.data && !d2.title.size && d2.metadata_json.data && !d2.metadata_json.size);
  d.title = t("title A"); d.metadata_json = t("{\"key\":\"a/b\",\"nested\":{\"v\":1}}");
  CHECK(ta_legacy_provenance_document_encode(&d, &dp) == TA_LEGACY_PROVENANCE_OK);
  chunk = (ta_revision_write_chunk_t){
    {c.text.data, c.text.size}, {c.text.data, c.text.size}};
  source = (ta_revision_write_source_t){
    id(&p.source_id), id(&p.revision_id), id(&p.extraction_id),
    p.revision_record, &chunk, 1, 0};
  member = (ta_revision_generation_member_t){
    source.source, source.revision, source.extraction, 1};
  chunk_provenance = (ta_revision_blob_t){cp.data, cp.size};
  legacy = (ta_revision_write_legacy_source_t){
    source.source, source.revision, {dp.data, dp.size}, &chunk_provenance, 1};
  memset(&request, 0, sizeof(request));
  request.scope = (ta_revision_scope_t){text("host-scope"),text("notes")};
  request.generation_id = 1;
  request.members = &member; request.member_count = 1;
  request.staged_sources = &source; request.staged_source_count = 1;
  request.legacy_sources = &legacy; request.legacy_source_count = 1;
  request.budget = (ta_revision_write_budget_t){4,4,UINT64_C(1)<<20};
  b = backend(&w);
  CHECK(ta_revision_write_publish_generation(&b,&request,&result)==TA_REVISION_WRITE_OK);
  CHECK(w.publishes==1 && w.aborts==0 && w.count==6 && result.commit_seq==200);
  CHECK(!strcmp(w.names[0],TA_REVISION_NS_SOURCE_REVISION));
  CHECK(!strcmp(w.names[1],TA_REVISION_NS_LEGACY_DOCUMENT));
  CHECK(!strcmp(w.names[2],TA_REVISION_NS_CHUNK));
  CHECK(!strcmp(w.names[3],TA_REVISION_NS_CHUNK));
  CHECK(!strcmp(w.names[4],TA_REVISION_NS_LEGACY_CHUNK));
  CHECK(!strcmp(w.names[5],TA_REVISION_NS_GENERATION));
  /* No legacy provenance may be dropped, accepted as a separate write or
   * silently published after a backend staging failure. */
  request.legacy_source_count=0; request.legacy_sources=NULL;
  memset(&w,0,sizeof(w)); b=backend(&w);
  CHECK(ta_revision_write_publish_generation(&b,&request,&result)==TA_REVISION_WRITE_INVALID);
  CHECK(w.count==0&&w.publishes==0&&w.aborts==1);
  request.legacy_source_count=1;request.legacy_sources=&legacy;
  memset(&w,0,sizeof(w));w.fail_at=4;b=backend(&w);
  CHECK(ta_revision_write_publish_generation(&b,&request,&result)==TA_REVISION_WRITE_BUSY);
  CHECK(w.count==3&&w.publishes==0&&w.aborts==1);
  /* A mismatched legacy hash cannot be attached to a different SourceRevision. */
  d.content_hash=t("ffffffffffffffff");
  CHECK(ta_legacy_provenance_document_encode(&d,&dp)==TA_LEGACY_PROVENANCE_OK);
  legacy.document=(ta_revision_blob_t){dp.data,dp.size};
  memset(&w,0,sizeof(w));b=backend(&w);
  CHECK(ta_revision_write_publish_generation(&b,&request,&result)==TA_REVISION_WRITE_INVALID);
  CHECK(w.count==0&&w.aborts==1);
  d.content_hash=t("0123456789abcdef");
  CHECK(ta_legacy_provenance_document_encode(&d,&dp)==TA_LEGACY_PROVENANCE_OK);
  /* A different chunk payload cannot be falsely indexed as legacy text. */
  chunk.text=blob("different indexed content");
  memset(&w,0,sizeof(w));b=backend(&w);
  CHECK(ta_revision_write_publish_generation(&b,&request,&result)==TA_REVISION_WRITE_INVALID);
  CHECK(w.count==0&&w.aborts==1);
  chunk.text=chunk.payload;
  cp.data[3]=2; /* invalid payload version */
  memset(&w,0,sizeof(w));b=backend(&w);
  CHECK(ta_revision_write_publish_generation(&b,&request,&result)==TA_REVISION_WRITE_INVALID);
  CHECK(w.count==0&&w.aborts==1);
  cp.data[3]=1;
  /* Legacy data can never be relabeled retained/verified in the writer. */
  source.revision_record.retention=TA_SOURCE_REVISION_CONTENT_RETAINED_VERIFIED;
  memset(&w,0,sizeof(w));b=backend(&w);
  CHECK(ta_revision_write_publish_generation(&b,&request,&result)==TA_REVISION_WRITE_INVALID);
  CHECK(w.count==0&&w.aborts==1);
  return 0;
}
static int malformed_and_large_chunk(void) {
  ta_legacy_migration_document_t d={0}, d2={0};
  ta_legacy_migration_chunk_t c={0};
  ta_legacy_migration_plan_t plan;
  ta_legacy_provenance_document_payload_t p;
  static unsigned char large_text[5000];
  d.id=t("a");d.uri=t("a://uri");d.kind=t("file");
  d.content_hash=t("16-hex-text-hash");d.size=5000;
  c.id=t("a#0");c.document_id=d.id;c.ordinal=0;
  memset(large_text,'x',sizeof(large_text));
  c.text=(ta_legacy_migration_text_t){large_text,sizeof(large_text)};
  CHECK(ta_legacy_knowledge_plan_document(&d,&c,1,&plan)==TA_LEGACY_MIGRATION_OK);
  CHECK(plan.extracted_text_bytes==sizeof(large_text));
  CHECK(ta_legacy_provenance_document_encode(&d,&p)==TA_LEGACY_PROVENANCE_OK);
  CHECK(ta_legacy_provenance_document_decode(p.data,p.size-1u,&d2)==TA_LEGACY_PROVENANCE_MALFORMED);
  CHECK(d2.id.data==NULL);
  p.data[3]=9;
  CHECK(ta_legacy_provenance_document_decode(p.data,p.size,&d2)==TA_LEGACY_PROVENANCE_MALFORMED);
  CHECK(d2.uri.data==NULL);
  d.id=(ta_legacy_migration_text_t){large_text,sizeof(large_text)};
  CHECK(ta_legacy_provenance_document_encode(&d,&p)==TA_LEGACY_PROVENANCE_INVALID);
  CHECK(p.size==0);
  return 0;
}
int main(void) {
  if (roundtrip_and_publish() || malformed_and_large_chunk()) return 1;
  puts("TurboAgent legacy provenance write qualification PASS");
  return 0;
}
