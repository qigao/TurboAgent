#include "turbo_agent_revision_read.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"revision read failed at %d: %s\n",__LINE__,#x); return 1; } } while (0)
#define GOK(x) CHECK((x)==TA_REVISION_GENERATION_OK)
#define KOK(x) CHECK((x)==TA_REVISION_KEY_OK)

static ta_revision_id_t bytes(const void *p,size_t n){ta_revision_id_t v={(const unsigned char*)p,n};return v;}
static ta_revision_id_t text(const char *s){return bytes(s,strlen(s));}
static ta_revision_scope_t scope(void){ta_revision_scope_t s={text("tenant-A"),text("workspace")};return s;}
static ta_revision_generation_member_t member(const char*s,const char*r,const char*e,uint64_t n){ta_revision_generation_member_t m={text(s),text(r),text(e),n};return m;}
static int prefix(ta_revision_id_t p,ta_revision_id_t k){return p.size&&k.size>=p.size&&!memcmp(p.data,k.data,p.size);}

typedef struct fake_item_s { ta_revision_key_t key; const char *value; } fake_item_t;
typedef struct fake_snapshot_s {
  ta_revision_generation_payload_t generation;
  fake_item_t chunks[8]; size_t chunk_count;
  fake_item_t edges[8]; size_t edge_count;
  unsigned get_calls, search_calls, scan_calls, close_calls;
  int inject_bad_search_key, inject_bad_edge_key;
} fake_snapshot_t;
typedef struct fake_cursor_s {
  fake_snapshot_t *db; const fake_item_t *items; size_t count,pos;
  ta_revision_key_t requested_prefix; int inject_bad;
} fake_cursor_t;

static ta_revision_backend_status_t fake_get(void *owner,const char *ns,ta_revision_id_t key,ta_revision_backend_value_t *out){
  fake_snapshot_t *db=owner; ta_revision_key_t expected; ta_revision_scope_t s=scope();
  memset(out,0,sizeof(*out)); db->get_calls++;
  if(strcmp(ns,TA_REVISION_NS_GENERATION))return TA_REVISION_BACKEND_ERROR;
  if(ta_revision_generation_key(&s,&expected)!=TA_REVISION_KEY_OK || key.size!=expected.size || memcmp(key.data,expected.data,key.size))return TA_REVISION_BACKEND_ERROR;
  if(!db->generation.size)return TA_REVISION_BACKEND_NOT_FOUND;
  out->bytes=bytes(db->generation.data,db->generation.size); return TA_REVISION_BACKEND_OK;
}
static ta_revision_backend_status_t open_items(fake_snapshot_t *db,const fake_item_t *items,size_t count,
    ta_revision_id_t p,const ta_revision_read_budget_t *b,void **out,int inject){
  static fake_cursor_t cursor;
  if(!b||!b->max_items||!b->max_result_bytes||!b->max_vm_steps||!p.data||!p.size)return TA_REVISION_BACKEND_ERROR;
  memset(&cursor,0,sizeof(cursor)); cursor.db=db;cursor.items=items;cursor.count=count;cursor.inject_bad=inject;
  if (p.size > sizeof(cursor.requested_prefix.data))
    return TA_REVISION_BACKEND_ERROR;
  memcpy(cursor.requested_prefix.data, p.data, p.size);
  cursor.requested_prefix.size = p.size;
  *out=&cursor; return TA_REVISION_BACKEND_OK;
}
static ta_revision_backend_status_t fake_search(void *owner,const char *ns,ta_revision_id_t p,ta_revision_id_t q,const ta_revision_read_budget_t*b,void **out){
  fake_snapshot_t *db=owner;db->search_calls++; if(strcmp(ns,TA_REVISION_NS_CHUNK)||!q.data||!q.size)return TA_REVISION_BACKEND_ERROR;
  return open_items(db,db->chunks,db->chunk_count,p,b,out,db->inject_bad_search_key);
}
static ta_revision_backend_status_t fake_scan(void *owner,const char *ns,ta_revision_id_t p,const ta_revision_read_budget_t*b,void **out){
  fake_snapshot_t *db=owner;db->scan_calls++;if(strcmp(ns,TA_REVISION_NS_GRAPH))return TA_REVISION_BACKEND_ERROR;
  return open_items(db,db->edges,db->edge_count,p,b,out,db->inject_bad_edge_key);
}
static ta_revision_backend_status_t fake_next(void *opaque,ta_revision_id_t*out_key,ta_revision_backend_value_t*out_value){
  fake_cursor_t *c=opaque;memset(out_value,0,sizeof(*out_value));
  while(c->pos<c->count){const fake_item_t *i=&c->items[c->pos++];ta_revision_id_t key=bytes(i->key.data,i->key.size);
    if(!c->inject_bad && !prefix(bytes(c->requested_prefix.data,c->requested_prefix.size),key))continue;
    *out_key=key;out_value->bytes=text(i->value?i->value:"");return TA_REVISION_BACKEND_OK;}
  return TA_REVISION_BACKEND_NOT_FOUND;
}
static void fake_close(void *opaque){fake_cursor_t*c=opaque;c->db->close_calls++;}
static const ta_revision_read_backend_ops_t ops={fake_get,fake_search,fake_scan,fake_next,fake_close};

