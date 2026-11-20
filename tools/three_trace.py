import struct,csv,io
COUNTERS=['raw_open_ns','raw_seek_ns','raw_actual_read_ns','raw_source_crc_ns','raw_close_ns','raw_memory_sample_ns','raw_callbacks','RGB_full_wait_ns','JPEG_RGB_wait_ns','reconstruction_CPU_ns','JPEG_CPU_ns','reconstruction_wall_ns','JPEG_wall_ns','RGB_queue_peak','reconstruction_joined','JPEG_joined','replay_sample_ns','replay_encode_ns','replay_write_ns','replay_crc32','replay_bytes','JPEG_packets','RGB_produced','RGB_consumed','primary_bytes','file_CPU_ns','RAW_empty_wait_count','RGB_empty_wait_count','RGB_full_wait_count','RAW_full_wait_count','reserved0','reserved1']
STRIPE_FIELDS=['read_begin','read_ready','merge_begin','merge_end']
EXTRA=['RGB_first','RGB_last','JPEG_first','JPEG_last','write_first','write_last']
def parse_trace(data,height,batch=320):
 magic,version,size,epoch=struct.unpack_from('<8sIIQ',data)
 assert magic==b'PS3TR019' and version==5 and len(data)==size==459088
 counters=dict(zip(COUNTERS,struct.unpack_from('<32Q',data,24)));at=280;stripes=[]
 for i in range(146):
  values=struct.unpack_from('<22Q',data,at);at+=176
  if i*batch<height:
   row=dict(stripe=i,y=i*batch,height=min(batch,height-i*batch))
   for j,name in enumerate(STRIPE_FIELDS):row[name]=list(values[j*4:j*4+4])
   row.update(zip(EXTRA,values[16:]));stripes.append(row)
 bands=[]
 for i in range(584):
  values=struct.unpack_from('<5Q',data,at);at+=40
  if i*16<height:bands.append(dict(y=i*16,**dict(zip(['render_begin','render_end','published','consume_begin','consume_end'],values))))
 packets=[]
 for i in range(512):
  values=struct.unpack_from('<4Q',data,at);at+=32
  if i<counters['JPEG_packets']:packets.append(dict(packet=i,**dict(zip(['queued','write_begin','write_end','associated_scanline'],values))))
 extra_names=['actual_read_calls','short_reads','big_requests','big_full_returns','maximum_request','logged_calls','dropped_records','pooled_opened','pooled_closed','pooled_peak','pool_fallback','pool_enabled','close_errors','memory_after_pool','source_CRC_tid','pool_open_ns','pool_close_ns']
 extra=dict(zip(extra_names,struct.unpack_from('<17Q',data,at)));at+=136;reads=[]
 for i in range(16384):
  values=struct.unpack_from('<IiIHHQ',data,at);at+=24
  if i<extra['logged_calls']:reads.append(dict(zip(['requested','returned','window','frame','flags','duration_ns'],values)))
 compute_copy_ns,compute_copy_bytes=struct.unpack_from('<2Q',data,at);at+=16
 assert at==len(data)
 return dict(compute_copy_ns=compute_copy_ns,compute_copy_bytes=compute_copy_bytes,batch=batch,extra=extra,read_calls=reads,version=version,epoch_monotonic_ns=epoch,counters=counters,stripes=stripes,bands=bands,packets=packets,time_units='nanoseconds relative to module start; CPU and duration counters are cumulative')
def stripe_csv(t):
 out=io.StringIO(newline='');fields=['stripe','y','height']+[f'{key}_{g}_ms' for key in STRIPE_FIELDS for g in range(4)]+[key+'_ms' for key in EXTRA]
 writer=csv.DictWriter(out,fieldnames=fields);writer.writeheader()
 for s in t['stripes']:
  row={k:s[k] for k in ('stripe','y','height')}
  row.update({f'{key}_{g}_ms':s[key][g]/1e6 for key in STRIPE_FIELDS for g in range(4)})
  row.update({key+'_ms':s[key]/1e6 for key in EXTRA});writer.writerow(row)
 return out.getvalue()

def validate_trace(t,result,cache):
 c=t['counters'];g=cache['global_stats'];calls=t['read_calls']
 assert c['reconstruction_joined']==c['JPEG_joined']==1 and c['reserved1']==result.get('compute_threads',2)-1
 assert c['RGB_produced']==c['RGB_consumed']==584 and 1<=c['RGB_queue_peak']<=16
 assert c['raw_callbacks']==((9344+t['batch']-1)//t['batch'])*16 and t['compute_copy_bytes']==t['compute_copy_ns']==0
 assert 0<c['replay_bytes']<4194304 and c['primary_bytes']+c['replay_bytes']==result['final_bytes']
 assert t['extra']['dropped_records']==0 and t['extra']['actual_read_calls']==len(calls)
 assert sum(r['returned'] for r in calls)-sum(r['flags'] for r in calls)+g['served_bytes']==result['raw_bytes']==16*65781760
 assert all(0<r['returned']<=r['requested']<=10813696 and r['flags'] in (0,256) for r in calls)
 assert t['extra']['source_CRC_tid']>0
 for stripe in t['stripes']:
  for k in range(4):assert 0<stripe['read_begin'][k]<=stripe['read_ready'][k]<=stripe['merge_begin'][k]<=stripe['merge_end'][k]
 for r in t['bands']:assert 0<r['render_begin']<=r['render_end']<=r['published']<=r['consume_begin']<=r['consume_end']
 for r in t['packets']:assert 0<r['queued']<=r['write_begin']<=r['write_end']
