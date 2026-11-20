import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tools.verify_firmware import verify_files


class OriginalFirmwareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root/'appFw.so'
        self.path.write_bytes(b'original firmware fixture')
        self.profile = {'files': {'appFw.so': {
            'path': 'appFw.so', 'bytes': self.path.stat().st_size,
            'sha256': hashlib.sha256(self.path.read_bytes()).hexdigest()}}}

    def test_original_is_read_only(self):
        before = self.path.read_bytes()
        self.assertIn('appFw.so', verify_files(self.root, self.profile))
        self.assertEqual(self.path.read_bytes(), before)

    def test_same_size_modified_firmware_rejected(self):
        content = bytearray(self.path.read_bytes())
        content[-1] ^= 1
        self.path.write_bytes(content)
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            verify_files(self.root, self.profile)

    def test_truncated_firmware_rejected(self):
        self.path.write_bytes(b'truncated')
        with self.assertRaisesRegex(ValueError, 'size mismatch'):
            verify_files(self.root, self.profile)

    def test_missing_firmware_rejected(self):
        self.path.unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            verify_files(self.root, self.profile)

    def test_profile_uses_original_unlock_inputs(self):
        root = Path(__file__).resolve().parents[1]
        profile = json.loads((root/'profiles/original-2.01.json').read_text(encoding='utf-8'))
        images = json.loads((root/'unlock/patches/c2-2.01.json').read_text(encoding='utf-8'))['images']
        for filename, image in [('appFw.so', 'app'), ('nflasha5', 'wbi')]:
            self.assertEqual(profile['files'][filename]['sha256'], images[image]['original_sha256'])
            self.assertNotEqual(profile['files'][filename]['sha256'], images[image]['candidate_sha256'])


if __name__ == '__main__':
    unittest.main()
