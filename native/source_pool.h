/* Optional OFFLINE candidate; hardware capacity/lock contract is not approved. */
static i64 pooled[16];static u8 pooled_active[16];static u32 source_close_uncertain;
static int close_source_pool(void){
 int failed=0;u64 tick=now();
 for(u32 i=0;i<16;i++)if(pooled_active[i]){
  int code=((int(*)(i64))(fs_base+FS_CLOSE_VA))(pooled[i]);pooled_active[i]=0;
  if(code){failed=1;source_close_uncertain++;three_trace.extra[X_CLOSE_ERRORS]++;if(!error)error=code;}
  else three_trace.extra[X_POOL_CLOSED]++;
 }
 three_trace.extra[X_POOL_CLOSE_NS]+=now()-tick;return failed?-1:0;
}
static int prepare_source_pool(void){
 if(!TEST_REUSE)return 0;int failed=0;u32 held=0;u64 tick=now();
 for(u32 i=0;i<8;i++){
  i64 handle=0;int code=((int(*)(const u8*,i64*,u32))(fs_base+FS_OPEN_VA))(paths[i],&handle,1);
  if(handle){
   int duplicate=handle==output_handle;for(u32 j=0;j<i;j++)if(pooled_active[j]&&pooled[j]==handle)duplicate=1;
   if(duplicate){source_close_uncertain++;error=-84;failed=1;break;}
   pooled[i]=handle;pooled_active[i]=1;held++;three_trace.extra[X_POOL_OPENED]++;
   if(held>three_trace.extra[X_POOL_PEAK])three_trace.extra[X_POOL_PEAK]=held;
  }
  if(code||!handle){failed=1;break;}
  code=((int(*)(i64,u32))(fs_base+FS_LOCK_VA))(handle,1);if(code){failed=1;break;}
 }
 three_trace.extra[X_POOL_OPEN_NS]+=now()-tick;
 if(!failed){u64 mem=available();three_trace.extra[X_POOL_MEM]=mem;if(mem<report.mem_min)report.mem_min=mem;if(mem<(70ULL<<20))failed=1;}
 if(failed){if(close_source_pool()||source_close_uncertain)return -1;three_trace.extra[X_POOL_FALLBACK]=1;return 0;}
 three_trace.extra[X_POOL_ENABLED]=1;return 0;
}
