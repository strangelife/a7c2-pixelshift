"""Inspect or patch separate local image copies. Never accesses a camera."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def transform(data, spec, restore=False):
    expected = spec['candidate_sha256'] if restore else spec['original_sha256']
    if len(data) != spec['size'] or sha(data) != expected:
        raise ValueError('Input size/SHA-256 differs from the exact supported image.')
    result = bytearray(data)
    previous_end = 0
    for edit in sorted(spec['edits'], key=lambda e: int(e['offset'], 16)):
        at = int(edit['offset'], 16)
        before = bytes.fromhex(edit['after'] if restore else edit['before'])
        after = bytes.fromhex(edit['before'] if restore else edit['after'])
        if not before or len(before) != len(after) or at < previous_end or at + len(before) > len(data):
            raise ValueError('Invalid or overlapping patch window.')
        if data[at:at + len(before)] != before:
            raise ValueError(f'Unexpected original bytes at {at:#x}.')
        result[at:at + len(after)] = after
        previous_end = at + len(after)
    target = spec['original_sha256'] if restore else spec['candidate_sha256']
    if sha(result) != target:
        raise ValueError('Full output digest differs from the pinned candidate.')
    changed = sum(a != b for a, b in zip(data, result))
    if changed != spec['changed_bytes']:
        raise ValueError('Unexpected number of changed bytes.')
    return bytes(result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['inspect', 'apply', 'restore'])
    p.add_argument('--image', choices=['app', 'wbi'], default='app')
    p.add_argument('--input', required=True, type=Path)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    spec = json.loads((ROOT/'unlock/patches/c2-2.01.json').read_text())['images'][args.image]
    data = args.input.read_bytes()
    digest = sha(data)
    state = 'original' if digest == spec['original_sha256'] else 'candidate' if digest == spec['candidate_sha256'] else 'unsupported'
    if args.action == 'inspect':
        print(json.dumps(dict(image=args.image, state=state, bytes=len(data), sha256=digest, camera_io=False), indent=2))
        return int(state == 'unsupported' or len(data) != spec['size'])
    if args.output is None or args.output.resolve() == args.input.resolve():
        p.error('A separate --output file is required. Input files are never overwritten.')
    out = transform(data, spec, restore=args.action == 'restore')
    with args.output.open('xb') as f:
        f.write(out)
    if sha(args.output.read_bytes()) != sha(out):
        raise OSError('Output readback digest failed.')
    print(json.dumps(dict(camera_io=False, bytes=len(out), sha256=sha(out), changed_bytes=spec['changed_bytes']), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
