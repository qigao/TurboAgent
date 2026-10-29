#include "turbo_agent_revision_generation.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr,"generation contract failed at %d: %s\n",__LINE__,#x); return 1; \
} } while(0)
#define OK(x) CHECK((x)==TA_REVISION_GENERATION_OK)
#define KEY_OK(x) CHECK((x)==TA_REVISION_KEY_OK)
static ta_revision_id_t bytes(const void *p, size_t n) {
  ta_revision_id_t id = {(const unsigned char *)p,n}; return id;
}
static ta_revision_id_t text(const char *s) { return bytes(s,strlen(s)); }
static ta_revision_id_t payload(const ta_revision_generation_payload_t *p) { return bytes(p->data,p->size); }
static ta_revision_id_t key_bytes(const ta_revision_key_t *p) { return bytes(p->data,p->size); }
static int equal(ta_revision_id_t a,ta_revision_id_t b) {
  return a.size==b.size && a.data && b.data && !memcmp(a.data,b.data,a.size);
}
static int zero(const void *ptr,size_t n) {
  const unsigned char *p=ptr; for(size_t i=0;i<n;++i) if(p[i])return 0; return 1;
}
static ta_revision_scope_t scope(void) {
  ta_revision_scope_t s={text("tenant-A"),text("workspace")}; return s;
}
static ta_revision_generation_member_t member(const char *src,const char *rev,const char *ext,uint64_t count) {
  ta_revision_generation_member_t m={text(src),text(rev),text(ext),count}; return m;
}
static int selection_and_history(void) {
  ta_revision_scope_t s=scope(),wrong=s;
  ta_revision_generation_member_t m[2]={member("A","rev1","ext1",2),member("B","rev7","ext2",1)};
  static ta_revision_generation_payload_t old,now,empty;
  ta_revision_generation_view_t view;
  ta_revision_generation_selection_t selected;
  ta_revision_key_t chunk,out,in;
  ta_revision_ref_t ref={s,m[0].source,m[0].revision,m[0].extraction};
  int match=99;
  OK(ta_revision_generation_encode(&s,11,m,2,&old));
  OK(ta_revision_generation_decode(payload(&old),&s,&view));
  CHECK(view.generation_id==11 && view.member_count==2);
  OK(ta_revision_generation_resolve(payload(&old),&s,text("A"),&selected));
  CHECK(selected.generation_id==11 && selected.chunk_count==2);
  CHECK(equal(selected.ref.revision,text("rev1")));
  KEY_OK(ta_revision_chunk_key(&ref,1,&chunk));
  CHECK(selected.chunk_prefix.size<chunk.size &&
        !memcmp(selected.chunk_prefix.data,chunk.data,selected.chunk_prefix.size));
  OK(ta_revision_generation_accept_chunk(payload(&old),&s,key_bytes(&chunk),&match)); CHECK(match);
  KEY_OK(ta_revision_chunk_key(&ref,2,&chunk));
  OK(ta_revision_generation_accept_chunk(payload(&old),&s,key_bytes(&chunk),&match)); CHECK(!match);
  KEY_OK(ta_revision_chunk_key(&ref,UINT64_MAX,&chunk));
  OK(ta_revision_generation_accept_chunk(payload(&old),&s,key_bytes(&chunk),&match)); CHECK(!match);
  KEY_OK(ta_revision_chunk_key(&ref,0,&chunk));
  /* New generation changes A's revision/extractor and removes B. Old bytes
   * remain a valid historical selection, not rewritten to today's source. */
  m[0]=member("A","rev2","ext3",1);
  OK(ta_revision_generation_encode(&s,12,m,1,&now));
  OK(ta_revision_generation_accept_chunk(payload(&now),&s,key_bytes(&chunk),&match)); CHECK(!match);
  OK(ta_revision_generation_accept_chunk(payload(&old),&s,key_bytes(&chunk),&match)); CHECK(match);
  memset(&selected,0xa5,sizeof(selected));
  CHECK(ta_revision_generation_resolve(payload(&now),&s,text("B"),&selected)==TA_REVISION_GENERATION_NOT_FOUND);
  CHECK(zero(&selected,sizeof(selected)));
  CHECK(ta_revision_generation_resolve(payload(&old),&s,text("AA"),&selected)==TA_REVISION_GENERATION_NOT_FOUND);
  OK(ta_revision_generation_resolve(payload(&now),&s,text("A"),&selected));
  ref=selected.ref; ref.extraction=text("ext1");
  KEY_OK(ta_revision_chunk_key(&ref,0,&chunk));
  OK(ta_revision_generation_accept_chunk(payload(&now),&s,key_bytes(&chunk),&match)); CHECK(!match);
  ref=selected.ref; ref.scope.authority=text("tenant-AA");
  KEY_OK(ta_revision_chunk_key(&ref,0,&chunk));
  OK(ta_revision_generation_accept_chunk(payload(&now),&s,key_bytes(&chunk),&match)); CHECK(!match);
  wrong.authority=text("tenant-B");
  CHECK(ta_revision_generation_decode(payload(&now),&wrong,&view)==TA_REVISION_GENERATION_SCOPE_MISMATCH);
  CHECK(zero(&view,sizeof(view)));
  wrong=s;wrong.collection=text("workspace/sub");
  CHECK(ta_revision_generation_resolve(payload(&now),&wrong,text("A"),&selected)==TA_REVISION_GENERATION_SCOPE_MISMATCH);
  CHECK(zero(&selected,sizeof(selected)));
  OK(ta_revision_generation_encode(&s,13,NULL,0,&empty));
  CHECK(ta_revision_generation_resolve(payload(&empty),&s,text("A"),&selected)==TA_REVISION_GENERATION_NOT_FOUND);
  OK(ta_revision_generation_accept_chunk(payload(&empty),&s,key_bytes(&chunk),&match));CHECK(!match);
  m[0]=member("A","rev2","ext3",0);
  OK(ta_revision_generation_encode(&s,14,m,1,&empty));
  ref.scope=s; ref.source=text("A");ref.revision=text("rev2");ref.extraction=text("ext3");
  KEY_OK(ta_revision_chunk_key(&ref,0,&chunk));
  OK(ta_revision_generation_accept_chunk(payload(&empty),&s,key_bytes(&chunk),&match)); CHECK(!match);
  ta_revision_edge_t edge={s,text("A"),text("rev1"),text("B"),text("rev7"),text("READS"),text("edge-1")};
  KEY_OK(ta_revision_edge_key(&edge,TA_REVISION_EDGE_OUT,&out));
  KEY_OK(ta_revision_edge_key(&edge,TA_REVISION_EDGE_IN,&in));
  OK(ta_revision_generation_accept_edge(payload(&old),&s,key_bytes(&out),&match)); CHECK(match);
  OK(ta_revision_generation_accept_edge(payload(&old),&s,key_bytes(&in),&match)); CHECK(match);
  OK(ta_revision_generation_accept_edge(payload(&now),&s,key_bytes(&out),&match)); CHECK(!match);
  edge.to_revision=text("rev8");KEY_OK(ta_revision_edge_key(&edge,TA_REVISION_EDGE_IN,&in));
  OK(ta_revision_generation_accept_edge(payload(&old),&s,key_bytes(&in),&match)); CHECK(!match);
  CHECK(ta_revision_generation_accept_edge(payload(&old),&s,key_bytes(&chunk),&match)==TA_REVISION_GENERATION_MALFORMED);
  CHECK(!match);
  return 0;
}

