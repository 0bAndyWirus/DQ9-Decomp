import struct
import tempfile
import unittest
from pathlib import Path

from tools.find_eu_matches import find_pattern_matches, parse_elf_object, scan_object, Module

# Invented ELF and module bytes; addresses use a synthetic non-hardware range.


class EuMatcherTests(unittest.TestCase):
    def make_arm_object(self, directory: Path) -> Path:
        text = bytes.fromhex("11 22 00 00 00 00 77 88")
        relocations = struct.pack("<II", 2, 2)  # R_ARM_ABS32 at .text+2
        strings = b"\0sample_function\0"
        symbols = bytes(16) + struct.pack("<IIIBBH", 1, 0, len(text), 0x12, 0, 1)
        section_names = b"\0.text\0.rel.text\0.symtab\0.strtab\0.shstrtab\0"
        name_offsets = {
            ".text": 1,
            ".rel.text": 7,
            ".symtab": 17,
            ".strtab": 25,
            ".shstrtab": 33,
        }

        payload = bytearray(52)
        sections = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
        for name, content, kind, link, info, alignment, entry_size in (
            (".text", text, 1, 0, 0, 4, 0),
            (".rel.text", relocations, 9, 3, 1, 4, 8),
            (".symtab", symbols, 2, 4, 1, 4, 16),
            (".strtab", strings, 3, 0, 0, 1, 0),
            (".shstrtab", section_names, 3, 0, 0, 1, 0),
        ):
            padding = (-len(payload)) % alignment
            payload.extend(bytes(padding))
            offset = len(payload)
            payload.extend(content)
            sections.append((name_offsets[name], kind, 0, 0, offset, len(content),
                             link, info, alignment, entry_size))

        padding = (-len(payload)) % 4
        payload.extend(bytes(padding))
        section_table_offset = len(payload)
        for section in sections:
            payload.extend(struct.pack("<IIIIIIIIII", *section))

        ident = b"\x7fELF\x01\x01\x01\0" + bytes(8)
        header = struct.pack(
            "<HHIIIIIHHHHHH", 1, 40, 1, 0, 0, section_table_offset, 0,
            52, 0, 0, 40, len(sections), 5,
        )
        payload[:52] = ident + header
        path = directory / "sample.o"
        path.write_bytes(payload)
        return path

    def test_parser_masks_relocation_and_finds_function(self):
        with tempfile.TemporaryDirectory() as temporary:
            object_path = self.make_arm_object(Path(temporary))
            sections, functions, relocations = parse_elf_object(object_path)
            text = next(section for section in sections if section.name == ".text")
            self.assertEqual(relocations[text.index], {2})
            self.assertEqual([(function.name, function.offset, function.size) for function in functions],
                             [("sample_function", 0, 8)])

            module = Module("main", 0x08000000, Path("main.bin"),
                            bytes.fromhex("ee 11 22 aa bb cc dd 77 88 ff"))
            result = scan_object(object_path, [module])
            self.assertEqual(result["functions"][0]["hits"][0]["address"], 0x08000001)
            self.assertEqual(result["functions"][0]["relocated_bytes_masked"], 4)
            self.assertEqual(result["functions"][0]["unique"], True)

    def test_finds_overlapping_hits_and_preserves_ambiguity(self):
        self.assertEqual(find_pattern_matches(b"ababa", b"aba"), [0, 2])
        self.assertEqual(find_pattern_matches(b"xxxx", b"xx"), [0, 1, 2])

    def test_empty_and_all_masked_patterns_are_handled(self):
        self.assertEqual(find_pattern_matches(b"abc", b""), [])
        self.assertEqual(find_pattern_matches(b"abc", b"xy", {0, 1}), [0, 1])


if __name__ == "__main__":
    unittest.main()
