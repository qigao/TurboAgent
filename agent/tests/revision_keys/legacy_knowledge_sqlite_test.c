#include "turbo_agent_legacy_knowledge_sqlite.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"legacy sqlite failed at %d: %s\n",__LINE__,#x);return 1;}}while(0)
typedef struct seen_s{unsigned count,empty,hex;}seen_t;
static int visit(void*ctx,const ta_legacy_migration_plan_t*p){seen_t*s=ctx;++s->count;if(!p->extracted_text_bytes)++s->empty;if(p->legacy_hash_shape==TA_LEGACY_HASH_HEX64_SHAPE_UNVERIFIED)++s->hex;CHECK(p->revision_record.digest_kind==TA_SOURCE_REVISION_DIGEST_UNVERIFIED_LEGACY);CHECK(p->revision_record.retention==TA_SOURCE_REVISION_CONTENT_UNAVAILABLE);return 0;}
static int exec(sqlite3*db,const char*sql){return sqlite3_exec(db,sql,NULL,NULL,NULL)==SQLITE_OK;}
int main(int argc,char**argv){const char*path=argc>1?argv[1]:"legacy-knowledge.db";sqlite3*db=NULL;sqlite3_stmt*st=NULL;sqlite3_int64 docs=0;ta_legacy_migration_sqlite_limits_t lim={8,8,65536};seen_t seen={0};uint32_t count=99;(void)remove(path);CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(exec(db,"PRAGMA foreign_keys=ON;CREATE TABLE documents(id TEXT PRIMARY KEY,uri TEXT NOT NULL UNIQUE,kind TEXT NOT NULL,title TEXT,mtime INTEGER NOT NULL DEFAULT 0,size INTEGER NOT NULL DEFAULT 0,content_hash TEXT NOT NULL,metadata_json TEXT,updated_at INTEGER NOT NULL DEFAULT (unixepoch()));CREATE TABLE chunks(id TEXT PRIMARY KEY,document_id TEXT NOT NULL,ordinal INTEGER NOT NULL,text TEXT NOT NULL,metadata_json TEXT,FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE);CREATE INDEX idx_chunks_document_id ON chunks(document_id);CREATE TABLE ingest_runs(id INTEGER PRIMARY KEY AUTOINCREMENT,uri TEXT NOT NULL,document_id TEXT,status TEXT NOT NULL,message TEXT,created_at INTEGER NOT NULL DEFAULT (unixepoch()));CREATE VIRTUAL TABLE chunks_fts USING fts5(chunk_id UNINDEXED,document_id UNINDEXED,uri UNINDEXED,text);"));CHECK(exec(db,"INSERT INTO documents(id,uri,kind,title,mtime,size,content_hash,updated_at) VALUES('/root/a.txt','/root/a.txt','file','a',5,11,'0123456789abcdef',9),('empty','memory://empty','note','e',0,0,'caller-hash',10);INSERT INTO chunks VALUES('/root/a.txt#0','/root/a.txt',0,'hello ',NULL),('/root/a.txt#1','/root/a.txt',1,'world',NULL),('empty#0','empty',0,'',NULL);INSERT INTO chunks_fts VALUES('wrong','/root/a.txt','/root/a.txt','STALE DERIVED ROW MUST BE IGNORED');"));CHECK(ta_legacy_knowledge_sqlite_plan_all(db,&lim,visit,&seen,&count)==TA_LEGACY_SQLITE_INVALID);CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;CHECK(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK);CHECK(ta_legacy_knowledge_sqlite_plan_all(db,&lim,visit,&seen,&count)==TA_LEGACY_SQLITE_OK);CHECK(count==2&&seen.count==2&&seen.empty==1&&seen.hex==1);

/* A late malformed document must be discovered in the validation pass before
 * the visitor sees ANY plan. */
CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
CHECK(sqlite3_open(path,&db)==SQLITE_OK);
CHECK(exec(db,"INSERT INTO documents(id,uri,kind,title,mtime,size,content_hash,updated_at) "
              "VALUES('latebad','memory://latebad','note','late',0,1,'bad',11);"
              "INSERT INTO chunks VALUES('latebad#1','latebad',1,'x',NULL);"));
CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
CHECK(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK);
memset(&seen,0,sizeof(seen));count=99;
CHECK(ta_legacy_knowledge_sqlite_plan_all(db,&lim,visit,&seen,&count)==TA_LEGACY_SQLITE_PLAN_ERROR);
CHECK(seen.count==0&&count==0);
CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
CHECK(sqlite3_open(path,&db)==SQLITE_OK);
CHECK(exec(db,"DELETE FROM chunks WHERE document_id='latebad';DELETE FROM documents WHERE id='latebad';"));

/* foreign_keys may be disabled on a legacy connection. Orphan authoritative
 * chunks must make the audit fail; they may not disappear from migration. */
CHECK(exec(db,"PRAGMA foreign_keys=OFF;"
              "INSERT INTO chunks(id,document_id,ordinal,text,metadata_json) "
              "VALUES('orphan#0','missing',0,'orphan',NULL);"));
CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
CHECK(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK);
memset(&seen,0,sizeof(seen));count=99;
CHECK(ta_legacy_knowledge_sqlite_plan_all(db,&lim,visit,&seen,&count)==TA_LEGACY_SQLITE_COVERAGE);
CHECK(seen.count==0&&count==0);
CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
CHECK(sqlite3_open(path,&db)==SQLITE_OK);
CHECK(exec(db,"DELETE FROM chunks WHERE document_id='missing';"));
CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
CHECK(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK);CHECK(sqlite3_prepare_v2(db,"SELECT count(*) FROM documents",-1,&st,NULL)==SQLITE_OK&&sqlite3_step(st)==SQLITE_ROW);docs=sqlite3_column_int64(st,0);sqlite3_finalize(st);st=NULL;CHECK(docs==2);lim.max_documents=1;count=99;memset(&seen,0,sizeof(seen));CHECK(ta_legacy_knowledge_sqlite_plan_all(db,&lim,visit,&seen,&count)==TA_LEGACY_SQLITE_LIMIT);CHECK(count==0);lim.max_documents=8;CHECK(exec(db,"BEGIN"));CHECK(ta_legacy_knowledge_sqlite_plan_all(db,&lim,visit,&seen,&count)==TA_LEGACY_SQLITE_BUSY);CHECK(exec(db,"ROLLBACK"));lim.max_documents=TA_LEGACY_SQLITE_MAX_DOCUMENTS+1u;CHECK(ta_legacy_knowledge_sqlite_plan_all(db,&lim,visit,&seen,&count)==TA_LEGACY_SQLITE_INVALID);lim.max_documents=8;CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;/* A missing document_id B-tree access path must fail admission. */
CHECK(sqlite3_open(path,&db)==SQLITE_OK);
CHECK(exec(db,"DROP INDEX idx_chunks_document_id;"));
CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
CHECK(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK);
memset(&seen,0,sizeof(seen));count=99;
CHECK(ta_legacy_knowledge_sqlite_plan_all(db,&lim,visit,&seen,&count)==TA_LEGACY_SQLITE_SCHEMA);
CHECK(seen.count==0&&count==0);
CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
CHECK(sqlite3_open(path,&db)==SQLITE_OK);
CHECK(exec(db,"CREATE INDEX idx_chunks_document_id ON chunks(document_id);"));
CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;

/* Forged same-name tables with identical column types/PK but no FK must
 * never be accepted as the authoritative legacy KnowledgeStore schema. */
{
  char fake_path[1024];
  CHECK(strlen(path)<sizeof(fake_path)-24u);
  (void)snprintf(fake_path,sizeof(fake_path),"%s-no-fk.db",path);
  (void)remove(fake_path);
  CHECK(sqlite3_open(fake_path,&db)==SQLITE_OK);
  CHECK(exec(db,"CREATE TABLE documents("
      "id TEXT PRIMARY KEY,uri TEXT NOT NULL UNIQUE,kind TEXT NOT NULL,"
      "title TEXT,mtime INTEGER NOT NULL DEFAULT 0,size INTEGER NOT NULL DEFAULT 0,"
      "content_hash TEXT NOT NULL,metadata_json TEXT,"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch()));"
      "CREATE TABLE chunks(id TEXT PRIMARY KEY,document_id TEXT NOT NULL,"
      "ordinal INTEGER NOT NULL,text TEXT NOT NULL,metadata_json TEXT);"
      "CREATE INDEX idx_chunks_document_id ON chunks(document_id);"));
  CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
  CHECK(sqlite3_open_v2(fake_path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK);
  memset(&seen,0,sizeof(seen));count=99;
  CHECK(ta_legacy_knowledge_sqlite_plan_all(db,&lim,visit,&seen,&count)==TA_LEGACY_SQLITE_SCHEMA);
  CHECK(seen.count==0&&count==0);
  CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;
  (void)remove(fake_path);
}
CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(exec(db,"ALTER TABLE documents ADD COLUMN injected TEXT"));CHECK(sqlite3_close(db)==SQLITE_OK);db=NULL;CHECK(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK);CHECK(ta_legacy_knowledge_sqlite_plan_all(db,&lim,visit,&seen,&count)==TA_LEGACY_SQLITE_SCHEMA);CHECK(sqlite3_close(db)==SQLITE_OK);(void)remove(path);puts("TurboAgent on-disk legacy KnowledgeStore audit PASS");return 0;}
