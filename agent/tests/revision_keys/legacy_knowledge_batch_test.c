#include "turbo_agent_legacy_knowledge_batch.h"
#include "turbo_agent_legacy_provenance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"legacy batch failed at %d: %s\n",__LINE__,#x);return 1;}}while(0)
typedef struct fake_s {
  unsigned ops, publishes, aborts, documents, chunks, texts, generations, empty;
  unsigned fail_at;
  int saw_original_uri, saw_original_metadata;
} fake_t;
static ta_revision_id_t txt(const char *s) {
  return (ta_revision_id_t){(const unsigned char *)s,strlen(s)};
}
static int allow(void *ctx,const ta_revision_scope_t *scope,
                 const ta_legacy_migration_plan_t *p) {
  int *denied=(int *)ctx;
  if (!scope || !p->document || !p->document->id.data) return 0;
  if (*denied && p->document->id.size==4u &&
      !memcmp(p->document->id.data,"zero",4u)) return 0;
  return 1;
}
static ta_revision_write_backend_status_t put(void *owner,const char *ns,
    ta_revision_id_t key,ta_revision_blob_t val,uint64_t seq) {
  fake_t *f=(fake_t *)owner;
  (void)seq;
  if (!key.data || !key.size || !val.data) return TA_REVISION_WRITE_BACKEND_ERROR;
  if (f->fail_at && f->ops+1u==f->fail_at) return TA_REVISION_WRITE_BACKEND_BUSY;
  ++f->ops;
  if (!strcmp(ns,TA_REVISION_NS_LEGACY_DOCUMENT)) {
    ta_legacy_migration_document_t row;
    if (ta_legacy_provenance_document_decode(val.data,val.size,&row)!=TA_LEGACY_PROVENANCE_OK)
      return TA_REVISION_WRITE_BACKEND_ERROR;
    ++f->documents;
    if (row.id.size==5u && !memcmp(row.id.data,"first",5u)) {
      f->saw_original_uri=(row.uri.size==12u &&
          !memcmp(row.uri.data,"memory://old",12u));
      f->saw_original_metadata=(row.metadata_json.size==7u &&
          !memcmp(row.metadata_json.data,"{\"x\":1}",7u));
    }
  } else if (!strcmp(ns,TA_REVISION_NS_LEGACY_CHUNK)) {
    ta_legacy_migration_chunk_t row;
    if (ta_legacy_provenance_chunk_decode(val.data,val.size,&row)!=TA_LEGACY_PROVENANCE_OK)
      return TA_REVISION_WRITE_BACKEND_ERROR;
    ++f->chunks;
  } else if (!strcmp(ns,TA_REVISION_NS_GENERATION)) {
    ++f->generations;
  } else if (!strcmp(ns,TA_REVISION_NS_CHUNK) && !val.size) ++f->empty;
  return TA_REVISION_WRITE_BACKEND_OK;
}
static ta_revision_write_backend_status_t project(void *owner,const char *ns,
    ta_revision_id_t key,ta_revision_blob_t value) {
  fake_t *f=(fake_t *)owner;
  if (!ns || !key.data || !value.data || !value.size)
    return TA_REVISION_WRITE_BACKEND_ERROR;
  if (f->fail_at && f->ops+1u==f->fail_at) return TA_REVISION_WRITE_BACKEND_BUSY;
  ++f->ops; ++f->texts;
  return TA_REVISION_WRITE_BACKEND_OK;
}
static ta_revision_write_backend_status_t publish(void *owner,uint64_t *seq) {
  fake_t *f=(fake_t *)owner;
  ++f->publishes; *seq=99u; return TA_REVISION_WRITE_BACKEND_OK;
}
static void abort_owner(void *owner) { ++((fake_t *)owner)->aborts; }
static const ta_revision_write_backend_ops_t ops={
  put,project,publish,abort_owner};
