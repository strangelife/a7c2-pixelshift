/* Read-only Fsys adapter for the existing ARW and MakerNote parsers.
 * FILE pointers below are opaque tokens used only by our local adapter.
 * No libc FILE object is dereferenced; no external stdio is called.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <string.h>
#ifdef SD_WRITE_HELPERS
#include "sd_write_helpers.h"
#else
#include "sd_read_helpers.h"
#endif
#include "metadata_candidates.h"
void *memcpy(void *d,const void *s,size_t n){u8 *a=d;const u8 *b=s;for(size_t i=0;i<n;i++)a[i]=b[i];return d;}
void *memset(void *d,int c,size_t n){u8 *a=d;for(size_t i=0;i<n;i++)a[i]=(u8)c;return d;}
int memcmp(const void *a,const void *b,size_t n){const u8 *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return x[i]<y[i]?-1:1;return 0;}
int strcmp(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return (u8)*a-(u8)*b;}
struct SdFile {i64 handle;u64 position,bytes;int active,close_error;};
static struct SdFile file;static u64 fs_base;
static int camera_fclose(FILE *opaque){
    if((void*)opaque!=(void*)&file||!file.active)return -1;
    int code=((int(*)(i64))(fs_base+FS_CLOSE_VA))(file.handle);
    file.close_error=code;file.active=0;return code;
}
static int camera_fseeko(FILE *opaque,off_t offset,int origin){
    if((void*)opaque!=(void*)&file||!file.active||origin<0||origin>2)return -1;
    u64 pos=0;int code=((int(*)(i64,i64,u32,u64*))(fs_base+FS_SEEK_VA))(file.handle,offset,(u32)origin,&pos);
    if(code||pos>file.bytes)return -1;file.position=pos;return 0;
}
static off_t camera_ftello(FILE *opaque){return (void*)opaque==(void*)&file&&file.active?(off_t)file.position:-1;}
static size_t camera_fread(void *out,size_t size,size_t count,FILE *opaque){
    if((void*)opaque!=(void*)&file||!file.active||!size||count>65536/size)return 0;
    size_t total=size*count;if(file.position>file.bytes||total>file.bytes-file.position)return 0;
    int n=((int(*)(i64,void*,u32))(fs_base+FS_READ_VA))(file.handle,out,(u32)total);
    if(n<0||(u32)n>total)return 0;file.position+=(u32)n;return (size_t)n/size;
}
static FILE *camera_fopen(const char *path,const char *mode){
    if(file.active||file.close_error||strcmp(mode,"rb"))return NULL;
    i64 handle=0;int code=((int(*)(const char*,i64*,u32))(fs_base+FS_OPEN_VA))(path,&handle,1);
    if(code||!handle)return NULL;
    file.handle=handle;file.active=1;file.position=0;file.bytes=0;
    code=((int(*)(i64,u32))(fs_base+FS_LOCK_VA))(handle,1);
    if(!code)code=((int(*)(i64,i64,u32,u64*))(fs_base+FS_SEEK_VA))(handle,0,2,&file.bytes);
    if(code||file.bytes<8||file.bytes>268435456UL||camera_fseeko((FILE*)&file,0,0)){
        camera_fclose((FILE*)&file);return NULL;
    }
    return (FILE*)&file;
}
#define fopen camera_fopen
#define fread camera_fread
#define fclose camera_fclose
#define fseeko camera_fseeko
#define ftello camera_ftello
#define u32 arw_le32
#include "arw_u16.c"
#undef u32
#include "capture_info.c"
#ifndef SD_METADATA_HELPERS_ONLY
struct Metadata {u8 path[96];i64 raw_status,capture_status,close_status;u64 bytes,offset,payload;u32 width,height,bits,black[4],white,crop[4],wb[4];CaptureInfo capture;};
_Static_assert(sizeof(struct Metadata)==264,"metadata record");
static struct Metadata metadata;
__attribute__((visibility("default"))) void _init(void){
    struct Report r={{'P','S','M','E','T','A','0','1'},1,64,-125,0,0,0,0,0,0};
    r.pid=sc(172,0,0,0,0,0,0);u64 start=now();
    i64 out=sc(56,-100,(i64)"/tmp/a7c2-pixelshift/a7c2.probe.result",0xc1,0600,0,0);if(out<0)return;
    if(sc(64,out,(i64)&r,sizeof(r),0,0,0)!=(i64)sizeof(r)){sc(57,out,0,0,0,0,0);return;}
    r.cleanup=0; /* Loader lifecycle is owned by the integrator. */
    i64 fd=sc(56,-100,(i64)"/proc/self/maps",0,0,0,0);if(fd<0){r.status=fd;goto finish;}
    u32 used=0,retries=0;int bad=0;
    while(used<sizeof(maps)){
        i64 n=sc(63,fd,(i64)(maps+used),sizeof(maps)-used,0,0,0);
        if(n==-4&&retries++<3)continue;
        if(n<0||(u64)n>sizeof(maps)-used){bad=1;break;}if(!n)break;used+=(u32)n;
    }
    sc(57,fd,0,0,0,0,0);
    if(bad||used==sizeof(maps)||find_base(used,&r.base)){r.status=-22;goto finish;}
    for(u32 i=0;i<sizeof(fs_signatures)/sizeof(fs_signatures[0]);i++)
        if(!eq((const u8*)(r.base+fs_signatures[i].va),fs_signatures[i].bytes,fs_signatures[i].size)){r.status=-84;goto finish;}
    if(!eq((const u8*)(r.base+QUERY_VA),query_signature,QUERY_BYTES)){r.status=-84;goto finish;}
    r.mask=((u32(*)(void))(r.base+QUERY_VA))();
    if(!r.mask||(r.mask&(r.mask-1))||(r.mask&0xfc000000U)){r.status=-19;goto finish;}
    u32 drive=0;while(drive<26&&!(r.mask&(1U<<drive)))drive++;
    fs_base=r.base;r.status=0;
    for(u32 i=0;i<CANDIDATE_COUNT;i++){
        memset(&metadata,0,sizeof(metadata));
        const u8 prefix[]="C:/DCIM/101MSDCF/DSC";u32 p=0;
        for(;p<sizeof(prefix)-1;p++)metadata.path[p]=prefix[p];metadata.path[0]=(u8)('A'+drive);
        u32 number=candidate_numbers[i];for(u32 k=0;k<5;k++){metadata.path[p+4-k]=(u8)('0'+number%10);number/=10;}
        p+=5;memcpy(metadata.path+p,".ARW",5);
        metadata.capture_status=-1;FILE *f=camera_fopen((const char*)metadata.path,"rb");
        if(f){
            ArwU16 view;memset(&view,0,sizeof(view));view.file=f;view.file_bytes=file.bytes;
            metadata.bytes=file.bytes;metadata.capture_status=ps_capture_info(&view,&metadata.capture);camera_fclose(f);
        }
        ArwU16 arw;metadata.raw_status=arw_u16_open(&arw,(const char*)metadata.path);
        if(!metadata.raw_status){
            metadata.bytes=arw.file_bytes;metadata.offset=arw.offset;metadata.payload=arw.payload_bytes;
            metadata.width=arw.width;metadata.height=arw.height;metadata.bits=arw.bits;metadata.white=arw.white;
            memcpy(metadata.black,arw.black,sizeof(arw.black));memcpy(metadata.crop,arw.crop,sizeof(arw.crop));memcpy(metadata.wb,arw.wb_rggb,sizeof(arw.wb_rggb));arw_u16_close(&arw);
        }
        metadata.close_status=file.close_error;r.called++;
        if(sc(64,out,(i64)&metadata,sizeof(metadata),0,0,0)!=(i64)sizeof(metadata)){r.status=-5;break;}
        if(file.active||file.close_error){r.status=-9;break;}
        u64 t=now();if(t<start||t-start>3000000000UL){r.status=-110;break;}
    }
finish:
    r.bytes=64+r.called*264;{u64 end=now();r.elapsed_ns=end>=start?end-start:0;}
    sc(68,out,(i64)&r,sizeof(r),0,0,0);sc(82,out,0,0,0,0,0);sc(57,out,0,0,0,0,0);
}
#endif
