import struct
import tempfile
import unittest
from pathlib import Path

from tools.propose_eu_symbols import (
    ImageModule,
    SourceModule,
    parse_relocations,
    parse_symbols,
    propose_object,
    propose_symbols,
    relocation_mask,
)


class EuSymbolProposalTests(unittest.TestCase):
    def make_source(self, image: bytes, symbols: str, relocations: str = "") -> SourceModule:
        source_image = ImageModule("usa-main", 0x1000, Path("usa.bin"), image)
        return SourceModule(
            source_image,
            tuple(parse_symbols(symbols)),
            tuple(parse_relocations(relocations)),
        )

    def make_eu(self, name: str, base: int, data: bytes) -> ImageModule:
        return ImageModule(name, base, Path(f"{name}.bin"), data)

    def make_arm_object(self, directory: Path, text: bytes) -> Path:
        relocations = struct.pack("<II", 2, 2)
        strings = b"\0object_function\0"
        symbols = bytes(16) + struct.pack(
            "<IIIBBH", 1, 0, len(text), 0x12, 0, 1
        )
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
            payload.extend(bytes((-len(payload)) % alignment))
            offset = len(payload)
            payload.extend(content)
            sections.append(
                (
                    name_offsets[name],
                    kind,
                    0,
                    0,
                    offset,
                    len(content),
                    link,
                    info,
                    alignment,
                    entry_size,
                )
            )

        payload.extend(bytes((-len(payload)) % 4))
        section_table_offset = len(payload)
        for section in sections:
            payload.extend(struct.pack("<IIIIIIIIII", *section))

        ident = b"\x7fELF\x01\x01\x01\0" + bytes(8)
        header = struct.pack(
            "<HHIIIIIHHHHHH",
            1,
            40,
            1,
            0,
            0,
            section_table_offset,
            0,
            52,
            0,
            0,
            40,
            len(sections),
            5,
        )
        payload[:52] = ident + header
        object_path = directory / "sample.o"
        object_path.write_bytes(payload)
        return object_path

    def test_unique_match_ignores_relocated_bytes(self):
        function = bytes.fromhex("10 20 30 40 50 60 70 80")
        usa_image = bytes.fromhex("ee ee") + function + bytes.fromhex("ff")
        source = self.make_source(
            usa_image,
            "MoveThing kind:function(arm,size=0x8) addr:0x1002\n",
            "from:0x1004 kind:load to:0x34567890 module:main\n",
        )
        changed_function = bytes.fromhex("10 20 aa bb cc dd 70 80")
        decoy = bytes.fromhex("10 20 aa bb cc dd 70 99")
        eu_data = bytes.fromhex("00 00 00 00") + changed_function + bytes.fromhex("00 00") + decoy
        eu_module = self.make_eu("eur-main", 0x2000, eu_data)

        result = propose_symbols([source], [eu_module])

        self.assertEqual(result["summary"]["proposed"], 1)
        self.assertEqual(result["proposals"], [
            {
                "usa_module": "usa-main",
                "symbol": "MoveThing",
                "usa_address": "0x00001002",
                "size": 8,
                "eu_module": "eur-main",
                "eu_address": "0x00002004",
            }
        ])
        symbol = result["symbols"][0]
        self.assertEqual(symbol["masked_bytes"], 4)
        self.assertEqual(symbol["status"], "proposed")
        self.assertEqual(symbol["matches"], [{"module": "eur-main", "address": "0x00002004"}])

    def test_repeated_matches_are_ambiguous_and_never_proposed(self):
        source = self.make_source(
            bytes.fromhex("01 02 03 04 05 06 07 08"),
            "KnownFunction kind:function(thumb,size=0x8) addr:0x1000\n",
            "from:0x1002 kind:thumb_call to:0x2000 module:main\n",
        )
        candidate = bytes.fromhex("01 02 aa bb cc dd 07 08")
        eu_image = candidate + bytes.fromhex("99 99") + candidate

        result = propose_symbols(
            [source], [self.make_eu("eur-main", 0x3000, eu_image)]
        )

        self.assertEqual(result["summary"]["ambiguous"], 1)
        self.assertEqual(result["proposals"], [])
        self.assertEqual(result["symbols"][0]["status"], "ambiguous")
        self.assertEqual(result["symbols"][0]["reason"], "multiple_eu_matches")
        self.assertEqual(
            result["symbols"][0]["matches"],
            [
                {"module": "eur-main", "address": "0x00003000"},
                {"module": "eur-main", "address": "0x0000300a"},
            ],
        )

    def test_data_unknown_and_unmatched_symbols_remain_distinct(self):
        symbols = "\n".join(
            (
                "GlobalTable kind:data(any) addr:0x1000",
                "Uninitialized kind:bss addr:0x1004",
                "UnknownLabel kind:label addr:0x1008",
                "MissingFunction kind:function(arm,size=0x4) addr:0x100c",
            )
        )
        source_image = bytes.fromhex("11 22 33 44") + bytes(8) + bytes.fromhex("55 66 77 88")
        source = self.make_source(source_image, symbols)
        eu_module = self.make_eu("eur-main", 0x2000, bytes.fromhex("aa bb cc dd"))

        result = propose_symbols([source], [eu_module])

        self.assertEqual(result["proposals"], [])
        self.assertEqual(
            [symbol["status"] for symbol in result["symbols"]],
            ["data", "data", "unknown", "unknown"],
        )
        self.assertEqual(result["symbols"][0]["reason"], "data_symbol_not_proposed")
        self.assertEqual(result["symbols"][2]["reason"], "unsupported_symbol_kind")
        self.assertEqual(result["symbols"][3]["reason"], "no_eu_match")

    def test_relocation_parser_and_thumb_branch_width(self):
        symbols = parse_symbols(
            "ThumbFunction kind:function(thumb,size=0x6) addr:0x1235 local\n"
            "Data kind:data(any) addr:0x1240\n"
        )
        relocations = parse_relocations(
            "from:0x1236 kind:thumb_branch to:0x2000 module:main\n"
        )

        self.assertEqual(symbols[0].mode, "thumb")
        self.assertEqual(symbols[0].size, 6)
        self.assertEqual(symbols[0].address, 0x1235)
        self.assertEqual(symbols[1].category, "data")
        self.assertEqual(relocation_mask(relocations, 0x1234, 6), {2, 3})

    def test_all_relocated_function_bytes_are_not_a_matchable_signature(self):
        source = self.make_source(
            bytes.fromhex("11 22 33 44"),
            "Tiny kind:function(arm,size=0x4) addr:0x1000\n",
            "from:0x1000 kind:load to:0x2000 module:main\n",
        )
        result = propose_symbols(
            [source], [self.make_eu("eur-main", 0x3000, bytes.fromhex("aa bb cc dd"))]
        )

        self.assertEqual(result["proposals"], [])
        self.assertEqual(result["symbols"][0]["status"], "unknown")
        self.assertEqual(result["symbols"][0]["reason"], "all_function_bytes_relocated")


    def test_compiled_object_masks_relocations_for_unique_proposal(self):
        original = bytes.fromhex("10 20 30 40 50 60 70 80")
        candidate = bytes.fromhex("10 20 aa bb cc dd 70 80")
        decoy = bytes.fromhex("10 20 aa bb cc dd 70 99")
        eu_module = self.make_eu(
            "eur-main", 0x5000, bytes(4) + candidate + bytes(2) + decoy
        )
        with tempfile.TemporaryDirectory() as temporary:
            object_path = self.make_arm_object(Path(temporary), original)
            result = propose_object(object_path, [eu_module])

        self.assertEqual(result["summary"]["proposed"], 1)
        self.assertEqual(result["summary"]["ambiguous"], 0)
        self.assertEqual(result["symbols"][0]["masked_bytes"], 4)
        self.assertEqual(result["symbols"][0]["status"], "proposed")
        self.assertEqual(result["symbols"][0]["eu_address"], "0x00005004")
        self.assertEqual(result["proposals"][0]["symbol"], "object_function")

    def test_compiled_object_rejects_ambiguous_relocation_masked_hits(self):
        original = bytes.fromhex("10 20 30 40 50 60 70 80")
        candidate = bytes.fromhex("10 20 aa bb cc dd 70 80")
        eu_module = self.make_eu(
            "eur-main", 0x6000, candidate + bytes.fromhex("99 99") + candidate
        )
        with tempfile.TemporaryDirectory() as temporary:
            object_path = self.make_arm_object(Path(temporary), original)
            result = propose_object(object_path, [eu_module])

        self.assertEqual(result["summary"]["ambiguous"], 1)
        self.assertEqual(result["proposals"], [])
        self.assertEqual(result["symbols"][0]["status"], "ambiguous")
        self.assertEqual(
            result["symbols"][0]["matches"],
            [
                {"module": "eur-main", "address": "0x00006000"},
                {"module": "eur-main", "address": "0x0000600a"},
            ],
        )


if __name__ == "__main__":
    unittest.main()
