#include "turbo_agent_legacy_knowledge_sqlite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct owned_text_s { unsigned char *data; size_t size; } owned_text_t;
typedef struct owned_document_s { ta_legacy_migration_document_t row; owned_text_t fields[6]; } owned_document_t;
typedef struct owned_chunk_s { ta_legacy_migration_chunk_t row; owned_text_t fields[4]; } owned_chunk_t;
typedef struct column_spec_s { const char *name; const char *type; int pk, notnull; } column_spec_t;
/* SQLite rowid tables permit NULL in TEXT PRIMARY KEY without explicit
 * NOT NULL: preserve the exact legacy DDL rather than a SQL assumption. */
static const column_spec_t document_columns[]={
  {"id","TEXT",1,0},{"uri","TEXT",0,1},{"kind","TEXT",0,1},
  {"title","TEXT",0,0},{"mtime","INTEGER",0,1},{"size","INTEGER",0,1},
  {"content_hash","TEXT",0,1},{"metadata_json","TEXT",0,0},
  {"updated_at","INTEGER",0,1}
};
static const column_spec_t chunk_columns[]={
  {"id","TEXT",1,0},{"document_id","TEXT",0,1},
  {"ordinal","INTEGER",0,1},{"text","TEXT",0,1},
  {"metadata_json","TEXT",0,0}
};
static void free_text(owned_text_t*t){if(t&&t->data)free(t->data);if(t)memset(t,0,sizeof(*t));}
static void free_document(owned_document_t*d){size_t i;if(!d)return;for(i=0;i<6u;++i)free_text(&d->fields[i]);memset(d,0,sizeof(*d));}
static void free_chunks(owned_chunk_t*c,size_t n){size_t i,j;if(!c)return;for(i=0;i<n;++i)for(j=0;j<4u;++j)free_text(&c[i].fields[j]);free(c);}
static int schema_matches(sqlite3*db,const char*table,const column_spec_t*e,size_t count){char sql[96];sqlite3_stmt*st=NULL;size_t i=0;int rc;if(snprintf(sql,sizeof(sql),"PRAGMA table_info(%s)",table)<0)return 0;if(sqlite3_prepare_v2(db,sql,-1,&st,NULL)!=SQLITE_OK)return 0;while((rc=sqlite3_step(st))==SQLITE_ROW){const char*n=(const char*)sqlite3_column_text(st,1);const char*t=(const char*)sqlite3_column_text(st,2);int pk=sqlite3_column_int(st,5);int notnull=sqlite3_column_int(st,3);
  if(i>=count||!n||!t||strcmp(n,e[i].name)||strcmp(t,e[i].type)||
     pk!=e[i].pk||notnull!=e[i].notnull){sqlite3_finalize(st);return 0;}++i;}sqlite3_finalize(st);if(rc!=SQLITE_DONE||i!=count)return 0;if(sqlite3_prepare_v2(db,"SELECT count(*) FROM sqlite_master WHERE type='trigger' AND tbl_name IN ('documents','chunks')",-1,&st,NULL)!=SQLITE_OK)return 0;rc=sqlite3_step(st);int ok=rc==SQLITE_ROW&&sqlite3_column_int64(st,0)==0;sqlite3_finalize(st);return ok;}
static int count_exactly_one(sqlite3 *db,const char *sql){
  sqlite3_stmt *st=NULL;int rc,ok=0;
  if(sqlite3_prepare_v2(db,sql,-1,&st,NULL)!=SQLITE_OK)return 0;
  rc=sqlite3_step(st);
  if(rc==SQLITE_ROW&&sqlite3_column_int64(st,0)==1)ok=1;
  if(sqlite3_step(st)!=SQLITE_DONE)ok=0;
  if(sqlite3_finalize(st)!=SQLITE_OK)ok=0;
  return ok;
}

/* Reject lookalike tables without the legacy URI unique index, chunk lookup
 * B-tree or actual foreign-key cascade; foreign_keys=ON is not proof of DDL. */
