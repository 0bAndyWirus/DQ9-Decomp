import tempfile
import unittest
from pathlib import Path

# All module images, symbols, and addresses below are invented synthetic fixtures.

from tools.port_eu_unit import (
    GAP_ANCHOR_PREFIX,
    Group,
    ObjectFile,
    ObjSection,
    ObjReloc,
    ObjSymbol,
    Plan,
    RelocationAddition,
    SymbolEntry,
    TargetModule,
    TargetReloc,
    Wanted,
    _diff_symbols,
    _plan_relocation_rebases,
    add_symbol_less_gap_anchors,
    apply_plan,
    parse_relocs,
    parse_symbols,
    _external_references,
    plan_unit,
)


class PadTextEndTests(unittest.TestCase):
    BASE = 0x1000

    def _module(self, data: bytes, symbols=()):
        entries = [SymbolEntry(i, f"s{i}", kind, address, []) for i, (kind, address) in enumerate(symbols)]
        return TargetModule("main", data, Path("d"), Path("s"), Path("r"),
                            {".text": (self.BASE, self.BASE + len(data))}, entries, {})

    def test_halfword_thumb_end_is_extended_over_zero_padding_with_no_owner(self):
        from tools.port_eu_unit import pad_text_end
        module = self._module(bytes(32), [("function(thumb,size=0xe)", self.BASE + 8 + 1),
                                         ("function(arm,size=0x8)", self.BASE + 24)])
        group = Group(".text", [], 14, module="main", address=self.BASE + 8)
        self.assertTrue(pad_text_end(group, module))
        self.assertEqual(group.size, 16)

    def test_word_aligned_end_is_untouched(self):
        from tools.port_eu_unit import pad_text_end
        module = self._module(bytes(32))
        group = Group(".text", [], 16, module="main", address=self.BASE + 8)
        self.assertFalse(pad_text_end(group, module))
        self.assertEqual(group.size, 16)

    def test_padding_that_another_symbol_owns_or_that_is_not_zero_is_not_claimed(self):
        from tools.port_eu_unit import pad_text_end
        owned = self._module(bytes(32), [("data(any)", self.BASE + 8 + 14)])
        group = Group(".text", [], 14, module="main", address=self.BASE + 8)
        self.assertFalse(pad_text_end(group, owned))
        data = bytearray(32)
        data[8 + 14] = 0xFF
        group = Group(".text", [], 14, module="main", address=self.BASE + 8)
        self.assertFalse(pad_text_end(group, self._module(bytes(data))))
        self.assertEqual(group.size, 14)

    def test_padding_beyond_the_text_section_is_not_claimed(self):
        from tools.port_eu_unit import pad_text_end
        module = self._module(bytes(22))
        group = Group(".text", [], 14, module="main", address=self.BASE + 8)
        self.assertFalse(pad_text_end(group, module))


class GapAnchorTests(unittest.TestCase):
    def test_symbol_less_adjacent_gaps_get_transactional_label_anchors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            delinks = root / "delinks.txt"
            delinks.write_text(
                "    .text start:0x1000 end:0x1100 kind:code align:4\n\n"
                "src/Recovered/main/previous.cpp:\n"
                "    complete\n"
                "    .text start:0x1000 end:0x1020\n\n"
                "src/Recovered/main/following.cpp:\n"
                "    complete\n"
                "    .text start:0x1060 end:0x1070\n",
                encoding="utf-8",
            )
            symbols = root / "symbols.txt"
            symbols.write_text(
                "previous kind:function(arm,size=0x20) addr:0x1000\n"
                "candidate kind:function(arm,size=0x10) addr:0x1040\n"
                "following kind:function(arm,size=0x10) addr:0x1060\n",
                encoding="utf-8",
            )
            module = TargetModule(
                "main", bytes(0x100), delinks, symbols, root / "relocs.txt",
                {".text": (0x1000, 0x1100)}, parse_symbols(symbols), {},
            )
            plan = Plan(
                "src/Recovered/main/candidate.cpp",
                {".text": Group(".text", [], 0x10, "main", 0x1040)},
            )
            add_symbol_less_gap_anchors(plan, module)
            expected = {
                f"{GAP_ANCHOR_PREFIX}main_00001020",
                f"{GAP_ANCHOR_PREFIX}main_00001050",
            }
            self.assertEqual({line.split()[0] for _owner, line in plan.additions}, expected)

            apply_plan(plan, {"main": module})
            updated = symbols.read_text(encoding="utf-8")
            self.assertTrue(all(name in updated for name in expected))
            self.assertIn("src/Recovered/main/candidate.cpp:", delinks.read_text(encoding="utf-8"))

    def test_anchor_inside_a_later_unit_is_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            anchor = f"{GAP_ANCHOR_PREFIX}main_00001040"
            module = TargetModule(
                "main", bytes(0x100), root / "delinks.txt", root / "symbols.txt", root / "relocs.txt",
                {".text": (0x1000, 0x1100)},
                [SymbolEntry(0, anchor, "label(arm)", 0x1040, [])], {},
            )
            plan = Plan(
                "src/Recovered/main/next.cpp",
                {".text": Group(".text", [], 0x10, "main", 0x1040)},
            )
            _diff_symbols(plan, {"main": module})
            self.assertEqual([(owner, entry.name) for owner, entry in plan.removals], [("main", anchor)])