static int malformed_and_limits(void) {
  ta_revision_scope_t s=scope();
  ta_revision_generation_member_t m[2]={member("A","r","e",2),member("B","r","e",3)};
  static ta_revision_generation_payload_t good,bad,encoded;
  ta_revision_generation_view_t view;
  ta_revision_generation_selection_t selected;
  OK(ta_revision_generation_encode(&s,UINT64_MAX,m,2,&good));
  for(size_t i=0;i<good.size;++i) {
    memset(&view,0xa5,sizeof(view));
    CHECK(ta_revision_generation_decode(bytes(good.data,i),&s,&view)!=TA_REVISION_GENERATION_OK);
    CHECK(zero(&view,sizeof(view)));
  }
  bad=good;bad.data[3]=2;
  CHECK(ta_revision_generation_decode(payload(&bad),&s,&view)==TA_REVISION_GENERATION_MALFORMED);
  bad=good;bad.data[bad.size++]=0;
  CHECK(ta_revision_generation_decode(payload(&bad),&s,&view)==TA_REVISION_GENERATION_MALFORMED);
  bad=good;memset(bad.data+4,0,8);
  CHECK(ta_revision_generation_decode(payload(&bad),&s,&view)==TA_REVISION_GENERATION_MALFORMED);
  bad=good;bad.data[12]=255;bad.data[13]=255;
  CHECK(ta_revision_generation_decode(payload(&bad),&s,&view)==TA_REVISION_GENERATION_MALFORMED);
  OK(ta_revision_generation_decode(payload(&good),&s,&view));
  size_t second=(size_t)(view.members[1].source.data-good.data);
  bad=good;bad.data[second]='A';
  CHECK(ta_revision_generation_decode(payload(&bad),&s,&view)==TA_REVISION_GENERATION_MALFORMED);
  CHECK(ta_revision_generation_resolve(payload(&bad),&s,text("A"),&selected)==TA_REVISION_GENERATION_MALFORMED);
  CHECK(zero(&selected,sizeof(selected))); /* later corruption cannot be hidden by early match */
  size_t count_at=12u+2u+s.authority.size+2u+s.collection.size;
  bad=good;bad.data[count_at]=0;bad.data[count_at+1u]=65;
  CHECK(ta_revision_generation_decode(payload(&bad),&s,&view)==TA_REVISION_GENERATION_LIMIT);
  bad=good;memset(bad.data+bad.size-8u,255,8);
  CHECK(ta_revision_generation_decode(payload(&bad),&s,&view)==TA_REVISION_GENERATION_LIMIT);
  m[0].source=text("B");
  CHECK(ta_revision_generation_encode(&s,1,m,2,&encoded)==TA_REVISION_GENERATION_INVALID);
  CHECK(zero(&encoded,sizeof(encoded)));
  m[0].source=text("Z");
  CHECK(ta_revision_generation_encode(&s,1,m,2,&encoded)==TA_REVISION_GENERATION_INVALID);
  m[0].source=text("A"); m[0].chunk_count=TA_REVISION_GENERATION_MAX_CHUNKS+1u;
  CHECK(ta_revision_generation_encode(&s,1,m,1,&encoded)==TA_REVISION_GENERATION_LIMIT);
  m[0].chunk_count=2;
  CHECK(ta_revision_generation_encode(&s,0,m,1,&encoded)==TA_REVISION_GENERATION_INVALID);
  CHECK(ta_revision_generation_encode(&s,1,NULL,1,&encoded)==TA_REVISION_GENERATION_INVALID);
  CHECK(ta_revision_generation_encode(&s,1,m,65,&encoded)==TA_REVISION_GENERATION_LIMIT);
  CHECK(ta_revision_generation_decode(bytes(good.data,65537),&s,&view)==TA_REVISION_GENERATION_LIMIT);
  CHECK(ta_revision_generation_decode(bytes(NULL,1),&s,&view)==TA_REVISION_GENERATION_MALFORMED);
  CHECK(ta_revision_generation_decode(payload(&good),NULL,&view)==TA_REVISION_GENERATION_INVALID);
  CHECK(ta_revision_generation_resolve(payload(&good),&s,bytes(NULL,1),&selected)==TA_REVISION_GENERATION_INVALID);
  CHECK(zero(&selected,sizeof(selected)));
  CHECK(ta_revision_generation_decode(payload(&good),&s,NULL)==TA_REVISION_GENERATION_INVALID);
  CHECK(ta_revision_generation_resolve(payload(&good),&s,text("A"),NULL)==TA_REVISION_GENERATION_INVALID);
  CHECK(ta_revision_generation_accept_chunk(payload(&good),&s,text("bad-key"),NULL)==TA_REVISION_GENERATION_INVALID);
  int match=99;
  CHECK(ta_revision_generation_accept_chunk(payload(&good),&s,text("bad-key"),&match)==TA_REVISION_GENERATION_MALFORMED);
  CHECK(!match);
  return 0;
}

