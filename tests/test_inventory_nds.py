import struct
import unittest
from tools.inventory_nds import NDSFormatError, inventory

# Invented cartridge header, file tables, payload, and synthetic load addresses.


class InventoryTests(unittest.TestCase):
    def make_image(self):
        image = bytearray(0x240)
        image[0:12] = b"SYNTHETIC\0\0\0"
        image[0x0C:0x10] = b"TEST"
        image[0x1E] = 0
        struct.pack_into("<IIII", image, 0x20, 0x100, 0x08000000, 0x08000000, 4)
        struct.pack_into("<IIII", image, 0x30, 0x110, 0x09000000, 0x09000000, 4)
        struct.pack_into("<II", image, 0x40, 0x200, 11)
        struct.pack_into("<II", image, 0x48, 0x220, 8)
        struct.pack_into("<IHH", image, 0x200, 8, 0, 1)
        image[0x208:0x20B] = b"\x01a\0"
        struct.pack_into("<II", image, 0x220, 0x230, 0x231)
        image[0x230] = 0xAA
        return image

    def test_reports_sections_and_nitrofs_counts_without_extracting(self):
        result = inventory(self.make_image())

        self.assertEqual(result["product_code"], "TEST")
        self.assertEqual(result["arm9_size"], 4)
        self.assertEqual(result["arm7_size"], 4)
        self.assertEqual(result["nitrofs_files"], 1)
        self.assertEqual(result["nitrofs_named_files"], 1)
        self.assertEqual(result["nitrofs_unnamed_files"], 0)
        self.assertEqual(result["nitrofs_directories"], 1)
        self.assertEqual(result["arm9_overlays"], 0)
        self.assertEqual(result["arm7_overlays"], 0)

    def test_rejects_out_of_bounds_program_section(self):
        image = self.make_image()
        struct.pack_into("<II", image, 0x20, len(image) - 1, 4)

        with self.assertRaisesRegex(NDSFormatError, "ARM9 region is outside"):
            inventory(image)

    def test_counts_unnamed_fat_entries_without_rejecting_them(self):
        image = self.make_image()
        struct.pack_into("<IHH", image, 0x200, 8, 1, 1)
        struct.pack_into("<II", image, 0x48, 0x220, 16)
        struct.pack_into("<II", image, 0x228, 0x230, 0x231)

        result = inventory(image)

        self.assertEqual(result["nitrofs_files"], 2)
        self.assertEqual(result["nitrofs_named_files"], 1)
        self.assertEqual(result["nitrofs_unnamed_files"], 1)

    def test_validates_overlay_file_reference(self):
        image = self.make_image()
        image.extend(bytearray(0x50))
        struct.pack_into("<II", image, 0x50, 0x250, 32)
        struct.pack_into("<I", image, 0x268, 0)

        self.assertEqual(inventory(image)["arm9_overlays"], 1)

        struct.pack_into("<I", image, 0x268, 1)
        with self.assertRaisesRegex(NDSFormatError, "overlay 0 references missing FAT file 1"):
            inventory(image)

    def test_rejects_fnt_file_id_outside_fat(self):
        image = self.make_image()
        struct.pack_into("<H", image, 0x204, 1)

        with self.assertRaisesRegex(NDSFormatError, "invalid or duplicate FAT file 1"):
            inventory(image)


if __name__ == "__main__":
    unittest.main()
