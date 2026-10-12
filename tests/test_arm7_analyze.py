"""Invented ARM/Thumb fixtures; no bytes or layouts come from a game image."""

import contextlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools.arm7_analyze import (
    Arm7Analyzer,
    analyze_image,
    main,
    ModuleState,
    build_modules,
    parse_autoload_layout,
)


# Synthetic public-test regions, unrelated to any Nintendo DS title layout.
SYNTHETIC_BASE = 0x08000000
SYNTHETIC_AUTOLOAD_BASE = 0x09000000


class Arm7AnalyzeTests(unittest.TestCase):
    def make_autoload_image(self):
        image = bytearray(0x80)
        params_offset = 0x08
        source_offset = 0x20
        list_offset = 0x30
        list_end = list_offset + 12
        struct.pack_into(
            "<III",
            image,
            params_offset,
            SYNTHETIC_BASE + list_offset,
            SYNTHETIC_BASE + list_end,
            SYNTHETIC_BASE + source_offset,
        )
        struct.pack_into("<II", image, params_offset + 12, SYNTHETIC_BASE + 0x50, SYNTHETIC_BASE + 0x54)
        struct.pack_into("<III", image, list_offset, SYNTHETIC_AUTOLOAD_BASE, 0x10, 4)
        image[source_offset : source_offset + 0x10] = bytes(range(0x10))
        return bytes(image)

    def test_parses_and_materializes_autoload_source_and_bss(self):
        image = self.make_autoload_image()

        layout = parse_autoload_layout(image, SYNTHETIC_BASE)
        modules = build_modules(image, layout, SYNTHETIC_BASE)

        self.assertEqual(layout.params_offset, 0x08)
        self.assertEqual(layout.list_start, SYNTHETIC_BASE + 0x30)
        self.assertEqual(len(layout.blocks), 1)
        self.assertEqual(layout.blocks[0].source_offset, 0x20)
        self.assertEqual([module.module_id for module in modules], [-4, -5])
        self.assertEqual(modules[1].base, SYNTHETIC_AUTOLOAD_BASE)
        self.assertEqual(modules[1].name, "arm7autoload_09000000")
        self.assertEqual(modules[1].loaded_size, 0x10)
        self.assertEqual(modules[1].image, bytes(range(0x10)) + bytes(4))
        self.assertEqual(modules[0].image[0x50:0x54], bytes(4))
        self.assertTrue(any(start <= 0x18 < end for start, end, _ in modules[0].data_ranges))

    def test_autoload_names_and_ids_follow_generic_descriptor_order(self):
        image = bytearray(self.make_autoload_image())
        struct.pack_into("<I", image, 0x0C, SYNTHETIC_BASE + 0x48)
        struct.pack_into("<III", image, 0x30, SYNTHETIC_AUTOLOAD_BASE, 8, 4)
        struct.pack_into("<III", image, 0x3C, SYNTHETIC_AUTOLOAD_BASE + 0x1000, 8, 4)
        layout = parse_autoload_layout(bytes(image), SYNTHETIC_BASE)
        modules = build_modules(bytes(image), layout, SYNTHETIC_BASE)

        self.assertEqual([module.module_id for module in modules], [-4, -5, -6])
        self.assertEqual(
            [module.name for module in modules[1:]],
            ["arm7autoload_09000000", "arm7autoload_09001000"],
        )
        self.assertEqual(modules[1].image, bytes(range(8)) + bytes(4))
        self.assertEqual(modules[2].image, bytes(range(8, 16)) + bytes(4))

    def test_discovers_direct_arm_call_and_thumb_function_pointer(self):
        base = SYNTHETIC_BASE
        image = bytearray(0x60)
        # Synthetic ARM entry: push {lr}; BL base+0x10; pop {pc}.
        struct.pack_into("<III", image, 0, 0xE92D4000, 0xEB000001, 0xE8BD8000)
        # ARM callee: push {lr}; mov r0,#7; bx lr.
        struct.pack_into("<III", image, 0x10, 0xE92D4000, 0xE3A00007, 0xE12FFF1E)
        # A function-pointer table entry uses bit 0 to select Thumb state.
        struct.pack_into("<I", image, 0x20, base + 0x41)
        struct.pack_into("<HHH", image, 0x40, 0xB500, 0x2001, 0xBD00)
        module = ModuleState(
            name="synthetic",
            module_id=-4,
            base=base,
            image=bytes(image),
            loaded_size=len(image),
            executable_ranges=[(0, len(image))],
        )
        analyzer = Arm7Analyzer([module])

        analyzer.discover([(base, False, "synthetic entry")])
        analyzer.finalize()

        self.assertIn((0, base, False), module.functions)
        self.assertIn((0, base + 0x10, False), module.functions)
        self.assertIn((0, base + 0x40, True), module.functions)
        self.assertTrue(module.functions[(0, base + 0x40, True)]["size"] >= 6)
        self.assertIn((0, base + 0x40, True), module.function_evidence)
        self.assertFalse(analyzer.decode_anomalies)
        self.assertIn((base + 4, base + 0x10, -4), module.function_calls[(0, base, False)])


    def test_discovers_frameless_callback_beyond_short_leaf_lookahead(self):
        base = SYNTHETIC_BASE
        image = bytearray(0x140)
        # Literal pointers are scanned as callback roots. The valid callback is
        # frameless and returns only after 24 instructions, beyond the old cap.
        struct.pack_into("<II", image, 0x10, base + 0x40, base + 0xC0)
        # Original synthetic routine: register arithmetic only, no prologue.
        arithmetic = (
            0xE2811001,  # add r1, r1, #1
            0xE0222001,  # eor r2, r2, r1
            0xE2433002,  # sub r3, r3, #2
            0xE1800003,  # orr r0, r0, r3
            0xE1A0C080,  # mov r12, r0, lsl #1
        )
        callback_words = tuple(arithmetic[i % len(arithmetic)] for i in range(23)) + (
            0xE12FFF1E,  # bx lr, the 24th instruction
        )
        struct.pack_into(f"<{len(callback_words)}I", image, 0x40, *callback_words)
        # A second pointer targets valid ARM decoding but no return/tail branch.
        struct.pack_into("<32I", image, 0xC0, *([0xE1A00000] * 32))
        module = ModuleState(
            name="synthetic",
            module_id=-4,
            base=base,
            image=bytes(image),
            loaded_size=len(image),
            executable_ranges=[(0, len(image))],
        )
        analyzer = Arm7Analyzer([module])

        analyzer.discover([])
        analyzer.finalize()

        self.assertIn((0, base + 0x40, False), module.functions)
        self.assertIn(
            f"validated function pointer at 0x{base + 0x10:08X}",
            module.function_evidence[(0, base + 0x40, False)],
        )
        self.assertNotIn((0, base + 0xC0, False), module.functions)

    def test_repeated_function_pointers_mark_data_before_prologue_scan(self):
        source_base = SYNTHETIC_BASE
        target_base = SYNTHETIC_AUTOLOAD_BASE + 0xB500
        source_image = bytearray(0x80)
        target_image = bytearray(0x40)
        target = target_base + 0x10
        struct.pack_into("<I", source_image, 0x10, target)
        struct.pack_into("<I", source_image, 0x44, target)
        # The pointer's low halfword resembles a valid Thumb PUSH prologue.
        struct.pack_into("<II", source_image, 0x48, 0xE92D4000, 0xE12FFF1E)
        struct.pack_into("<II", target_image, 0x10, 0xE92D4000, 0xE12FFF1E)
        source = ModuleState(
            name="source",
            module_id=-4,
            base=source_base,
            image=bytes(source_image),
            loaded_size=len(source_image),
            executable_ranges=[(0, len(source_image))],
        )
        target_module = ModuleState(
            name="target",
            module_id=-5,
            base=target_base,
            image=bytes(target_image),
            loaded_size=len(target_image),
            executable_ranges=[(0, len(target_image))],
        )
        analyzer = Arm7Analyzer([source, target_module])

        analyzer.discover([])
        analyzer.finalize()

        for address in (source_base + 0x10, source_base + 0x44):
            offset = address - source.base
            self.assertTrue(any(start <= offset < end for start, end, _ in source.data_ranges))
        self.assertNotIn((0, source_base + 0x10, True), source.functions)
        self.assertNotIn((0, source_base + 0x44, True), source.functions)
        self.assertIn((0, source_base + 0x48, False), source.functions)
        self.assertIn((1, target, False), target_module.functions)

    def test_thumb_prologue_scan_rejects_volatile_only_push(self):
        base = SYNTHETIC_BASE
        image = bytearray(0x60)
        # PUSH {r0} is a stack temporary, not a reliable function-root shape.
        struct.pack_into("<HHH", image, 0x10, 0xB401, 0x2001, 0x4770)
        struct.pack_into("<HH", image, 0x30, 0xB510, 0x4770)
        module = ModuleState(
            name="synthetic",
            module_id=-4,
            base=base,
            image=bytes(image),
            loaded_size=len(image),
            executable_ranges=[(0, len(image))],
        )
        analyzer = Arm7Analyzer([module])

        analyzer.discover([])
        analyzer.finalize()

        self.assertNotIn((0, base + 0x10, True), module.functions)
        self.assertIn((0, base + 0x30, True), module.functions)


    def test_register_load_invalidates_a_previously_known_pointer(self):
        base = SYNTHETIC_BASE
        image = bytearray(0x60)
        # LDR r1,=thumb_fn; LDR r1,[r1,#4]; BX r1.
        struct.pack_into("<III", image, 0, 0xE59F1018, 0xE5911004, 0xE12FFF11)
        struct.pack_into("<I", image, 0x20, base + 0x41)
        struct.pack_into("<HHH", image, 0x40, 0xB500, 0x2001, 0xBD00)
        module = ModuleState(
            name="synthetic",
            module_id=-4,
            base=base,
            image=bytes(image),
            loaded_size=len(image),
            executable_ranges=[(0, len(image))],
        )
        analyzer = Arm7Analyzer([module])

        analyzer.discover([(base, False, "synthetic entry")])

        self.assertNotIn((base + 0x40, True), module.function_labels[(0, base, False)])

    def test_backward_loop_converges_without_enumerating_counter_values(self):
        base = SYNTHETIC_BASE
        image = bytearray(0x20)
        # MOV r0,#0; ADD r0,r0,#1; CMP r0,#3; BNE back to ADD; BX lr.
        struct.pack_into("<IIIII", image, 0, 0xE3A00000, 0xE2800001, 0xE3500003, 0x1AFFFFFC, 0xE12FFF1E)
        module = ModuleState(
            name="synthetic",
            module_id=-4,
            base=base,
            image=bytes(image),
            loaded_size=len(image),
            executable_ranges=[(0, 20)],
        )
        analyzer = Arm7Analyzer([module])

        analyzer.discover([(base, False, "synthetic entry")])
        analyzer.finalize()

        self.assertEqual(len(module.function_code[(0, base, False)]), 20)
        self.assertFalse(analyzer.decode_anomalies)

    def make_analysis_image(self):
        image = bytearray(self.make_autoload_image())
        # Synthetic entry and callback are separate frameless leaf routines.
        struct.pack_into("<I", image, 0, 0xE12FFF1E)
        struct.pack_into("<I", image, 0x58, 0xE12FFF1E)
        # An adjacent routine must not become an implicit callback root.
        struct.pack_into("<II", image, 0x5C, 0xE3A00001, 0xE12FFF1E)
        # Code-shaped data and a metadata-only root exercise caller hints.
        struct.pack_into("<II", image, 0x68, 0xE92D4000, 0xE12FFF1E)
        struct.pack_into("<II", image, 0x70, 0xE3A01002, 0xE12FFF1E)
        return bytes(image)

    def test_analysis_has_no_implicit_adjacent_callback_root(self):
        result = analyze_image(
            self.make_analysis_image(),
            base=SYNTHETIC_BASE,
            entry=SYNTHETIC_BASE,
            callback=SYNTHETIC_BASE + 0x58,
        )

        main_module = result.modules[0]
        self.assertIn((0, SYNTHETIC_BASE, False), main_module.functions)
        self.assertIn((0, SYNTHETIC_BASE + 0x58, False), main_module.functions)
        self.assertNotIn((0, SYNTHETIC_BASE + 0x5C, False), main_module.functions)
        self.assertNotIn((0, SYNTHETIC_BASE + 0x70, False), main_module.functions)

    def test_analysis_accepts_caller_roots_data_ranges_and_names(self):
        extra = SYNTHETIC_BASE + 0x70
        result = analyze_image(
            self.make_analysis_image(),
            base=SYNTHETIC_BASE,
            entry=SYNTHETIC_BASE,
            callback=SYNTHETIC_BASE + 0x58,
            extra_seeds=[(extra, False, "synthetic metadata root")],
            data_ranges=[(SYNTHETIC_BASE + 0x68, SYNTHETIC_BASE + 0x70, "synthetic data")],
            routine_names={extra: "synthetic_routine"},
        )

        main_module = result.modules[0]
        self.assertEqual(main_module.functions[(0, extra, False)]["name"], "synthetic_routine")
        self.assertNotIn((0, SYNTHETIC_BASE + 0x68, False), main_module.functions)
        self.assertIn("synthetic metadata root", result.report)
        self.assertIn("Root provenance", result.report)

    def test_cli_requires_each_layout_value_without_metadata(self):
        values = {
            "--base": hex(SYNTHETIC_BASE),
            "--entry": hex(SYNTHETIC_BASE),
            "--autoload-callback": hex(SYNTHETIC_BASE + 0x58),
        }
        for missing in values:
            args = ["--arm7", "unused-arm7.bin", "--out", "unused-output"]
            for option, value in values.items():
                if option != missing:
                    args.extend((option, value))
            stderr = io.StringIO()
            with self.subTest(missing=missing), contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as raised:
                    main(args)
            self.assertEqual(raised.exception.code, 2)
            self.assertIn(f"{missing} is required", stderr.getvalue())

    def test_cli_reads_private_metadata_and_explicit_entry_override(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image_path = root / "synthetic-arm7.bin"
            image_path.write_bytes(self.make_analysis_image())
            metadata_path = root / "metadata.json"
            metadata_path.write_text(json.dumps({
                "base": hex(SYNTHETIC_BASE),
                "entry": hex(SYNTHETIC_BASE + 0x5C),
                "autoload_callback": hex(SYNTHETIC_BASE + 0x58),
                "extra_roots": [{
                    "address": hex(SYNTHETIC_BASE + 0x70),
                    "thumb": False,
                    "reason": "synthetic metadata root",
                }],
                "data_ranges": [{
                    "start": hex(SYNTHETIC_BASE + 0x68),
                    "end": hex(SYNTHETIC_BASE + 0x70),
                    "reason": "synthetic data",
                }],
                "routine_names": {hex(SYNTHETIC_BASE + 0x70): "synthetic_routine"},
            }))
            output = root / "output"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main([
                    "--arm7", str(image_path), "--out", str(output),
                    "--metadata", str(metadata_path), "--entry", hex(SYNTHETIC_BASE),
                ]), 0)

            manifest = json.loads((output / "arm7.manifest.json").read_text())
            functions = {function["addr"]: function for function in manifest["modules"][0]["functions"]}
            self.assertIn(SYNTHETIC_BASE, functions)
            self.assertNotIn(SYNTHETIC_BASE + 0x5C, functions)
            self.assertNotIn(SYNTHETIC_BASE + 0x68, functions)
            self.assertEqual(functions[SYNTHETIC_BASE + 0x70]["name"], "synthetic_routine")
            module = manifest["modules"][0]
            for data_start, data_end in module["data_ranges"]:
                self.assertFalse(any(
                    code_start < data_end and data_start < code_end
                    for code_start, code_end in module["code_ranges"]
                ))

if __name__ == "__main__":
    unittest.main()