typedef struct seen_s {unsigned count; ta_revision_key_t keys[8];} seen_t;
static int collect(void *ctx,ta_revision_id_t key,ta_revision_id_t value){seen_t*s=ctx;(void)value;if(s->count>=8)return 1;s->keys[s->count].size=key.size;memcpy(s->keys[s->count].data,key.data,key.size);s->count++;return 0;}
static ta_revision_read_budget_t budget(void){ta_revision_read_budget_t b={8,UINT64_C(65536),UINT64_C(100000)};return b;}
static int add_chunk(fake_snapshot_t*db,const ta_revision_scope_t*s,const char*src,const char*rev,const char*ext,uint64_t ordinal,const char*value){
  ta_revision_ref_t r={*s,text(src),text(rev),text(ext)};fake_item_t*i=&db->chunks[db->chunk_count++];
  if (ta_revision_chunk_key(&r, ordinal, &i->key) != TA_REVISION_KEY_OK)
    return 1;
  i->value = value;
  return 0;
}
static int add_edge(fake_snapshot_t*db,const ta_revision_scope_t*s,uint64_t generation,const char*from,const char*fromrev,const char*to,const char*torev,const char*rel,const char*id,ta_revision_edge_direction_t d){
  ta_revision_edge_t e={*s,generation,text(from),text(fromrev),text(to),text(torev),text(rel),text(id)};fake_item_t*i=&db->edges[db->edge_count++];
  if (ta_revision_edge_key(&e, d, &i->key) != TA_REVISION_KEY_OK)
    return 1;
  i->value = "evidence";
  return 0;
}

static int same_snapshot_and_no_widen(void){
  ta_revision_scope_t s=scope();ta_revision_generation_member_t oldm[2]={member("A","rev1","ext1",2),member("B","rev7","ext2",1)};
  ta_revision_generation_member_t newm[1]={member("A","rev2","ext3",1)};fake_snapshot_t olddb={0},newdb={0};
  ta_revision_read_backend_t oldread={&olddb,&ops},newread={&newdb,&ops};ta_revision_read_budget_t b=budget();seen_t seen={0};uint64_t gen=0;
  GOK(ta_revision_generation_encode(&s,11,oldm,2,&olddb.generation));GOK(ta_revision_generation_encode(&s,12,newm,1,&newdb.generation));
  CHECK(!add_chunk(&olddb,&s,"A","rev1","ext1",0,"old-0"));CHECK(!add_chunk(&olddb,&s,"A","rev2","ext3",0,"new-physical"));
  CHECK(ta_revision_read_search_text(&oldread,&s,text("A"),text("lake"),&b,collect,&seen,&gen)==TA_REVISION_READ_OK);
  CHECK(gen==11&&seen.count==1);ta_revision_ref_t decoded;uint64_t ord=9;KOK(ta_revision_chunk_decode(bytes(seen.keys[0].data,seen.keys[0].size),&decoded,&ord));CHECK(ord==0&&!memcmp(decoded.revision.data,"rev1",4));
  /* A separately pinned new read sees rev2; changing a live head cannot rewrite oldread. */
  newdb.chunks[0]=olddb.chunks[1];newdb.chunk_count=1;memset(&seen,0,sizeof(seen));
  CHECK(ta_revision_read_search_text(&newread,&s,text("A"),text("lake"),&b,collect,&seen,&gen)==TA_REVISION_READ_OK);CHECK(gen==12&&seen.count==1);
  KOK(ta_revision_chunk_decode(bytes(seen.keys[0].data,seen.keys[0].size),&decoded,&ord));CHECK(!memcmp(decoded.revision.data,"rev2",4));
  unsigned searches=oldread.read_owner?olddb.search_calls:0;CHECK(ta_revision_read_search_text(&oldread,&s,text("missing"),text("lake"),&b,collect,&seen,&gen)==TA_REVISION_READ_NOT_FOUND);CHECK(olddb.search_calls==searches&&gen==0);
  fake_snapshot_t empty={0};ta_revision_generation_member_t none[1];(void)none;GOK(ta_revision_generation_encode(&s,13,NULL,0,&empty.generation));ta_revision_read_backend_t er={&empty,&ops};
  CHECK(ta_revision_read_search_text(&er,&s,text("A"),text("lake"),&b,collect,&seen,&gen)==TA_REVISION_READ_NOT_FOUND);CHECK(empty.search_calls==0);
  fake_snapshot_t bad=olddb;bad.generation.data[3]=2;bad.search_calls=0;ta_revision_read_backend_t br={&bad,&ops};
  CHECK(ta_revision_read_search_text(&br,&s,text("A"),text("lake"),&b,collect,&seen,&gen)==TA_REVISION_READ_MALFORMED);CHECK(bad.search_calls==0);
  return 0;
}

