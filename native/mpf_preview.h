/* MPF APP2 follows the reserved EXIF APP1. Offsets are relative to MP TIFF. */
enum { MPF_BYTES=90, REPLAY_WIDTH=1616, REPLAY_HEIGHT=1080, REPLAY_ACTIVE=1078 };
static u8 mpf[MPF_BYTES],replay[REPLAY_WIDTH*REPLAY_HEIGHT*3];
static u32 replay_next,replay_crc=0xffffffffU;static u64 replay_bytes,primary_bytes;
extern int a7c2_encode_replay(GetRows,WriteBytes,void*);
static void build_mpf(void){
 memset(mpf,0,sizeof(mpf));mpf[0]=255;mpf[1]=226;mpf[2]=0;mpf[3]=88;memcpy(mpf+4,"MPF\0II",6);
 u8 *t=mpf+8;mw16(t+2,42);mw32(t+4,8);mw16(t+8,3);
 mw16(t+10,0xb000);mw16(t+12,7);mw32(t+14,4);memcpy(t+18,"0100",4);
 mw16(t+22,0xb001);mw16(t+24,4);mw32(t+26,1);mw32(t+30,2);
 mw16(t+34,0xb002);mw16(t+36,7);mw32(t+38,32);mw32(t+42,50);
 mw32(t+50,0xa0030000U);mw32(t+54,(u32)primary_bytes);mw32(t+58,0);mw16(t+62,2);
 mw32(t+66,0x40010002U);mw32(t+70,(u32)replay_bytes);mw32(t+74,(u32)(primary_bytes-16394));
}
static const u8 *replay_rows(void *context,int y,int count){(void)context;if(count!=8||y<0||y+8>1080)return NULL;return replay+(size_t)y*1616*3;}
static int replay_sink(void *context,const void *bytes,size_t count){
 (void)context;if(count>65536||replay_bytes+count>4194304||error||expired())return -1;
 u64 tick=now();if(write_exact(bytes,(u32)count))return -1;three_trace.counters[T_PREVIEW_WRITE]+=now()-tick;
 replay_crc=crc_add(replay_crc,bytes,(u32)count);replay_bytes+=count;return 0;
}
