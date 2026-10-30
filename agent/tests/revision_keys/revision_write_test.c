#include "turbo_agent_revision_write.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"revision write failed at %d: %s\n",__LINE__,#x); return 1; } } while(0)
static ta_revision_id_t text(const char*s){ta_revision_id_t v={(const unsigned char*)s,strlen(s)};return v;}
static ta_revision_blob_t blob(const char*s){ta_revision_blob_t v={(const unsigned char*)s,strlen(s)};return v;}
static ta_revision_scope_t scope(void){ta_revision_scope_t s={text("tenant-A"),text("workspace")};return s;}
static ta_revision_generation_member_t member(const char*s,const char*r,const char*e,uint64_t n){ta_revision_generation_member_t m={text(s),text(r),text(e),n};return m;}

typedef enum op_kind_e { OP_PUT=1, OP_TEXT=2 } op_kind_t;
typedef struct op_s { op_kind_t kind; const char *ns; ta_revision_key_t key; uint64_t expected; } op_t;
typedef struct fake_writer_s {
  op_t ops[64]; size_t count; unsigned aborts,publishes;
  size_t fail_at; ta_revision_write_backend_status_t fail_status;
  ta_revision_write_backend_status_t publish_status; uint64_t publish_seq;
} fake_writer_t;

static ta_revision_write_backend_status_t maybe_fail(fake_writer_t*w){
  if(w->fail_at && w->count+1u==w->fail_at)return w->fail_status;
  return TA_REVISION_WRITE_BACKEND_OK;
}
static ta_revision_write_backend_status_t put(void*o,const char*ns,ta_revision_id_t key,ta_revision_blob_t value,uint64_t expected){
  fake_writer_t*w=o;ta_revision_write_backend_status_t f=maybe_fail(w);(void)value;if(f!=TA_REVISION_WRITE_BACKEND_OK)return f;
  if(w->count>=64||key.size>sizeof(w->ops[0].key.data))return TA_REVISION_WRITE_BACKEND_ERROR;
  op_t*p=&w->ops[w->count++];memset(p,0,sizeof(*p));p->kind=OP_PUT;p->ns=ns;p->expected=expected;p->key.size=key.size;memcpy(p->key.data,key.data,key.size);return TA_REVISION_WRITE_BACKEND_OK;
}
static ta_revision_write_backend_status_t project(void*o,const char*ns,ta_revision_id_t key,ta_revision_blob_t txt){
  fake_writer_t*w=o;ta_revision_write_backend_status_t f=maybe_fail(w);(void)txt;if(f!=TA_REVISION_WRITE_BACKEND_OK)return f;
  if(w->count>=64||key.size>sizeof(w->ops[0].key.data))return TA_REVISION_WRITE_BACKEND_ERROR;
  op_t*p=&w->ops[w->count++];memset(p,0,sizeof(*p));p->kind=OP_TEXT;p->ns=ns;p->key.size=key.size;memcpy(p->key.data,key.data,key.size);return TA_REVISION_WRITE_BACKEND_OK;
}
static ta_revision_write_backend_status_t publish(void*o,uint64_t*out){fake_writer_t*w=o;w->publishes++;if(w->publish_status!=TA_REVISION_WRITE_BACKEND_OK)return w->publish_status;*out=w->publish_seq;return TA_REVISION_WRITE_BACKEND_OK;}
static void abort_write(void*o){((fake_writer_t*)o)->aborts++;}
static const ta_revision_write_backend_ops_t ops={put,project,publish,abort_write};
static ta_revision_write_backend_t backend(fake_writer_t*w){ta_revision_write_backend_t b={w,&ops};return b;}

