/* File-thread-owned RAW cache. All compute buffers and worker code are unchanged. */
#include <dlfcn.h>
enum { NC_STATUS,NC_REQUESTED,NC_COUNT,NC_TYPE,NC_CAPACITY,NC_HANDLE,NC_PHYSICAL,NC_EXTENTS,
 NC_ALLOC_CALL,NC_ALLOC_RET,NC_ALLOC_NS,NC_MAP_RET,NC_PATTERN,NC_PREPARE_NS,NC_PREFILL_NS,
 NC_SD_BYTES,NC_SD_CALLS,NC_SHORTS,NC_HITS,NC_COPY_NS,NC_SERVED_BYTES,NC_MISSES,
 NC_FREE_CALL,NC_FREE_RET,NC_UNMAP_RET,NC_CLOSE_RET,NC_PRESENT_BEFORE,NC_PRESENT_AFTER,
 NC_RELEASE_NS,NC_ATTEMPTS,NC_MIN_AVAILABLE,NC_ACTIVE_AFTER,NC_NFIELDS };
enum { NF_FRAME,NF_CROP_AT,NF_PREFIX,NF_FILE_AT,NF_IO_BYTES,NF_CALLS,NF_SHORTS,NF_READ_NS,
 NF_OPEN_NS,NF_SEEK_NS,NF_CLOSE_NS,NF_STATUS,NF_HITS,NF_COPY_NS,NF_SERVED_BYTES,NF_CRC,NF_NFIELDS };