class PristineImageTests(unittest.TestCase):
    CONFIG = ("main_module:\n  name: main\n  object: ../../../build/test/build/arm9.bin\n  delinks: delinks.txt\n"
              "  symbols: symbols.txt\n  relocations: relocs.txt\nautoloads: []\noverlays:\n"
              "  - name: ov004\n    object: ../../../build/test/build/arm9_ov004.bin\n    delinks: ov4/delinks.txt\n"
              "    symbols: ov4/symbols.txt\n    relocations: ov4/relocs.txt\n")

    def _project(self, root: Path, *, with_extract: bool):
        arm9 = root / "config/test/arm9"
        (arm9 / "ov4").mkdir(parents=True)
        (arm9 / "config.yaml").write_text(self.CONFIG)
        for directory in (arm9, arm9 / "ov4"):
            (directory / "delinks.txt").write_text("    .text start:0x08000000 end:0x08000010\n")
            (directory / "symbols.txt").write_text("")
            (directory / "relocs.txt").write_text("")
        build = root / "build/test/build"
        build.mkdir(parents=True)
        (build / "arm9.bin").write_bytes(b"CONFIG-main")
        (build / "arm9_ov004.bin").write_bytes(b"CONFIG-overlay")
        if with_extract:
            (root / "extract/test/arm9").mkdir(parents=True)
            (root / "extract/test/arm9_overlays").mkdir(parents=True)
            (root / "extract/test/arm9/arm9.bin").write_bytes(b"EXTRACT-main")
            (root / "extract/test/arm9_overlays/ov004.bin").write_bytes(b"EXTRACT-overlay")

    def test_default_placement_prefers_extracted_images(self):
        from tools.port_eu_unit import load_project
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._project(root, with_extract=True)
            modules = load_project(root, "test")
            self.assertEqual((modules["main"].data, modules["ov004"].data),
                             (b"EXTRACT-main", b"EXTRACT-overlay"))

    def test_caller_selects_which_configured_images_are_preserved(self):
        from tools.port_eu_unit import load_project
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._project(root, with_extract=True)
            for preserved, expected in (
                    ({"ov004"}, (b"EXTRACT-main", b"CONFIG-overlay")),
                    ({"main"}, (b"CONFIG-main", b"EXTRACT-overlay")),
                    ({"main", "ov004"}, (b"CONFIG-main", b"CONFIG-overlay"))):
                with self.subTest(preserved=preserved):
                    modules = load_project(root, "test", keep_configured_image=preserved)
                    self.assertEqual((modules["main"].data, modules["ov004"].data), expected)

    def test_without_an_extract_tree_the_configured_image_is_used(self):
        from tools.port_eu_unit import load_project
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._project(root, with_extract=False)
            modules = load_project(root, "test")
            self.assertEqual((modules["main"].data, modules["ov004"].data),
                             (b"CONFIG-main", b"CONFIG-overlay"))

    def test_cli_forwards_repeated_configured_image_selections(self):
        from unittest.mock import patch
        from tools.port_eu_unit import PortError, main
        with patch("tools.port_eu_unit.load_project", side_effect=PortError("synthetic stop")) as load, \
                patch("sys.stderr"):
            result = main(["unit.o", "src/Example.cpp", "--version", "test",
                           "--keep-configured-image", "main", "--keep-configured-image", "ov004"])
        self.assertEqual(result, 2)
        load.assert_called_once_with(Path("."), "test", ["main", "ov004"])



