import hashlib
import json
import io
import os
import tempfile
import unittest
import zlib
from dataclasses import asdict
from pathlib import Path
from unittest.mock import patch

from tools.verify_reference_rom import fingerprint, load_profile, main, mismatches


class FingerprintTests(unittest.TestCase):
    def make_image(self, product_code="TEST", revision=0):
        data = bytearray(range(64))
        data[0x0C:0x10] = product_code.encode("ascii")
        data[0x1E] = revision
        return bytes(data)

    def test_reads_header_and_hashes_entire_stream(self):
        data = self.make_image()
        result = fingerprint(io.BytesIO(data))

        self.assertEqual(result.size, len(data))
        self.assertEqual(result.product_code, "TEST")
        self.assertEqual(result.revision, 0)
        self.assertEqual(result.sha1, hashlib.sha1(data).hexdigest())
        self.assertEqual(result.md5, hashlib.md5(data).hexdigest())
        self.assertEqual(result.crc32, zlib.crc32(data))

    def test_rejects_wrong_product_revision_and_size(self):
        expected = fingerprint(io.BytesIO(self.make_image()))
        result = fingerprint(io.BytesIO(self.make_image("FAIL", 1) + b"synthetic"))
        differences = mismatches(result, expected)

        self.assertIn("product code", differences)
        self.assertIn("revision", differences)
        self.assertIn("file size", differences)

    def test_short_file_has_unreadable_header(self):
        result = fingerprint(io.BytesIO(b"short"))

        self.assertIsNone(result.product_code)
        self.assertIsNone(result.revision)
        expected = fingerprint(io.BytesIO(self.make_image()))
        self.assertIn("product code", mismatches(result, expected))
        self.assertIn("revision", mismatches(result, expected))

    def test_private_profile_and_cli_image_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "synthetic.nds"
            image.write_bytes(self.make_image())
            expected = fingerprint(io.BytesIO(image.read_bytes()))
            profile = root / "profile.json"
            values = asdict(expected)
            values["crc32"] = hex(expected.crc32)
            profile.write_text(json.dumps(values))
            self.assertEqual(load_profile(profile), expected)
            self.assertEqual(main(["--rom", str(image), "--profile", str(profile)]), 0)
            with patch.dict(os.environ, {"DQ9_ROM": str(image)}):
                self.assertEqual(main(["--profile", str(profile)]), 0)
            image.write_bytes(self.make_image("FAIL", 1))
            self.assertEqual(main(["--rom", str(image), "--profile", str(profile)]), 1)

    def test_malformed_profiles_and_missing_input_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "profile.json"
            image = root / "synthetic.nds"
            image.write_bytes(self.make_image())
            values = asdict(fingerprint(io.BytesIO(self.make_image())))
            for field, value in (("size", -1), ("revision", 256), ("product_code", "bad"),
                                 ("sha1", "not a digest"), ("md5", None), ("crc32", True)):
                with self.subTest(field=field):
                    profile.write_text(json.dumps({**values, field: value}))
                    self.assertEqual(main(["--rom", str(image), "--profile", str(profile)]), 2)
            self.assertEqual(main(["--rom", str(root / "absent.nds"), "--profile", str(profile)]), 2)


if __name__ == "__main__":
    unittest.main()
