import hashlib
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('patch_unlock', Path(__file__).resolve().parents[1]/'tools/patch_unlock.py')
patch = importlib.util.module_from_spec(spec); spec.loader.exec_module(patch)


class UnlockContractTests(unittest.TestCase):
    def setUp(self):
        self.original = b'abcdef'
        self.changed = b'abXYef'
        self.spec = dict(size=6, original_sha256=hashlib.sha256(self.original).hexdigest(),
                         candidate_sha256=hashlib.sha256(self.changed).hexdigest(), changed_bytes=2,
                         edits=[dict(offset='0x2', before=b'cd'.hex(), after=b'XY'.hex())])

    def test_exact_roundtrip(self):
        self.assertEqual(patch.transform(self.original, self.spec), self.changed)
        self.assertEqual(patch.transform(self.changed, self.spec, restore=True), self.original)

    def test_foreign_and_partial_input_rejected(self):
        for value in [b'abcdefg', b'abXdef', self.changed]:
            with self.assertRaises(ValueError): patch.transform(value, self.spec)

    def test_overlapping_manifest_rejected(self):
        self.spec['edits'] *= 2
        with self.assertRaises(ValueError): patch.transform(self.original, self.spec)

    def test_wrong_output_digest_rejected(self):
        self.spec['candidate_sha256'] = '0'*64
        with self.assertRaises(ValueError): patch.transform(self.original, self.spec)


if __name__ == '__main__':
    unittest.main()
