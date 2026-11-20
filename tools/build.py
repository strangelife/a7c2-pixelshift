"""Cross-compile the measured camera pipeline; never execute it on the host."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
JPEG_SHA = 'b2db824adb96ed9f4c1864caff2ae5dae92bac135359f5ce3e8b4b2888e0138a'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--zig', default='zig')
    p.add_argument('--jpeg-dir', required=True, type=Path, help='AArch64 libjpeg.a and its matching headers')
    p.add_argument('--dcf', type=int, default=173)
    args = p.parse_args()
    if not 172 <= args.dcf < 999:
        p.error('The historical baseline uses a new unused DCF number in 172..998.')
    jpeg = args.jpeg_dir.resolve()
    if hashlib.sha256((jpeg/'libjpeg.a').read_bytes()).hexdigest() != JPEG_SHA:
        p.error('Different Turbo archive. The measured profile pins the original AArch64 libjpeg-turbo archive.')
    for name in ['jpeglib.h', 'jconfig.h', 'jmorecfg.h', 'jerror.h']:
        if not (jpeg/name).is_file():
            p.error('Missing JPEG header '+name)
    from elftools.elf.elffile import ELFFile
    dest = ROOT/'build'; dest.mkdir(exist_ok=True)
    native = ROOT/'native'; output = dest/'camera-merge.so'
    flags = dict(TEST_HEIGHT=9344, TEST_CODEC=1, TEST_PARALLEL=1, TEST_SINGLE_WRITE=1,
                 TEST_BATCH=256, TEST_REUSE=1, TEST_READ_BYTES=1802240, RESIDENT_COUNT=0,
                 IO_ROWS=256, RECON_ROWS=256, REAR_SLOTS=3, RGB_SLOTS=8,
                 COMPUTE_THREADS=2, AUX_MERGE=1, PARALLEL_TONE=1)
    cmd = [args.zig, 'cc', '-target', 'aarch64-linux-gnu.2.25', '-std=c11', '-O2', '-shared',
           '-fPIC', '-fvisibility=hidden', '-fno-builtin', '-fno-stack-protector', '-g0', '-pthread',
           '-Wl,--strip-debug', '-Wl,--no-undefined', '-Wl,-Bsymbolic', '-Wl,-z,noexecstack',
           '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
           '-Wno-misleading-indentation', '-I', str(native), '-I', str(jpeg)]
    cmd += [f'-D{k}={v}' for k, v in flags.items()]
    cmd += [f'-DTEST_DIRECTORY="PA{args.dcf}DMA"', f'-DTEST_DCF="{args.dcf}MSDCF"']
    cmd += [str(native/name) for name in ['target_module.c', 'stream_core.c', 'stream_jpeg.c', 'stream_turbo.c', 'stream_pipeline.c']]
    cmd += [str(jpeg/'libjpeg.a'), '-lm', '-ldl', '-o', str(output)]
    env = os.environ.copy()
    env['ZIG_GLOBAL_CACHE_DIR'] = str(dest/'zig-global')
    env['ZIG_LOCAL_CACHE_DIR'] = str(dest/'zig-local')
    subprocess.run(cmd, check=True, env=env)
    blob = output.read_bytes(); elf = ELFFile(io.BytesIO(blob))
    if not (elf['e_type'] == 'ET_DYN' and elf['e_machine'] == 'EM_AARCH64' and len(blob) <= 1048576):
        raise ValueError('Unexpected module type or size.')
    if any(s['p_type'] == 'PT_LOAD' and s['p_flags'] & 3 == 3 for s in elf.iter_segments()):
        raise ValueError('Writable executable segment.')
    tags = list(elf.get_section_by_name('.dynamic').iter_tags())
    if any(t.entry.d_tag == 'DT_TEXTREL' for t in tags):
        raise ValueError('Unexpected text relocations.')
    if next(t.entry.d_val for t in tags if t.entry.d_tag == 'DT_INIT_ARRAYSZ') != 8:
        raise ValueError('Unexpected number of constructors.')
    manifest = dict(sha256=hashlib.sha256(blob).hexdigest(), bytes=len(blob), jpeg_sha256=JPEG_SHA,
                    dcf=f'{args.dcf}MSDCF', directory=f'PA{args.dcf}DMA', width=14016, height=9344,
                    batch=256, parallel=True, single_body_write=True, codec='turbo', elf_aarch64=True,
                    host_execution=False, camera_execution=False,
                    source_files={p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in native.iterdir() if p.is_file()})
    (dest/'build.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps({k:v for k,v in manifest.items() if k != 'source_files'}, indent=2))


if __name__ == '__main__':
    main()