static int schema_constraints(sqlite3 *db){
  return count_exactly_one(db,
      "SELECT count(*) FROM pragma_index_list('documents') AS l "
      "JOIN pragma_index_info(l.name) AS i "
      "WHERE l.\"unique\"=1 AND l.partial=0 AND i.name='uri' "
      "AND (SELECT count(*) FROM pragma_index_info(l.name))=1") &&
    count_exactly_one(db,
      "SELECT count(*) FROM pragma_index_list('chunks') AS l "
      "JOIN pragma_index_info(l.name) AS i "
      "WHERE l.partial=0 AND i.seqno=0 AND i.name='document_id'") &&
    count_exactly_one(db,
      "SELECT count(*) FROM pragma_foreign_key_list('chunks')") &&
    count_exactly_one(db,
      "SELECT count(*) FROM pragma_foreign_key_list('chunks') "
      "WHERE \"table\"='documents' AND \"from\"='document_id' "
      "AND \"to\"='id' AND on_update='NO ACTION' AND on_delete='CASCADE'");
}

static int copy_column(sqlite3_stmt*st,int col,int optional,uint64_t*bytes,uint64_t max,owned_text_t*owned,ta_legacy_migration_text_t*view){int type=sqlite3_column_type(st,col);const void*p;int n;memset(owned,0,sizeof(*owned));*view=(ta_legacy_migration_text_t){NULL,0};if(type==SQLITE_NULL)return optional;if(type!=SQLITE_TEXT)return 0;p=sqlite3_column_text(st,col);n=sqlite3_column_bytes(st,col);if(n<0||(!p&&n)||((uint64_t)n>max-*bytes))return 0;owned->data=(unsigned char*)malloc((size_t)n+1u);if(!owned->data)return 0;if(n)memcpy(owned->data,p,(size_t)n);owned->data[n]=0;owned->size=(size_t)n;*bytes+=(uint64_t)n;*view=(ta_legacy_migration_text_t){owned->data,owned->size};return 1;}
static int load_document(sqlite3_stmt*st,uint64_t*bytes,uint64_t max,owned_document_t*d){memset(d,0,sizeof(*d));if(!copy_column(st,0,0,bytes,max,&d->fields[0],&d->row.id)||!copy_column(st,1,0,bytes,max,&d->fields[1],&d->row.uri)||!copy_column(st,2,0,bytes,max,&d->fields[2],&d->row.kind)||!copy_column(st,3,1,bytes,max,&d->fields[3],&d->row.title)||sqlite3_column_type(st,4)!=SQLITE_INTEGER||sqlite3_column_type(st,5)!=SQLITE_INTEGER||!copy_column(st,6,0,bytes,max,&d->fields[4],&d->row.content_hash)||!copy_column(st,7,1,bytes,max,&d->fields[5],&d->row.metadata_json)||sqlite3_column_type(st,8)!=SQLITE_INTEGER){free_document(d);return 0;}d->row.mtime=sqlite3_column_int64(st,4);d->row.size=sqlite3_column_int64(st,5);d->row.updated_at=sqlite3_column_int64(st,8);return 1;}
static int load_chunks(sqlite3*db,const owned_document_t*d,const ta_legacy_migration_sqlite_limits_t*l,uint64_t*bytes,owned_chunk_t**out,size_t*out_count){sqlite3_stmt*st=NULL;owned_chunk_t*rows=NULL;size_t count=0;int rc;*out=NULL;*out_count=0;rows=(owned_chunk_t*)calloc(l->max_chunks_per_document,sizeof(*rows));if(!rows)return 0;if(sqlite3_prepare_v2(db,"SELECT id,document_id,ordinal,text,metadata_json FROM chunks WHERE document_id=?1 ORDER BY ordinal ASC",-1,&st,NULL)!=SQLITE_OK){free(rows);return 0;}if(sqlite3_bind_text(st,1,(const char*)d->row.id.data,(int)d->row.id.size,SQLITE_TRANSIENT)!=SQLITE_OK){sqlite3_finalize(st);free(rows);return 0;}while((rc=sqlite3_step(st))==SQLITE_ROW){owned_chunk_t*c;if(count>=l->max_chunks_per_document){sqlite3_finalize(st);free_chunks(rows,count);return -1;}c=&rows[count];if(!copy_column(st,0,0,bytes,l->max_copied_bytes_per_document,&c->fields[0],&c->row.id)||!copy_column(st,1,0,bytes,l->max_copied_bytes_per_document,&c->fields[1],&c->row.document_id)||sqlite3_column_type(st,2)!=SQLITE_INTEGER||!copy_column(st,3,0,bytes,l->max_copied_bytes_per_document,&c->fields[2],&c->row.text)||!copy_column(st,4,1,bytes,l->max_copied_bytes_per_document,&c->fields[3],&c->row.metadata_json)){sqlite3_finalize(st);free_chunks(rows,count+1u);return 0;}c->row.ordinal=sqlite3_column_int64(st,2);++count;}sqlite3_finalize(st);if(rc!=SQLITE_DONE){free_chunks(rows,count);return 0;}*out=rows;*out_count=count;return 1;}
static ta_legacy_migration_sqlite_status_t verify_chunk_coverage(sqlite3 *db){
  sqlite3_stmt *st=NULL;
  int rc;
  if(sqlite3_prepare_v2(db,
      "SELECT 1 FROM chunks AS c LEFT JOIN documents AS d ON d.id=c.document_id "
      "WHERE d.id IS NULL LIMIT 1",-1,&st,NULL)!=SQLITE_OK)
    return TA_LEGACY_SQLITE_STORAGE_ERROR;
  rc=sqlite3_step(st);
  sqlite3_finalize(st);
  if(rc==SQLITE_ROW)return TA_LEGACY_SQLITE_COVERAGE;
  return rc==SQLITE_DONE?TA_LEGACY_SQLITE_OK:TA_LEGACY_SQLITE_STORAGE_ERROR;
}

