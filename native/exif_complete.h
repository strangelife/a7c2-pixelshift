#ifndef EXIF_COMPLETE_H
#define EXIF_COMPLETE_H
#include "capture_metadata.h"
static MetaField mr[16],me[64],mi[4],mt[8];
static uint32_t mrn,men,min,mtn;
static int mf_add(MetaField *list,uint32_t *n,uint32_t max,uint16_t tag,uint16_t type,uint32_t count,const void *data,uint32_t length){
    if(*n>=max||length>128)return -1;MetaField *f=&list[(*n)++];mz(f,sizeof(*f));f->tag=tag;f->type=type;f->count=count;f->length=length;mc(f->value,data,length);return 0;
}
static int mf_num(MetaField *list,uint32_t *n,uint32_t max,uint16_t tag,uint16_t type,uint32_t value){uint8_t b[4];mw32(b,value);return mf_add(list,n,max,tag,type,1,b,type==3?2:4);}
static int mf_text(MetaField *list,uint32_t *n,uint32_t max,uint16_t tag,const char *text){uint32_t len=0;while(len<127&&text[len])len++;if(text[len])return -1;return mf_add(list,n,max,tag,2,len+1,text,len+1);}
static int mf_present(MetaField *l,uint32_t n,uint16_t tag){for(uint32_t i=0;i<n;i++)if(l[i].tag==tag)return 1;return 0;}
static int mf_ifd(uint8_t *t,uint32_t capacity,uint32_t at,MetaField *fields,uint32_t n,uint32_t next,uint32_t *end){
    for(uint32_t i=1;i<n;i++){MetaField v=fields[i];uint32_t j=i;while(j&&fields[j-1].tag>v.tag){fields[j]=fields[j-1];j--;}fields[j]=v;}
    if(at>capacity||2+12*n+4>capacity-at)return -1;mw16(t+at,n);uint32_t p=at+2;
    for(uint32_t i=0;i<n;i++,p+=12){MetaField *f=&fields[i];if(i&&fields[i-1].tag==f->tag)return -1;
        mw16(t+p,f->tag);mw16(t+p+2,f->type);mw32(t+p+4,f->count);
        if(f->length<=4)mc(t+p+8,f->value,f->length);
        else{*end=(*end+1)&~1U;if(*end>capacity||f->length>capacity-*end)return -1;mw32(t+p+8,*end);mc(t+*end,f->value,f->length);*end+=f->length;}
    }
    mw32(t+p,next);return 0;
}
static int exif_complete(uint8_t *out,uint32_t capacity,uint32_t *used,uint32_t width,uint32_t height,const CaptureMetadata *source,const uint8_t *thumbnail,uint32_t thumb_bytes,const char *description){
    if(!out||!source||!thumbnail||capacity<4096||thumb_bytes>16000||width>65535||height>65535)return -1;
    mz(out,capacity);mrn=men=min=mtn=0;
    for(uint32_t i=0;i<source->nr;i++)mr[mrn++]=source->root[i];
    for(uint32_t i=0;i<source->ne;i++)me[men++]=source->exif[i];
    enum{ROOT_IFD=8,EXIF_IFD=256,INTEROP_IFD=1152,THUMB_IFD=1216,DATA=1408};
    uint8_t resolution[8];mw32(resolution,72);mw32(resolution+4,1);
#define ADD(expr) do{if(expr)return -1;}while(0)
    if(!mf_present(mr,mrn,0x010f))ADD(mf_text(mr,&mrn,16,0x010f,"SONY"));
    if(!mf_present(mr,mrn,0x0110))ADD(mf_text(mr,&mrn,16,0x0110,"ILCE-7CM2"));
    if(!mf_present(mr,mrn,0x0112))ADD(mf_num(mr,&mrn,16,0x0112,3,1));
    ADD(mf_text(mr,&mrn,16,0x010e,description));
    ADD(mf_add(mr,&mrn,16,0x011a,5,1,resolution,8));ADD(mf_add(mr,&mrn,16,0x011b,5,1,resolution,8));ADD(mf_num(mr,&mrn,16,0x0128,3,2));
    ADD(mf_text(mr,&mrn,16,0x0131,"A7C2 PixelShift Research"));ADD(mf_num(mr,&mrn,16,0x0213,3,1));ADD(mf_num(mr,&mrn,16,0x8769,4,EXIF_IFD));
    const uint8_t components[4]={1,2,3,0};
    if(!mf_present(me,men,0x9000))ADD(mf_add(me,&men,64,0x9000,7,4,"0232",4));
    ADD(mf_add(me,&men,64,0x9101,7,4,components,4));ADD(mf_add(me,&men,64,0xa000,7,4,"0100",4));
    ADD(mf_num(me,&men,64,0xa001,3,1));ADD(mf_num(me,&men,64,0xa002,4,width));ADD(mf_num(me,&men,64,0xa003,4,height));ADD(mf_num(me,&men,64,0xa005,4,INTEROP_IFD));
    ADD(mf_text(mi,&min,4,0x0001,"R98"));ADD(mf_add(mi,&min,4,0x0002,7,4,"0100",4));
    ADD(mf_num(mt,&mtn,8,0x0103,3,6));ADD(mf_num(mt,&mtn,8,0x0112,3,1));
    ADD(mf_add(mt,&mtn,8,0x011a,5,1,resolution,8));ADD(mf_add(mt,&mtn,8,0x011b,5,1,resolution,8));ADD(mf_num(mt,&mtn,8,0x0128,3,2));
    ADD(mf_num(mt,&mtn,8,0x0201,4,0));ADD(mf_num(mt,&mtn,8,0x0202,4,thumb_bytes));
    uint32_t orientation=1;for(uint32_t i=0;i<mrn;i++)if(mr[i].tag==0x0112)orientation=m16(mr[i].value);mw16(mt[1].value,orientation);
    if(2+12*mrn+4+ROOT_IFD>EXIF_IFD||2+12*men+4+EXIF_IFD>INTEROP_IFD)return -1;
    mc(out,"Exif\0\0",6);uint8_t *t=out+6;uint32_t cap=capacity-6,pos=DATA;t[0]=t[1]='I';mw16(t+2,42);mw32(t+4,ROOT_IFD);
    ADD(mf_ifd(t,cap,ROOT_IFD,mr,mrn,THUMB_IFD,&pos));ADD(mf_ifd(t,cap,EXIF_IFD,me,men,0,&pos));ADD(mf_ifd(t,cap,INTEROP_IFD,mi,min,0,&pos));
    /* Reserve thumbnail IFD external values before setting JPEG offset. */
    uint32_t thumb_start=(pos+16+1)&~1U;
    for(uint32_t i=0;i<mtn;i++)if(mt[i].tag==0x0201)mw32(mt[i].value,thumb_start);
    ADD(mf_ifd(t,cap,THUMB_IFD,mt,mtn,0,&pos));
    if(pos>thumb_start||thumb_start>cap||thumb_bytes>cap-thumb_start)return -1;
    mc(t+thumb_start,thumbnail,thumb_bytes);*used=6+thumb_start+thumb_bytes;
#undef ADD
    return *used<=65533?0:-1;
}
#endif
