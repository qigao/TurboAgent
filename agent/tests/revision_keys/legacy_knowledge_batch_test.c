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

typedef struct saved_row_s {
  const char *ns;
  ta_revision_key_t key;
  unsigned char *value;
  size_t value_size;
} saved_row_t;
typedef struct saved_backend_s {
  saved_row_t values[16];
  size_t count;
  uint64_t committed_seq;
  unsigned get_calls,sequence_calls,releases;
  int return_limit,oversized_value,missing_release,drift;
} saved_backend_t;
static void saved_close(saved_backend_t *db) {
  size_t i;
  for (i=0;i<db->count;++i) free(db->values[i].value);
  memset(db,0,sizeof(*db));
}
static ta_revision_write_backend_status_t saved_put(
    void *owner,const char *ns,ta_revision_id_t key,
    ta_revision_blob_t value,uint64_t expected) {
  saved_backend_t *db=(saved_backend_t *)owner;
  saved_row_t *row;
  (void)expected;
  if (db->count>=16u || !key.data || !key.size ||
      key.size>sizeof(db->values[0].key.data) || !value.data)
    return TA_REVISION_WRITE_BACKEND_ERROR;
  row=&db->values[db->count];
  row->value=(unsigned char *)malloc(value.size?value.size:1u);
  if (!row->value) return TA_REVISION_WRITE_BACKEND_ERROR;
  if (value.size) memcpy(row->value,value.data,value.size);
  else row->value[0]=0;
  row->ns=ns;row->key.size=key.size;
  memcpy(row->key.data,key.data,key.size);
  row->value_size=value.size;
  ++db->count;
  return TA_REVISION_WRITE_BACKEND_OK;
}
static ta_revision_write_backend_status_t saved_project(
    void *owner,const char *ns,ta_revision_id_t key,ta_revision_blob_t text) {
  (void)owner;(void)ns;(void)key;(void)text;
  return TA_REVISION_WRITE_BACKEND_OK;
}
static ta_revision_write_backend_status_t saved_publish(
    void *owner,uint64_t *seq) {
  saved_backend_t *db=(saved_backend_t *)owner;
  db->committed_seq=77u;*seq=db->committed_seq;
  return TA_REVISION_WRITE_BACKEND_OK;
}
static void saved_abort(void *owner) {(void)owner;}
static const ta_revision_write_backend_ops_t saved_write_ops={
  saved_put,saved_project,saved_publish,saved_abort};
