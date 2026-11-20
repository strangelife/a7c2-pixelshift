/* Native synchronous host for the shared 256-row JPG pipeline. All SD I/O
 * stays on this caller; the optional compute worker is joined before the
 * constructor returns. No AP/CP/settings writes or retained callbacks.
 */
#define SD_METADATA_HELPERS_ONLY
#define SD_WRITE_HELPERS
#include "sd_metadata.c"
#undef fopen
#undef fread
#undef fclose
#undef fseeko
#undef ftello
#include "stream_pipeline.h"
#include "crc_neon.h"
#include "exif_complete.h"
#include <stdatomic.h>
#include "three_trace.h"
ThreeTrace three_trace;
_Static_assert(sizeof(ThreeTrace)==459088,"trace ABI");

struct JpegReport {
    u8 magic[8];u32 version,bytes;i64 status,cleanup;
    u64 elapsed_ns,mem_before,mem_min,mem_after,sd_free,raw_bytes,body_bytes,final_bytes,readback_bytes;
    u64 prepare_ns,pipeline_ns,thumbnail_ns,wrap_ns,flush_ns,readback_ns,publish_ns,tail_ns;
    u64 body_write_bytes,wrap_copy_bytes,header_write_bytes;
    u32 width,height,frames,bands,linear_crc,body_crc,final_crc,readback_crc,flags,thumbnail_bytes;
    u8 path[96];A7c2PipelineStats stats;u32 source_crc[16];
};
_Static_assert(sizeof(A7c2PipelineStats)==120,"pipeline stats ABI");
_Static_assert(sizeof(struct JpegReport)==512,"native JPG report ABI");
static struct JpegReport report;
static FrameSource descriptors[16];static ArwU16 sources[16];static CaptureInfo captures[16];
static CaptureMetadata capture_extra;
static u8 paths[16][32],find_info[3112],io[65536],preview[160*120*3],thumb_jpeg[16000];
static u8 app1[16386],header[16386],curves[3][256],body_soi[2];
static u32 thumb_bytes,header_bytes,body_soi_bytes,body_tail_crc,crc_table[256];
static int hardware_crc,input_active,output_active,temp_dir_owned,dcf_dir_owned,temp_owned,final_owned;
static i64 input_handle,output_handle,owner_tid;static _Atomic int error;static u64 started;
static u8 temp_directory[]="C:/" TEST_DIRECTORY;
static u8 temp_path[]="C:/" TEST_DIRECTORY "/BODY.JPG";
static u8 dcf_directory[]="C:/DCIM/" TEST_DCF;
static u8 stage_path[]="C:/DCIM/" TEST_DCF "/INPROG.TMP";
static u8 final_path[]="C:/DCIM/" TEST_DCF "/DSC00001.JPG";
static const volatile u32 expected_linear_crc=0x3d757bedU;
static const u64 RENAME_VA=0x33e7cUL;
#include "rename_signature.inc"