static struct {u8 magic[8];u32 version,bytes;u64 global[NC_NFIELDS],frames[8][NF_NFIELDS];} nc_trace;
_Static_assert(sizeof(nc_trace)==1296,"native cache trace ABI");
enum { NC_DEST=0x61003c,NC_SRC=0x16f04aa };
static const u64 nc_crop_bytes=65781760,nc_stride=65785856;
static u8 nc_units[16384];static u32 nc_prior[1024],nc_prior_count,nc_count;
static u64 nc_crop_at[8],nc_prefix[8];static i64 nc_fd=-1,nc_mapping=-1;static u64 nc_map_bytes;static u8 *nc_data;static u32 nc_handle;
static int (*nc_valloc)(u32,void**,unsigned short,int),(*nc_sync)(u32,void*),(*nc_msgfree)(u32,void*);static void (*nc_src)(void*,u32);
static int nc_present(u32 handle){
 i64 fd=sc(56,-100,(i64)"/proc/udm/dmm_units",0,0,0,0);if(fd<0)return -1;
 i64 n=0;while(n<(i64)sizeof(nc_units)){i64 got=sc(63,fd,(i64)(nc_units+n),sizeof(nc_units)-(u64)n,0,0,0);if(got<0){n=-1;break;}if(!got)break;n+=got;}sc(57,fd,0,0,0,0,0);if(n<0||n==(i64)sizeof(nc_units))return -1;
 if(!handle)nc_prior_count=0;
 for(u32 at=0;at<(u32)n;){u32 end=at;while(end<(u32)n&&nc_units[end]!='\n')end++;u32 value=0,k=at;
  for(;k<end;k++){u8 c=nc_units[k];u32 d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;if(d==16)break;value=(value<<4)|d;}
  if(k-at>=4){if(!handle&&nc_prior_count<1024)nc_prior[nc_prior_count++]=value;else if(value==handle)return 1;}at=end+1;
 }return 0;
}
static int nc_exchange(unsigned short cmd,u32 value,u8 **packet){
 *packet=0;int code=nc_valloc(NC_DEST,(void**)packet,40,1);if(code||!*packet)return code?code:-1;
 memset(*packet,0,40);*(unsigned short*)*packet=cmd;
 if(cmd==0x20){*(u32*)(*packet+4)=value;*(u32*)(*packet+12)=1000;*(unsigned short*)(*packet+22)=1;}
 else{*(u32*)(*packet+8)=value;*(unsigned short*)(*packet+22)=2;}
 nc_src(*packet,NC_SRC);return nc_sync(NC_DEST,*packet);
}
static int nc_close(void){
 if(!nc_handle)return 0;u64 tick=now();int failed=0;
 if(nc_data){nc_trace.global[NC_UNMAP_RET]=sc(215,nc_mapping,nc_map_bytes,0,0,0,0);if(nc_trace.global[NC_UNMAP_RET])failed=1;else{nc_data=0;nc_mapping=-1;}}
 if(nc_fd>=0){nc_trace.global[NC_CLOSE_RET]=sc(57,nc_fd,0,0,0,0,0);if(nc_trace.global[NC_CLOSE_RET])failed=1;else nc_fd=-1;}
 /* Never return a unit to DMM while its CPU mapping is still live. */
 if(nc_data){nc_trace.global[NC_STATUS]=(i64)-84;return -1;}
 u8 *msg=0;int code=nc_exchange(0x23,nc_handle,&msg);nc_trace.global[NC_FREE_CALL]=(i64)code;
 if(msg){nc_trace.global[NC_FREE_RET]=(i64)*(short*)(msg+2);if(nc_msgfree(NC_DEST,msg))failed=1;}else nc_trace.global[NC_FREE_RET]=(i64)-1;
 nc_trace.global[NC_PRESENT_AFTER]=(i64)nc_present(nc_handle);
 if(code||nc_trace.global[NC_FREE_RET]||nc_trace.global[NC_PRESENT_AFTER])failed=1;
 if(!failed){nc_handle=0;nc_count=0;}else nc_trace.global[NC_STATUS]=(i64)-84;
 nc_trace.global[NC_ACTIVE_AFTER]=nc_handle!=0;nc_trace.global[NC_RELEASE_NS]+=now()-tick;return failed?-1:0;
}
static int nc_prepare(void){memcpy(nc_trace.magic,"PSNCACHE",8);nc_trace.version=1;nc_trace.bytes=sizeof(nc_trace);return 0;}
static int nc_cached_read(int frame,u64 at,void *dest,size_t bytes){
 if((u32)frame>=nc_count){nc_trace.global[NC_MISSES]++;return 0;}
 if(!nc_data||at<nc_crop_at[frame]||at-nc_crop_at[frame]>nc_crop_bytes||bytes>nc_crop_bytes-(at-nc_crop_at[frame]))return -1;
 u64 tick=now();memcpy(dest,nc_data+(u64)frame*nc_stride+nc_prefix[frame]+at-nc_crop_at[frame],bytes);u64 elapsed=now()-tick;
 nc_trace.global[NC_HITS]++;nc_trace.global[NC_COPY_NS]+=elapsed;nc_trace.global[NC_SERVED_BYTES]+=bytes;
 u64 *f=nc_trace.frames[frame];f[NF_HITS]++;f[NF_COPY_NS]+=elapsed;f[NF_SERVED_BYTES]+=bytes;return 1;
}
static void nc_export(void){
 if(nc_trace.magic[0]!='P'){memcpy(nc_trace.magic,"PSNCACHE",8);nc_trace.version=1;nc_trace.bytes=sizeof(nc_trace);nc_trace.global[NC_STATUS]=(i64)report.status;}
 if((i64)nc_trace.global[NC_STATUS]==-125)nc_trace.global[NC_STATUS]=(i64)report.status;
 for(u32 i=0;i<8;i++)nc_trace.frames[i][NF_CRC]=report.source_crc[i];
 i64 fd=sc(56,-100,(i64)"/tmp/jiritsu/ps-native-cache.trace",0xc1,0600,0,0);if(fd<0)return;
 for(u32 at=0;at<sizeof(nc_trace);){i64 n=sc(64,fd,(i64)((u8*)&nc_trace+at),sizeof(nc_trace)-at,0,0,0);if(n<=0||(u64)n>sizeof(nc_trace)-at)break;at+=(u32)n;}
 sc(82,fd,0,0,0,0,0);sc(57,fd,0,0,0,0,0);
}

static void nc_views(const uint16_t **v){for(u32 i=0;i<8;i++)v[i]=0;}
static uint16_t *nc_rear(void){return 0;}
static u64 nc_rear_bytes(void){return 0;}
