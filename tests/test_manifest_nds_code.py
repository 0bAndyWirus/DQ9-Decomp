import hashlib
import struct
import unittest

from tools.manifest_nds_code import build_manifest

# Invented cartridge metadata and payload; load/entry values are synthetic addresses.


class CodeManifestTests(unittest.TestCase):
    def make_image(self):
        image = bytearray(0x280)
        image[0:12] = b"SYNTHETIC\0\0\0"
        image[0x0C:0x10] = b"TEST"
        image[0x1E] = 0
        struct.pack_into("<IIII", image, 0x20, 0x100, 0x08000100, 0x08000000, 4)
        struct.pack_into("<IIII", image, 0x30, 0x110, 0x09000110, 0x09000000, 4)
        struct.pack_into("<II", image, 0x40, 0x200, 9)
        struct.pack_into("<II", image, 0x48, 0x210, 8)
        struct.pack_into("<II", image, 0x50, 0x220, 32)
        struct.pack_into("<IHH", image, 0x200, 8, 0, 1)
        image[0x208] = 0
        struct.pack_into("<II", image, 0x210, 0x260, 0x264)
        struct.pack_into("<8I", image, 0x220, 7, 0x08010000, 4, 2, 0x08010000, 0x08010004, 0, 0)
        image[0x100:0x104] = b"ARM9"
        image[0x110:0x114] = b"ARM7"
        image[0x260:0x264] = b"OVL0"
        return image

    def test_lists_all_executable_modules_with_hashes_and_addresses(self):
        image = self.make_image()
        manifest = build_manifest(image)
        modules = {module["module_id"]: module for module in manifest["modules"]}

        self.assertEqual(manifest["module_count"], 3)
        self.assertEqual(manifest["decompiled_module_count"], 0)
        self.assertEqual(set(modules), {"arm9.main", "arm7.main", "arm9.overlay.007"})
        self.assertEqual(modules["arm9.main"]["entry_address"], 0x08000100)
        self.assertEqual(modules["arm9.main"]["sha256"], hashlib.sha256(b"ARM9").hexdigest())
        self.assertEqual(modules["arm7.main"]["load_address"], 0x09000000)
        self.assertEqual(modules["arm9.overlay.007"]["fat_file_id"], 0)
        self.assertEqual(modules["arm9.overlay.007"]["sha256"], hashlib.sha256(b"OVL0").hexdigest())
        self.assertEqual({module["decomp_status"] for module in modules.values()}, {"not_started"})


if __name__ == "__main__":
    unittest.main()