static ta_revision_write_backend_t backend(fake_t *f) {
  return (ta_revision_write_backend_t){f,&ops};
}
static int sql(sqlite3 *db,const char *text) {
  return sqlite3_exec(db,text,NULL,NULL,NULL)==SQLITE_OK;
}
int main(int argc,char **argv) {
  const char *path=argc>1?argv[1]:"legacy-batch.db";
  sqlite3 *db=NULL;
  ta_legacy_migration_sqlite_limits_t limits={8,8,65536};
  ta_revision_write_budget_t budget={8,1,UINT64_C(1)<<20};
  ta_revision_scope_t scope={txt("host-issued-scope"),txt("knowledge")};
  ta_legacy_knowledge_batch_t batch={0};
  ta_revision_write_request_t req;
  ta_revision_write_result_t result={0};
  fake_t f={0};
  ta_revision_write_backend_t be;
  int deny=0;
  (void)remove(path);
  CHECK(sqlite3_open(path,&db)==SQLITE_OK);
  CHECK(sql(db,
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE documents(id TEXT PRIMARY KEY,uri TEXT NOT NULL UNIQUE,"
    "kind TEXT NOT NULL,title TEXT,mtime INTEGER NOT NULL DEFAULT 0,"
    "size INTEGER NOT NULL DEFAULT 0,content_hash TEXT NOT NULL,"
    "metadata_json TEXT,updated_at INTEGER NOT NULL DEFAULT (unixepoch()));"
    "CREATE TABLE chunks(id TEXT PRIMARY KEY,document_id TEXT NOT NULL,"
    "ordinal INTEGER NOT NULL,text TEXT NOT NULL,metadata_json TEXT,"
    "FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE);"
    "CREATE INDEX idx_chunks_document_id ON chunks(document_id);"
    "INSERT INTO documents(id,uri,kind,title,mtime,size,content_hash,metadata_json,updated_at)"
    " VALUES('first','memory://old','note','Original',8,12,'abcd1234','{""x"":1}',10),"
    " ('zero','memory://zero','note',NULL,0,0,'opaque',NULL,11);"
    "INSERT INTO chunks VALUES('first#0','first',0,'hello world','{""i"":0}'),"
    "('zero#0','zero',0,'',NULL);"));
  CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
  CHECK(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK);
  /* Host denial MUST NOT silently drop a source from a partial migration. */
  deny=1;
  CHECK(ta_legacy_knowledge_batch_capture(db,&limits,&budget,&scope,allow,&deny,&batch)==
        TA_LEGACY_BATCH_DENIED);
  CHECK(batch.private_state==NULL);
  deny=0;
  budget.max_payload_bytes=1u;
  CHECK(ta_legacy_knowledge_batch_capture(db,&limits,&budget,&scope,allow,&deny,&batch)==
        TA_LEGACY_BATCH_LIMIT);
  budget.max_payload_bytes=UINT64_C(1)<<20;
  CHECK(ta_legacy_knowledge_batch_capture(db,&limits,&budget,&scope,allow,&deny,&batch)==
        TA_LEGACY_BATCH_OK);
  CHECK(batch.private_state!=NULL);
  CHECK(ta_legacy_knowledge_batch_initial_request(&batch,1u,&req)==TA_LEGACY_BATCH_OK);
  CHECK(req.member_count==2u && req.staged_source_count==2u &&
        req.legacy_source_count==2u && req.expected_generation_head_seq==0u);
  CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
  /* Captured bytes must no longer borrow sqlite3_column_text buffers. */
  CHECK(sqlite3_open(path,&db)==SQLITE_OK);
  CHECK(sql(db,"UPDATE documents SET uri='memory://new',metadata_json='changed' "
               "WHERE id='first'; UPDATE chunks SET text='changed' WHERE id='first#0';"));
  CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
  be=backend(&f);
  CHECK(ta_revision_write_publish_generation(&be,&req,&result)==TA_REVISION_WRITE_OK);
  CHECK(result.commit_seq==99u && f.aborts==0u && f.publishes==1u);
  CHECK(f.ops==10u && f.documents==2u && f.chunks==2u &&
        f.texts==1u && f.generations==1u && f.empty==1u);
  CHECK(f.saw_original_uri && f.saw_original_metadata);
  /* A staged backend failure must never reach publish. */
  memset(&f,0,sizeof(f)); f.fail_at=3u; be=backend(&f);
  CHECK(ta_revision_write_publish_generation(&be,&req,&result)==TA_REVISION_WRITE_BUSY);
  CHECK(f.publishes==0u && f.aborts==1u);
  ta_legacy_knowledge_batch_release(&batch);
  CHECK(batch.private_state==NULL);
  CHECK(ta_legacy_knowledge_batch_initial_request(&batch,1u,&req)==TA_LEGACY_BATCH_INVALID);
  (void)remove(path);
  puts("TurboAgent complete owned legacy snapshot / write contract PASS");
  return 0;
}
