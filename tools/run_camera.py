"""Run exactly one verified native JPG experiment through A7C2Tools Python."""
from pathlib import Path
from datetime import datetime
import argparse,hashlib,importlib.resources,json,msvcrt,re,struct,sys,time,traceback
sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent;ROOT=HERE.parent
cli=argparse.ArgumentParser(description=__doc__)
cli.add_argument('--tools',required=True,type=Path,help='Locally installed A7C2Tools _internal directory')
cli.add_argument('--profile',required=True,type=Path,help='Private same-body binding profile')
cli.add_argument('--calibration',required=True,type=Path,help='Private 792-byte PSCLR001 reference curve file')
args=cli.parse_args()
TOOLS=args.tools.resolve()
profile=json.loads(args.profile.read_text(encoding='utf-8'))
if profile.get('schema')!=1 or not profile.get('usb_serial_sha256') or not profile.get('identity',{}).get('serial_sha256'):
    raise ValueError('Complete the private same-body profile; do not disable identity checks.')
if profile['identity'].get('model')!='ILCE-7CM2':raise ValueError('Unsupported camera model')
(ROOT/'private/runs').mkdir(parents=True,exist_ok=True)
sys.path[:0]=[str(TOOLS/'src/camera_runtime'),str(TOOLS/'src/camera_runtime/vendor'),str(TOOLS/'src/looks')]
sys.path.append(str(TOOLS));sys.modules.setdefault('importlib_resources',importlib.resources)
import transport
from looktool.usbservice import open_service
from looktool.usbidentity import capture
from report import parse
from cache_report import parse_cache,validate_cache
from three_trace import parse_trace,validate_trace,stripe_csv

def wait_for_cooldown():
    latest=0
    for path in (ROOT/'private/runs').glob('*/run.private.json'):
        record=json.loads(path.read_text(encoding='utf-8'))
        if record.get('service_exit_requested') and record.get('finished'):
            latest=max(latest,datetime.fromisoformat(record['finished']).timestamp())
    left=30-(time.time()-latest)
    if 0<left<=30:time.sleep(left)