class PortEuUnitExplicitOverlayResolutionTests(unittest.TestCase):
    @staticmethod
    def _synthetic_name(module, suffix=0x08000020):
        return f"func_{module}_{suffix:08x}"

    def _plan(self, symbol_name, owner_name, owner_module="ov009", owner_address=0x08000028,
              relocation_module="overlays(9,11)", relocation_kind="arm_call"):
        section = ObjSection(1, ".text", 1, 0x2, 4, 4, bytes(4))
        target = ObjSymbol(2, symbol_name, 0, 0, 0, 0, 0)
        source = TargetModule("ov003", bytes(0x100), Path("d"), Path("s"), Path("r"),
                              {".text": (0x08000000, 0x08000100)}, [], {})
        owner = TargetModule(
            owner_module, bytes(0x100), Path("d"), Path("s"), Path("r"),
            {".text": (0x08000000, 0x08000100), ".data": (0x08000000, 0x08000100)},
            [SymbolEntry(0, owner_name, "function(arm,size=0x10)", owner_address, [])], {})
        owner.index_symbols()
        source.relocs = {0x08000000: TargetReloc(0x08000000, relocation_kind, 0x08000028,
                                               relocation_module, 0, 1)}
        obj_addend = -8 if relocation_kind == "arm_call" else 0
        obj = ObjectFile(Path("unit.o"), [section], [target],
                         {1: [ObjReloc(0, 2 if relocation_kind == "load" else 28, target, obj_addend)]})
        groups = {".text": Group(".text", [(section, 0)], 4, source.name, 0x08000000)}
        return plan_unit(obj, {source.name: source, owner.name: owner}, "src/Example.cpp", groups)

    def test_explicit_overlay_name_resolves_owner_not_ambiguous_external_alias(self):
        name = self._synthetic_name("ov009")
        plan = self._plan(name, name)
        self.assertEqual(plan.conflicts, [])
        self.assertEqual([(item.module, item.address) for item in plan.wanted],
                         [("ov009", 0x08000028)])

    def test_explicit_owner_resolves_ambiguous_function_pointer_load(self):
        name = self._synthetic_name("ov009")
        plan = self._plan(name, name, relocation_kind="load")
        self.assertEqual(plan.conflicts, [])
        self.assertEqual([(item.module, item.address, item.category) for item in plan.wanted],
                         [("ov009", 0x08000028, "function")])

    def test_unqualified_ambiguous_symbol_stays_ambiguous(self):
        plan = self._plan("SharedFunc", "SharedFunc")
        self.assertTrue(any("ambiguous target module" in conflict for conflict in plan.conflicts))

    def test_invalid_or_missing_explicit_owner_stays_ambiguous(self):
        for owner_module, owner_name, owner_address in (
                ("ov011", self._synthetic_name("ov009", 0x08000028), 0x08000028),
                ("ov009", self._synthetic_name("ov009", 0x08000028), 0x0800002C),
                ("ov009", self._synthetic_name("ov011", 0x08000028), 0x08000028)):
            with self.subTest(owner_module=owner_module, owner_name=owner_name,
                              owner_address=owner_address):
                plan = self._plan(self._synthetic_name("ov009", 0x08000028),
                                  owner_name, owner_module, owner_address)
                self.assertTrue(any("ambiguous target module" in conflict
                                    for conflict in plan.conflicts))

