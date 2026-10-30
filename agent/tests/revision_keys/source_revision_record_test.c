#include "turbo_agent_source_revision_record.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if(!(x)){fprintf(stderr,"source revision record failed at %d: %s\n",__LINE__,#x);return 1;} } while(0)
static int zero(const void*p,size_t n){const unsigned char*b=p;for(size_t i=0;i<n;++i)if(b[i])return 0;return 1;}
static uint32_t rnd(uint32_t*s){*s=*s*UINT32_C(1664525)+UINT32_C(1013904223);return *s;}

static int basics(void){
  ta_source_revision_record_t r={0},d;
  ta_source_revision_record_payload_t p;
  CHECK(ta_source_revision_record_encode(&r,&p)==TA_SOURCE_REVISION_RECORD_OK);
  CHECK(p.size==TA_SOURCE_REVISION_RECORD_BYTES);
  CHECK(ta_source_revision_record_decode(p.data,p.size,&d)==TA_SOURCE_REVISION_RECORD_OK);
  CHECK(d.content_length==0&&d.digest_kind==TA_SOURCE_REVISION_DIGEST_UNVERIFIED_LEGACY&&d.retention==TA_SOURCE_REVISION_CONTENT_UNAVAILABLE&&zero(d.sha256,32));
  memset(&r,0,sizeof(r));r.content_length=1234;r.source_mtime_seconds=7;r.observed_at_seconds=11;r.digest_kind=TA_SOURCE_REVISION_DIGEST_SHA256_ORIGINAL;r.retention=TA_SOURCE_REVISION_CONTENT_RETAINED_VERIFIED;for(size_t i=0;i<32;++i)r.sha256[i]=(unsigned char)(i+1u);
  CHECK(ta_source_revision_record_encode(&r,&p)==TA_SOURCE_REVISION_RECORD_OK);
  CHECK(ta_source_revision_record_decode(p.data,p.size,&d)==TA_SOURCE_REVISION_RECORD_OK);
  CHECK(d.content_length==1234&&d.source_mtime_seconds==7&&d.observed_at_seconds==11&&!memcmp(d.sha256,r.sha256,32)&&d.retention==TA_SOURCE_REVISION_CONTENT_RETAINED_VERIFIED);
  return 0;
}
static int reject_claims(void){
  ta_source_revision_record_t r={0},d;ta_source_revision_record_payload_t p;
  r.digest_kind=TA_SOURCE_REVISION_DIGEST_UNVERIFIED_LEGACY;r.retention=TA_SOURCE_REVISION_CONTENT_RETAINED_VERIFIED;
  CHECK(ta_source_revision_record_encode(&r,&p)==TA_SOURCE_REVISION_RECORD_INVALID&&p.size==0);
  r.retention=TA_SOURCE_REVISION_CONTENT_UNAVAILABLE;r.sha256[0]=1;
  CHECK(ta_source_revision_record_encode(&r,&p)==TA_SOURCE_REVISION_RECORD_INVALID&&p.size==0);
  memset(&r,0,sizeof(r));r.digest_kind=TA_SOURCE_REVISION_DIGEST_SHA256_ORIGINAL;r.retention=TA_SOURCE_REVISION_CONTENT_UNAVAILABLE;CHECK(ta_source_revision_record_encode(&r,&p)==TA_SOURCE_REVISION_RECORD_OK);
  for(size_t n=0;n<p.size;++n){memset(&d,0xa5,sizeof(d));CHECK(ta_source_revision_record_decode(p.data,n,&d)!=TA_SOURCE_REVISION_RECORD_OK);CHECK(zero(&d,sizeof(d)));}
  unsigned char bad[TA_SOURCE_REVISION_RECORD_BYTES+1];memcpy(bad,p.data,p.size);bad[p.size]=0;
  CHECK(ta_source_revision_record_decode(bad,sizeof(bad),&d)==TA_SOURCE_REVISION_RECORD_MALFORMED);
  memcpy(bad,p.data,p.size);bad[3]=1;CHECK(ta_source_revision_record_decode(bad,p.size,&d)==TA_SOURCE_REVISION_RECORD_MALFORMED);
  memcpy(bad,p.data,p.size);bad[14]=1;CHECK(ta_source_revision_record_decode(bad,p.size,&d)==TA_SOURCE_REVISION_RECORD_MALFORMED);
  memcpy(bad,p.data,p.size);bad[12]=99;CHECK(ta_source_revision_record_decode(bad,p.size,&d)==TA_SOURCE_REVISION_RECORD_MALFORMED);
  return 0;
}
static int randomized(void){
  uint32_t seed=UINT32_C(0x29530001);ta_source_revision_record_t r,d;ta_source_revision_record_payload_t p;
  for(size_t it=0;it<10000u;++it){memset(&r,0,sizeof(r));r.content_length=((uint64_t)rnd(&seed)<<32)|rnd(&seed);r.source_mtime_seconds=((uint64_t)rnd(&seed)<<32)|rnd(&seed);r.observed_at_seconds=((uint64_t)rnd(&seed)<<32)|rnd(&seed);if(rnd(&seed)&1u){r.digest_kind=TA_SOURCE_REVISION_DIGEST_SHA256_ORIGINAL;r.retention=(rnd(&seed)&1u)?TA_SOURCE_REVISION_CONTENT_RETAINED_VERIFIED:TA_SOURCE_REVISION_CONTENT_UNAVAILABLE;for(size_t i=0;i<32;++i)r.sha256[i]=(unsigned char)(rnd(&seed)>>24);}CHECK(ta_source_revision_record_encode(&r,&p)==TA_SOURCE_REVISION_RECORD_OK);CHECK(ta_source_revision_record_decode(p.data,p.size,&d)==TA_SOURCE_REVISION_RECORD_OK);CHECK(d.content_length==r.content_length&&d.source_mtime_seconds==r.source_mtime_seconds&&d.observed_at_seconds==r.observed_at_seconds&&d.digest_kind==r.digest_kind&&d.retention==r.retention&&!memcmp(d.sha256,r.sha256,32));}
  return 0;
}
int main(void){if(basics()||reject_claims()||randomized())return 1;puts("TurboAgent SourceRevision record PASS (10000 roundtrips)");return 0;}
