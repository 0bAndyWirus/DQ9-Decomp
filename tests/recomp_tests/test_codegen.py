"""Code-generation checks using hand-assembled synthetic ARM/Thumb fixtures."""

import json
from pathlib import Path
import tempfile
import unittest

from recomp.emitter import emit_file, emit_manifest

SYNTHETIC_BASE = 0x08000000  # Invented address space; no console memory map required.


class RecompilerCodegenTests(unittest.TestCase):
    def make_manifest(self, root: Path, code: bytes, functions: list[dict], calls=None) -> dict:
        image = root / "guest.bin"
        image.write_bytes(code)
        return {
            "cpu": "arm9",
            "arch": "v5te",
            "modules": [
                {
                    "name": "main",
                    "module_id": -1,
                    "base": SYNTHETIC_BASE,
                    "image": str(image.resolve()),
                    "functions": functions,
                    "labels": [],
                    "data_ranges": [],
                    "calls": calls or [],
                }
            ],
        }

    def test_emits_slot_table_and_readable_instructions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # LDR r0,[pc,#0]; BX lr; literal word (outside the function body).
            code = bytes.fromhex("00009fe5 1eff2fe1") + (0x12345678).to_bytes(4, "little")
            manifest = self.make_manifest(
                root,
                code,
                [{"addr": SYNTHETIC_BASE, "thumb": False, "size": 8, "name": "literal_fn"}],
            )
            output = root / "generated"
            stats = emit_manifest(manifest, output)
            body = (output / "chunk_000.cpp").read_text(encoding="utf-8")
            table = (output / "fn_table.cpp").read_text(encoding="utf-8")

            self.assertIn("0x08000000: ldr", body)
            self.assertIn("p_arm9_main_08000000", table)
            self.assertEqual(stats["functions"], 1)
            self.assertEqual(stats["instructions"], 2)
            self.assertEqual(stats["unsupported_sites"], [])

    def test_memory_stores_include_guest_instruction_pc(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # STR r0, [r1]; BX lr.
            code = bytes.fromhex("000081e5 1eff2fe1")
            manifest = self.make_manifest(
                root,
                code,
                [{"addr": SYNTHETIC_BASE, "thumb": False, "size": 8, "name": "store"}],
            )
            output = root / "generated"
            emit_manifest(manifest, output)
            body = (output / "chunk_000.cpp").read_text(encoding="utf-8")

            self.assertIn(
                "::dsr::wr32(c, (c->r[1] + (0u)), c->r[0], 0x08000000u);",
                body,
            )

    def test_guarded_branch_table_after_rebasing_subs_gets_every_case(self):
        # Synthetic regression: an unrelated cmp of r2 precedes the guard on
        # r5. Rebasing r5 by 3 makes indices 0..3 valid, not just index zero.
        words = [
            0xE3520000,  # 00 cmp r2, #0
            0xE3550006,  # 04 cmp r5, #6
            0xCA000007,  # 08 bgt synthetic offset 0x2c
            0xE2552003,  # 0c subs r2, r5, #3
            0x508FF102,  # 10 addpl pc, pc, r2, lsl #2
            0xE12FFF1E,  # 14 bx lr
            0xEA000004,  # 18 b synthetic offset 0x30
            0xEA000003,  # 1c b synthetic offset 0x30
            0xEA000002,  # 20 b synthetic offset 0x30
            0xEA000001,  # 24 b synthetic offset 0x30
            0xE12FFF1E,  # 28 bx lr
            0xE12FFF1E,  # 2c bx lr
            0xE3A0000B,  # 30 mov r0, #11
            0xE12FFF1E,  # 34 bx lr
        ]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = self.make_manifest(
                root, b"".join(word.to_bytes(4, "little") for word in words),
                [{"addr": SYNTHETIC_BASE, "thumb": False, "size": len(words) * 4, "name": "synthetic_switch"}])
            output = root / "generated"
            emit_manifest(manifest, output)
            body = (output / "chunk_000.cpp").read_text(encoding="utf-8")
            self.assertIn("0x08000010: addpl pc, pc, r2, lsl #2", body)
            for index in range(4):
                self.assertIn(f"case 0x{index:08x}u: {{", body)
            self.assertNotIn("case 0x00000004u: {", body)


    def test_secondary_tail_accepts_only_agreeing_overlapping_owners(self):
        for cpu, thumb in (("arm7", False), ("arm9", False), ("arm9", True)):
            if thumb:
                code = bytes.fromhex("0121 0221 0d43 7047 0028 fbd1 7047")
                unit = 2
            else:
                code = b"".join(word.to_bytes(4, "little") for word in (
                    0xE3A01001, 0xE3A01002, 0xE1915000, 0xE12FFF1E,
                    0xE3500000, 0x1AFFFFFB, 0xE12FFF1E,
                ))
                unit = 4
            for disagreement in (None, "bounds", "decoded"):
                with self.subTest(cpu=cpu, thumb=thumb, disagreement=disagreement):
                    with tempfile.TemporaryDirectory() as temporary:
                        root = Path(temporary)
                        image = code
                        if disagreement == "decoded":
                            ret = bytes.fromhex("7047") if thumb else bytes.fromhex("1eff2fe1")
                            image = ret + image[unit:]
                        base = SYNTHETIC_BASE
                        manifest = self.make_manifest(root, image, [
                            {"addr": base, "thumb": thumb, "size": 4 * unit, "name": "owner"},
                            {"addr": base + unit, "thumb": thumb,
                             "size": (4 if disagreement == "bounds" else 3) * unit, "name": "alias"},
                            {"addr": base + 4 * unit, "thumb": thumb,
                             "size": 3 * unit, "name": "source"},
                        ])
                        manifest["cpu"] = cpu
                        manifest["arch"] = "v4t" if cpu == "arm7" else "v5te"
                        module_id = -5 if cpu == "arm7" else -1
                        manifest["modules"][0]["module_id"] = module_id
                        output = root / "generated"
                        stats = emit_manifest(manifest, output)
                        self.assertEqual(stats["unsupported_sites"], [])
                        table = (output / "fn_table.cpp").read_text()
                        body = (output / "chunk_000.cpp").read_text()
                        tag = "arm7wram" if cpu == "arm7" else "main"
                        slot = f"p_{cpu}_{tag}_{base + 2 * unit:08x}"
                        if disagreement is None:
                            self.assertIn(slot, table)
                            self.assertIn(f"(*::dsr::{slot})(c);", body)
                        else:
                            self.assertNotIn(slot, table)

    def test_direct_calls_use_mutable_slot_and_postcall_check(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # BL callee; BX lr; MOV r0,#7; BX lr.
            code = bytes.fromhex("000000eb 1eff2fe1 0700a0e3 1eff2fe1")
            manifest = self.make_manifest(
                root,
                code,
                [
                    {"addr": SYNTHETIC_BASE, "thumb": False, "size": 8, "name": "caller"},
                    {"addr": SYNTHETIC_BASE + 8, "thumb": False, "size": 8, "name": "callee"},
                ],
                [{"from": SYNTHETIC_BASE, "to": SYNTHETIC_BASE + 8, "modules": [-1]}],
            )
            output = root / "generated"
            stats = emit_manifest(manifest, output)
            body = (output / "chunk_000.cpp").read_text(encoding="utf-8")

            self.assertIn("(*::dsr::p_arm9_main_08000008)(c)", body)
            self.assertIn("DSR_POSTCALL(c, 0x08000004u)", body)
            self.assertEqual(stats["direct_calls"], 1)
            self.assertEqual(stats["dsr_call_sites"], 0)

    def test_camera_builder_calls_use_per_machine_dispatch(self):
        for opcode in ("000000eb", "000000ea"):  # BL and tail B.
            with self.subTest(opcode=opcode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                base = 0x020c28a8
                code = bytes.fromhex(opcode + " 1eff2fe1 0700a0e3 1eff2fe1")
                manifest = self.make_manifest(root, code, [
                    {"addr": base, "thumb": False, "size": 8, "name": "caller"},
                    {"addr": base + 8, "thumb": False, "size": 8, "name": "projection"},
                ], [{"from": base, "to": base + 8, "modules": [-1]}])
                manifest["modules"][0]["base"] = base
                output = root / "generated"
                emit_manifest(manifest, output)
                body = (output / "chunk_000.cpp").read_text(encoding="utf-8")
                self.assertIn("::dsr::dsr_call(c, 0x020c28b0u);", body)
                self.assertNotIn("(*::dsr::p_arm9_main_020c28b0)(c);", body)

    def test_extra_entry_is_emitted_and_registered(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code = bytes.fromhex("0000a0e1 1eff2fe1 0100a0e3 1eff2fe1")
            manifest = self.make_manifest(
                root,
                code,
                [{"addr": SYNTHETIC_BASE, "thumb": False, "size": 16, "name": "owner"}],
            )
            extra = root / "extra.txt"
            extra.write_text("-1 08000008 0\n", encoding="utf-8")
            output = root / "generated"
            stats = emit_manifest(manifest, output, extra)
            table = (output / "fn_table.cpp").read_text(encoding="utf-8")

            self.assertIn("p_arm9_main_08000008", table)
            self.assertIn("extra_08000008", table)
            self.assertEqual(stats["extra_entries"], 1)
            self.assertEqual(stats["functions"], 2)

    def test_manifest_cli_emits_stats_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code = bytes.fromhex("1eff2fe1")
            manifest = self.make_manifest(
                root,
                code,
                [{"addr": SYNTHETIC_BASE, "thumb": False, "size": 4, "name": "ret"}],
            )
            source = root / "manifest.json"
            source.write_text(json.dumps(manifest), encoding="utf-8")
            output = root / "generated"
            stats = emit_file(source, output)
            self.assertEqual(stats["functions"], 1)
            self.assertTrue((output / "decls.h").is_file())
            self.assertTrue((output / "fn_table.cpp").is_file())
            self.assertEqual(json.loads((output / "stats.json").read_text())["cpu"], "arm9")


class StressRegressionCodegenTests(unittest.TestCase):
    make_manifest = RecompilerCodegenTests.make_manifest

    def emit_one(self, code: bytes, thumb: bool = False, base: int = SYNTHETIC_BASE):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = self.make_manifest(root, code, [
                {"addr": base, "thumb": thumb, "size": len(code), "name": "stress"},
            ])
            manifest["modules"][0]["base"] = base
            output = root / "generated"
            stats = emit_manifest(manifest, output)
            self.assertEqual(stats["unsupported_sites"], [])
            return (output / "chunk_000.cpp").read_text(), (output / "fn_table.cpp").read_text()

    def test_carry_input_without_s_preserves_flags(self):
        for opcode in (5, 6, 7):  # ADC/SBC/RSC without S.
            with self.subTest(opcode=opcode):
                word = 0xE0000003 | (opcode << 21) | (2 << 16)
                body, _ = self.emit_one(word.to_bytes(4, "little") + bytes.fromhex("1eff2fe1"))
                self.assertIn("with_carry", body)
                for flag in ("n", "z", "cf", "v"):
                    self.assertNotIn(f"c->{flag} =", body)

    def test_register_shifted_pc_uses_twelve_for_both_operands_and_alias(self):
        for word in (0xE08F0413, 0xE1A0041F):  # ADD Rn=PC; LSL Rm=PC.
            with self.subTest(word=hex(word)):
                body, _ = self.emit_one(word.to_bytes(4, "little") + bytes.fromhex("1eff2fe1"))
                self.assertIn("0x0800000cu", body)
                self.assertNotIn("0x08000008u", body)

    def test_thumb_adr_aligns_pc_and_adds_offset(self):
        body, _ = self.emit_one(bytes.fromhex("01a0 7047"), thumb=True, base=SYNTHETIC_BASE + 2)
        self.assertIn("c->r[0] = 0x08000008u;", body)

    def test_thumb_local_bl_gets_callable_secondary_entry(self):
        # Save LR; BL internal leaf beyond the caller's return; restore; return.
        code = bytes.fromhex("7546 00f0 03f8 ae46 7047 c046 0130 7047")
        body, table = self.emit_one(code, thumb=True)
        self.assertIn("p_arm9_main_0800000c", table)
        self.assertIn("(*::dsr::p_arm9_main_0800000c)(c);", body)


if __name__ == "__main__":
    unittest.main()


class ControlFlowIdiomTests(unittest.TestCase):
    def emit(self, words: list[int], size: int) -> str:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image = root / "guest.bin"
            image.write_bytes(b"".join(w.to_bytes(4, "little") for w in words))
            manifest = {
                "cpu": "arm9", "arch": "v5te",
                "modules": [{
                    "name": "main", "module_id": -1, "base": SYNTHETIC_BASE, "image": str(image.resolve()),
                    "functions": [{"addr": SYNTHETIC_BASE, "thumb": False, "size": size, "name": "f"}],
                    "labels": [], "data_ranges": [], "calls": [],
                }],
            }
            stats = emit_manifest(manifest, root / "out")
            self.assertEqual(stats["unsupported_sites"], [])
            return (root / "out" / "chunk_000.cpp").read_text(encoding="utf-8")


    def emit_region(self, words: list[int], size: int, entries: list[int], data_ranges=()):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = SYNTHETIC_BASE
            image = root / "guest.bin"
            image.write_bytes(b"".join(word.to_bytes(4, "little") for word in words))
            manifest = {
                "cpu": "arm9",
                "arch": "v5te",
                "modules": [{
                    "name": "main", "module_id": -1, "base": base, "image": str(image.resolve()),
                    "functions": [{"addr": base, "thumb": False, "size": size, "name": "inferred"}],
                    "labels": [], "data_ranges": [], "calls": [],
                    "dispatch_regions": [{
                        "addr": base, "size": size, "thumb": False,
                        "entries": [base + offset for offset in entries],
                        "data_ranges": [[base + low, base + high] for low, high in data_ranges],
                    }],
                }],
            }
            output = root / "out"
            stats = emit_manifest(manifest, output)
            self.assertEqual(stats["unsupported_sites"], [])
            return (output / "chunk_000.cpp").read_text(encoding="utf-8"), stats

    def test_shared_region_emits_an_entry_slot_per_entry(self):
        _, stats = self.emit_region(
            [0xE2800001, 0xE2811001, 0xE2822001, 0xE12FFF1E],
            16,
            [0, 8],
        )
        self.assertEqual(stats["functions"], 2)

    def test_stack_pop_into_pc_is_a_static_return(self):
        # mov r0, #1; pop {r4, pc}
        body = self.emit([0xE3A00001, 0xE8BD8010], 8)
        tail = body[body.index("0x08000004: pop"):]
        self.assertIn("return;", tail)
        self.assertNotIn("dsr_call", tail)

    def test_backward_branch_inside_a_walk_gets_a_budget_check(self):
        # mov r1, #0; ldr r0, [sp]; cmp r0, #0; bne <ldr>; bx lr
        body = self.emit([0xE3A01000, 0xE59D0000, 0xE3500000, 0x1AFFFFFC, 0xE12FFF1E], 20)
        header = body[body.index("L_08000004: ;"):]
        self.assertIn("dsr_checkpoint(c, 0x08000004u)", header[:200])