class PortEuUnitThumbPointerTests(unittest.TestCase):
    def _plan(self, kind, entry_kind, entry_address=0x1000, include_export=True):
        source = TargetModule(
            "ov003", bytes(0x4000), Path("d"), Path("s"), Path("r"),
            {".text": (0x1000, 0x1100)}, [], {})
        source.symbols = [SymbolEntry(0, "synthetic_thumb_entry", entry_kind,
                                      entry_address, [])]
        source.index_symbols()
        external = TargetModule(
            "ov009", bytes(0x4000), Path("d"), Path("s"), Path("r"),
            {".text": (0x3000, 0x3100)}, [], {})
        external.relocs = {0x3000: TargetReloc(0x3000, kind, 0x1001, "overlay(3)", 0, 1)}
        group = Group(".text", [], 8, "ov003", 0x1000)
        plan = Plan("src/Example.cpp", {".text": group})
        if include_export:
            plan.wanted = [Wanted("synthetic_thumb_entry", "ov003", 0x1000,
                                  "function", 8, False, True)]
        return plan, {"ov003": source, "ov009": external}

    def test_external_thumb_function_pointer_keeps_odd_target_as_valid_reference(self):
        plan, modules = self._plan("load", "function(thumb,size=0x8)")
        _external_references(plan, modules)
        self.assertEqual(plan.conflicts, [])

    def test_odd_data_arm_function_and_unexported_thumb_pointer_remain_errors(self):
        for kind, declaration, address, exported in (
                ("load", "data(any)", 0x1000, True),
                ("load", "function(arm,size=0x8)", 0x1000, True),
                ("load", "function(thumb,size=0x8)", 0x1002, True),
                ("load", "function(thumb,size=0x8)", 0x1000, False)):
            with self.subTest(kind=kind, declaration=declaration,
                              address=address, exported=exported):
                plan, modules = self._plan(kind, declaration, address, exported)
                _external_references(plan, modules)
                self.assertTrue(plan.conflicts)