static void saved_release(void *owner) {
  ++((saved_backend_t *)owner)->releases;
}
static ta_revision_backend_status_t read_seq(void *owner,uint64_t *out) {
  saved_backend_t *db=(saved_backend_t *)owner;
  ++db->sequence_calls;
  *out=db->committed_seq+(db->drift && db->sequence_calls==2u?1u:0u);
  return db->committed_seq?TA_REVISION_BACKEND_OK:TA_REVISION_BACKEND_NOT_FOUND;
}
static ta_revision_backend_status_t saved_get(void *owner,const char *ns,
    ta_revision_id_t key,ta_revision_backend_value_t *out) {
  saved_backend_t *db=(saved_backend_t *)owner;
  size_t i;
  ++db->get_calls;
  if (db->return_limit) return TA_REVISION_BACKEND_LIMIT;
  for(i=0;i<db->count;++i) {
    saved_row_t *row=&db->values[i];
    if (!strcmp(ns,row->ns) && row->key.size==key.size &&
        !memcmp(key.data,row->key.data,key.size)) {
      out->bytes=(ta_revision_id_t){
        row->value,db->oversized_value?SIZE_MAX:row->value_size};
      out->owner=db;
      out->release=db->missing_release?NULL:saved_release;
      return TA_REVISION_BACKEND_OK;
    }
  }
  return TA_REVISION_BACKEND_NOT_FOUND;
}
static int check_readback(const ta_legacy_knowledge_batch_t *batch,
                          const ta_revision_write_request_t *req) {
  saved_backend_t saved={0};
  ta_revision_write_backend_t writer={&saved,&saved_write_ops};
  ta_legacy_readback_backend_t reader={&saved,read_seq,saved_get};
  ta_legacy_readback_budget_t budget={32u,UINT64_C(1)<<20};
  ta_legacy_readback_result_t proof={0};
  ta_revision_write_result_t receipt={0};
  unsigned before;
  CHECK(ta_legacy_knowledge_batch_verify_readback(
      batch,&reader,77u,req->generation_id,&budget,&proof)==
      TA_LEGACY_READBACK_STORAGE_ERROR);
  CHECK(!saved.get_calls && !proof.commit_seq);
  CHECK(ta_revision_write_publish_generation(&writer,req,&receipt)==TA_REVISION_WRITE_OK);
  CHECK(receipt.commit_seq==77u);
  CHECK(ta_legacy_knowledge_batch_verify_readback(
      batch,&reader,receipt.commit_seq,req->generation_id,&budget,&proof)==
      TA_LEGACY_READBACK_OK);
  CHECK(proof.commit_seq==77u && proof.generation_id==req->generation_id &&
        proof.documents==2u && proof.chunks==2u && proof.records==9u &&
        proof.checked_bytes>0u && saved.releases==9u);
  CHECK(ta_legacy_knowledge_batch_verify_readback(
      batch,&reader,76u,req->generation_id,&budget,&proof)==
      TA_LEGACY_READBACK_MISMATCH);
  CHECK(!proof.records);
  before=saved.get_calls;
  saved.values[1].ns="missing";
  CHECK(ta_legacy_knowledge_batch_verify_readback(
      batch,&reader,77u,req->generation_id,&budget,&proof)==
      TA_LEGACY_READBACK_NOT_FOUND);
  CHECK(saved.get_calls>before && !proof.records);
  saved.values[1].ns=TA_REVISION_NS_LEGACY_DOCUMENT;
  saved.values[1].value[0]^=1u;
  CHECK(ta_legacy_knowledge_batch_verify_readback(
      batch,&reader,77u,req->generation_id,&budget,&proof)==
      TA_LEGACY_READBACK_MISMATCH);
  CHECK(!proof.checked_bytes);
  saved.values[1].value[0]^=1u;
  budget.max_records=3u;
  before=saved.get_calls;
  CHECK(ta_legacy_knowledge_batch_verify_readback(
      batch,&reader,77u,req->generation_id,&budget,&proof)==
      TA_LEGACY_READBACK_LIMIT);
  CHECK(saved.get_calls==before);
  budget.max_records=32u;
  saved.return_limit=1;
  CHECK(ta_legacy_knowledge_batch_verify_readback(
      batch,&reader,77u,req->generation_id,&budget,&proof)==
      TA_LEGACY_READBACK_LIMIT);
  saved.return_limit=0;
  saved.oversized_value=1;
  CHECK(ta_legacy_knowledge_batch_verify_readback(
      batch,&reader,77u,req->generation_id,&budget,&proof)==
      TA_LEGACY_READBACK_LIMIT);
  CHECK(!proof.checked_bytes);
  saved.oversized_value=0;
  saved.missing_release=1;
  CHECK(ta_legacy_knowledge_batch_verify_readback(
      batch,&reader,77u,req->generation_id,&budget,&proof)==
      TA_LEGACY_READBACK_BACKEND_CONTRACT);
  saved.missing_release=0;
  saved.sequence_calls=0;
  saved.drift=1;
  CHECK(ta_legacy_knowledge_batch_verify_readback(
      batch,&reader,77u,req->generation_id,&budget,&proof)==
      TA_LEGACY_READBACK_MISMATCH);
  CHECK(saved.sequence_calls==2u && !proof.checked_bytes);
  saved_close(&saved);
  return 0;
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
    " VALUES('first','memory://old','note','Original',8,12,'abcd1234','{\"x\":1}',10),"
    " ('zero','memory://zero','note',NULL,0,0,'opaque',NULL,11);"
    "INSERT INTO chunks VALUES('first#0','first',0,'hello world','{\"i\":0}'),"
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
  CHECK(check_readback(&batch,&req)==0);
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
