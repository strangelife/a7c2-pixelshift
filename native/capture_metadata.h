#ifndef CAPTURE_METADATA_H
#define CAPTURE_METADATA_H
#include <stdint.h>
#include <stddef.h>
typedef int (*MetaRead)(void *,uint64_t,void *,uint32_t);
typedef struct {uint16_t tag,type;uint32_t count,length;uint8_t value[128];} MetaField;
typedef struct {MetaField root[16],exif[56];uint32_t nr,ne,thumbnail_offset,thumbnail_bytes,omitted;} CaptureMetadata;
static void mz(void *p,size_t n){uint8_t *b=p;while(n--)*b++=0;}
static void mc(void *d,const void *s,size_t n){uint8_t *a=d;const uint8_t *b=s;while(n--)*a++=*b++;}
static uint16_t m16(const uint8_t *b){return (uint16_t)(b[0]|(uint16_t)b[1]<<8);}
static uint32_t m32(const uint8_t *b){return (uint32_t)m16(b)|(uint32_t)m16(b+2)<<16;}
static void mw16(uint8_t *b,uint32_t v){b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);}
static void mw32(uint8_t *b,uint32_t v){mw16(b,v);mw16(b+2,v>>16);}
static int selected_root(uint16_t t){return t==0x010f||t==0x0110||t==0x0112||t==0x0132||t==0x013b||t==0x8298;}
static int selected_exif(uint16_t t){
    static const uint16_t ids[]={0x829a,0x829d,0x8822,0x8827,0x8830,0x8832,0x9000,0x9003,0x9004,0x9010,0x9011,0x9012,
        0x9201,0x9202,0x9203,0x9204,0x9205,0x9206,0x9207,0x9208,0x9209,0x920a,0x9290,0x9291,0x9292,
        0xa20e,0xa20f,0xa210,0xa215,0xa217,0xa300,0xa301,0xa401,0xa402,0xa403,0xa404,0xa405,0xa406,0xa408,0xa409,0xa40a,
        0xa431,0xa432,0xa433,0xa434,0xa435};
    for(size_t i=0;i<sizeof(ids)/sizeof(ids[0]);i++)if(ids[i]==t)return 1;return 0;
}
static int capture_field(void *ctx,MetaRead read,const uint8_t *e,MetaField *f){
    uint32_t unit;f->tag=m16(e);f->type=m16(e+2);f->count=m32(e+4);
    switch(f->type){case 1:case 2:case 7:unit=1;break;case 3:unit=2;break;case 4:case 9:unit=4;break;case 5:case 10:unit=8;break;default:return 1;}
    if(!f->count||f->count>sizeof(f->value)/unit)return 1;f->length=f->count*unit;
    if(f->length<=4)mc(f->value,e+8,f->length);else if(read(ctx,m32(e+8),f->value,f->length))return -1;
    if(f->type==2&&f->value[f->length-1]!=0)return 1;
    if(f->type==5||f->type==10)for(uint32_t i=0;i<f->count;i++)if(!m32(f->value+8*i+4))return 1;
    return 0;
}
static int capture_metadata(void *ctx,MetaRead read,CaptureMetadata *out){
    uint8_t h[12];mz(out,sizeof(*out));if(read(ctx,0,h,8)||h[0]!='I'||h[1]!='I'||m16(h+2)!=42)return -1;
    uint32_t root=m32(h+4),exif=0,next=0;
    if(read(ctx,root,h,2))return -1;uint32_t n=m16(h);if(n>512)return -1;
    for(uint32_t i=0;i<n;i++){
        if(read(ctx,(uint64_t)root+2+i*12,h,12))return -1;uint16_t t=m16(h);
        if(t==0x8769){if(m16(h+2)!=4||m32(h+4)!=1)return -1;exif=m32(h+8);}
        if(selected_root(t)){if(out->nr>=16)return -1;int k=capture_field(ctx,read,h,&out->root[out->nr]);if(k<0)return -1;if(k)out->omitted++;else out->nr++;}
    }
    if(read(ctx,(uint64_t)root+2+n*12,h,4))return -1;next=m32(h);
    if(!exif||read(ctx,exif,h,2))return -1;n=m16(h);if(n>512)return -1;
    for(uint32_t i=0;i<n;i++){
        if(read(ctx,(uint64_t)exif+2+i*12,h,12))return -1;
        if(selected_exif(m16(h))){if(out->ne>=56)return -1;int k=capture_field(ctx,read,h,&out->exif[out->ne]);if(k<0)return -1;if(k)out->omitted++;else out->ne++;}
    }
    if(next){if(read(ctx,next,h,2))return -1;n=m16(h);if(n>512)return -1;
        for(uint32_t i=0;i<n;i++){if(read(ctx,(uint64_t)next+2+i*12,h,12))return -1;
            if(m16(h)==0x0201&&m16(h+2)==4&&m32(h+4)==1)out->thumbnail_offset=m32(h+8);
            if(m16(h)==0x0202&&m16(h+2)==4&&m32(h+4)==1)out->thumbnail_bytes=m32(h+8);
        }
    }
    return 0;
}
#endif