static ta_legacy_migration_sqlite_status_t scan_documents(
    sqlite3 *db,const ta_legacy_migration_sqlite_limits_t*l,
    ta_legacy_migration_plan_visitor_fn visitor,void*context,int emit,
    uint32_t*out_count){
  sqlite3_stmt*docs=NULL;
  uint32_t count=0;
  int rc;
  ta_legacy_migration_sqlite_status_t status=TA_LEGACY_SQLITE_OK;
  if(out_count)*out_count=0;
  if(sqlite3_prepare_v2(db,
      "SELECT id,uri,kind,title,mtime,size,content_hash,metadata_json,updated_at "
      "FROM documents ORDER BY id COLLATE BINARY",-1,&docs,NULL)!=SQLITE_OK)
    return TA_LEGACY_SQLITE_STORAGE_ERROR;
  while((rc=sqlite3_step(docs))==SQLITE_ROW){
    owned_document_t d;
    owned_chunk_t*chunks=NULL;
    ta_legacy_migration_chunk_t*views=NULL;
    size_t chunk_count=0,k;
    uint64_t copied=0;
    ta_legacy_migration_plan_t plan;
    ta_legacy_migration_status_t prc;
    int lrc;
    if(count>=l->max_documents){status=TA_LEGACY_SQLITE_LIMIT;break;}
    if(!load_document(docs,&copied,l->max_copied_bytes_per_document,&d)){
      status=TA_LEGACY_SQLITE_LIMIT;break;
    }
    lrc=load_chunks(db,&d,l,&copied,&chunks,&chunk_count);
    if(lrc<0){free_document(&d);status=TA_LEGACY_SQLITE_LIMIT;break;}
    if(!lrc){free_document(&d);status=TA_LEGACY_SQLITE_STORAGE_ERROR;break;}
    if(chunk_count){
      views=(ta_legacy_migration_chunk_t*)calloc(chunk_count,sizeof(*views));
      if(!views){
        free_chunks(chunks,chunk_count);free_document(&d);
        status=TA_LEGACY_SQLITE_STORAGE_ERROR;break;
      }
      for(k=0;k<chunk_count;++k)views[k]=chunks[k].row;
    }
    prc=ta_legacy_knowledge_plan_document(&d.row,views,chunk_count,&plan);
    if(prc!=TA_LEGACY_MIGRATION_OK){
      free(views);free_chunks(chunks,chunk_count);free_document(&d);
      status=prc==TA_LEGACY_MIGRATION_LIMIT?
          TA_LEGACY_SQLITE_LIMIT:TA_LEGACY_SQLITE_PLAN_ERROR;
      break;
    }
    if(emit&&visitor(context,&plan)){
      free(views);free_chunks(chunks,chunk_count);free_document(&d);
      status=TA_LEGACY_SQLITE_VISITOR_ABORTED;break;
    }
    free(views);free_chunks(chunks,chunk_count);free_document(&d);++count;
  }
  if(rc!=SQLITE_DONE&&status==TA_LEGACY_SQLITE_OK)
    status=TA_LEGACY_SQLITE_STORAGE_ERROR;
  sqlite3_finalize(docs);
  if(status==TA_LEGACY_SQLITE_OK&&out_count)*out_count=count;
  return status;
}