static ta_revision_write_request_t make_request(uint64_t generation,
    const ta_revision_generation_member_t *members,size_t member_count,
    ta_revision_write_source_t *staged,size_t staged_count,
    ta_revision_blob_t previous,uint64_t expected_seq,
    ta_revision_write_edge_t *edges,size_t edge_count){
  ta_revision_write_request_t r;memset(&r,0,sizeof(r));r.scope=scope();r.generation_id=generation;r.expected_generation_head_seq=expected_seq;r.members=members;r.member_count=member_count;r.staged_sources=staged;r.staged_source_count=staged_count;r.previous_generation=previous;r.edges=edges;r.edge_count=edge_count;r.budget=(ta_revision_write_budget_t){32,32,UINT64_C(1)<<20};return r;
}
static ta_revision_blob_t manifest_blob(ta_revision_generation_payload_t *p){return (ta_revision_blob_t){p->data,p->size};}

static int initial_generation(void){
  ta_revision_write_chunk_t ac[2]={{blob("chunk-a0"),blob("lake zero")},{blob("chunk-a1"),blob("lake one")}},bc[1]={{blob("chunk-b0"),blob("river")}};
  ta_revision_generation_member_t members[2]={member("A","rev1","ext1",2),member("B","rev7","ext2",1)};
  ta_revision_write_source_t staged[2]={{text("A"),text("rev1"),text("ext1"),blob("meta-a"),ac,2},{text("B"),text("rev7"),text("ext2"),blob("meta-b"),bc,1}};
  ta_revision_write_edge_t edge={text("A"),text("rev1"),text("B"),text("rev7"),text("READS"),text("edge-1"),blob("evidence")};
  ta_revision_write_request_t req=make_request(77,members,2,staged,2,(ta_revision_blob_t){NULL,0},0,&edge,1);
  fake_writer_t w={0};w.publish_seq=101;ta_revision_write_backend_t b=backend(&w);ta_revision_write_result_t out={0};
  CHECK(ta_revision_write_publish_generation(&b,&req,&out)==TA_REVISION_WRITE_OK);CHECK(out.generation_id==77&&out.commit_seq==101);CHECK(w.aborts==0&&w.publishes==1&&w.count==11);
  CHECK(!strcmp(w.ops[0].ns,TA_REVISION_NS_SOURCE_REVISION)&&w.ops[0].expected==0);
  CHECK(!strcmp(w.ops[10].ns,TA_REVISION_NS_GENERATION)&&w.ops[10].expected==0);
  ta_revision_edge_t decoded;ta_revision_edge_direction_t d;CHECK(ta_revision_edge_decode((ta_revision_id_t){w.ops[8].key.data,w.ops[8].key.size},&decoded,&d)==TA_REVISION_KEY_OK);CHECK(decoded.generation_id==77&&d==TA_REVISION_EDGE_OUT);
  CHECK(ta_revision_edge_decode((ta_revision_id_t){w.ops[9].key.data,w.ops[9].key.size},&decoded,&d)==TA_REVISION_KEY_OK);CHECK(decoded.generation_id==77&&d==TA_REVISION_EDGE_IN);
  ta_revision_key_t p;CHECK(ta_revision_source_revision_prefix(&req.scope,text("A"),&p)==TA_REVISION_KEY_OK);CHECK(w.ops[0].key.size>p.size&&!memcmp(w.ops[0].key.data,p.data,p.size));return 0;
}

