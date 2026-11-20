#define _FILE_OFFSET_BITS 64
#include "arw_u16.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define seek64 _fseeki64
#define tell64 _ftelli64
static FILE *open_utf8(const char *path) {
    wchar_t wide[4096];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,4096))return NULL;
    return _wfopen(wide,L"rb");
}
#else
#define seek64 fseeko
#define tell64 ftello
static FILE *open_utf8(const char *path){return fopen(path,"rb");}
#endif
static uint16_t u16(const uint8_t *b){return (uint16_t)(b[0]|(uint16_t)b[1]<<8);}
static uint32_t u32(const uint8_t *b){return u16(b)|(uint32_t)u16(b+2)<<16;}
static int read_at(ArwU16 *s,uint64_t at,void *p,size_t n,int metadata) {
    if(!n||n>65536||at>s->file_bytes||n>s->file_bytes-at||at>INT64_MAX)return -1;
    if(metadata&&(s->metadata_read_bytes>65536||n>65536-s->metadata_read_bytes))return -1;
    if(seek64(s->file,(int64_t)at,SEEK_SET)||fread(p,1,n,s->file)!=n)return -1;
    if(metadata)s->metadata_read_bytes+=n;
    if(n>s->max_read_request)s->max_read_request=(uint32_t)n;
    return 0;
}
static int numbers(ArwU16 *s,const uint8_t e[12],uint32_t out[4],unsigned expected) {
    uint16_t type=u16(e+2);uint32_t n=u32(e+4);unsigned size=type==1?1:(type==3||type==8)?2:type==4?4:0;
    if(!size||!n||n>4||(expected&&n!=expected))return -1;
    uint8_t local[16];const uint8_t *p=e+8;
    if(n*size>4){if(read_at(s,u32(e+8),local,n*size,1))return -1;p=local;}
    for(uint32_t i=0;i<n;i++){
        out[i]=size==1?p[i]:size==2?u16(p+i*size):u32(p+i*size);
        if(type==8&&(out[i]&0x8000))return -1;
    }
    return 0;
}
void arw_u16_close(ArwU16 *s){if(s&&s->file){fclose(s->file);s->file=NULL;}}
int arw_u16_open(ArwU16 *s,const char *path) {
    if(!s||!path)return -1;memset(s,0,sizeof(*s));
    s->file=open_utf8(path);if(!s->file)return -1;
    uint8_t header[8];uint32_t queue[32],seen[32];unsigned used=0,head=0,tail=0,raw_count=0;
    char model[64]={0},make[64]={0};
    if(seek64(s->file,0,SEEK_END))goto fail;
    int64_t length=tell64(s->file);if(length<8)goto fail;s->file_bytes=(uint64_t)length;
    if(read_at(s,0,header,8,1)||memcmp(header,"II\x2a\0",4))goto fail;
    queue[tail++]=u32(header+4);
    while(head<tail){
        uint32_t at=queue[head++];if(!at)continue;
        if(used>=32)goto fail;
        for(unsigned k=0;k<used;k++)if(seen[k]==at)goto fail;
        seen[used++]=at;
        uint8_t b[12];if(read_at(s,at,b,2,1))goto fail;
        unsigned count=u16(b);if(count>512||(uint64_t)at+2+(uint64_t)count*12+4>s->file_bytes)goto fail;
        uint32_t w=0,h=0,bits=0,compression=0,photo=0,samples=0,planar=0,rows=0;
        uint32_t offset=0,bytes=0,cfa[4]={9,9,9,9},repeat[4]={0},crop_origin[4]={0},crop_size[4]={0},black[4]={0},wb[4]={0},white=0;
        uint16_t relevant_seen[32];unsigned relevant_count=0;int have_crop=0,have_black=0,have_wb=0;
        for(unsigned i=0;i<count;i++){
            if(read_at(s,(uint64_t)at+2+i*12,b,12,1))goto fail;
            uint16_t tag=u16(b),type=u16(b+2);uint32_t n=u32(b+4),values[4]={0};
            if((tag==271||tag==272)&&head==1){
                char *out=tag==271?make:model;
                if(type!=2||!n||n>=64)goto fail;
                if(n<=4)memcpy(out,b+8,n);else if(read_at(s,u32(b+8),out,n,1))goto fail;
                if(out[n-1])goto fail;continue;
            }
            if(tag==330){
                if(numbers(s,b,values,0))goto fail;
                for(uint32_t k=0;k<n;k++){if(tail>=32)goto fail;queue[tail++]=values[k];}continue;
            }
            switch(tag){
                case 256:case 257:case 258:case 259:case 262:case 273:case 277:case 278:case 279:case 284:
                case 33421:case 33422:case 50717:case 50719:case 50720:case 0x7310:case 0x7313:break;
                default:continue;
            }
            if(relevant_count>=32)goto fail;
            for(unsigned k=0;k<relevant_count;k++)if(relevant_seen[k]==tag)goto fail;
            relevant_seen[relevant_count++]=tag;
            /* Non-CFA preview IFDs may have e.g. 3 BitsPerSample values. */
            if(numbers(s,b,values,0))goto fail;
            switch(tag){
                case 256:w=values[0];break;case 257:h=values[0];break;
                case 258:bits=values[0];break;case 259:compression=values[0];break;
                case 262:photo=values[0];break;case 273:if(n!=1)goto fail;offset=values[0];break;
                case 277:samples=values[0];break;case 278:rows=values[0];break;
                case 279:if(n!=1)goto fail;bytes=values[0];break;case 284:planar=values[0];break;
                case 33421:if(n!=2)goto fail;memcpy(repeat,values,8);break;
                case 33422:if(n!=4)goto fail;memcpy(cfa,values,16);break;
                case 50717:if(n!=1)goto fail;white=values[0];break;
                case 50719:if(n!=2)goto fail;memcpy(crop_origin,values,8);have_crop|=1;break;
                case 50720:if(n!=2)goto fail;memcpy(crop_size,values,8);have_crop|=2;break;
                case 0x7310:if(n!=4)goto fail;memcpy(black,values,16);have_black=1;break;
                case 0x7313:if(n!=4)goto fail;memcpy(wb,values,16);have_wb=1;break;
            }
        }
        if(read_at(s,(uint64_t)at+2+count*12,b,4,1))goto fail;
        uint32_t next=u32(b);if(next){if(tail>=32)goto fail;queue[tail++]=next;}
        if(photo!=32803)continue;
        if(++raw_count!=1||!w||!h||w>16384||h>16384||(bits!=14&&bits!=16)||compression!=1||samples!=1||planar!=1||rows!=h)goto fail;
        if((uint64_t)w*h*2!=bytes||offset>s->file_bytes||bytes>s->file_bytes-offset)goto fail;
        if(repeat[0]!=2||repeat[1]!=2||have_crop!=3||!have_black||!have_wb||!white||white>((1U<<bits)-1))goto fail;
        unsigned colors[3]={0};for(unsigned k=0;k<4;k++){if(cfa[k]>2)goto fail;colors[cfa[k]]++;if(black[k]>=white||!wb[k])goto fail;}
        if(colors[0]!=1||colors[1]!=2||colors[2]!=1||!crop_size[0]||!crop_size[1]||crop_origin[0]>=w||crop_origin[1]>=h||crop_size[0]>w-crop_origin[0]||crop_size[1]>h-crop_origin[1])goto fail;
        s->width=w;s->height=h;s->bits=bits;s->offset=offset;s->payload_bytes=bytes;s->white=white;
        memcpy(s->cfa,cfa,16);memcpy(s->black,black,16);memcpy(s->wb_rggb,wb,16);
        s->crop[0]=crop_origin[0];s->crop[1]=crop_origin[1];s->crop[2]=crop_size[0];s->crop[3]=crop_size[1];
    }
    if(raw_count!=1||strcmp(make,"SONY")||strcmp(model,"ILCE-7CM2"))goto fail;
    return 0;
fail:arw_u16_close(s);return -1;
}
int arw_u16_row(ArwU16 *s,uint32_t y,uint16_t *out,size_t samples) {
    if(!s||!s->file||!out||y>=s->height||samples<s->width)return -1;
    if(read_at(s,s->offset+(uint64_t)y*s->width*2,out,(size_t)s->width*2,0))return -1;
    uint32_t limit=(1U<<s->bits)-1;
    for(uint32_t x=0;x<s->width;x++){out[x]=u16((const uint8_t*)out+x*2);if(out[x]>limit)return -1;}
    return 0;
}