static u32 crc_add(u32 c,const u8 *p,u32 n){if(hardware_crc)return crc32_arm(c,p,n);while(n--)c=crc_table[(c^*p++)&255]^(c>>8);return c;}
static int expired(void){u64 t=now();if(t<started||t-started>300000000000UL){error=-110;return 1;}return 0;}
static u64 available(void){
    i64 fd=sc(56,-100,(i64)"/proc/meminfo",0,0,0,0);if(fd<0)return 0;
    i64 n=sc(63,fd,(i64)maps,4096,0,0,0);sc(57,fd,0,0,0,0,0);if(n<16||n>4096)return 0;
    const u8 key[]="MemAvailable:";
    for(u32 i=0;i+sizeof(key)<(u32)n;i++)if(eq(maps+i,key,sizeof(key)-1)){
        u32 p=i+sizeof(key)-1;while(p<(u32)n&&maps[p]==' ')p++;u64 value=0;u32 digits=0;
        while(p<(u32)n&&maps[p]>='0'&&maps[p]<='9'){if(++digits>12)return 0;value=value*10+maps[p++]-'0';}
        return digits?value*1024:0;
    }return 0;
}
static int raw_window_active,raw_window_frame;static u32 raw_window_index,raw_window_prefix;
static int close_in(void){if(!input_active)return 0;int c=((int(*)(i64))(fs_base+FS_CLOSE_VA))(input_handle);input_active=0;if(c&&!error)error=c;return c;}
static int close_out(void){if(!output_active)return 0;int c=((int(*)(i64))(fs_base+FS_CLOSE_VA))(output_handle);output_active=0;if(c&&!error)error=c;return c;}
static int seek_handle(i64 handle,u64 at){u64 pos=0;int c=((int(*)(i64,i64,u32,u64*))(fs_base+FS_SEEK_VA))(handle,(i64)at,0,&pos);if(c||pos!=at){error=c?c:-5;return -1;}return 0;}
static int read_exact(i64 handle,void *destination,u32 size){
    for(u32 at=0;at<size;){if(error||expired())return -1;
        u32 wanted=size-at>10813696?10813696:size-at;
        u64 read_tick=raw_window_active?now():0;
        int n=((int(*)(i64,void*,u32))(fs_base+FS_READ_VA))(handle,(u8*)destination+at,wanted);
        if(raw_window_active){
            three_trace.extra[X_IO_CALLS]++;if(n>0&&(u32)n<wanted)three_trace.extra[X_SHORT_READS]++;
            if(wanted>=TEST_READ_BYTES){three_trace.extra[X_BIG_REQUESTS]++;if(n==(int)wanted)three_trace.extra[X_BIG_FULL_RETURNS]++;}
            if(wanted>three_trace.extra[X_MAX_REQUEST])three_trace.extra[X_MAX_REQUEST]=wanted;
            u64 id=three_trace.extra[X_LOGGED];if(id<16384){three_trace.reads[id]=(ReadCall){wanted,n,raw_window_index,(uint16_t)raw_window_frame,at==0?(uint16_t)raw_window_prefix:0,now()-read_tick};three_trace.extra[X_LOGGED]++;}else three_trace.extra[X_DROPPED]++;
        }
        if(n<=0||(u32)n>wanted){error=-5;return -1;}at+=(u32)n;
    }return 0;
}
static int write_exact(const void *data,u32 size){
    for(u32 at=0,tries=0;at<size;){if(error||expired()||++tries>128)return -1;
        u32 wanted=size-at>65536?65536:size-at;
        int n=((int(*)(i64,const void*,u32))(fs_base+FS_WRITE_VA))(output_handle,(const u8*)data+at,wanted);
        if(n<=0||(u32)n>wanted){error=-5;return -1;}at+=(u32)n;
    }return 0;
}
static int open_input(const u8 *path){
    int c=((int(*)(const u8*,i64*,u32))(fs_base+FS_OPEN_VA))(path,&input_handle,1);
    if(c||!input_handle){error=c?c:-9;return -1;}input_active=1;
    c=((int(*)(i64,u32))(fs_base+FS_LOCK_VA))(input_handle,1);if(c){error=c;return -1;}return 0;
}
static int open_output(const u8 *path){
    int c=((int(*)(const u8*,i64*,u32))(fs_base+FS_OPEN_VA))(path,&output_handle,6);
    if(c||!output_handle){error=c?c:-9;return -1;}output_active=1;
    c=((int(*)(i64,u32))(fs_base+FS_LOCK_VA))(output_handle,1);if(c){error=c;return -1;}return 0;
}
static int mkdir_absent(const u8 *path){
    int c=((int(*)(const u8*,u32,u8*))(fs_base+FS_FIND_FIRST_VA))(path,0x10,find_info);
    if(c!=2){error=c?c:-17;return -1;}
    c=((int(*)(const u8*))(fs_base+FS_MKDIR_VA))(path);if(c){error=c;return -1;}return 0;
}
static int flush_close(void){
    u64 tick=now();int c=((int(*)(i64))(fs_base+FS_FLUSH_VA))(output_handle);report.flush_ns+=now()-tick;
    if(c){error=c;return -1;}report.flags|=4;return close_out();
}
static int capture_read(void *ctx,uint64_t at,void *p,uint32_t n){ArwU16 *a=ctx;if(at>a->file_bytes||n>a->file_bytes-at||n>65536)return -1;return camera_fseeko(a->file,(off_t)at,0)||camera_fread(p,1,n,a->file)!=n?-1:0;}
static int load_curves(void){
    u8 p[792],extra=0;i64 fd=sc(56,-100,(i64)"/tmp/a7c2-pixelshift/ps-color-calibration.input",0,0,0,0);if(fd<0)return -1;
    i64 n=sc(63,fd,(i64)p,sizeof(p),0,0,0),tail=n==sizeof(p)?sc(63,fd,(i64)&extra,1,0,0,0):-1;sc(57,fd,0,0,0,0,0);
    if(n!=sizeof(p)||tail||memcmp(p,"PSCLR001",8)||m32(p+8)!=1||m32(p+12)!=792||m32(p+16)!=1256095283U||(crc_add(0xffffffffU,p+24,768)^0xffffffffU)!=m32(p+20))return -1;
    for(u32 c=0;c<3;c++)for(u32 i=1;i<256;i++)if(p[24+c*256+i]<p[24+c*256+i-1])return -1;
    memcpy(curves,p+24,768);return 0;
}
#include "source_pool.h"
#include "native_cache.h"
extern int fusion_run(const FrameSource*,const uint8_t (*)[256],const uint16_t *const*,uint16_t*,size_t,ReadRange,WriteBytes,ObserveBand,ObservePreview,void*,A7c2PipelineStats*);
int a7c2_source_crc(int frame,const uint16_t *data,size_t bytes){
    if(frame<0||frame>=16||bytes>2252800||error||expired())return -1;
    three_trace.extra[X_CRC_TID]=sc(178,0,0,0,0,0,0);u64 tick=now();
    report.source_crc[frame]=crc_add(report.source_crc[frame],(const u8*)data,(u32)bytes);
three_trace.counters[T_CRC]+=now()-tick;return 0;
}
static int read_range(void *context,int frame,uint64_t at,void *dest,size_t bytes){
    (void)context;if(sc(178,0,0,0,0,0,0)!=owner_tid||frame<0||frame>=16||bytes>10813440||error||expired()){if(!error)error=-22;return -1;}
    raw_window_index=(u32)three_trace.counters[T_READ_CALLS];raw_window_frame=frame;three_trace.counters[T_READ_CALLS]++;
    u64 tick=now();int code=0;i64 handle=0;
    if(three_trace.extra[X_POOL_ENABLED]&&pooled_active[frame]){handle=pooled[frame];}
    else{code=open_input(paths[frame]);three_trace.counters[T_OPEN]+=now()-tick;if(code){close_in();return -1;}handle=input_handle;}
    u32 prefix=(u32)(at&511);raw_window_prefix=prefix;
    if(at<prefix){error=-22;return -1;}
    tick=now();code=seek_handle(handle,at-prefix);three_trace.counters[T_SEEK]+=now()-tick;
    if(!code){tick=now();raw_window_active=1;code=read_exact(handle,(u8*)dest-prefix,(u32)bytes+prefix);raw_window_active=0;three_trace.counters[T_READ]+=now()-tick;}
    if(!pooled_active[frame]){tick=now();int closed=close_in();three_trace.counters[T_CLOSE]+=now()-tick;if(closed)code=-1;}
    if(code)return -1;report.raw_bytes+=bytes;
    tick=now();u64 mem=available();if(mem<report.mem_min)report.mem_min=mem;three_trace.counters[T_MEM]+=now()-tick;return 0;
}
static int observe(void *context,const uint16_t *pixels,size_t bytes){
    (void)context;if(bytes!=16*14016*6||error||expired()){if(!error)error=-22;return -1;}
    report.linear_crc=crc_add(report.linear_crc,(const u8*)pixels,(u32)bytes);report.bands++;return 0;
}
#include "mpf_preview.h"
static int preview_rows(void *context,int y,const uint8_t *pixels){
    (void)context;if(y<0||y+16>TEST_HEIGHT)return -1;u64 sample_tick=now();
    while(replay_next<REPLAY_ACTIVE){u32 sy=replay_next*TEST_HEIGHT/REPLAY_ACTIVE;if(sy>=(u32)y+16)break;if(sy<(u32)y)return -1;
        for(u32 x=0;x<1616;x++)for(u32 c=0;c<3;c++)replay[((replay_next+1)*1616+x)*3+c]=pixels[((sy-(u32)y)*14016+x*14016/1616)*3+c];
        replay_next++;
    }
    three_trace.counters[T_PREVIEW_SAMPLE]+=now()-sample_tick;
    for(u32 row=0;row<106;row++){u32 sy=row*TEST_HEIGHT/106;
        if(sy>=(u32)y&&sy<(u32)y+16)for(u32 x=0;x<160;x++)for(u32 c=0;c<3;c++)preview[((row+7)*160+x)*3+c]=pixels[((sy-(u32)y)*14016+x*14016/160)*3+c];
    }return 0;
}
static int emit_body(void *context,const void *bytes,size_t count){
    (void)context;if(sc(178,0,0,0,0,0,0)!=owner_tid||count>262144||report.body_bytes+count>134217728||error||expired()){if(!error)error=-22;return -1;}
    const u8 *p=bytes;u32 n=(u32)count;report.body_crc=crc_add(report.body_crc,p,n);report.body_bytes+=n;
    u32 skip=0;while(body_soi_bytes<2&&skip<n)body_soi[body_soi_bytes++]=p[skip++];
    if(body_soi_bytes==2&&(body_soi[0]!=255||body_soi[1]!=216)){error=-84;return -1;}
    body_tail_crc=crc_add(body_tail_crc,p+skip,n-skip);
    if(TEST_SINGLE_WRITE){p+=skip;n-=skip;}
    if(write_exact(p,n))return -1;report.body_write_bytes+=n;return 0;
}
static void thumbnail_sink(void *context,void *bytes,int count){
    (void)context;if(count<0||(u32)count>sizeof(thumb_jpeg)-thumb_bytes){error=-27;return;}
    memcpy(thumb_jpeg+thumb_bytes,bytes,(u32)count);thumb_bytes+=(u32)count;
}
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define PS_ENTROPY_BUFFER 4096
#define PS_FAST_JPEG_BITS 1
#define PS_BATCH_ENTROPY 1
#include "stb_422_batch.h"
static int make_metadata(void){
    u64 tick=now();if(!stbi_write_jpg_to_func(thumbnail_sink,NULL,160,120,3,preview,80)||error||!thumb_bytes){if(!error)error=-5;return -1;}
    report.thumbnail_ns=now()-tick;report.thumbnail_bytes=thumb_bytes;u32 payload=0;
    if(exif_complete(app1+4,sizeof(app1)-4,&payload,14016,TEST_HEIGHT,&capture_extra,thumb_jpeg,thumb_bytes,"16-frame pixel-shift composite; camera-preview tone calibration")||payload>16380){error=-22;return -1;}
    app1[0]=255;app1[1]=225;app1[2]=(u8)((payload+2)>>8);app1[3]=(u8)(payload+2);
    header[0]=255;header[1]=216;memcpy(header+2,app1,payload+4);header_bytes=payload+6;
    if(TEST_SINGLE_WRITE){header[2]=255;header[3]=225;header[4]=0x3f;header[5]=0xfe;header_bytes=16386;}
    return 0;
}
static int verify_final(const u8 *path,int single){
    (void)single;u64 tick=now();if(open_input(path))return -1;u64 size=0,pos=0;
    if(((int(*)(i64,i64,u32,u64*))(fs_base+FS_SEEK_VA))(input_handle,0,2,&size)||size!=report.final_bytes||seek_handle(input_handle,0)){if(!error)error=-84;close_in();return -1;}
    u32 full=0xffffffffU,body=0xffffffffU,replay_check=0xffffffffU;u8 last[2]={0,0};
    while(pos<size){u32 n=size-pos>sizeof(io)?sizeof(io):(u32)(size-pos);if(read_exact(input_handle,io,n)){close_in();return -1;}
        full=crc_add(full,io,n);
        for(u32 at=0;at<n;){u64 absolute=pos+at;u64 boundary=absolute<header_bytes?header_bytes:absolute<header_bytes+MPF_BYTES?header_bytes+MPF_BYTES:absolute<primary_bytes?primary_bytes:size;
            u32 part=(u32)(boundary-absolute);if(part>n-at)part=n-at;
            if(absolute<header_bytes){if(memcmp(io+at,header+absolute,part)){error=-84;close_in();return -1;}}
            else if(absolute<header_bytes+MPF_BYTES){if(memcmp(io+at,mpf+absolute-header_bytes,part)){error=-84;close_in();return -1;}}
            else if(absolute<primary_bytes)body=crc_add(body,io+at,part);else replay_check=crc_add(replay_check,io+at,part);at+=part;
        }
        pos+=n;if(n>=2){last[0]=io[n-2];last[1]=io[n-1];}
    }
    if(close_in())return -1;if(body!=body_tail_crc||replay_check!=replay_crc||last[0]!=255||last[1]!=217){error=-84;return -1;}
    report.final_crc=full;report.readback_crc=full^0xffffffffU;report.readback_bytes=pos;report.readback_ns+=now()-tick;report.flags|=16;return 0;
}
static int finish_single(void){
    primary_bytes=report.body_bytes-2+16386+MPF_BYTES;three_trace.counters[T_PRIMARY_BYTES]=primary_bytes;
    u64 tick=now();if(replay_next!=REPLAY_ACTIVE||a7c2_encode_replay(replay_rows,replay_sink,NULL)||!replay_bytes){if(!error)error=-5;return -1;}
    three_trace.counters[T_PREVIEW_ENCODE]=now()-tick;three_trace.counters[T_PREVIEW_BYTES]=replay_bytes;three_trace.counters[T_PREVIEW_CRC]=replay_crc^0xffffffffU;
    build_mpf();tick=now();if(seek_handle(output_handle,0)||write_exact(header,header_bytes)||write_exact(mpf,sizeof(mpf)))return -1;
    report.header_write_bytes+=header_bytes+sizeof(mpf);report.final_bytes=primary_bytes+replay_bytes;
    if(flush_close()||verify_final(stage_path,1))return -1;report.wrap_ns=now()-tick;tick=now();
    int code=((int(*)(const u8*,const u8*))(fs_base+RENAME_VA))(stage_path,(const u8*)"DSC00001.JPG");
    if(code){error=code;return -1;}temp_owned=0;final_owned=1;
    if(open_input(final_path))return -1;u64 size=0;code=((int(*)(i64,i64,u32,u64*))(fs_base+FS_SEEK_VA))(input_handle,0,2,&size);
    if(code||size!=report.final_bytes||seek_handle(input_handle,0)||read_exact(input_handle,io,header_bytes+MPF_BYTES)||memcmp(io,header,header_bytes)||memcmp(io+header_bytes,mpf,MPF_BYTES)){if(!error)error=-84;close_in();return -1;}
    if(close_in())return -1;report.publish_ns=now()-tick;report.flags|=0x10000;return 0;
}
static int finish_copy(void){
    u64 tick=now();if(flush_close()||mkdir_absent(dcf_directory))return -1;dcf_dir_owned=1;
    if(open_output(final_path))return -1;final_owned=1;
    report.final_crc=crc_add(0xffffffffU,header,header_bytes);
    if(write_exact(header,header_bytes))return -1;report.header_write_bytes+=header_bytes;
    if(open_input(temp_path)||seek_handle(input_handle,2))return -1;
    u64 left=report.body_bytes-2;
    while(left){u32 n=left>sizeof(io)?sizeof(io):(u32)left;
        if(read_exact(input_handle,io,n)||write_exact(io,n))return -1;
        report.final_crc=crc_add(report.final_crc,io,n);report.wrap_copy_bytes+=n;left-=n;
    }
    if(close_in()||flush_close())return -1;report.final_bytes=header_bytes+report.body_bytes-2;
    if(verify_final(final_path,0))return -1;
    int c=((int(*)(const u8*))(fs_base+FS_REMOVE_VA))(temp_path);if(c){error=c;return -1;}temp_owned=0;
    c=((int(*)(const u8*))(fs_base+FS_RMDIR_VA))(temp_directory);if(c){error=c;return -1;}temp_dir_owned=0;
    report.wrap_ns=now()-tick;return 0;
}
extern int jsimd_can_rgb_ycc(void);extern int jsimd_can_h2v1_downsample(void);extern int jsimd_can_fdct_islow(void);
__attribute__((constructor(101))) static void native_run(void){
    memcpy(report.magic,"PSJPG019",8);report.version=1;report.bytes=sizeof(report);report.status=-125;
    report.width=14016;report.height=TEST_HEIGHT;started=now();three_trace.epoch=started;memcpy(three_trace.magic,"PS3TR019",8);three_trace.version=5;three_trace.bytes=sizeof(three_trace);owner_tid=sc(178,0,0,0,0,0,0);
    hardware_crc=crc32_available();report.linear_crc=report.body_crc=body_tail_crc=0xffffffffU;
    for(u32 i=0;i<16;i++)report.source_crc[i]=0xffffffffU;
    if(hardware_crc)report.flags|=32768;else for(u32 i=0;i<256;i++){u32 c=i;for(u32 j=0;j<8;j++)c=(c>>1)^((0U-(c&1))&0xedb88320U);crc_table[i]=c;}
    i64 out=sc(56,-100,(i64)"/tmp/a7c2-pixelshift/a7c2.probe.result",0xc1,0600,0,0);if(out<0)return;
    if(sc(64,out,(i64)&report,sizeof(report),0,0,0)!=(i64)sizeof(report)){sc(57,out,0,0,0,0,0);return;}
    report.cleanup=0; /* Loader lifecycle is owned by the integrator. */
    if(load_curves()){error=-84;goto finish;}
    i64 fd=sc(56,-100,(i64)"/proc/self/maps",0,0,0,0);if(fd<0){error=(int)fd;goto finish;}
    u32 used=0;int bad=0;
    while(used<sizeof(maps)){i64 n=sc(63,fd,(i64)(maps+used),sizeof(maps)-used,0,0,0);if(n<0||(u64)n>sizeof(maps)-used){bad=1;break;}if(!n)break;used+=(u32)n;}
    sc(57,fd,0,0,0,0,0);if(bad||used==sizeof(maps)||find_base(used,&fs_base)){error=-22;goto finish;}
    for(u32 i=0;i<sizeof(fs_signatures)/sizeof(fs_signatures[0]);i++)if(!eq((const u8*)(fs_base+fs_signatures[i].va),fs_signatures[i].bytes,fs_signatures[i].size)){error=-84;goto finish;}
    if(!eq((const u8*)(fs_base+QUERY_VA),query_signature,QUERY_BYTES)||!eq((const u8*)(fs_base+RENAME_VA),rename_signature,sizeof(rename_signature))){error=-84;goto finish;}
    u32 mask=((u32(*)(void))(fs_base+QUERY_VA))();if(!mask||(mask&(mask-1))||(mask&0xfc000000U)){error=-19;goto finish;}
    u32 drive=0;while(drive<26&&!(mask&(1U<<drive)))drive++;
    temp_directory[0]=temp_path[0]=dcf_directory[0]=stage_path[0]=final_path[0]=(u8)('A'+drive);
    memcpy(report.path,final_path,sizeof(final_path));report.mem_before=report.mem_min=available();if(report.mem_before<(70ULL<<20)){error=-12;goto finish;}
    u32 space[13]={0};int code=((int(*)(u32,u32*))(fs_base+FS_FREE_VA))('A'+drive,space);if(code){error=code;goto finish;}
    u64 unit=(u64)space[6]*space[7],units=space[3];if(space[5]>1)units+=space[4];
    if(!unit||unit>0xffffffffUL||units>~(u64)0/unit){error=-22;goto finish;}report.sd_free=units*unit;if(report.sd_free<268435456){error=-28;goto finish;}
    for(u32 i=0;i<16;i++){
        u8 path[]="C:/DCIM/101MSDCF/DSC03044.ARW";path[0]=(u8)('A'+drive);u32 number=3044+i;
        for(u32 k=0;k<5;k++){path[24-k]=(u8)('0'+number%10);number/=10;}
        if(arw_u16_open(&sources[i],(const char*)path)){error=-5;goto finish;}
        int ok=ps_capture_info(&sources[i],&captures[i])==0&&captures[i].group==1256095283U&&captures[i].frame==i+1&&captures[i].total==16&&sources[i].width==7040&&sources[i].height==4688&&sources[i].bits==14;
        for(u32 k=0;k<4;k++)if(sources[i].black[k]!=512)ok=0;
        if(sources[i].white!=16383||memcmp(sources[i].wb_rggb,(const u32[]){2552,1024,1024,2053},16)||memcmp(sources[i].crop,(const u32[]){12,8,7008,4672},16)||memcmp(sources[i].cfa,(const u32[]){0,1,1,2},16))ok=0;
        if(i&&(captures[i].iso!=captures[0].iso||captures[i].shutter_num!=captures[0].shutter_num||captures[i].shutter_den!=captures[0].shutter_den||captures[i].aperture_num!=captures[0].aperture_num||captures[i].aperture_den!=captures[0].aperture_den))ok=0;
        if(i==0&&capture_metadata(&sources[i],capture_read,&capture_extra))ok=0;
        descriptors[i]=(FrameSource){sources[i].offset,sources[i].file_bytes};memcpy(paths[i],path,sizeof(path));arw_u16_close(&sources[i]);
        if(file.active||file.close_error||!ok){error=-22;goto finish;}report.frames++;if(expired())goto finish;
    }
    if(TEST_CODEC&&jsimd_can_rgb_ycc()&&jsimd_can_h2v1_downsample()&&jsimd_can_fdct_islow())report.flags|=0x200000;
    if(TEST_SINGLE_WRITE){
        if(mkdir_absent(dcf_directory))goto finish;dcf_dir_owned=1;
        if(open_output(stage_path))goto finish;temp_owned=1;
        u8 reserve[16386]={255,216,255,225,0x3f,0xfe};
        if(write_exact(reserve,sizeof(reserve)))goto finish;report.header_write_bytes+=sizeof(reserve);
    }else{
        code=((int(*)(const u8*,u32,u8*))(fs_base+FS_FIND_FIRST_VA))(dcf_directory,0x10,find_info);if(code!=2){error=code?code:-17;goto finish;}
        if(mkdir_absent(temp_directory))goto finish;temp_dir_owned=1;
        if(open_output(temp_path))goto finish;temp_owned=1;
    }
    if(write_exact((const u8[MPF_BYTES]){0},MPF_BYTES))goto finish;report.header_write_bytes+=MPF_BYTES;
    if(prepare_source_pool()){if(!error)error=-5;goto finish;}
    if(nc_prepare()){if(!error)error=-12;goto finish;}
    report.prepare_ns=now()-started;u64 pipeline_start=now();
    const uint16_t *resident[8]={0};nc_views(resident);
    code=fusion_run(descriptors,curves,resident,nc_rear(),nc_rear_bytes(),read_range,emit_body,observe,preview_rows,NULL,&report.stats);
    report.raw_bytes+=nc_trace.global[NC_SERVED_BYTES];
    report.pipeline_ns=now()-pipeline_start;u64 tail_started=now();if(close_source_pool()&&!error)error=-5;
    if(code||error||report.bands!=TEST_HEIGHT/16||body_soi_bytes!=2){if(!error)error=-5;goto finish;}
    if(TEST_PARALLEL&&!report.stats.worker_joined){error=-9;goto finish;}
    if(TEST_PARALLEL)report.flags|=0x100000;
    if(TEST_HEIGHT==9344&&(report.linear_crc^0xffffffffU)!=expected_linear_crc){error=-84;goto finish;}
    static const u32 required_source_crc[16]={0x1da2c134U,0x8f894c0dU,0x3bc7adbaU,0x88c61cf9U,0x8b161ec3U,0xe0e77ae5U,0xdddb0ae9U,0x5755d745U,0x6baafff4U,0x5f64f6b7U,0xc0d07249U,0xd43c70b7U,0x518a829aU,0x58b85cc7U,0x800e3995U,0x95ecd63fU};
    for(u32 i=0;i<16;i++)if((report.source_crc[i]^0xffffffffU)!=required_source_crc[i]){error=-84;goto finish;}
    if((report.body_crc^0xffffffffU)!=0x174197d3U||report.body_bytes!=23098539UL){error=-84;goto finish;}
    if(make_metadata()||(TEST_SINGLE_WRITE?finish_single():finish_copy()))goto finish;
    report.tail_ns=now()-tail_started;
finish:
    if(fs_base){close_source_pool();close_in();close_out();if(file.active)camera_fclose((FILE*)&file);
        if(nc_close()&&!error)error=-84;
        if(error){
            if(temp_owned)((int(*)(const u8*))(fs_base+FS_REMOVE_VA))(TEST_SINGLE_WRITE?stage_path:temp_path);
            if(final_owned)((int(*)(const u8*))(fs_base+FS_REMOVE_VA))(final_path);
            if(temp_dir_owned)((int(*)(const u8*))(fs_base+FS_RMDIR_VA))(temp_directory);
            if(dcf_dir_owned)((int(*)(const u8*))(fs_base+FS_RMDIR_VA))(dcf_directory);
        }
    }
    if(!input_active&&!output_active&&!file.active&&!file.close_error&&!source_close_uncertain)report.flags|=64;
    report.status=error;report.linear_crc^=0xffffffffU;report.body_crc^=0xffffffffU;report.final_crc^=0xffffffffU;
    for(u32 i=0;i<16;i++)report.source_crc[i]^=0xffffffffU;
    report.mem_after=available();report.elapsed_ns=now()-started;
    nc_export();
    i64 trace_fd=sc(56,-100,(i64)"/tmp/a7c2-pixelshift/ps-j3-three.trace",0xc1,0600,0,0);
    if(trace_fd>=0){u32 trace_at=0;while(trace_at<sizeof(three_trace)){u32 count=sizeof(three_trace)-trace_at;if(count>65536)count=65536;i64 n=sc(64,trace_fd,(i64)((u8*)&three_trace+trace_at),count,0,0,0);if(n<=0||(u64)n>count)break;trace_at+=(u32)n;}sc(82,trace_fd,0,0,0,0,0);sc(57,trace_fd,0,0,0,0,0);}
    sc(68,out,(i64)&report,sizeof(report),0,0,0);sc(82,out,0,0,0,0,0);sc(57,out,0,0,0,0,0);
}