static int backend_contract_and_budgets(void){
  ta_revision_scope_t s=scope();ta_revision_generation_member_t m=member("A","rev1","ext1",2);fake_snapshot_t db={0};ta_revision_read_backend_t r={&db,&ops};ta_revision_read_budget_t b=budget();seen_t seen={0};uint64_t gen=0;
  GOK(ta_revision_generation_encode(&s,21,&m,1,&db.generation));CHECK(!add_chunk(&db,&s,"A","rev1","ext1",0,"one"));CHECK(!add_chunk(&db,&s,"A","rev2","ext2",0,"stale"));
  db.inject_bad_search_key=1;CHECK(ta_revision_read_search_text(&r,&s,text("A"),text("x"),&b,collect,&seen,&gen)==TA_REVISION_READ_BACKEND_CONTRACT);CHECK(gen==0&&db.close_calls==1);
  db.inject_bad_search_key=0;seen.count=0;b.max_result_bytes=1;CHECK(ta_revision_read_search_text(&r,&s,text("A"),text("x"),&b,collect,&seen,&gen)==TA_REVISION_READ_LIMIT);CHECK(seen.count==0&&gen==0);
  b=budget();seen.count=0;CHECK(ta_revision_read_search_text(&r,&s,text("A"),bytes("x\0y",3),&b,collect,&seen,&gen)==TA_REVISION_READ_INVALID);CHECK(db.search_calls==2);
  return 0;
}

static int graph_generation_filter(void){
  ta_revision_scope_t s=scope();ta_revision_generation_member_t m[2]={member("A","rev1","ext1",1),member("B","rev7","ext2",1)};fake_snapshot_t db={0};ta_revision_read_backend_t r={&db,&ops};ta_revision_read_budget_t b=budget();seen_t seen={0};uint64_t gen=0;
  GOK(ta_revision_generation_encode(&s,31,m,2,&db.generation));
  CHECK(!add_edge(&db,&s,31,"A","rev1","B","rev7","READS","current",TA_REVISION_EDGE_OUT));
  CHECK(!add_edge(&db,&s,31,"A","rev1","B","rev6","READS","historical",TA_REVISION_EDGE_OUT));
  CHECK(!add_edge(&db,&s,30,"A","rev1","B","rev7","READS","old-generation",TA_REVISION_EDGE_OUT));
  CHECK(!add_edge(&db,&s,31,"A","rev1","B","rev7","OTHER","other",TA_REVISION_EDGE_OUT));
  CHECK(ta_revision_read_scan_edges(&r,&s,text("A"),text("READS"),TA_REVISION_EDGE_OUT,&b,collect,&seen,&gen)==TA_REVISION_READ_OK);
  CHECK(gen==31&&seen.count==1&&db.scan_calls==1&&db.close_calls==1);
  db.inject_bad_edge_key=1;seen.count=0;CHECK(ta_revision_read_scan_edges(&r,&s,text("A"),text("READS"),TA_REVISION_EDGE_OUT,&b,collect,&seen,&gen)==TA_REVISION_READ_BACKEND_CONTRACT);CHECK(gen==0);
  unsigned scans=db.scan_calls;CHECK(ta_revision_read_scan_edges(&r,&s,text("missing"),text("READS"),TA_REVISION_EDGE_OUT,&b,collect,&seen,&gen)==TA_REVISION_READ_NOT_FOUND);CHECK(db.scan_calls==scans);
  return 0;
}

int main(void){if(same_snapshot_and_no_widen()||backend_contract_and_budgets()||graph_generation_filter())return 1;puts("TurboAgent pinned revision read adapter PASS");return 0;}
