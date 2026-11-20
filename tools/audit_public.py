"""Check the source publication boundary without opening private files."""
import ast
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
EXCLUDE = {'.git', 'private', 'build', 'dist', '__pycache__', '.venv'}
FORBIDDEN_SUFFIXES = {'.so', '.bin', '.arw', '.jpg', '.jpeg', '.dng', '.exe', '.dll', '.a', '.zip', '.pdb'}


def files():
    # Prune before walking: compiler cache and private logs can be very large.
    import os
    for root, dirs, names in os.walk(ROOT):
        dirs[:] = sorted(d for d in dirs if d not in EXCLUDE)
        for name in sorted(names):
            yield Path(root)/name


def main():
    errors = []; count = 0
    for path in files():
        rel = path.relative_to(ROOT).as_posix(); count += 1
        if '.private.' in path.name or path.suffix.lower() in FORBIDDEN_SUFFIXES:
            errors.append('Private/binary file: '+rel); continue
        text = path.read_text(encoding='utf-8')
        if path.suffix == '.py':
            ast.parse(text, filename=rel)
        if path.suffix == '.json':
            value = json.loads(text)
            if rel.startswith('profiles/'):
                if value.get('usb_serial_sha256') is not None or value.get('identity', {}).get('serial_sha256') is not None:
                    errors.append('Nonempty device identity in public template: '+rel)
        if re.search(r'(?:C:[\\/]+Users[\\/]|D:[\\/]+java[\\/]|gh[pousr]_[A-Za-z0-9]{24,}|github_pat_[A-Za-z0-9_]{30,})', text):
            # The check's own pattern contains a literal example of the path.
            if rel != 'tools/audit_public.py': errors.append('Local path or token pattern: '+rel)
        if path.suffix in ('.c', '.h', '.inc'):
            for include in re.findall(r'#include\s+"([^"\n]+)"', text):
                if not (path.parent/include).is_file(): errors.append('Missing native include: '+include)
        if path.suffix == '.md':
            for target in re.findall(r'(?<!!)\[[^\]]*\]\(([^\s)]+)\)', text):
                if '://' in target or target.startswith('#'): continue
                if not (path.parent/target.split('#')[0]).exists(): errors.append('Broken document link: '+rel+' -> '+target)
    if errors:
        raise SystemExit('\n'.join(errors))
    print(json.dumps(dict(public_files=count, forbidden_files=0, syntax_and_links='passed', camera_io=False), indent=2))


if __name__ == '__main__':
    main()