class PortEuUnitRelocationTests(unittest.TestCase):
    def _module(self, directory, name, relocation_lines):
        directory.mkdir(parents=True, exist_ok=True)
        symbols_path = directory / "symbols.txt"
        delinks_path = directory / "delinks.txt"
        relocs_path = directory / "relocs.txt"
        symbols_path.write_text(
            "@stringBase0 kind:data(any) addr:0x00001000 local\n"
            "synthetic_tail kind:data(any) addr:0x00001007\n",
            encoding="utf-8",
        )
        delinks_path.write_text("    .data start:0x00001000 end:0x00002000 kind:data align:4\n", encoding="utf-8")
        relocs_path.write_text("\n".join(relocation_lines) + "\n", encoding="utf-8")
        return TargetModule(
            name=name,
            data=bytes(0x3000),
            delinks_path=delinks_path,
            symbols_path=symbols_path,
            relocs_path=relocs_path,
            sections={".data": (0x1000, 0x2000), ".text": (0x2000, 0x4000)},
            symbols=[],
            relocs=parse_relocs(relocs_path),
        )

    def test_plan_unit_records_symbol_additions(self):
        with tempfile.TemporaryDirectory() as directory:
            main = self._module(Path(directory) / "main", "main", [])
            section = ObjSection(1, ".text", 1, 0x2, 4, 4, bytes(4))
            obj = ObjectFile(
                Path("unit.o"),
                [section],
                [ObjSymbol(1, "new_function", 0, 4, 2, 1, 1)],
                {},
            )
            groups = {
                ".text": Group(".text", [(section, 0)], 4, "main", 0x2000),
            }

            plan = plan_unit(obj, {"main": main}, "src/System/Example.cpp", groups)

            self.assertEqual(plan.conflicts, [])
            self.assertEqual(
                plan.additions,
                [("main", "new_function kind:function(arm,size=0x4) addr:0x00002000")],
            )

    def test_plan_unit_normalizes_thumb_function_pointer_relocations(self):
        with tempfile.TemporaryDirectory() as directory:
            main = self._module(Path(directory) / "main", "main", [
                "from:0x00002000 kind:load to:0x00002201 module:main",
                "from:0x00002004 kind:arm_call to:0x00002208 module:main",
            ])
            function = SymbolEntry(0, "ThumbTarget", "function(thumb,size=0x8)", 0x2200, [])
            main.symbols = [function]
            main.index_symbols()
            section = ObjSection(1, ".text", 1, 0x2, 8, 4, bytes(8))
            target = ObjSymbol(2, "ThumbTarget", 0, 0, 0, 1, 0)
            obj = ObjectFile(
                Path("unit.o"),
                [section],
                [],
                {
                    1: [
                        ObjReloc(0, 2, target, 0),
                        ObjReloc(4, 28, target, 0),
                    ],
                },
            )
            groups = {
                ".text": Group(".text", [(section, 0)], 8, "main", 0x2000),
            }

            plan = plan_unit(obj, {"main": main}, "src/System/Example.cpp", groups)

            self.assertEqual(plan.conflicts, [])
            self.assertEqual(
                [(item.name, item.address, item.category) for item in plan.wanted],
                [("ThumbTarget", 0x2200, "function")],
            )

    def test_plan_unit_adds_missing_absolute_relocation(self):
        with tempfile.TemporaryDirectory() as directory:
            main = self._module(Path(directory) / "main", "main", [
                "from:0x00001008 kind:load to:0x00002000 module:main",
            ])
            main.sections = {".text": (0x1000, 0x1004), ".data": (0x2000, 0x3000)}
            main.symbols = [SymbolEntry(0, "g_table", "data(any)", 0x2000, [])]
            main.index_symbols()
            section = ObjSection(1, ".text", 1, 0x2, 4, 4, bytes(4))
            target = ObjSymbol(2, "g_table", 0, 0, 1, 1, 0)
            obj = ObjectFile(
                Path("unit.o"),
                [section],
                [target],
                {1: [ObjReloc(0, 2, target, 0)]},
            )
            groups = {
                ".text": Group(".text", [(section, 0)], 4, "main", 0x1000),
            }

            plan = plan_unit(obj, {"main": main}, "src/Graphics/Example.cpp", groups)

            self.assertEqual(plan.conflicts, [])
            self.assertEqual(
                plan.relocation_additions,
                [RelocationAddition("main", 0x1000, "load", 0x2000, "main")],
            )
            updated = apply_plan(plan, {"main": main})

            self.assertEqual(
                main.relocs_path.read_text(encoding="utf-8").splitlines(),
                [
                    "from:0x00001000 kind:load to:0x00002000 module:main",
                    "from:0x00001008 kind:load to:0x00002000 module:main",
                ],
            )
            self.assertIn(main.relocs_path, updated)

    def test_plan_unit_promotes_referenced_local_data(self):
        with tempfile.TemporaryDirectory() as directory:
            main = self._module(Path(directory) / "main", "main", [
                "from:0x00003000 kind:load to:0x00002000 module:main",
            ])
            main.sections = {".text": (0x1000, 0x1100), ".data": (0x2000, 0x3000)}
            symbol = SymbolEntry(0, "payload", "data(any)", 0x2000, ["local"])
            main.symbols = [symbol]
            main.index_symbols()
            text = ObjSection(1, ".text", 1, 0x2, 4, 4, bytes(4))
            data = ObjSection(2, ".data", 1, 0x2, 4, 4, bytes(4))
            obj = ObjectFile(
                Path("unit.o"),
                [text, data],
                [ObjSymbol(1, "payload", 0, 4, 1, 0, 2)],
                {},
            )
            groups = {
                ".text": Group(".text", [(text, 0)], 4, "main", 0x1000),
                ".data": Group(".data", [(data, 0)], 4, "main", 0x2000),
            }

            plan = plan_unit(obj, {"main": main}, "src/Graphics/Example.cpp", groups)

            self.assertEqual(plan.conflicts, [])
            self.assertEqual(
                [(owner, entry.name, local) for owner, entry, local in plan.relocal],
                [("main", "payload", False)],
            )

    def test_plan_unit_adds_data_section_boundary_symbol(self):
        with tempfile.TemporaryDirectory() as directory:
            main = self._module(Path(directory) / "main", "main", [])
            main.symbols = [
                SymbolEntry(0, "@stringBase0", "data(any)", 0x1008, ["local"]),
                SymbolEntry(1, "synthetic_neighbor", "data(any)", 0x1012, []),
            ]
            main.index_symbols()
            text = ObjSection(1, ".text", 1, 0x2, 4, 4, bytes(4))
            data = ObjSection(2, ".data", 1, 0x2, 0x10, 4, bytes(0x10))
            obj = ObjectFile(
                Path("unit.o"),
                [text, data],
                [ObjSymbol(1, "@stringBase0", 8, 0, 1, 0, 2)],
                {},
            )
            groups = {
                ".text": Group(".text", [(text, 0)], 4, "main", 0x2000),
                ".data": Group(".data", [(data, 0)], 0x10, "main", 0x1000),
            }

            plan = plan_unit(obj, {"main": main}, "src/System/Example.cpp", groups)

            self.assertEqual(plan.conflicts, [])
            # Boundary markers use the tool's generic address-derived naming format.
            marker_name = f"data_{0x1010:08x}"
            self.assertEqual(
                plan.additions,
                [("main", f"{marker_name} kind:data(any) addr:0x00001010 local")],
            )

    def test_parse_relocs_keeps_addend_line_and_module(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "relocs.txt"
            path.write_text(
                "from:0x08000010 kind:load to:0x09000020 add:0x7 module:main\n",
                encoding="utf-8",
            )

            reloc = parse_relocs(path)[0x08000010]

            self.assertEqual(reloc.target, 0x09000020)
            self.assertEqual(reloc.addend, 0x7)
            self.assertEqual(reloc.effective_target, 0x09000027)
            self.assertEqual(reloc.module, "main")
            self.assertEqual(reloc.line, 0)

    def test_rebases_removed_symbol_references_and_preserves_effective_addends(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main = self._module(root / "main", "main", [
                "from:0x00002000 kind:load to:0x00001007 module:main",
                "from:0x00002004 kind:load to:0x00001007 add:0x3 module:main",
            ])
            overlay = self._module(root / "ov001", "ov001", [
                "from:0x00003000 kind:load to:0x00001007 add:0x4 module:main",
            ])
            modules = {"main": main, "ov001": overlay}
            plan = Plan("src/World/Example.cpp", {
                ".text": Group(".text", [], 0x20, module="main", address=0x2000),
                ".data": Group(".data", [], 0x20, module="main", address=0x1000),
            })
            plan.wanted = [Wanted("@stringBase0", "main", 0x1000, "data", 0x20, True, True)]
            plan.removals = [(
                "main",
                SymbolEntry(1, "synthetic_tail", "data(any)", 0x1007, []),
            )]

            _plan_relocation_rebases(plan, modules)

            self.assertEqual(plan.conflicts, [])
            self.assertEqual(
                [(rebase.source_module, rebase.target, rebase.addend, rebase.relocation.module)
                 for rebase in plan.relocation_rebases],
                [
                    ("main", 0x1000, 0x7, "main"),
                    ("main", 0x1000, 0xa, "main"),
                    ("ov001", 0x1000, 0xb, "main"),
                ],
            )

            updated = apply_plan(plan, modules)

            self.assertEqual(
                main.relocs_path.read_text(encoding="utf-8").splitlines(),
                [
                    "from:0x00002000 kind:load to:0x00001000 add:0x7 module:main",
                    "from:0x00002004 kind:load to:0x00001000 add:0xa module:main",
                ],
            )
            self.assertEqual(
                overlay.relocs_path.read_text(encoding="utf-8").splitlines(),
                ["from:0x00003000 kind:load to:0x00001000 add:0xb module:main"],
            )
            self.assertIn(main.relocs_path, updated)
            self.assertIn(overlay.relocs_path, updated)
            self.assertNotIn("synthetic_tail", main.symbols_path.read_text(encoding="utf-8"))
            self.assertIn("@stringBase0", main.symbols_path.read_text(encoding="utf-8"))


class StrippedDefinitionTests(unittest.TestCase):
    """A strong function that placement dropped but the module defines elsewhere stays in the gap object."""

    BASE = 0x1000

    def _check(self, *, bind, module_address, module_name="func_b"):
        from tools.port_eu_unit import STB_GLOBAL, STT_FUNC, _reject_stripped_definitions
        kept = ObjSection(1, ".text", 1, 0x6, 0x10, 4, bytes(0x10))
        dropped = ObjSection(2, ".text", 1, 0x6, 0x10, 4, bytes(0x10))
        symbols = [ObjSymbol(1, "func_a", 0, 0x10, STT_FUNC, STB_GLOBAL, 1),
                   ObjSymbol(2, "func_b", 0, 0x10, STT_FUNC, bind, 2)]
        obj = ObjectFile(Path("o"), [kept, dropped], symbols, {})
        module = TargetModule("ov", bytes(0x40), Path("d"), Path("s"), Path("r"),
                              {".text": (self.BASE, self.BASE + 0x40)},
                              [SymbolEntry(0, "func_a", "function(arm,size=0x10)", self.BASE, []),
                               SymbolEntry(1, module_name, "function(arm,size=0x10)", module_address, [])], {})
        group = Group(".text", [(kept, 0)], 0x10, module="ov", address=self.BASE, stripped=[dropped])
        plan = Plan("unit.cpp", {".text": group}, obj=obj)
        _reject_stripped_definitions(plan, module)
        return plan.conflicts

    def test_strong_function_defined_elsewhere_in_the_module_is_a_conflict(self):
        from tools.port_eu_unit import STB_GLOBAL
        conflicts = self._check(bind=STB_GLOBAL, module_address=self.BASE + 0x10)
        self.assertEqual(len(conflicts), 1)
        self.assertIn("func_b", conflicts[0])
        self.assertIn("ascending address order", conflicts[0])

    def test_alias_of_a_kept_address_is_not_a_conflict(self):
        from tools.port_eu_unit import STB_GLOBAL
        self.assertEqual(self._check(bind=STB_GLOBAL, module_address=self.BASE + 0x8), [])

    def test_weak_duplicate_and_unknown_names_are_not_conflicts(self):
        from tools.port_eu_unit import STB_GLOBAL
        self.assertEqual(self._check(bind=2, module_address=self.BASE + 0x10), [])
        self.assertEqual(self._check(bind=STB_GLOBAL, module_address=self.BASE + 0x10, module_name="other"), [])


class FunctionOrderPlacementTests(unittest.TestCase):
    """Object section order must follow the synthetic module's address order."""

    BASE = 0x08000000

    def _plan(self, object_order):
        from tools.port_eu_unit import STB_GLOBAL, STT_FUNC, place
        body_a = bytes(range(1, 0x11))
        body_b = bytes(range(0x41, 0x61))
        data = body_a + body_b
        entries = [SymbolEntry(0, "func_a", "function(arm,size=0x10)", self.BASE, []),
                   SymbolEntry(1, "func_b", "function(arm,size=0x20)", self.BASE + 0x10, [])]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            module = TargetModule("ov001", data, root / "delinks.txt", root / "symbols.txt", root / "relocs.txt",
                                  {".text": (self.BASE, self.BASE + len(data))}, entries, {})
            bodies = {"func_a": body_a, "func_b": body_b}
            sections = [ObjSection(0, "", 0, 0, 0, 0, b"")]
            symbols = []
            for name in object_order:
                index = len(sections)
                sections.append(ObjSection(index, ".text", 1, 0x6, len(bodies[name]), 4, bodies[name]))
                symbols.append(ObjSymbol(len(symbols) + 1, name, 0, len(bodies[name]), STT_FUNC, STB_GLOBAL, index))
            obj = ObjectFile(Path("unit.o"), sections, symbols, {})
            groups = place(obj, {"ov001": module}, "ov001", self.BASE)
            return groups[".text"], plan_unit(obj, {"ov001": module}, "unit.cpp", groups)

    def test_functions_in_address_order_cover_the_whole_range(self):
        text, plan = self._plan(["func_a", "func_b"])
        self.assertEqual((text.address, text.size, text.stripped), (self.BASE, 0x30, []))
        self.assertEqual(plan.conflicts, [])

    def test_functions_in_reverse_order_are_rejected_with_the_order_hint(self):
        text, plan = self._plan(["func_b", "func_a"])
        self.assertEqual(text.size, 0x10)
        self.assertEqual(len(text.stripped), 1)
        self.assertEqual(len(plan.conflicts), 1)
        self.assertIn("func_b", plan.conflicts[0])
        self.assertIn("ascending address order", plan.conflicts[0])


if __name__ == "__main__":
    unittest.main()
