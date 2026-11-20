#define _FILE_OFFSET_BITS 64
#include "capture_info.h"
#include <string.h>
#ifdef _WIN32
#define seek64 _fseeki64
#else
#define seek64 fseeko
#endif
static uint16_t le16(const uint8_t *p){return (uint16_t)(p[0]|(unsigned)p[1]<<8);}
static uint32_t le32(const uint8_t *p){return le16(p)|(uint32_t)le16(p+2)<<16;}
static int at(ArwU16 *s,uint64_t offset,void *p,size_t n){
    if(!s||!s->file||offset>s->file_bytes||n>s->file_bytes-offset||n>64)return -1;
    return seek64(s->file,(int64_t)offset,SEEK_SET)||fread(p,1,n,s->file)!=n?-1:0;
}
static int entry(ArwU16 *s,uint32_t directory,uint16_t target,uint8_t found[12]){
    uint8_t data[12];if(at(s,directory,data,2))return -1;
    unsigned count=le16(data),hits=0;if(count>512||(uint64_t)directory+2+(uint64_t)count*12+4>s->file_bytes)return -1;
    for(unsigned i=0;i<count;i++){
        if(at(s,(uint64_t)directory+2+i*12,data,12))return -1;
        if(le16(data)==target){memcpy(found,data,12);hits++;}
    }
    return hits==1?0:-1;
}
static int rational(ArwU16 *s,uint32_t ifd,uint16_t tag,uint32_t *num,uint32_t *den){
    uint8_t e[12],value[8];if(entry(s,ifd,tag,e)||le16(e+2)!=5||le32(e+4)!=1||at(s,le32(e+8),value,8))return -1;
    *num=le32(value);*den=le32(value+4);return !*num||!*den?-1:0;
}
int ps_capture_info(ArwU16 *s,CaptureInfo *info){
    uint8_t e[12],data[64];if(!s||!info)return -1;memset(info,0,sizeof(*info));info->orientation=1;
    if(at(s,0,data,8)||memcmp(data,"II\x2a\0",4))return -1;
    uint32_t root=le32(data+4);
    if(!entry(s,root,274,e)){
        if(le16(e+2)!=3||le32(e+4)!=1)return -1;info->orientation=le16(e+8);
        if(info->orientation<1||info->orientation>8)return -1;
    }
    if(entry(s,root,34665,e)||le16(e+2)!=4||le32(e+4)!=1)return -1;
    uint32_t exif=le32(e+8);
    if(rational(s,exif,33434,&info->shutter_num,&info->shutter_den)||rational(s,exif,33437,&info->aperture_num,&info->aperture_den))return -1;
    if(entry(s,exif,34855,e)||le16(e+2)!=3||le32(e+4)!=1||!(info->iso=le16(e+8)))return -1;
    if(entry(s,exif,36867,e)||le16(e+2)!=2||le32(e+4)!=20||at(s,le32(e+8),info->datetime,20)||info->datetime[19])return -1;
    for(unsigned i=0;i<19;i++)if(info->datetime[i]<32||info->datetime[i]>126)return -1;
    if(entry(s,exif,37500,e)||le16(e+2)!=7)return -1;
    uint32_t size=le32(e+4),maker=le32(e+8),base=maker;
    if(size<6||maker>s->file_bytes||size>s->file_bytes-maker||at(s,maker,data,size<12?size:12))return -1;
    if(size>=12&&(!memcmp(data,"SONY DSC ",9)||!memcmp(data,"SONY CAM ",9)))base+=12;
    if(entry(s,base,0x202f,e)||le16(e+2)!=7||le32(e+4)!=6)return -1;
    uint32_t pos=le32(e+8);if(pos<maker||pos>(uint64_t)maker+size-6||at(s,pos,data,6))return -1;
    info->group=le32(data);info->frame=data[4];info->total=data[5];
    if(!info->group||(info->total!=4&&info->total!=16)||!info->frame||info->frame>info->total)return -1;
    return 0;
}
