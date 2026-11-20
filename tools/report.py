"""Parse the strict 512-byte JPG host report and pipeline wait counters."""
import struct

def parse(data):
    if len(data)!=512 or struct.unpack_from('<8sII',data)!=(b'PSJPG019',1,512):raise ValueError('Invalid JPG report')
    status,cleanup=struct.unpack_from('<2q',data,16)
    names=['elapsed_ns','mem_before','mem_min','mem_after','sd_free','raw_bytes','body_bytes','final_bytes','readback_bytes','prepare_ns','pipeline_ns','thumbnail_ns','wrap_ns','flush_ns','readback_ns','publish_ns','tail_ns','body_write_bytes','wrap_copy_bytes','header_write_bytes']
    result=dict(status=status,cleanup=cleanup,**dict(zip(names,struct.unpack_from('<20Q',data,32))))
    w,h,frames,bands,linear,body,final,readback,flags,thumb=struct.unpack_from('<10I',data,192)
    result.update(width=w,height=h,frames=frames,bands=bands,linear_crc=f'{linear:08x}',body_crc=f'{body:08x}',file_crc=f'{final:08x}',readback_crc=f'{readback:08x}',flags=flags,thumbnail_bytes=thumb,output_path=data[232:328].split(b'\0')[0].decode('ascii'),source_window_crc=[f'{v:08x}' for v in struct.unpack_from('<16I',data,448)],all_handles_closed=bool(flags&64),full_readback_verified=bool(flags&16),worker_joined=bool(flags&0x100000),single_body_write=bool(flags&0x10000),neon_preflight=bool(flags&0x200000))
    stats=dict(zip(['raw_wait_ns','output_wait_ns','file_wait_ns','read_ns','write_ns','group_merge_ns','linear_check_ns','render_ns','encoder_total_ns'],struct.unpack_from('<9Q',data,328)))
    peak,joined,*extra=struct.unpack_from('<2I5Q',data,400)
    stats.update(output_high_water=peak,worker_joined=bool(joined),**dict(zip(['file_thread_cpu_ns','compute_thread_cpu_ns','pipeline_shutdown_ns','fetch_total_ns','emit_packet_ns'],extra)))
    if stats['fetch_total_ns']+stats['emit_packet_ns']<=stats['encoder_total_ns']:stats['jpeg_exclusive_ns']=stats['encoder_total_ns']-stats['fetch_total_ns']-stats['emit_packet_ns']
    result['pipeline_stats']=stats;result['total_wall_seconds']=result['elapsed_ns']/1e9
    if status==0:
        assert result['all_handles_closed'] and result['full_readback_verified'] and result['final_bytes']==result['readback_bytes']
        assert result['file_crc']==result['readback_crc']
        assert result['bands']==h//16 and result['frames']==16
    return result