static int binary_and_maximum(void) {
  unsigned char auth[]={0,255,':','/'},coll[]={255,0};
  unsigned char s1[]={'A',0,255},s2[]={'A',0,255,0};
  ta_revision_scope_t s={bytes(auth,sizeof(auth)),bytes(coll,sizeof(coll))};
  ta_revision_generation_member_t m[TA_REVISION_GENERATION_MAX_SOURCES];
  static unsigned char ids[TA_REVISION_GENERATION_MAX_SOURCES][TA_REVISION_ID_MAX_BYTES];
  static ta_revision_generation_payload_t p,reencoded;
  ta_revision_generation_view_t view;
  ta_revision_generation_selection_t selected;
  m[0]=(ta_revision_generation_member_t){bytes(s1,sizeof(s1)),text("r/1"),bytes(coll,sizeof(coll)),1};
  m[1]=(ta_revision_generation_member_t){bytes(s2,sizeof(s2)),text("r/2"),bytes(auth,sizeof(auth)),2};
  OK(ta_revision_generation_encode(&s,1,m,2,&p));
  OK(ta_revision_generation_resolve(payload(&p),&s,m[1].source,&selected));
  CHECK(equal(selected.ref.source,m[1].source));
  for(size_t i=0;i<TA_REVISION_GENERATION_MAX_SOURCES;++i) {
    memset(ids[i],255,sizeof(ids[i]));ids[i][0]=(unsigned char)i;
    m[i]=(ta_revision_generation_member_t){bytes(ids[i],sizeof(ids[i])),bytes(ids[i],sizeof(ids[i])),bytes(ids[i],sizeof(ids[i])),TA_REVISION_GENERATION_MAX_CHUNKS};
  }
  s.authority=bytes(ids[0],sizeof(ids[0]));s.collection=bytes(ids[1],sizeof(ids[1]));
  OK(ta_revision_generation_encode(&s,99,m,TA_REVISION_GENERATION_MAX_SOURCES,&p));
  CHECK(p.size<=TA_REVISION_GENERATION_MAX_BYTES && p.size>50000u);
  OK(ta_revision_generation_decode(payload(&p),&s,&view));
  OK(ta_revision_generation_encode(&view.scope,view.generation_id,view.members,view.member_count,&reencoded));
  CHECK(reencoded.size==p.size && !memcmp(p.data,reencoded.data,p.size));
  OK(ta_revision_generation_resolve(payload(&p),&s,m[63].source,&selected));
  CHECK(selected.chunk_count==TA_REVISION_GENERATION_MAX_CHUNKS);
  return 0;
}