static int incremental_generation(void){
  ta_revision_scope_t s=scope();ta_revision_generation_payload_t previous;
  ta_revision_generation_member_t oldm[2]={member("A","rev1","ext1",2),member("B","rev7","ext2",1)};
  ta_revision_generation_member_t newm[2]={member("A","rev1","ext1",2),member("B","rev8","ext3",1)};
  CHECK(ta_revision_generation_encode(&s,77,oldm,2,&previous)==TA_REVISION_GENERATION_OK);
  ta_revision_write_chunk_t bc[1]={{blob("chunk-b-new"),blob("river new")}};
  ta_revision_write_source_t staged[1]={{text("B"),text("rev8"),text("ext3"),blob("meta-b8"),bc,1}};
  ta_revision_write_edge_t edge={text("A"),text("rev1"),text("B"),text("rev8"),text("READS"),text("edge-2"),blob("evidence-2")};
  ta_revision_write_request_t req=make_request(78,newm,2,staged,1,manifest_blob(&previous),101,&edge,1);
  fake_writer_t w={0};w.publish_seq=102;ta_revision_write_backend_t b=backend(&w);ta_revision_write_result_t out={0};
  CHECK(ta_revision_write_publish_generation(&b,&req,&out)==TA_REVISION_WRITE_OK);CHECK(out.generation_id==78&&out.commit_seq==102);CHECK(w.count==6&&w.publishes==1&&w.aborts==0);
  CHECK(!strcmp(w.ops[0].ns,TA_REVISION_NS_SOURCE_REVISION));ta_revision_key_t ap;CHECK(ta_revision_source_revision_prefix(&s,text("A"),&ap)==TA_REVISION_KEY_OK);CHECK(memcmp(w.ops[0].key.data,ap.data,ap.size));
  CHECK(!strcmp(w.ops[5].ns,TA_REVISION_NS_GENERATION)&&w.ops[5].expected==101);
  ta_revision_edge_t decoded;ta_revision_edge_direction_t d;CHECK(ta_revision_edge_decode((ta_revision_id_t){w.ops[3].key.data,w.ops[3].key.size},&decoded,&d)==TA_REVISION_KEY_OK);CHECK(decoded.generation_id==78&&d==TA_REVISION_EDGE_OUT);
  return 0;
}

static int failures(void){
  ta_revision_generation_member_t m=member("A","rev1","ext1",1);ta_revision_write_chunk_t ch={blob("p"),blob("t")};ta_revision_write_source_t staged={text("A"),text("rev1"),text("ext1"),blob("meta"),&ch,1};ta_revision_write_request_t req=make_request(1,&m,1,&staged,1,(ta_revision_blob_t){NULL,0},0,NULL,0);ta_revision_write_result_t out;fake_writer_t w={0};w.publish_seq=9;ta_revision_write_backend_t b=backend(&w);
  w.fail_at=3;w.fail_status=TA_REVISION_WRITE_BACKEND_BUSY;CHECK(ta_revision_write_publish_generation(&b,&req,&out)==TA_REVISION_WRITE_BUSY);CHECK(w.aborts==1&&w.publishes==0&&out.commit_seq==0);
  memset(&w,0,sizeof(w));w.fail_at=2;w.fail_status=TA_REVISION_WRITE_BACKEND_COMMIT_UNKNOWN;b=backend(&w);CHECK(ta_revision_write_publish_generation(&b,&req,&out)==TA_REVISION_WRITE_BACKEND_CONTRACT);CHECK(w.aborts==1&&w.publishes==0);
  memset(&w,0,sizeof(w));w.publish_status=TA_REVISION_WRITE_BACKEND_COMMIT_UNKNOWN;b=backend(&w);CHECK(ta_revision_write_publish_generation(&b,&req,&out)==TA_REVISION_WRITE_COMMIT_UNKNOWN);CHECK(w.publishes==1&&w.aborts==1&&out.commit_seq==0);
  memset(&w,0,sizeof(w));w.publish_seq=0;b=backend(&w);CHECK(ta_revision_write_publish_generation(&b,&req,&out)==TA_REVISION_WRITE_BACKEND_CONTRACT);CHECK(w.publishes==1&&w.aborts==0&&out.commit_seq==0);
  return 0;
}

