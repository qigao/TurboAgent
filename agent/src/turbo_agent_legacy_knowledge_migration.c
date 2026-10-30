#include "turbo_agent_legacy_knowledge_migration.h"
#include <string.h>

static const unsigned char migration_magic[4]={'T','A','L',1};
static const unsigned char extraction_id_bytes[]="legacy-knowledge-store/chunks-v1";
enum { LEGACY_ID_SOURCE=1, LEGACY_ID_REVISION=2 };

static int valid_required(ta_legacy_migration_text_t v){return v.data&&v.size&&v.size<=TA_LEGACY_MIGRATION_FIELD_MAX_BYTES;}
static int valid_chunk_text(ta_legacy_migration_text_t v){return v.data && v.size<=TA_LEGACY_MIGRATION_CHUNK_MAX_BYTES;}
static int valid_optional(ta_legacy_migration_text_t v){return(!v.data&&v.size==0)||(v.data&&v.size<=TA_LEGACY_MIGRATION_FIELD_MAX_BYTES);}
static int no_nul(ta_legacy_migration_text_t v){return !v.size||!memchr(v.data,0,v.size);}
static int equal_text(ta_legacy_migration_text_t a,ta_legacy_migration_text_t b){return a.size==b.size&&(!a.size||(a.data&&b.data&&!memcmp(a.data,b.data,a.size)));}
static int append_byte(ta_legacy_migration_id_t*out,unsigned char v){if(out->size>=sizeof(out->data))return 0;out->data[out->size++]=v;return 1;}
static int append_u16(ta_legacy_migration_id_t*out,size_t v){return v<=UINT16_MAX&&append_byte(out,(unsigned char)(v>>8))&&append_byte(out,(unsigned char)v);}
static int append_bytes(ta_legacy_migration_id_t*out,const unsigned char*data,size_t n){if(!data||n>sizeof(out->data)-out->size)return 0;memcpy(out->data+out->size,data,n);out->size+=n;return 1;}
static int append_field(ta_legacy_migration_id_t*out,ta_legacy_migration_text_t v){return append_u16(out,v.size)&&append_bytes(out,v.data,v.size);}
static int append_u64(ta_legacy_migration_id_t*out,uint64_t v){unsigned i;if(sizeof(out->data)-out->size<8u)return 0;for(i=0;i<8u;++i)out->data[out->size++]=(unsigned char)(v>>(56u-8u*i));return 1;}
static int begin_id(ta_legacy_migration_id_t*out,unsigned char kind){memset(out,0,sizeof(*out));return append_bytes(out,migration_magic,sizeof(migration_magic))&&append_byte(out,kind);}
static int make_source_id(const ta_legacy_migration_document_t*d,ta_legacy_migration_id_t*out){return begin_id(out,LEGACY_ID_SOURCE)&&append_field(out,d->id);}
static int make_revision_id(const ta_legacy_migration_document_t*d,size_t count,ta_legacy_migration_id_t*out){return begin_id(out,LEGACY_ID_REVISION)&&append_field(out,d->id)&&append_field(out,d->content_hash)&&append_u64(out,(uint64_t)d->updated_at)&&append_u64(out,(uint64_t)count);}
static void decimal_u64(uint64_t v,unsigned char out[20],size_t*out_size){unsigned char rev[20];size_t n=0,i;do{rev[n++]=(unsigned char)('0'+(v%10u));v/=10u;}while(v);for(i=0;i<n;++i)out[i]=rev[n-1u-i];*out_size=n;}
static int valid_chunk_id(const ta_legacy_migration_document_t*d,const ta_legacy_migration_chunk_t*c,uint64_t ordinal){unsigned char digits[20];size_t dn=0;decimal_u64(ordinal,digits,&dn);if(c->id.size!=d->id.size+1u+dn||!c->id.data)return 0;return !memcmp(c->id.data,d->id.data,d->id.size)&&c->id.data[d->id.size]=='#'&&!memcmp(c->id.data+d->id.size+1u,digits,dn);}
static ta_legacy_hash_shape_t hash_shape(ta_legacy_migration_text_t h){size_t i;if(h.size!=16u)return TA_LEGACY_HASH_OPAQUE_UNVERIFIED;for(i=0;i<h.size;++i){unsigned char c=h.data[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return TA_LEGACY_HASH_OPAQUE_UNVERIFIED;}return TA_LEGACY_HASH_HEX64_SHAPE_UNVERIFIED;}

ta_legacy_migration_status_t ta_legacy_knowledge_plan_document(
    const ta_legacy_migration_document_t*d,const ta_legacy_migration_chunk_t*chunks,
    size_t chunk_count,ta_legacy_migration_plan_t*out){
  ta_legacy_migration_plan_t p;uint64_t text_bytes=0;size_t i;
  if(!out)return TA_LEGACY_MIGRATION_INVALID;
  memset(out,0,sizeof(*out));memset(&p,0,sizeof(p));
  if(!d||!valid_required(d->id)||!valid_required(d->uri)||!valid_required(d->kind)||
     !valid_required(d->content_hash)||!valid_optional(d->title)||!valid_optional(d->metadata_json)||
     !no_nul(d->id)||!no_nul(d->uri)||!no_nul(d->kind)||!no_nul(d->title)||
     !no_nul(d->content_hash)||!no_nul(d->metadata_json)||d->mtime<0||d->size<0||
     d->updated_at<0||chunk_count>TA_REVISION_GENERATION_MAX_CHUNKS||(chunk_count&&!chunks))
    return TA_LEGACY_MIGRATION_INVALID;
  if(!chunk_count)return TA_LEGACY_MIGRATION_MALFORMED;
  for(i=0;i<chunk_count;++i){
    const ta_legacy_migration_chunk_t*c=&chunks[i];
    if(!valid_required(c->id)||!valid_required(c->document_id)||!valid_chunk_text(c->text)||
       !valid_optional(c->metadata_json)||!no_nul(c->id)||!no_nul(c->document_id)||
       !no_nul(c->text)||!no_nul(c->metadata_json)||c->ordinal<0||
       (uint64_t)c->ordinal!=(uint64_t)i||!equal_text(c->document_id,d->id)||
       !valid_chunk_id(d,c,(uint64_t)i)||c->text.size>TA_LEGACY_MIGRATION_CHUNK_MAX_BYTES)
      return TA_LEGACY_MIGRATION_MALFORMED;
    if((uint64_t)c->text.size>UINT64_MAX-text_bytes)return TA_LEGACY_MIGRATION_LIMIT;
    text_bytes+=(uint64_t)c->text.size;
  }
  if(!make_source_id(d,&p.source_id)||!make_revision_id(d,chunk_count,&p.revision_id))
    return TA_LEGACY_MIGRATION_REINGEST_REQUIRED;
  memcpy(p.extraction_id.data,extraction_id_bytes,sizeof(extraction_id_bytes)-1u);
  p.extraction_id.size=sizeof(extraction_id_bytes)-1u;
  p.revision_record.content_length=(uint64_t)d->size;
  p.revision_record.source_mtime_seconds=(uint64_t)d->mtime;
  p.revision_record.observed_at_seconds=(uint64_t)d->updated_at;
  p.revision_record.digest_kind=TA_SOURCE_REVISION_DIGEST_UNVERIFIED_LEGACY;
  p.revision_record.retention=TA_SOURCE_REVISION_CONTENT_UNAVAILABLE;
  memset(p.revision_record.sha256,0,sizeof(p.revision_record.sha256));
  p.legacy_hash_shape=hash_shape(d->content_hash);p.extracted_text_bytes=text_bytes;
  p.chunk_count=chunk_count;p.document=d;p.chunks=chunks;*out=p;
  return TA_LEGACY_MIGRATION_OK;
}
