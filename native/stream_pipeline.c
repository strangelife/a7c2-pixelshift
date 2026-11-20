#include "stream_pipeline.h"
#include "three_trace.h"
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
extern int posix_memalign(void **,size_t,size_t);
#include <string.h>
#ifndef REAR_SLOTS
#define REAR_SLOTS 2
#endif
#ifndef AUX_MERGE
#define AUX_MERGE 0
#endif
#ifndef RECON_ROWS
#define RECON_ROWS 256
#endif
#ifndef IO_ROWS
#define IO_ROWS 512
#endif
#ifndef RGB_SLOTS
#define RGB_SLOTS 2
#endif
#ifndef PARALLEL_TONE
#define PARALLEL_TONE 0
#endif
#ifndef COMPUTE_THREADS
#define COMPUTE_THREADS 2
#endif
#ifndef RESIDENT_COUNT
#define RESIDENT_COUNT 8
#endif
#define COMPUTE_AUX (COMPUTE_THREADS>1)
enum {AUX_COUNT=COMPUTE_THREADS-1,AUX_CAP=COMPUTE_THREADS>1?COMPUTE_THREADS-1:1,FRONT_GROUPS=RESIDENT_COUNT/4,REAR_GROUPS=4-FRONT_GROUPS};
_Static_assert((RESIDENT_COUNT==0||RESIDENT_COUNT==4||RESIDENT_COUNT==8)&&COMPUTE_THREADS>=1&&COMPUTE_THREADS<=4,"cache/thread range");
_Static_assert(REAR_SLOTS>=REAR_GROUPS||IO_ROWS==RECON_ROWS,"live parent groups require slots");
_Static_assert(!PARALLEL_TONE||RGB_SLOTS>=COMPUTE_THREADS,"distinct RGB slots for parallel tone");
_Static_assert(IO_ROWS%RECON_ROWS==0 && RECON_ROWS%32==0 && RGB_SLOTS>=2,"tile and queue contract");
enum { BATCH=RECON_ROWS,IO_BATCH=IO_ROWS,SAMPLES=16*OUTPUT_WIDTH*3,PACKET=262144,Q=4,RAW_ROWS=IO_ROWS/2,JOBS=((RGB_HEIGHT+IO_ROWS-1)/IO_ROWS)*REAR_GROUPS };
typedef struct {uint32_t tag;int y;uint8_t data[SAMPLES];} RGBSlot;
typedef struct {int state;size_t length;uint32_t row,id;uint8_t data[PACKET];} Packet;
typedef struct {const uint16_t *input[4];int origin[4],available[4],y,rows,group,result,kind,begin,end;const uint16_t *linear;uint8_t *rgb;} MergeTask;
struct Pipeline;
typedef struct {struct Pipeline *pipeline;int index;} AuxContext;
typedef struct Pipeline {
 const FrameSource *sources;const uint16_t *resident[8];ReadRange read;WriteBytes write;ObserveBand observe;ObservePreview preview;void *context;
 A7c2PipelineStats *stats;RenderState *renderer;uint16_t *raw,*assembly,*cached;
 RGBSlot rgb[RGB_SLOTS];Packet packets[Q];uint8_t accumulation[PACKET];size_t accumulated;
 uint32_t raw_tag[REAR_SLOTS];pthread_mutex_t mutex;pthread_cond_t changed;
 uint32_t head,tail,queued;int stopped,recon_done,jpeg_done,recon_status,jpeg_status;
 int held,next_row,last_row,aux_pending[AUX_CAP],aux_done[AUX_CAP];MergeTask tasks[AUX_CAP];AuxContext aux_context[AUX_CAP];uint64_t aux_cpu[AUX_CAP];
} Pipeline;
extern int a7c2_source_crc(int,const uint16_t*,size_t);
extern int a7c2_merge_view(int,int,int,const uint16_t *const[4],const int[4],const int[4],uint16_t*,int,int);
static uint64_t ns(int id){struct timespec t;if(clock_gettime(id,&t))return 0;return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}static uint64_t now_ns(void){return ns(CLOCK_MONOTONIC);}
static uint64_t stamp(void){return now_ns()-three_trace.epoch;}
static void lock(Pipeline *p){pthread_mutex_lock(&p->mutex);}static void unlock(Pipeline *p){pthread_mutex_unlock(&p->mutex);}static void wake(Pipeline *p){pthread_cond_broadcast(&p->changed);}
static void stop_locked(Pipeline *p){p->stopped=1;wake(p);}
static int first_row(int frame,int y){return (frame%4==1||frame%4==2?7:8)+y/2;}
enum { RAW_FRAME_BYTES=RAW_ROWS*RAW_ROW_SAMPLES*2,RAW_FRAME_STRIDE=RAW_FRAME_BYTES+4096 };
static uint16_t *raw_frame(Pipeline *p,uint32_t job,int f){int frame=(FRONT_GROUPS+(job%REAR_GROUPS))*4+f;int y=(job/REAR_GROUPS)*IO_BATCH;uint64_t at=p->sources[frame].payload_offset+(uint64_t)first_row(frame,y)*RAW_ROW_SAMPLES*2;size_t prefix=(size_t)(at&511);return (uint16_t*)((uint8_t*)p->raw+((job%REAR_SLOTS)*(size_t)4+f)*RAW_FRAME_STRIDE+prefix);}
static void *aux_worker(void *context){
 AuxContext *a=context;Pipeline *p=a->pipeline;int i=a->index;uint64_t cpu=ns(CLOCK_THREAD_CPUTIME_ID);
 for(;;){lock(p);while(!p->aux_pending[i]&&!p->stopped)pthread_cond_wait(&p->changed,&p->mutex);
  if(p->stopped&&!p->aux_pending[i]){unlock(p);break;}MergeTask task=p->tasks[i];unlock(p);int result=0;
  if(task.kind==1)a7c2_render_band(p->renderer,task.linear,task.rgb);else result=a7c2_merge_view(task.y,task.rows,task.group,task.input,task.origin,task.available,p->assembly,task.begin,task.end);
  lock(p);p->tasks[i].result=result;p->aux_pending[i]=0;p->aux_done[i]=1;wake(p);unlock(p);
 }p->aux_cpu[i]=ns(CLOCK_THREAD_CPUTIME_ID)-cpu;return NULL;
}
static int wait_aux(Pipeline *p,int count){int failed=0;lock(p);for(int i=0;i<count;i++){while(!p->aux_done[i]&&!p->stopped)pthread_cond_wait(&p->changed,&p->mutex);if(!p->aux_done[i]||p->tasks[i].result)failed=1;}unlock(p);return failed?-1:0;}
static int reconstruct(Pipeline *p){
 for(int y=0;y<RGB_HEIGHT;y+=BATCH){int stripe=y/BATCH,rows=RGB_HEIGHT-y<BATCH?RGB_HEIGHT-y:BATCH,parent=y/IO_BATCH*IO_BATCH;
  int parent_rows=RGB_HEIGHT-parent<IO_BATCH?RGB_HEIGHT-parent:IO_BATCH;
  for(int group=0;group<4;group++){
   uint32_t seq=(uint32_t)(parent/IO_BATCH)*REAR_GROUPS+(uint32_t)(group-FRONT_GROUPS);MergeTask task;memset(&task,0,sizeof(task));task.y=y;task.rows=rows;task.group=group;
   if(group<FRONT_GROUPS){three_trace.stripes[stripe].read_begin[group]=three_trace.stripes[stripe].read_end[group]=stamp();
    for(int f=0;f<4;f++){int frame=group*4+f;task.input[f]=p->resident[frame];task.origin[f]=first_row(frame,0);task.available[f]=RGB_HEIGHT/2;}
   }else{lock(p);uint64_t waited=now_ns();while(!p->stopped&&p->raw_tag[seq%REAR_SLOTS]!=seq+1){three_trace.counters[T_RAW_EMPTY_COUNT]++;pthread_cond_wait(&p->changed,&p->mutex);}
    p->stats->raw_wait_ns+=now_ns()-waited;int stopped=p->stopped;unlock(p);if(stopped)return -1;
    for(int f=0;f<4;f++){int frame=group*4+f;task.input[f]=raw_frame(p,seq,f);task.origin[f]=first_row(frame,parent);task.available[f]=parent_rows/2;}
   }
   three_trace.stripes[stripe].merge_begin[group]=stamp();
   for(int f=0;f<4;f++){int frame=group*4+f,relative=first_row(frame,y)-task.origin[f];if(relative<0||relative+rows/2>task.available[f]||a7c2_source_crc(frame,task.input[f]+(size_t)relative*RAW_ROW_SAMPLES,(size_t)(rows/2)*RAW_ROW_SAMPLES*2))return -1;}
   uint64_t tick=now_ns();int result;
   if(AUX_MERGE&&COMPUTE_THREADS>1){
    lock(p);for(int i=0;i<AUX_COUNT;i++){p->tasks[i]=task;p->tasks[i].begin=(i+1)*rows/COMPUTE_THREADS;p->tasks[i].end=(i+2)*rows/COMPUTE_THREADS;p->aux_done[i]=0;p->aux_pending[i]=1;}wake(p);unlock(p);
    result=a7c2_merge_view(y,rows,group,task.input,task.origin,task.available,p->assembly,0,rows/COMPUTE_THREADS);if(wait_aux(p,AUX_COUNT))result=-1;
   }else result=a7c2_merge_view(y,rows,group,task.input,task.origin,task.available,p->assembly,0,rows);
   p->stats->group_merge_ns+=now_ns()-tick;three_trace.stripes[stripe].merge_end[group]=stamp();if(result)return -1;
   /* Retain a parent until every reconstruction child is consumed. */
   if(group>=FRONT_GROUPS&&y+rows>=parent+parent_rows){lock(p);p->raw_tag[seq%REAR_SLOTS]=seq+REAR_SLOTS;wake(p);unlock(p);}
  }
  for(int row=y;row<y+rows;row+=16*(PARALLEL_TONE?COMPUTE_THREADS:1)){
   int count=PARALLEL_TONE?COMPUTE_THREADS:1;if(count>(y+rows-row)/16)count=(y+rows-row)/16;
   uint32_t band=(uint32_t)row/16;RGBSlot *slot[COMPUTE_THREADS];for(int k=0;k<count;k++)slot[k]=&p->rgb[(band+k)%RGB_SLOTS];
   lock(p);uint64_t tick=now_ns();for(;;){int ready=1;for(int k=0;k<count;k++)if(slot[k]->tag!=band+(uint32_t)k)ready=0;if(ready||p->stopped)break;three_trace.counters[T_RGB_FULL_COUNT]++;pthread_cond_wait(&p->changed,&p->mutex);}
   three_trace.counters[T_RGB_FULL_WAIT]+=now_ns()-tick;int stopped=p->stopped;unlock(p);if(stopped)return -1;
   const uint16_t *linear=p->assembly+(size_t)(row-y)*OUTPUT_WIDTH*3;
   tick=now_ns();for(int k=0;k<count;k++)if(p->observe&&p->observe(p->context,linear+(size_t)k*SAMPLES,(size_t)SAMPLES*2))return -1;p->stats->linear_check_ns+=now_ns()-tick;
   tick=now_ns();for(int k=0;k<count;k++)three_trace.bands[band+k].render_begin=stamp();
   if(count>1){lock(p);for(int i=0;i<count-1;i++){memset(&p->tasks[i],0,sizeof(p->tasks[i]));p->tasks[i].kind=1;p->tasks[i].linear=linear+(size_t)(i+1)*SAMPLES;p->tasks[i].rgb=slot[i+1]->data;p->aux_done[i]=0;p->aux_pending[i]=1;}wake(p);unlock(p);}
   a7c2_render_band(p->renderer,linear,slot[0]->data);
   for(int k=0;k<count;k++){
    if(k){lock(p);while(!p->aux_done[k-1]&&!p->stopped)pthread_cond_wait(&p->changed,&p->mutex);int failed=!p->aux_done[k-1]||p->tasks[k-1].result;unlock(p);if(failed)return -1;}
    if(p->preview&&p->preview(p->context,row+k*16,slot[k]->data))return -1;
    three_trace.bands[band+k].render_end=stamp();slot[k]->y=row+k*16;lock(p);if(p->stopped){unlock(p);return -1;}slot[k]->tag=band+k+1;three_trace.bands[band+k].published=stamp();if(!three_trace.stripes[stripe].rgb_first)three_trace.stripes[stripe].rgb_first=stamp();three_trace.stripes[stripe].rgb_last=stamp();three_trace.counters[T_RGB_PRODUCED]++;uint64_t used=three_trace.counters[T_RGB_PRODUCED]-three_trace.counters[T_RGB_CONSUMED];if(used>three_trace.counters[T_RGB_PEAK])three_trace.counters[T_RGB_PEAK]=used;wake(p);unlock(p);
   }p->stats->render_ns+=now_ns()-tick;
  }
 }return 0;
}
static void *recon_worker(void *context){Pipeline *p=context;uint64_t cpu=ns(CLOCK_THREAD_CPUTIME_ID),wall=now_ns();int result=reconstruct(p);three_trace.counters[T_RECON_CPU]=ns(CLOCK_THREAD_CPUTIME_ID)-cpu;three_trace.counters[T_RECON_WALL]=now_ns()-wall;lock(p);p->recon_status=result;p->recon_done=1;if(result)stop_locked(p);wake(p);unlock(p);return NULL;}
static void release_rgb(Pipeline *p){if(p->held<0)return;uint32_t band=(uint32_t)p->held;three_trace.bands[band].consume_end=stamp();three_trace.stripes[(band*16)/BATCH].jpeg_last=stamp();p->rgb[band%RGB_SLOTS].tag=band+RGB_SLOTS;three_trace.counters[T_RGB_CONSUMED]++;p->held=-1;wake(p);}
static const uint8_t *get_rows(void *context,int y,int count){
 Pipeline *p=context;uint64_t tick=now_ns();if(count!=16||y!=p->next_row||y+count>RGB_HEIGHT)return NULL;lock(p);release_rgb(p);uint32_t band=(uint32_t)y/16;RGBSlot *slot=&p->rgb[band%RGB_SLOTS];uint64_t waited=now_ns();
 while(!p->stopped&&slot->tag!=band+1&&!p->recon_done){three_trace.counters[T_RGB_EMPTY_COUNT]++;pthread_cond_wait(&p->changed,&p->mutex);}three_trace.counters[T_JPEG_RGB_WAIT]+=now_ns()-waited;
 if(p->stopped||slot->tag!=band+1||slot->y!=y){unlock(p);return NULL;}p->held=(int)band;three_trace.bands[band].consume_begin=stamp();if(!three_trace.stripes[y/BATCH].jpeg_first)three_trace.stripes[y/BATCH].jpeg_first=stamp();unlock(p);
 p->next_row+=count;p->last_row=y;p->stats->fetch_total_ns+=now_ns()-tick;return slot->data;
}
static int emit(Pipeline *p){
 if(!p->accumulated)return 0;uint64_t tick=now_ns();lock(p);Packet *packet=&p->packets[p->tail%Q];uint64_t waited=now_ns();while(packet->state&&!p->stopped)pthread_cond_wait(&p->changed,&p->mutex);p->stats->output_wait_ns+=now_ns()-waited;if(p->stopped){unlock(p);return -1;}
 packet->state=1;packet->id=p->tail;unlock(p);packet->length=p->accumulated;packet->row=(uint32_t)p->last_row;memcpy(packet->data,p->accumulation,p->accumulated);p->accumulated=0;lock(p);if(p->stopped){unlock(p);return -1;}packet->state=2;p->tail++;p->queued++;
 if(p->queued>p->stats->output_high_water)p->stats->output_high_water=p->queued;if(packet->id<512){three_trace.packets[packet->id].queued=stamp();three_trace.packets[packet->id].row=packet->row;}three_trace.counters[T_PACKETS]++;wake(p);unlock(p);p->stats->emit_packet_ns+=now_ns()-tick;return 0;
}
static int write_bytes(void *context,const void *bytes,size_t count){Pipeline *p=context;const uint8_t *cursor=bytes;while(count){size_t n=PACKET-p->accumulated;if(n>count)n=count;memcpy(p->accumulation+p->accumulated,cursor,n);p->accumulated+=n;cursor+=n;count-=n;if(p->accumulated==PACKET&&emit(p))return -1;}return 0;}
static void *jpeg_worker(void *context){Pipeline *p=context;uint64_t cpu=ns(CLOCK_THREAD_CPUTIME_ID),wall=now_ns();int result=a7c2_encode_jpeg_turbo(get_rows,write_bytes,p)||p->next_row!=RGB_HEIGHT||emit(p)?-1:0;p->stats->encoder_total_ns=now_ns()-wall;three_trace.counters[T_JPEG_WALL]=p->stats->encoder_total_ns;three_trace.counters[T_JPEG_CPU]=ns(CLOCK_THREAD_CPUTIME_ID)-cpu;lock(p);release_rgb(p);p->jpeg_status=result;p->jpeg_done=1;if(result)stop_locked(p);wake(p);unlock(p);return NULL;}
static int file_owner(Pipeline *p){uint32_t job=0;
 for(;;){lock(p);uint64_t tick=now_ns();while(!p->stopped&&!p->jpeg_done&&p->packets[p->head%Q].state!=2&&(job==JOBS||p->raw_tag[job%REAR_SLOTS]!=job)){if(job<JOBS)three_trace.counters[T_RAW_FULL_COUNT]++;pthread_cond_wait(&p->changed,&p->mutex);}p->stats->file_wait_ns+=now_ns()-tick;if(p->stopped){unlock(p);return -1;}
  Packet *packet=&p->packets[p->head%Q];if(packet->state==2){packet->state=3;unlock(p);tick=now_ns();uint64_t begin=stamp();int result=p->write(p->context,packet->data,packet->length);uint64_t end=stamp();p->stats->write_ns+=now_ns()-tick;if(packet->id<512){three_trace.packets[packet->id].write_begin=begin;three_trace.packets[packet->id].write_end=end;}StripeTrace *stripe=&three_trace.stripes[packet->row/BATCH];if(!stripe->write_first)stripe->write_first=begin;stripe->write_last=end;if(result)return -1;lock(p);packet->state=0;p->head++;p->queued--;wake(p);unlock(p);continue;}
  if(p->jpeg_done){int result=p->jpeg_status||p->recon_status||job!=JOBS;unlock(p);return result?-1:0;}unlock(p);
  int y=(int)(job/REAR_GROUPS)*IO_BATCH,group=(int)(job%REAR_GROUPS)+FRONT_GROUPS,rows=RGB_HEIGHT-y<IO_BATCH?RGB_HEIGHT-y:IO_BATCH;tick=now_ns();uint64_t began=stamp();
  for(int f=0;f<4;f++){int frame=group*4+f;uint64_t at=p->sources[frame].payload_offset+(uint64_t)first_row(frame,y)*RAW_ROW_SAMPLES*2;size_t bytes=(size_t)(rows/2)*RAW_ROW_SAMPLES*2;
   if(at>p->sources[frame].file_bytes||bytes>p->sources[frame].file_bytes-at||p->read(p->context,frame,at,raw_frame(p,job,f),bytes))return -1;}
  p->stats->read_ns+=now_ns()-tick;lock(p);if(p->stopped){unlock(p);return -1;}p->raw_tag[job%REAR_SLOTS]=job+1;uint64_t ready=stamp();for(int sub=0;sub<rows;sub+=BATCH){three_trace.stripes[(y+sub)/BATCH].read_begin[group]=began;three_trace.stripes[(y+sub)/BATCH].read_end[group]=ready;}wake(p);unlock(p);job++;
 }
}
STREAM_EXPORT int fusion_run(const FrameSource sources[16],const uint8_t curves[3][256],const uint16_t *const resident[8],uint16_t *rear_storage,size_t rear_bytes,ReadRange read,WriteBytes write,ObserveBand observe,ObservePreview preview,void *context,A7c2PipelineStats *stats){
 (void)rear_storage;(void)rear_bytes;
 if(!sources||!curves||!resident||!stats)return -1;Pipeline *p=calloc(1,sizeof(*p));if(!p)return -1;p->sources=sources;for(int i=0;i<RESIDENT_COUNT;i++){if(!resident[i]){free(p);return -1;}p->resident[i]=resident[i];}p->read=read;p->write=write;p->observe=observe;p->preview=preview;p->context=context;p->stats=stats;p->held=-1;memset(stats,0,sizeof(*stats));for(int i=0;i<REAR_SLOTS;i++)p->raw_tag[i]=(uint32_t)i;for(int i=0;i<RGB_SLOTS;i++)p->rgb[i].tag=(uint32_t)i;
 p->renderer=malloc(a7c2_renderer_bytes());if(posix_memalign((void**)&p->raw,4096,(size_t)REAR_SLOTS*4*RAW_FRAME_STRIDE))p->raw=NULL;p->assembly=malloc((size_t)BATCH*OUTPUT_WIDTH*3*2);p->cached=NULL;int result=-1;if(!p->renderer||!p->assembly||!p->raw)goto finish;a7c2_renderer_init(p->renderer,curves);
 if(pthread_mutex_init(&p->mutex,NULL))goto finish;if(pthread_cond_init(&p->changed,NULL)){pthread_mutex_destroy(&p->mutex);goto finish;}
 pthread_t recon=0,jpeg=0,aux[AUX_CAP]={0};int have_recon=0,have_jpeg=0,have_aux=0;uint64_t cpu=ns(CLOCK_THREAD_CPUTIME_ID);
 for(int i=0;i<AUX_COUNT;i++){p->aux_context[i]=(AuxContext){p,i};if(pthread_create(&aux[i],NULL,aux_worker,&p->aux_context[i]))break;have_aux++;}
 if(have_aux==AUX_COUNT&&!pthread_create(&recon,NULL,recon_worker,p)){have_recon=1;if(!pthread_create(&jpeg,NULL,jpeg_worker,p)){have_jpeg=1;result=file_owner(p);}}
 lock(p);stop_locked(p);unlock(p);uint64_t tick=now_ns();
 if(have_recon){int code=pthread_join(recon,NULL);three_trace.counters[T_RECON_JOINED]=code==0;if(code)result=-1;}
 if(have_jpeg){int code=pthread_join(jpeg,NULL);three_trace.counters[T_JPEG_JOINED]=code==0;if(code)result=-1;}
 for(int i=0;i<have_aux;i++){int code=pthread_join(aux[i],NULL);if(!code)three_trace.counters[T_RESERVED1]++;else result=-1;three_trace.counters[T_RESERVED0]+=p->aux_cpu[i];}
 stats->pipeline_shutdown_ns=now_ns()-tick;stats->worker_joined=have_recon&&have_jpeg&&three_trace.counters[T_RECON_JOINED]&&three_trace.counters[T_JPEG_JOINED]&&have_aux==AUX_COUNT&&three_trace.counters[T_RESERVED1]==AUX_COUNT;stats->file_thread_cpu_ns=ns(CLOCK_THREAD_CPUTIME_ID)-cpu;three_trace.counters[T_FILE_CPU]=stats->file_thread_cpu_ns;stats->compute_thread_cpu_ns=three_trace.counters[T_RECON_CPU]+three_trace.counters[T_JPEG_CPU]+three_trace.counters[T_RESERVED0];pthread_cond_destroy(&p->changed);pthread_mutex_destroy(&p->mutex);
finish:free(p->raw);free(p->cached);free(p->assembly);free(p->renderer);free(p);return result;
}