static int incremental_validation(void){
  ta_revision_scope_t s=scope();ta_revision_generation_payload_t prev;ta_revision_generation_member_t oldm[2]={member("A","r1","e1",1),member("B","r2","e2",1)};CHECK(ta_revision_generation_encode(&s,5,oldm,2,&prev)==TA_REVISION_GENERATION_OK);
  ta_revision_generation_member_t newm[2]={member("A","r1","e1",1),member("B","r3","e3",1)};ta_revision_write_chunk_t ch={blob("p"),blob("t")};ta_revision_write_source_t bnew={text("B"),text("r3"),text("e3"),blob("m"),&ch,1};ta_revision_write_request_t req=make_request(6,newm,2,NULL,0,manifest_blob(&prev),10,NULL,0);fake_writer_t w={0};ta_revision_write_backend_t be=backend(&w);ta_revision_write_result_t out;
  CHECK(ta_revision_write_publish_generation(&be,&req,&out)==TA_REVISION_WRITE_INVALID);CHECK(w.count==0&&w.aborts==1); /* changed B not staged */
  req=make_request(6,newm,2,&bnew,1,manifest_blob(&prev),10,NULL,0);ta_revision_write_source_t astale={text("A"),text("r1"),text("e1"),blob("m"),&ch,1};req.staged_sources=&astale;memset(&w,0,sizeof(w));be=backend(&w);CHECK(ta_revision_write_publish_generation(&be,&req,&out)==TA_REVISION_WRITE_INVALID);CHECK(w.count==0&&w.aborts==1); /* unchanged A rewritten */
  req=make_request(5,newm,2,&bnew,1,manifest_blob(&prev),10,NULL,0);memset(&w,0,sizeof(w));be=backend(&w);CHECK(ta_revision_write_publish_generation(&be,&req,&out)==TA_REVISION_WRITE_INVALID);CHECK(w.count==0&&w.aborts==1); /* non-monotonic gen */
  req=make_request(6,newm,2,&bnew,1,(ta_revision_blob_t){NULL,0},10,NULL,0);memset(&w,0,sizeof(w));be=backend(&w);CHECK(ta_revision_write_publish_generation(&be,&req,&out)==TA_REVISION_WRITE_INVALID);CHECK(w.count==0&&w.aborts==1); /* CAS without previous manifest */
  return 0;
}

static int structural_validation(void){
  ta_revision_generation_member_t unsorted[2]={member("B","r1","e",1),member("A","r2","e",1)};ta_revision_write_chunk_t ch={blob("p"),blob("t")};ta_revision_write_source_t staged[2]={{text("B"),text("r1"),text("e"),blob("m"),&ch,1},{text("A"),text("r2"),text("e"),blob("m"),&ch,1}};ta_revision_write_request_t req=make_request(1,unsorted,2,staged,2,(ta_revision_blob_t){NULL,0},0,NULL,0);fake_writer_t w={0};ta_revision_write_backend_t b=backend(&w);ta_revision_write_result_t out;
  CHECK(ta_revision_write_publish_generation(&b,&req,&out)==TA_REVISION_WRITE_INVALID);CHECK(w.count==0&&w.aborts==1);
  ta_revision_generation_member_t sorted[2]={member("A","r1","e",1),member("B","r2","e",1)};staged[0]=(ta_revision_write_source_t){text("A"),text("r1"),text("e"),blob("m"),&ch,1};staged[1]=(ta_revision_write_source_t){text("B"),text("r2"),text("e"),blob("m"),&ch,1};req=make_request(1,sorted,2,staged,2,(ta_revision_blob_t){NULL,0},0,NULL,0);req.budget.max_chunks=1;memset(&w,0,sizeof(w));b=backend(&w);CHECK(ta_revision_write_publish_generation(&b,&req,&out)==TA_REVISION_WRITE_LIMIT);CHECK(w.count==0&&w.aborts==1);
  req=make_request(1,NULL,0,NULL,0,(ta_revision_blob_t){NULL,0},0,NULL,0);memset(&w,0,sizeof(w));w.publish_seq=1;b=backend(&w);CHECK(ta_revision_write_publish_generation(&b,&req,&out)==TA_REVISION_WRITE_OK);CHECK(w.count==1&&!strcmp(w.ops[0].ns,TA_REVISION_NS_GENERATION)&&w.publishes==1);
  return 0;
}

int main(void){if(initial_generation()||incremental_generation()||failures()||incremental_validation()||structural_validation())return 1;puts("TurboAgent incremental revision write adapter PASS");return 0;}