ta_legacy_migration_sqlite_status_t ta_legacy_knowledge_sqlite_plan_all(
    sqlite3*db,const ta_legacy_migration_sqlite_limits_t*l,
    ta_legacy_migration_plan_visitor_fn visitor,void*context,
    uint32_t*out_documents){
  ta_legacy_migration_sqlite_status_t status;
  uint32_t validated=0,emitted=0;
  if(out_documents)*out_documents=0;
  if(!db||!l||!visitor||!out_documents||!l->max_documents||
     !l->max_chunks_per_document||!l->max_copied_bytes_per_document||
     l->max_documents>TA_LEGACY_SQLITE_MAX_DOCUMENTS||
     l->max_chunks_per_document>TA_LEGACY_SQLITE_MAX_CHUNKS_PER_DOCUMENT||
     l->max_copied_bytes_per_document>TA_LEGACY_SQLITE_MAX_COPIED_BYTES_PER_DOCUMENT)
    return TA_LEGACY_SQLITE_INVALID;
  if(sqlite3_db_readonly(db,"main")!=1)return TA_LEGACY_SQLITE_INVALID;
  if(!sqlite3_get_autocommit(db))return TA_LEGACY_SQLITE_BUSY;
  if(sqlite3_exec(db,"BEGIN",NULL,NULL,NULL)!=SQLITE_OK)
    return TA_LEGACY_SQLITE_BUSY;
  /* Keep schema admission, authoritative orphan coverage, validation and
   * emission under the same pinned source snapshot. */
  if(!schema_matches(db,"documents",document_columns,
       sizeof(document_columns)/sizeof(document_columns[0]))||
     !schema_matches(db,"chunks",chunk_columns,
       sizeof(chunk_columns)/sizeof(chunk_columns[0]))||
     !schema_constraints(db)){
    status=TA_LEGACY_SQLITE_SCHEMA;
    goto done;
  }

  status=verify_chunk_coverage(db);
  if(status!=TA_LEGACY_SQLITE_OK)goto done;

  /* First pass validates the COMPLETE pinned source snapshot. No migration
   * visitor observes a plan until every legacy document/chunk is admissible. */
  status=scan_documents(db,l,visitor,context,0,&validated);
  if(status!=TA_LEGACY_SQLITE_OK)goto done;

  /* Second pass emits from the same read transaction/snapshot. */
  status=scan_documents(db,l,visitor,context,1,&emitted);
  if(status==TA_LEGACY_SQLITE_OK&&emitted!=validated)
    status=TA_LEGACY_SQLITE_COVERAGE;

done:
  (void)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
  if(status==TA_LEGACY_SQLITE_OK)*out_documents=emitted;
  return status;
}