def run_camera(variant,shared_link=None):
    cfg=json.loads((ROOT/'build/build.json').read_text())
    blob=(ROOT/'build/camera-merge.so').read_bytes()
    if not cfg['elf_aarch64'] or hashlib.sha256(blob).hexdigest()!=cfg['sha256'] or len(blob)>1048576:
        raise ValueError('Build/module mismatch')
    for name,digest in cfg['source_files'].items():
        if hashlib.sha256((ROOT/name).read_bytes()).hexdigest()!=digest:raise ValueError('Source changed: '+name)
    proof={'library_sha256':profile['files']['libInfraFsys.so']['sha256']}
    expected=profile
    params=args.calibration.read_bytes()
    if hashlib.sha256(params).hexdigest()!='5b84ac2dfac24713448b942a139d9f706b747280854d1340b0cfccda52ee5646':
        raise ValueError('This historical reproduction requires the original reference curve profile')
    folder=ROOT/'private/runs'/('camera-'+datetime.now().strftime('%Y%m%d-%H%M%S'));folder.mkdir(exist_ok=False)
    prefix='/tmp/jiritsu/';owned={'a7c2-preview-staged.so','libJit.so','a7c2.probe.result','ps-color-calibration.input','ps-j3-three.trace','ps-native-cache.trace'}
    fs_path=prefix+'../../usr/lib/libInfraFsys.so'
    runtime_paths={name:prefix+'../../lib/'+name for name in ('libc-2.25.so','libpthread-2.25.so','libdl-2.25.so')}
    runtime_hashes={n:profile['files'][n]['sha256'] for n in runtime_paths}
    allowed={fs_path,prefix+'../../usr/lib/appFw.so',prefix+'../../cp/cpapp-b.bin',prefix+'../../etc/ld.so.preload',*runtime_paths.values()}|{prefix+n for n in owned}
    writes={prefix+'a7c2-preview-staged.so':blob,prefix+'libJit.so':blob,prefix+'ps-color-calibration.input':params}
    state=dict(execution_environment='actual_camera',user_authorized_handle_reuse=True,compositions_requested=1,local_performance_tests=0,new_emulation_tests=0,native_cache_requested_frames=0,started=datetime.now().astimezone().isoformat(),phase='STARTING',config=cfg,module_sha256=cfg['sha256'],persistent_program_writes=0,settings_writes=0,temporary_writes=[],temporary_cleanup={},firmware_hashes_before={},firmware_hashes_after={},runtime_hashes={},events=[])
    def save():(folder/'run.private.json').write_text(json.dumps(state,ensure_ascii=False,indent=2),encoding='utf8')
    def event(name,**kw):state['events'].append(dict(time=datetime.now().astimezone().isoformat(),event=name,**kw));save();print(name,kw,flush=True)
    class Guard(transport.Session):
        def call(self,method,*args):
            name=getattr(method,'__name__','')
            if name=='getBackupPresetData' and args==(True,):pass
            elif name=='sendSenserPacket' and len(args)==2 and args[0]==0x40 and len(args[1])>=4 and struct.unpack_from('<HH',args[1]) in [(0x1001,n) for n in (15,16,17,18)]:pass
            else:raise PermissionError('Unlisted camera operation '+name)
            return super().call(method,*args)
        def select(self,path):
            if path not in allowed:raise PermissionError('Unlisted path '+path)
            self.selected_path=path;return super().select(path)
        def jiritsu(self,cmd,data=b'',missing=False):
            if cmd not in (15,16,17,18):raise PermissionError('Unlisted command')
            if cmd==16:
                if self.selected_path not in writes or data!=b'\0\xff\0'+writes[self.selected_path]:raise PermissionError('Unexpected bytes')
                state['temporary_writes'].append(self.selected_path);save()
            if cmd==18 and (self.selected_path not in self.delete_authorized or data!=b'\0\xff'):raise PermissionError('Delete without ownership proof')
            return super().jiritsu(cmd,data,missing)
        def ap_write(self,*a,**kw):raise PermissionError('AP/settings writes forbidden')
        def cp_write(self,*a,**kw):raise PermissionError('CP writes forbidden')
    def program_hash(link,name):
        path=prefix+'../../'+('usr/lib/appFw.so' if name=='appFw.so' else 'cp/cpapp-b.bin');data=link.read(path);digest=hashlib.sha256(data).hexdigest()
        if digest!=expected['files'][name]['sha256'] or len(data)!=expected['files'][name]['bytes']:raise ValueError('Program mismatch '+name)
        return digest
    def cleanup(link,name,data):
        path=prefix+name;actual=link.read(path,True)
        if actual is not None:
            if actual!=data:raise ValueError('Changed temporary file preserved '+name)
            link.delete_authorized.add(path)
            try:link.select(path);link.jiritsu(18,b'\0\xff')
            finally:link.delete_authorized.remove(path)
        if link.read(path,True) is not None:raise ValueError('Cleanup incomplete '+name)
        state['temporary_cleanup'][name]=True;save()
    lock=None;acquired=False;link=None;code=1
    try:
        if shared_link is None:
            lock=(Path.home()/'.a7c2-lut/device.lock').open('rb');msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1);acquired=True
            wait_for_cooldown()
            if capture(transport)!=expected['usb_serial_sha256']:raise ValueError('Different USB body')
            link=open_service(transport)
        else:
            if not shared_link.healthy:raise ValueError('Shared service channel is unhealthy')
            link=shared_link;state['USB_verification_scope']='after_shared_suite'
        link.__class__=Guard;link.selected_path=None;link.delete_authorized=set();event('service_connected')
        try:
            snapshot=link.call(link.camera.getBackupPresetData,True)
            if transport.service_identity(snapshot,transport.core.MODEL)!=expected['identity']:raise ValueError('Different service identity')
            del snapshot
            for n in ('appFw.so','cpapp-b.bin'):state['firmware_hashes_before'][n]=program_hash(link,n)
            if hashlib.sha256(link.read(fs_path)).hexdigest()!=proof['library_sha256']:raise ValueError('Fsys library differs')
            for name,path in runtime_paths.items():
                actual=hashlib.sha256(link.read(path)).hexdigest()
                if actual!=runtime_hashes[name]:raise ValueError('Runtime differs '+name)
                state['runtime_hashes'][name]=actual
            if link.read(prefix+'../../etc/ld.so.preload',True):raise ValueError('Existing preload configuration')
            for name in sorted(owned):
                if link.read(prefix+name,True) is not None:raise ValueError('Existing temporary file preserved '+name)
            event('body_programs_and_runtime_verified')
            link.temp('a7c2-preview-staged.so',blob)
            if link.read(prefix+'a7c2-preview-staged.so')!=blob:raise ValueError('Exact large-module staging failed')
            link.select(prefix+'ps-color-calibration.input');link.jiritsu(16,b'\0\xff\0'+params)
            if link.read(prefix+'ps-color-calibration.input')!=params:raise ValueError('Parameter staging differs')
            state['phase']='ABOUT_TO_RUN';event('about_to_run_JPG_once',codec=cfg['codec'],parallel=cfg['parallel'],single_body_write=cfg['single_body_write'],dimensions=[cfg['width'],cfg['height']])
            link.backend.budget=360;start=time.monotonic();link.temp('libJit.so',blob);state['native_command_wall_seconds']=time.monotonic()-start
            raw=link.read(prefix+'a7c2.probe.result',True)
            if raw is None:raise ValueError('Constructor produced no report; do not repeat blindly')
            (folder/'result.bin').write_bytes(raw)
            if len(raw)!=512 or raw[:8]!=b'PSJPG019':raise ValueError('Unrecognized native result retained')
            cleanup(link,'a7c2.probe.result',raw)
            for name in ('libJit.so','a7c2-preview-staged.so','ps-color-calibration.input'):cleanup(link,name,writes[prefix+name])
            result=parse(raw);state['result']=result
            trace_raw=link.read(prefix+'ps-j3-three.trace',True)
            if trace_raw is None:raise ValueError('Missing three-thread timeline; do not rerun blindly')
            (folder/'trace.bin').write_bytes(trace_raw);cleanup(link,'ps-j3-three.trace',trace_raw);trace=parse_trace(trace_raw,cfg['height'],cfg['batch']);state['trace']=trace
            (folder/'trace.json').write_text(json.dumps(trace,indent=2),encoding='utf8');(folder/'stripes.csv').write_text(stripe_csv(trace),encoding='utf8')
            import csv,io
            rows=io.StringIO(newline='');writer=csv.DictWriter(rows,fieldnames=['requested','returned','window','frame','flags','duration_ns']);writer.writeheader();writer.writerows(trace['read_calls']);(folder/'read-calls.csv').write_text(rows.getvalue(),encoding='utf8')
            cache_raw=link.read(prefix+'ps-native-cache.trace',True)
            if cache_raw is None:raise ValueError('Missing native-cache trace; do not repeat blindly')
            (folder/'native-cache.bin').write_bytes(cache_raw);cleanup(link,'ps-native-cache.trace',cache_raw)
            cache=parse_cache(cache_raw);state['native_cache']=cache
            (folder/'native-cache.json').write_text(json.dumps(cache,indent=2),encoding='utf8')
            if result['status']==0:
                validate_cache(cache,result)
                validate_trace(trace,result,cache)
                baseline=json.loads((ROOT/'evidence/baseline-result.json').read_text(encoding='utf8'))
                if cfg['height']==9344 and any(result[k]!=baseline[k] for k in ('source_window_crc','linear_crc','body_crc','body_bytes')):raise ValueError('Three-thread data differs from pinned J3')
            save();link.backend.budget=180
            for n in ('appFw.so','cpapp-b.bin'):state['firmware_hashes_after'][n]=program_hash(link,n)
            if state['firmware_hashes_before']!=state['firmware_hashes_after']:raise ValueError('Program bytes changed')
            if result['status']==0:
                if not re.fullmatch(r'[A-Z]:/DCIM/'+re.escape(cfg['dcf'])+r'/DSC00001[.]JPG',result['output_path']):raise ValueError('Unexpected output path')
                if cfg['height']==9344 and result['linear_crc']!='3d757bed':raise ValueError('Full linear output differs from pinned reference')
                if result['worker_joined']!=cfg['parallel'] or result['single_body_write']!=cfg['single_body_write']:raise ValueError('Scheduling/container contract differs')
                if cfg['codec']=='turbo' and not result['neon_preflight']:raise ValueError('Turbo SIMD/TLS preflight failed')
                if cfg['single_body_write'] and (result['wrap_copy_bytes'] or result['body_write_bytes']!=result['body_bytes']-2):raise ValueError('Body was not single-write')
            event('JPG_function_returned',**result)
            state['phase']='JPG_COMPLETE_CLEANED' if result['status']==0 else 'JPG_ERROR_CLEANED';code=0 if result['status']==0 else 1
        finally:
            try:
                if link.healthy:
                    for name in ('libJit.so','a7c2-preview-staged.so','ps-color-calibration.input'):
                        if prefix+name in state['temporary_writes'] and not state['temporary_cleanup'].get(name):cleanup(link,name,writes[prefix+name])
            except BaseException as exc:state['cleanup_error']=str(exc);save()
            finally:
                if shared_link is None:link.__exit__(None,None,None);state['service_exit_requested']=True
                else:state['service_kept_open_for_suite']=True
                save()
        if shared_link is None:
            deadline=time.monotonic()+20
            while time.monotonic()<deadline and '0e8a' not in transport._sony_pids():time.sleep(.5)
            state['normal_USB_body_verified']='0e8a' in transport._sony_pids() and capture(transport)==expected['usb_serial_sha256']
            if not state['normal_USB_body_verified']:raise ValueError('Final USB verification failed')
            event('normal_USB_body_verified')
        if state.get('cleanup_error'):raise ValueError('Final cleanup verification failed')
    except BaseException as exc:
        state.update(phase='FAILED',error_type=type(exc).__name__,error=str(exc));traceback.print_exc();code=1
    finally:
        if lock:
            if acquired:lock.seek(0);msvcrt.locking(lock.fileno(),msvcrt.LK_UNLCK,1)
            lock.close()
        state['finished']=datetime.now().astimezone().isoformat();save();print(json.dumps(dict(phase=state['phase'],evidence=str(folder),result=state.get('result')),ensure_ascii=False),flush=True)
    return code

if __name__=='__main__':
    sys.exit(run_camera('measured-baseline'))
