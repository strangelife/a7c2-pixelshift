"""Verify an extracted, unmodified 2.01 firmware tree offline; never write it."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT/'profiles/original-2.01.json'


def verify_files(root, profile):
    root = Path(root).resolve()
    verified = {}
    for name, spec in profile['files'].items():
        path = (root/spec['path']).resolve()
        if not path.is_relative_to(root) or not path.is_file():
            raise ValueError('Missing or out-of-tree firmware file: '+name)
        if path.stat().st_size != spec['bytes']:
            raise ValueError('Original firmware size mismatch: '+name)
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != spec['sha256']:
            raise ValueError('Original firmware SHA-256 mismatch: '+name)
        verified[name] = dict(bytes=spec['bytes'], sha256=digest)
    return verified


def verify_signatures(root, profile):
    from elftools.elf.elffile import ELFFile
    path = Path(root)/profile['files']['libInfraFsys.so']['path']
    checked = []
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        if elf['e_machine'] != 'EM_AARCH64':
            raise ValueError('Expected AArch64 Fsys library')

        def check(name, va, expected):
            for segment in elf.iter_segments():
                if (segment['p_type'] == 'PT_LOAD' and segment['p_flags'] & 1
                        and segment['p_vaddr'] <= va
                        and va+len(expected) <= segment['p_vaddr']+segment['p_filesz']):
                    stream.seek(segment['p_offset']+va-segment['p_vaddr'])
                    if stream.read(len(expected)) != expected:
                        raise ValueError('Original Fsys signature mismatch: '+name)
                    checked.append(name)
                    return
            raise ValueError('Fsys signature outside executable file segment: '+name)

        def byte_array(text, pattern):
            match = re.search(pattern, text)
            if not match:
                raise ValueError('Missing native byte signature')
            return bytes(int(n.strip(), 0) for n in match[1].split(',') if n.strip())

        for header in ['sd_read_helpers.h', 'sd_write_helpers.h']:
            text = (ROOT/'native'/header).read_text(encoding='utf-8')
            for name, numbers in re.findall(r'static const u8 fs_(\w+)_bytes\[\]=\{([^}]+)\}', text):
                va = int(re.search(r'#define FS_'+name+r'_VA (0x[0-9a-fA-F]+)', text)[1], 16)
                check(header+':'+name, va, bytes(int(n) for n in numbers.split(',')))
        text = (ROOT/'native/mount_query_signature.h').read_text(encoding='utf-8')
        va = int(re.search(r'#define QUERY_VA (0x[0-9a-fA-F]+)', text)[1], 16)
        check('mount_query', va, byte_array(text, r'query_signature\[QUERY_BYTES\]=\{([^}]+)\}'))
        text = (ROOT/'native/target_module.c').read_text(encoding='utf-8')
        va = int(re.search(r'RENAME_VA=(0x[0-9a-fA-F]+)', text)[1], 16)
        text = (ROOT/'native/rename_signature.inc').read_text(encoding='utf-8')
        check('rename', va, byte_array(text, r'rename_signature\[\]=\{([^}]+)\}'))
    if len(checked) != 19:
        raise ValueError('Unexpected native Fsys signature coverage')
    return checked


def verify_firmware(root):
    profile = json.loads(PROFILE.read_text(encoding='utf-8'))
    files = verify_files(root, profile)
    signatures = verify_signatures(root, profile)
    return dict(model=profile['model'], firmware=profile['firmware'],
                baseline=profile['baseline'], files=files,
                native_Fsys_signatures=signatures, camera_io=False,
                firmware_written=False, camera_compatibility_tested=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True, help='Root of the extracted original firmware tree')
    args = parser.parse_args()
    try:
        report = verify_firmware(args.root)
    except (ValueError, OSError) as error:
        parser.exit(1, str(error)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
