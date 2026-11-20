import struct
GLOBAL='status requested_frames cached_frames unit_type capacity handle physical extent_count alloc_call alloc_return alloc_ns map_return pattern prepare_ns prefill_ns SD_bytes SD_calls short_reads hits copy_ns served_bytes misses free_call free_return unmap_return close_return present_before present_after release_ns allocation_attempts minimum_available active_after'.split()
FRAME='frame crop_at prefix file_at IO_bytes calls short_reads read_ns open_ns seek_ns close_ns status hits copy_ns served_bytes source_crc'.split()
def parse_cache(data):
    assert struct.unpack_from('<8sII',data)==(b'PSNCACHE',1,1296) and len(data)==1296
    signed=lambda v:v-(1<<64) if v>>63 else v
    g=dict(zip(GLOBAL,struct.unpack_from('<32Q',data,16)))
    for k in ('status','alloc_call','alloc_return','map_return','free_call','free_return','unmap_return','close_return','present_before','present_after'):g[k]=signed(g[k])
    frames=[]
    for i in range(g['cached_frames']):
        f=dict(zip(FRAME,struct.unpack_from('<16Q',data,272+i*128)));f['status']=signed(f['status']);f['source_crc']=f"{f['source_crc']:08x}";frames.append(f)
    return dict(global_stats=g,frames=frames)
def validate_cache(cache,result):
 g=cache['global_stats'];assert cache['frames']==[]
 assert all(g[k]==0 for k in ('status','requested_frames','cached_frames','capacity','handle','active_after','SD_bytes','SD_calls','served_bytes','hits','copy_ns'))