static uint32_t random_u32(uint32_t *s) { *s=*s*UINT32_C(1664525)+UINT32_C(1013904223); return *s; }
static int randomized(void) {
  uint32_t seed=UINT32_C(0x3041122);
  unsigned char fields[4][3][TA_REVISION_ID_MAX_BYTES];
  ta_revision_scope_t s=scope();
  ta_revision_generation_member_t members[4];
  static ta_revision_generation_payload_t p,copy,mutated;
  ta_revision_generation_view_t view;
  ta_revision_generation_selection_t sel;
  ta_revision_key_t chunk;
  for(size_t it=0;it<10000u;++it) {
    size_t count=1u+random_u32(&seed)%4u;
    for(size_t i=0;i<count;++i) {
      ta_revision_id_t ids[3];
      for(size_t j=0;j<3u;++j) {
        size_t n=1u+random_u32(&seed)%TA_REVISION_ID_MAX_BYTES;
        for(size_t k=0;k<n;++k) fields[i][j][k]=(unsigned char)(random_u32(&seed)>>24);
        fields[i][j][0]=(unsigned char)(i+1u); /* canonical unique source ordering */
        ids[j]=bytes(fields[i][j],n);
      }
      members[i]=(ta_revision_generation_member_t){ids[0],ids[1],ids[2],1u+random_u32(&seed)%100u};
    }
    OK(ta_revision_generation_encode(&s,(uint64_t)it+1u,members,count,&p));
    OK(ta_revision_generation_decode(payload(&p),&s,&view));
    OK(ta_revision_generation_encode(&view.scope,view.generation_id,view.members,view.member_count,&copy));
    CHECK(copy.size==p.size && !memcmp(copy.data,p.data,p.size));
    OK(ta_revision_generation_resolve(payload(&p),&s,members[count-1u].source,&sel));
    KEY_OK(ta_revision_chunk_key(&sel.ref,sel.chunk_count-1u,&chunk));
    int match=0;
    OK(ta_revision_generation_accept_chunk(payload(&p),&s,key_bytes(&chunk),&match));CHECK(match);
    mutated=p;
    size_t at=random_u32(&seed)%mutated.size;
    mutated.data[at]^=(unsigned char)(1u+random_u32(&seed)%255u);
    ta_revision_generation_status_t rc=ta_revision_generation_decode(payload(&mutated),&s,&view);
    if(rc==TA_REVISION_GENERATION_OK) {
      OK(ta_revision_generation_encode(&view.scope,view.generation_id,view.members,view.member_count,&copy));
      CHECK(copy.size==mutated.size && !memcmp(copy.data,mutated.data,copy.size));
    } else CHECK(zero(&view,sizeof(view)));
    size_t truncated=random_u32(&seed)%p.size;
    CHECK(ta_revision_generation_decode(bytes(p.data,truncated),&s,&view)!=TA_REVISION_GENERATION_OK);
  }
  return 0;
}
int main(void) {
  if(selection_and_history() || malformed_and_limits() || binary_and_maximum() || randomized()) return 1;
  puts("TurboAgent generation membership PASS (10000 canonical roundtrips + mutations)");
  return 0;
}
