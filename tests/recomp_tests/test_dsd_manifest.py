"""Manifest discovery tests with invented module maps and synthetic ISA snippets."""

import json
import tempfile
import unittest
from pathlib import Path

from recomp.dsd_manifest import (
    _GapDiscovery,
    _GapScanner,
    _effect,
    _function_entry_targets,
    _indirect_entry_targets,
    _region_dispatch_entries,
    _read_symbols,
    build_arm9_manifest,
    write_arm9_manifest,
)


class DsdManifestTests(unittest.TestCase):
    def make_config(self, root: Path) -> Path:
        config_dir = root / "config"
        config_dir.mkdir()
        (config_dir / "main-symbols.txt").write_text(
            "entry kind:function(arm,size=0x10) addr:0x08000000\n"
            "thumb_fn kind:function(thumb,size=4) addr:0x08000010\n"
            "missing_size kind:function(arm) addr:0x08000020\n"
            "bad_isa kind:function(any,size=0x8) addr:0x08000028\n"
            ".L_local kind:label(arm) addr:0x08000030\n"
            "counter kind:data(any) addr:0x08000040\n"
            ".L_after_data kind:label(arm) addr:0x08000050\n",
            encoding="utf-8",
        )
        (config_dir / "main-relocs.txt").write_text(
            "from:0x08000004 kind:arm_call to:0x08000000 module:main\n"
            "from:0x08000008 kind:thumb_call to:0x09000000 module:itcm\n"
            "from:0x0800000c kind:arm_call to:0x09010000 module:dtcm\n"
            "from:0x08000010 kind:arm_call to:0x08010000 module:overlay(3)\n"
            "from:0x08000014 kind:arm_call to:0x08010004 module:overlays(3,4)\n"
            "from:0x08000018 kind:arm_call to:0x08010008 module:none\n"
            "from:0x0800001c kind:load to:0x08000040 module:main\n",
            encoding="utf-8",
        )
        (config_dir / "itcm-symbols.txt").write_text(
            "itcm_fn kind:function(arm,size=0x4) addr:0x09000000\n", encoding="utf-8"
        )
        (config_dir / "dtcm-symbols.txt").write_text(
            "dtcm_data kind:data(any) addr:0x09010000\n", encoding="utf-8"
        )
        (config_dir / "overlay-symbols.txt").write_text(
            "overlay_fn kind:function(thumb,size=0x6) addr:0x08010000\n"
            ".L_overlay kind:label(thumb) addr:0x08010002\n",
            encoding="utf-8",
        )
        (config_dir / "overlay-relocs.txt").write_text(
            "from:0x08010000 kind:arm_call to:0x08000000 module:main\n", encoding="utf-8"
        )
        metadata_dir = root / "build" / "build"
        metadata_dir.mkdir(parents=True)
        (metadata_dir / "arm9.yaml").write_text("base_address: 0x08000000\n", encoding="utf-8")
        (metadata_dir / "itcm.yaml").write_text("base_address: 0x09000000\n", encoding="utf-8")
        (metadata_dir / "dtcm.yaml").write_text("base_address: 0x09010000\n", encoding="utf-8")
        (metadata_dir / "arm9_overlays.yaml").write_text(
            "overlays:\n"
            "  - id: 3\n"
            "    base_address: 0x08010000\n"
            "  - id: 4\n"
            "    base_address: 0x08010000\n",
            encoding="utf-8",
        )
        config = config_dir / "arm9.yaml"
        config.write_text(
            """build_path: ../build
main_module:
  name: main
  object: ../build/main.bin
  symbols: main-symbols.txt
  relocations: main-relocs.txt
autoloads:
  - kind: Itcm
    name: itcm
    object: ../build/itcm.bin
    symbols: itcm-symbols.txt
  - kind: Dtcm
    name: dtcm
    object: ../build/dtcm.bin
    symbols: dtcm-symbols.txt
overlays:
  - id: 3
    name: ov003
    object: ../build/overlay-3.bin
    symbols: overlay-symbols.txt
    relocations: overlay-relocs.txt
  - id: 4
    name: ov004
    object: ../build/overlay-4.bin
""",
            encoding="utf-8",
        )
        return config

    def make_literal_thunk_module(self, root: Path, thunk_instruction: int) -> tuple[dict, list]:
        base = 0x1000  # Compact invented address space for gap-discovery fixtures.
        image = bytearray(0xC8)

        def write_word(address: int, value: int) -> None:
            offset = address - base
            image[offset : offset + 4] = value.to_bytes(4, "little")

        for address in range(base, base + len(image), 4):
            write_word(address, 0xE3000000)
        write_word(base, 0xE59F0008)  # ldr r0, [pc, #8]
        write_word(base + 4, 0xE12FFF1E)  # bx lr
        write_word(base + 8, 0xE1A00000)
        write_word(base + 12, 0xE1A00000)
        write_word(base + 16, base + 0x80)
        write_word(base + 0x80, thunk_instruction)
        write_word(base + 0xC0, 0xE92D4010)  # push {r4, lr}
        write_word(base + 0xC4, 0xE12FFF1E)  # bx lr

        image_path = root / "overlay.bin"
        image_path.write_bytes(image)
        module = {
            "name": "ov003",
            "module_id": 3,
            "base": base,
            "image": str(image_path),
            "functions": [{"addr": base, "thumb": False, "size": 0x14, "name": "literal_loader"}],
            "labels": [],
            "data_ranges": [],
        }
        return module, [(base, base + len(image))]

    def write_module_words(self, module: dict, words: dict[int, int], size: int = 0x100) -> list:
        base = module["base"]
        image = bytearray(Path(module["image"]).read_bytes())
        fill = (0xE3000000).to_bytes(4, "little")
        image.extend(fill * ((size - len(image)) // 4))
        for address, value in words.items():
            offset = address - base
            image[offset : offset + 4] = value.to_bytes(4, "little")
        Path(module["image"]).write_bytes(image)
        return [(base, base + size)]

    def test_discovers_internal_calls_and_post_return_entries_recursively(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            module, _ = self.make_literal_thunk_module(root, 0xEA00000E)
            code = self.write_module_words(module, {
                0x10C0: 0xE92D4010,  # push {r4, lr}
                0x10C4: 0x1A000009,  # bne 0x10f0 (keeps the broad gap entry live)
                0x10C8: 0xEB000000,  # bl 0x10d0
                0x10CC: 0xA1A0F00E,  # movge pc, lr
                0x10D0: 0xE3A01001,  # mov r1, #1
                0x10D4: 0xE12FFF1E,  # bx lr
                0x10D8: 0xE92D4020,  # push {r5, lr}, post-return entry
                0x10DC: 0xEB000001,  # bl 0x10e8
                0x10E0: 0xE12FFF1E,  # bx lr
                0x10E4: 0xE1A00000,  # padding
                0x10E8: 0xE3A02001,  # mov r2, #1
                0x10EC: 0xE12FFF1E,  # bx lr
                0x10F0: 0xE3A03001,  # bne target
                0x10F4: 0xE12FFF1E,  # bx lr
            })

            discovery = _GapDiscovery([module], [code])
            entries = {function["addr"]: function for function in module["functions"]}
            sources = {record["addr"]: record["source"] for record in discovery.summary()["added"]}

            self.assertEqual(entries[0x10C0]["size"], 0x40)
            self.assertEqual(entries[0x10D0]["size"], 8)
            self.assertEqual(entries[0x10D8]["size"], 0xC)
            self.assertEqual(entries[0x10E8]["size"], 8)
            self.assertEqual(sources[0x10D0], "call")
            self.assertEqual(sources[0x10D8], "post-return")
            self.assertEqual(sources[0x10E8], "call")


    def test_discovers_conditional_pc_dispatch_inside_literal_inferred_function(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            module, _ = self.make_literal_thunk_module(root, 0xE3000000)
            code = self.write_module_words(module, {
                0x1014: 0xE7F000F0,  # keep the unrelated gap from becoming a root
                0x1080: 0xE1A03003,  # literal-reached inferred function
                0x1084: 0x228F4034,  # addhs r4, pc, #0x34 -> 0x10c0
                0x1088: 0x328F403C,  # addlo r4, pc, #0x3c -> 0x10cc
                0x108C: 0xE2522003,  # subs r2, r2, #3
                0x1090: 0xBBFFFFDA,  # bllt 0x1000
                0x1094: 0xE28FE000,  # add lr, pc, #0
                0x1098: 0xE12FFF14,  # bx r4
                0x109C: 0xE2800001,  # continuation after the manual-link call
                0x10A0: 0xE12FFF1E,  # bx lr
                0x10C0: 0xE92D4020,  # push {r5, lr}
                0x10C4: 0xEA000005,  # b 0x10e0 keeps this entry's range live
                0x10C8: 0xE1A00000,  # padding
                0x10CC: 0xE92D4010,  # push {r4, lr}, second indirect entry
                0x10D0: 0xE1A06006,  # mov r6, r6
                0x10D4: 0xE12FFF1E,  # bx lr
                0x10D8: 0xE1A00000,  # padding
                0x10DC: 0xE92D4010,  # next prologue bounds 0x10cc
                0x10E0: 0xE1A00000,  # b target
                0x10E4: 0xE12FFF1E,  # bx lr
                0x10E8: 0xE7F000F0,  # undefined instruction ends the code
            }, size=0x200)

            discovery = _GapDiscovery([module], [code])
            entries = {function["addr"]: function for function in module["functions"]}
            sources = {record["addr"]: record["source"] for record in discovery.summary()["added"]}

            self.assertEqual(sources[0x1080], "literal")
            self.assertEqual(entries[0x1080]["size"], 0x180)
            self.assertEqual(entries[0x10C0]["size"], 0xC)
            self.assertEqual(entries[0x10CC]["size"], 0xC)
            self.assertEqual(sources[0x10C0], "indirect")
            self.assertEqual(sources[0x10CC], "indirect")

    def test_dispatch_regions_resolve_manual_branch_and_relative_table_targets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            module, _ = self.make_literal_thunk_module(root, 0xE3000000)
            words = {
                0x1014: 0xE7F000F0,  # keep the unrelated gap from becoming a root
                0x1080: 0xE28FE00C,  # add lr, pc, #0xc -> continuation 0x1094
                0x1084: 0x1A00000D,  # bne 0x10c0, a manual-link call
                0x1088: 0xE28F5F14,  # add r5, pc, #0x50 -> relative table 0x10e0
                0x108C: 0xE7956102,  # ldr r6, [r5, r2, lsl #2]
                0x1090: 0xE086F005,  # add pc, r6, r5
                0x1094: 0xE12FFF1E,  # continuation returns to the parent
                0x10A0: 0xE3A00001,  # first table target
                0x10A4: 0xE12FFF1E,
                0x10B0: 0xE3A00002,  # second table target
                0x10B4: 0xE12FFF1E,
                0x10C0: 0xE3A01003,  # conditional B target
                0x10C4: 0xEA000004,  # keep the later continuation in this function
                0x10DC: 0xE12FFF1E,  # terminal code before the table
                0x10E0: 0xFFFFFFC0,  # 0x10e0 + offset -> 0x10a0
                0x10E4: 0xFFFFFFD0,  # 0x10e0 + offset -> 0x10b0
                0x10E8: 0xE7F000F0,  # first invalid table target
            }
            for address in range(0x1098, 0x10C0, 4):
                words.setdefault(address, 0xE1A00000)
            for address in range(0x10C8, 0x10DC, 4):
                words.setdefault(address, 0xE1A00000)
            code = self.write_module_words(module, words, size=0x200)

            discovery = _GapDiscovery([module], [code])
            region = next(
                region for region in module["dispatch_regions"]
                if region["addr"] == 0x1080 and not region["thumb"]
            )
            entries = set(region["entries"])
            sources = {record["addr"]: record["source"] for record in discovery.summary()["added"]}

            self.assertEqual(sources[0x1080], "literal")
            self.assertTrue({0x1080, 0x1088, 0x1094, 0x10A0, 0x10B0, 0x10C0} <= entries)
            self.assertNotIn(0x10E0, entries)
            self.assertNotIn(0x10E4, entries)
            self.assertTrue(any(
                low <= 0x10E0 and high >= 0x10E8
                for low, high in region["data_ranges"]
            ))

    def test_direct_call_infers_mixed_code_data_dispatch_region(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            module, _ = self.make_literal_thunk_module(root, 0xE3000000)
            code = self.write_module_words(module, {
                0x1014: 0xE7F000F0,  # keep the leading gap from becoming a root
                0x1080: 0xE92D4000,  # push {lr}
                0x1084: 0xEB00000D,  # bl 0x10c0
                0x1088: 0xE8BD8000,  # pop {pc}
                0x108C: 0xE7F000F0,  # trailing data after the caller
                0x10C0: 0xEA000006,  # b 0x10e0 over embedded data
                0x10C4: 0xE7F000F0,  # non-code inside the dispatch region
                0x10E0: 0xE28F5028,  # add r5, pc, #0x28 -> table at 0x1110
                0x10E4: 0xE7956102,  # ldr r6, [r5, r2, lsl #2]
                0x10E8: 0xE086F005,  # add pc, r6, r5
                0x10F0: 0xE12FFF1E,  # first table target
                0x1100: 0xE12FFF1E,  # second table target
                0x1110: 0xFFFFFFE0,  # 0x1110 + offset -> 0x10f0
                0x1114: 0xFFFFFFF0,  # 0x1110 + offset -> 0x1100
                0x1118: 0xE7F000F0,  # terminates the relative table
            }, size=0x200)

            discovery = _GapDiscovery([module], [code])
            functions = {function["addr"]: function for function in module["functions"]}
            sources = {record["addr"]: record["source"] for record in discovery.summary()["added"]}
            region = next(
                region for region in module["dispatch_regions"]
                if not region["thumb"]
                and region["addr"] <= 0x10C0 < region["addr"] + region["size"]
                and {0x10C0, 0x10F0, 0x1100} <= set(region["entries"])
            )

            self.assertEqual(sources[0x1080], "literal")
            self.assertEqual(sources[0x10C0], "call")
            self.assertEqual(functions[0x10C0]["size"], 0x140)
            self.assertTrue({0x10C0, 0x10E0, 0x10F0, 0x1100} <= set(region["entries"]))
            self.assertTrue(any(
                low <= 0x10C4 and high >= 0x10C8
                for low, high in region["data_ranges"]
            ))
            self.assertTrue(any(
                low <= 0x1110 and high >= 0x1118
                for low, high in region["data_ranges"]
            ))

    def test_discovers_branch_stub_inside_dsd_function(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            module, _ = self.make_literal_thunk_module(root, 0xE3000000)
            module["functions"].extend([
                {"addr": 0x1080, "thumb": False, "size": 8, "name": "dsd_owner"},
                {"addr": 0x10A8, "thumb": False, "size": 4, "name": "known_target"},
            ])
            code = self.write_module_words(module, {
                0x1010: 0x1084,       # the DSD caller contains a pointer to the stub
                0x1014: 0xE7F000F0,  # keep the leading gap from becoming a root
                0x1080: 0xE8BD8038,  # existing DSD function entry
                0x1084: 0xEA000007,  # branch stub to 0x10a8
                0x10A8: 0xE12FFF1E,  # known target
            }, size=0x200)

            discovery = _GapDiscovery([module], [code])
            functions = {function["addr"]: function for function in module["functions"]}
            sources = {record["addr"]: record["source"] for record in discovery.summary()["added"]}

            self.assertEqual(functions[0x1084]["size"], 4)
            self.assertEqual(sources[0x1084], "literal")



    def test_dispatch_regions_reset_at_decode_gaps_and_unconditional_branches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            module, _ = self.make_literal_thunk_module(root, 0xE3000000)
            sections = self.write_module_words(module, {
                0x1080: 0xE28F4020,  # add r4, pc, #0x20 -> 0x10a8
                0x1084: 0xE7F000F0,  # undecodable gap
                0x1088: 0xE12FFF14,  # bx r4 must not use stale r4
                0x108C: 0xEA000000,  # b 0x1094, no fallthrough at 0x1090
                0x1090: 0xE3A00000,  # unreachable instruction
                0x1094: 0xE12FFF1E,  # branch target
                0x10A8: 0xE3A00001,  # possible stale indirect target
            }, size=0x200)
            scanner = _GapScanner(
                module,
                Path(module["image"]).read_bytes(),
                sections,
                set(),
                sections,
                set(),
            )

            entries, data_ranges = _region_dispatch_entries(
                scanner, {"addr": 0x1080, "size": 0x30, "thumb": False}
            )

            self.assertIn([0x1084, 0x1088], data_ranges)
            self.assertIn([0x1098, 0x10A8], data_ranges)

            self.assertNotIn(0x10A8, entries)
            self.assertNotIn(0x1090, entries)
            instructions = []
            for address in (0x1080, 0x1088):
                insn = scanner._instruction(address, 0x10B0, False)
                self.assertIsNotNone(insn)
                effect = _effect(insn, False)
                self.assertIsNotNone(effect)
                instructions.append((address, insn, effect))
            indirect = _indirect_entry_targets(
                instructions, False, sections, lambda _address: True
            )
            self.assertNotIn((0x10A8, False), indirect)
            self.assertIn(0x1094, entries)

    def test_does_not_split_or_modify_dsd_function_interiors(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            module, _ = self.make_literal_thunk_module(root, 0xEA00000E)
            dsd_function = {"addr": 0x10D0, "thumb": False, "size": 8, "name": "dsd_owner"}
            module["functions"].append(dsd_function.copy())
            module["functions"].sort(key=lambda function: function["addr"])
            code = self.write_module_words(module, {
                0x10C0: 0xE92D4010,  # push {r4, lr}
                0x10C4: 0xEB000002,  # bl 0x10d4, inside the DSD function
                0x10C8: 0xE12FFF1E,  # bx lr
                0x10D0: 0xE92D4010,
                0x10D4: 0xE12FFF1E,
            })

            discovery = _GapDiscovery([module], [code])
            entries = {function["addr"]: function for function in module["functions"]}

            self.assertEqual(entries[0x10D0], dsd_function)
            self.assertNotIn(0x10D4, entries)
            self.assertNotIn(0x10D4, {record["addr"] for record in discovery.summary()["added"]})

    def test_discovers_pc_relative_register_transfer_targets(self):
        with tempfile.TemporaryDirectory() as temporary:
            module, _ = self.make_literal_thunk_module(Path(temporary), 0xE3000000)
            module["functions"][0]["size"] = 0x20
            code = self.write_module_words(module, {
                0x1000: 0xE92D4010,  # push {r4, lr}
                0x1004: 0xE1A03003,  # mov r3, r3
                0x1008: 0x228F4070,  # addhs r4, pc, #0x70 -> 0x1080
                0x100C: 0x328F407D,  # addlo r4, pc, #0x7d -> Thumb 0x1090
                0x1010: 0xE28F6089,  # add r6, pc, #0x89 -> Thumb 0x10a0
                0x1014: 0xE12FFF36,  # blx r6
                0x1018: 0xE12FFF14,  # bx r4
                0x101C: 0xE12FFF1E,  # bx lr
                0x1080: 0xE92D4010,
                0x1084: 0xE12FFF1E,
                0x1088: 0xE7F000F0,  # undefined instruction ends the synthetic function
                0x1090: 0x4770B510,  # Thumb push {r4, lr}; bx lr
                0x1094: 0x0000DE00,
                0x10A0: 0x4770B510,
                0x10A4: 0x0000DE00,
            })

            discovery = _GapDiscovery([module], [code])
            added = {
                (record["addr"], record["thumb"]): record["source"]
                for record in discovery.summary()["added"]
            }

            self.assertEqual(added[(0x1080, False)], "indirect")
            self.assertEqual(added[(0x1090, True)], "indirect")
            self.assertEqual(added[(0x10A0, True)], "indirect-call")

    def test_conditional_branch_preserves_fallthrough_indirect_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            module, _ = self.make_literal_thunk_module(Path(temporary), 0xE3000000)
            sections = self.write_module_words(module, {
                0x1000: 0xE28F4070,  # add r4, pc, #0x70 -> 0x1078
                0x1004: 0x1A000000,  # bne 0x100c; fallthrough remains reachable
                0x1008: 0xE12FFF14,  # bx r4
                0x1078: 0xE1A00000,
            })
            scanner = _GapScanner(
                module,
                Path(module["image"]).read_bytes(),
                sections,
                set(),
                sections,
                set(),
            )
            instructions = []
            for address in (0x1000, 0x1004, 0x1008):
                insn = scanner._instruction(address, 0x10C8, False)
                self.assertIsNotNone(insn)
                effect = _effect(insn, False)
                self.assertIsNotNone(effect)
                instructions.append((address, insn, effect))

            targets = _indirect_entry_targets(
                instructions, False, sections, lambda _address: True
            )

            self.assertEqual(targets[(0x1078, False)], "indirect")

    def test_discovers_thumb_adr_to_mov_pc_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            module, _ = self.make_literal_thunk_module(Path(temporary), 0xE3000000)
            module["functions"] = [
                {"addr": 0x1000, "thumb": True, "size": 4, "name": "thumb_adr"}
            ]
            image_path = Path(module["image"])
            image = bytearray(image_path.read_bytes())
            for address, halfword in (
                (0x1000, 0xA41F),  # adr r4, 0x1080
                (0x1002, 0x46A7),  # mov pc, r4
                (0x1080, 0xB510),  # push {r4, lr}
                (0x1082, 0x4770),  # bx lr
                (0x1084, 0xBE00),  # unsupported BKPT ends the synthetic function
            ):
                offset = address - module["base"]
                image[offset : offset + 2] = halfword.to_bytes(2, "little")
            image_path.write_bytes(image)
            sections = [(module["base"], module["base"] + len(image))]

            discovery = _GapDiscovery([module], [sections])

            self.assertIn(
                (0x1080, True),
                {(record["addr"], record["thumb"]) for record in discovery.summary()["added"]},
            )

    def test_kills_pc_relative_data_address_after_load(self):
        with tempfile.TemporaryDirectory() as temporary:
            module, sections = self.make_literal_thunk_module(Path(temporary), 0xE3000000)
            self.write_module_words(module, {
                0x1038: 0xE92D4010,  # valid function bytes at the data address
                0x103C: 0xE12FFF1E,
                0x1080: 0xE24F4050,  # sub r4, pc, #0x50 -> 0x1038
                0x1084: 0xE7D44006,  # ldrb r4, [r4, r6]
                0x1088: 0xE12FFF14,  # bx r4
            })
            image = Path(module["image"]).read_bytes()
            scanner = _GapScanner(module, image, sections, {0x1080}, sections, set())
            entries, _ = _function_entry_targets(
                scanner, {"addr": 0x1080, "thumb": False, "size": 12}
            )

            self.assertNotIn((0x1038, False), entries)

    def test_discovers_literal_pointer_thunk_and_branch_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            module, code = self.make_literal_thunk_module(Path(temporary), 0xEA00000E)
            discovery = _GapDiscovery([module], [code])
            entries = {function["addr"]: function for function in module["functions"]}

            self.assertEqual(entries[0x1080]["size"], 4)
            self.assertEqual(entries[0x10C0]["size"], 8)
            self.assertEqual(
                {entry["addr"] for entry in discovery.summary()["added"]},
                {0x1080, 0x10C0},
            )

    def test_ignores_literal_pointer_to_invalid_gap_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            module, code = self.make_literal_thunk_module(Path(temporary), 0xE3000000)
            _GapDiscovery([module], [code])

            self.assertEqual([function["addr"] for function in module["functions"]], [0x1000])


    def test_reads_local_symbol_annotations(self):
        with tempfile.TemporaryDirectory() as temporary:
            symbols = Path(temporary) / "symbols.txt"
            symbols.write_text(
                "local_function kind:function(arm,size=0x8) addr:0x1000 local\n"
                "local_data kind:data(any) addr:0x1008 local\n"
                "next_data kind:data(any) addr:0x1010\n",
                encoding="utf-8",
            )

            functions, labels, data_ranges = _read_symbols(symbols)

            self.assertEqual(functions, [
                {"addr": 0x1000, "thumb": False, "size": 8, "name": "local_function"},
            ])
            self.assertEqual(labels, [])
            self.assertEqual(data_ranges, [[0x1008, 0x1010]])

    def test_local_function_annotations_cover_execution_coverage(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.make_config(Path(temporary))
            symbols = config.parent / "main-symbols.txt"
            with symbols.open("a", encoding="utf-8") as stream:
                stream.write(
                    "local_function kind:function(arm,size=0x8) "
                    "addr:0x08000060 local\n"
                )
            coverage = Path(temporary) / "coverage.txt"
            coverage.write_text("08000060 a\n08000064 a\n", encoding="utf-8")

            manifest = build_arm9_manifest(config, coverage_path=coverage)

            self.assertEqual(manifest["coverage"]["misses"], 0)
            self.assertEqual(manifest["coverage"]["covered"], 2)

    def test_builds_all_modules_with_absolute_images_and_runtime_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.make_config(Path(temporary))
            manifest = build_arm9_manifest(config)

            self.assertEqual(manifest["cpu"], "arm9")
            self.assertEqual(manifest["arch"], "v5te")
            modules = {module["module_id"]: module for module in manifest["modules"]}
            self.assertEqual(set(modules), {-1, -2, -3, 3, 4})
            self.assertEqual(modules[-1]["base"], 0x08000000)
            self.assertEqual(modules[-2]["base"], 0x09000000)
            self.assertEqual(modules[-3]["base"], 0x09010000)
            self.assertEqual(modules[3]["base"], 0x08010000)
            self.assertEqual(
                modules[-1]["image"],
                str((Path(temporary) / "build/main.bin").resolve()),
            )
            self.assertEqual(
                modules[-1]["functions"],
                [
                    {"addr": 0x08000000, "thumb": False, "size": 0x10, "name": "entry"},
                    {"addr": 0x08000010, "thumb": True, "size": 4, "name": "thumb_fn"},
                ],
            )
            self.assertEqual(
                modules[-1]["labels"],
                [{"addr": 0x08000030, "thumb": False}, {"addr": 0x08000050, "thumb": False}],
            )
            self.assertEqual(modules[-1]["data_ranges"], [[0x08000040, 0x08000050]])

    def test_parses_relocation_targets_and_preserves_overlay_ambiguity(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.make_config(Path(temporary))
            modules = {module["module_id"]: module for module in build_arm9_manifest(config)["modules"]}
            calls = modules[-1]["calls"]
            self.assertEqual(len(calls), 6)
            self.assertEqual(calls[0], {"from": 0x08000004, "to": 0x08000000, "modules": [-1]})
            self.assertEqual(calls[1]["modules"], [-2])
            self.assertEqual(calls[2]["modules"], [-3])
            self.assertEqual(calls[3]["modules"], [3])
            self.assertEqual(calls[4]["modules"], [3, 4])
            self.assertEqual(calls[5]["modules"], [])
            self.assertEqual(modules[3]["calls"][0]["modules"], [-1])

    def test_write_api_serializes_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = self.make_config(root)
            output = root / "nested" / "manifest.json"
            write_arm9_manifest(config, output)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), build_arm9_manifest(config))


if __name__ == "__main__":
    unittest.main()
