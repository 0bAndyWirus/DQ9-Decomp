import tempfile
import unittest
from pathlib import Path
import json
import os
from unittest import mock


from tools.decomp_queue import build_queue, load_modules, main
from tools.port_eu_unit import SymbolEntry, TargetModule, TargetReloc


class DecompQueueTests(unittest.TestCase):
    @staticmethod
    def _module(name, symbols, relocs=(), sections=None):
        sections = sections or {".text": (0x1000, 0x1200)}
        return TargetModule(
            name=name,
            data=b"",
            delinks_path=Path(f"{name}-delinks.txt"),
            symbols_path=Path(f"{name}-symbols.txt"),
            relocs_path=Path(f"{name}-relocs.txt"),
            sections=sections,
            symbols=[
                SymbolEntry(index, symbol_name, f"function(arm,size=0x{size:x})", address, [])
                for index, (symbol_name, address, size) in enumerate(symbols)
            ],
            relocs={relocation.source: relocation for relocation in relocs},
        )

    @staticmethod
    def _reloc(source, kind, target, module="main", addend=0):
        return TargetReloc(source, kind, target, module, addend, 0)

    def _synthetic_inputs(self):
        # All function labels, addresses and module identities in this fixture are invented.
        main_symbols = [
            ("caller", 0x1000, 0x20),
            ("GX_Init", 0x1040, 0x8),
            ("small_leaf", 0x1060, 0x4),
            ("big_leaf", 0x1080, 0x40),
            ("hard_func", 0x10C0, 0x20),
            ("__aeabi_dadd", 0x10E0, 0x4),
            ("_dadd", 0x10E4, 0x4),
        ]
        main_relocs = [
            self._reloc(0x1004, "arm_call", 0x3021, "overlay(1)"),
            self._reloc(0x1008, "arm_call_thumb", 0x3001, "overlays(1,17)"),
            self._reloc(0x1044, "thumb_call", 0x10E0),
            self._reloc(0x10C0, "load", 0x2000),
            self._reloc(0x10C4, "load", 0x2004),
            self._reloc(0x10C8, "load", 0x2008),
            self._reloc(0x10D0, "arm_call", 0x10E0),
            self._reloc(0x10D8, "arm_call", 0x10E4),
            self._reloc(0x10D4, "arm_branch", 0x10E0),
        ]
        main = self._module(
            "main",
            main_symbols,
            main_relocs,
            {".text": (0x1000, 0x1200), ".rodata": (0x2000, 0x2100)},
        )
        ov001 = self._module(
            "ov001", [("OverlayOneFunction", 0x3001, 0x4), ("OS_HotLeaf", 0x3021, 0x8)],
            sections={".text": (0x3000, 0x3100)},
        )
        ov017 = self._module(
            "ov017", [("StoryLeaf", 0x3001, 0x4)],
            sections={".text": (0x3000, 0x3100)},
        )
        report = {
            "version": 2,
            "units": [
                {
                    "name": "_dsd_gap@main_0",
                    "metadata": {"auto_generated": True},
                    "functions": [
                        {"name": "GX_Init", "size": "8"},
                        {"name": "small_leaf", "size": "4"},
                        {"name": "__dsd_gap_anchor_main_1000", "size": "16"},
                        {"name": "hard_func", "size": "32"},
                        {"name": "already_matched", "size": "8", "fuzzy_match_percent": 100.0},
                        {"name": "almost_matched", "size": "4", "fuzzy_match_percent": 99.5},
                    ],
                },
                {
                    "name": "_dsd_gap@ov001_1",
                    "metadata": {"auto_generated": True},
                    "functions": [{"name": "OS_HotLeaf", "size": "8"}],
                },
                {
                    "name": "_dsd_gap@ov017_2",
                    "metadata": {"auto_generated": True},
                    "functions": [{"name": "StoryLeaf", "size": "4"}],
                },
            ],
        }
        event_catalog = {
            "secondary_dispatch_catalog": {
                "module": "ov001",
                "entries": [
                    {"callable_by_vm": True, "callback_id": 7, "handler_address": 0x3021},
                    {"callable_by_vm": False, "callback_id": 8, "handler_address": 0x1061},
                ],
            },
        }
        table_catalog = {
            "dispatch_review": {
                "scenario_story_handler_disassembly": {
                    "table_module": "ov017",
                    "handlers": [{"address": 0x3001, "opcode": 3}],
                },
            },
        }
        return report, {"main": main, "ov001": ov001, "ov017": ov017}, event_catalog, table_catalog

    def test_build_queue_filters_matches_and_applies_priority_order(self):
        report, modules, events, tables = self._synthetic_inputs()
        result = build_queue(report, modules, events, tables, report_path="report.json")

        queue = result["queue"]
        names = [function["function_name"] for function in queue]
        self.assertEqual(result["schema_version"], 2)
        self.assertEqual(result["report_source"]["objdiff_version"], 2)
        self.assertEqual(result["report_source"]["matched_function_count"], 1)
        self.assertEqual(result["report_source"]["unmatched_function_count"], 6)
        self.assertNotIn("__dsd_gap_anchor_main_1000", names)
        self.assertNotIn("already_matched", names)
        self.assertIn("almost_matched", names)
        self.assertEqual(names[:2], ["OS_HotLeaf", "StoryLeaf"])
        self.assertLess(names.index("GX_Init"), names.index("small_leaf"))
        self.assertLess(names.index("small_leaf"), names.index("hard_func"))
        self.assertEqual([function["priority_rank"] for function in queue], list(range(1, len(queue) + 1)))

        hot_sdk = queue[0]
        self.assertEqual(hot_sdk["module"], "ov001")
        self.assertEqual(hot_sdk["symbol_name"], "OS_HotLeaf")
        self.assertEqual(hot_sdk["address"], 0x3020)
        self.assertTrue(hot_sdk["hot_path"]["is_hot"])
        self.assertEqual(hot_sdk["hot_path"]["markers"][0]["kind"], "event_secondary_callback")
        self.assertTrue(hot_sdk["sdk_classification"]["classified"])
        self.assertEqual(hot_sdk["sdk_classification"]["prefix"], "OS_")
        self.assertEqual(hot_sdk["callers"][0]["symbol_name"], "caller")

    def test_call_graph_preserves_ambiguous_overlays_and_separates_difficulty(self):
        report, modules, events, tables = self._synthetic_inputs()
        queue = build_queue(report, modules, events, tables)["queue"]
        by_name = {function["function_name"]: function for function in queue}

        story = by_name["StoryLeaf"]
        self.assertEqual(story["hot_path"]["markers"][0]["kind"], "scenario_story_handler")
        story_caller = story["callers"][0]
        self.assertEqual(story_caller["symbol_name"], "caller")
        self.assertEqual(story_caller["target_module_candidates"], ["ov001", "ov017"])

        hard = by_name["hard_func"]
        self.assertEqual(hard["call_count"], 2)
        self.assertEqual([callee["symbol_name"] for callee in hard["callees"]], ["__aeabi_dadd", "_dadd"])
        factors = hard["difficulty"]["factors"]
        self.assertTrue(factors["float_softfp"]["detected"])
        self.assertEqual(factors["jump_table_heuristic"]["rodata_load_references"], 3)
        self.assertTrue(factors["jump_table_heuristic"]["likely"])
        self.assertGreater(hard["difficulty"]["score"], by_name["small_leaf"]["difficulty"]["score"])
        self.assertEqual(hard["gap_object"], "_dsd_gap@main_0")
        self.assertEqual(hard["object_identity"], "_dsd_gap@main_0")

    def test_dispatch_catalog_without_a_module_does_not_mark_a_default_overlay_hot(self):
        report, modules, events, tables = self._synthetic_inputs()
        del events["secondary_dispatch_catalog"]["module"]
        queue = build_queue(report, modules, events, tables)["queue"]
        hot_sdk = next(function for function in queue if function["function_name"] == "OS_HotLeaf")
        self.assertFalse(hot_sdk["hot_path"]["is_hot"])

    def test_entry_module_overrides_the_catalog_module(self):
        report, modules, events, _tables = self._synthetic_inputs()
        events["secondary_dispatch_catalog"]["module"] = "ov017"
        events["secondary_dispatch_catalog"]["entries"] = [
            {"module": "main", "callable_by_vm": True, "callback_id": 9, "handler_address": 0x1061},
        ]
        queue = build_queue(report, modules, events)["queue"]
        by_name = {function["function_name"]: function for function in queue}
        self.assertTrue(by_name["small_leaf"]["hot_path"]["is_hot"])
        self.assertFalse(by_name["OS_HotLeaf"]["hot_path"]["is_hot"])

    def test_no_catalogs_produces_a_regular_queue(self):
        report, modules, _events, _tables = self._synthetic_inputs()
        result = build_queue(report, modules)
        self.assertTrue(result["queue"])
        self.assertTrue(all(not function["hot_path"]["is_hot"] for function in result["queue"]))
        self.assertEqual(result["catalog_sources"], {"event_scripts": None, "script_tables": None})

    def test_load_modules_treats_missing_relocation_file_as_empty(self):
        with tempfile.TemporaryDirectory() as directory:
            config_dir = Path(directory)
            (config_dir / "delinks.txt").write_text(
                "    .text start:0x00001000 end:0x00001100 kind:code align:4\n",
                encoding="utf-8",
            )
            (config_dir / "symbols.txt").write_text(
                "entry kind:function(arm,size=0x4) addr:0x00001000\n",
                encoding="utf-8",
            )
            config_path = config_dir / "config.yaml"
            config_path.write_text(
                "main_module:\n"
                "  name: main\n"
                "  delinks: delinks.txt\n"
                "  symbols: symbols.txt\n"
                "  relocations: absent-relocs.txt\n"
                "autoloads: []\n"
                "overlays: []\n",
                encoding="utf-8",
            )

            modules = load_modules(config_path)

        self.assertEqual(modules["main"].relocs, {})
        self.assertEqual(modules["main"].sections[".text"], (0x1000, 0x1100))


class QueueFingerprintTests(unittest.TestCase):
    def test_generation_check_and_changed_input_detection(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            config_dir = project / "config"
            config_dir.mkdir()
            for name in ("delinks.txt", "symbols.txt", "relocs.txt"):
                (config_dir / name).write_text("synthetic\n", encoding="utf-8")
            config = config_dir / "config.yaml"
            config.write_text(
                "main_module:\n  name: main\n  delinks: delinks.txt\n  symbols: symbols.txt\n  relocations: relocs.txt\nautoloads: []\noverlays: []\n",
                encoding="utf-8",
            )
            report = project / "report.json"
            event = project / "events.json"
            tables = project / "tables.json"
            report.write_text('{"units":[]}\n', encoding="utf-8")
            event.write_text('{"events":[]}\n', encoding="utf-8")
            tables.write_text('{"tables":[]}\n', encoding="utf-8")
            output = project / "queue.json"
            args = [
                "--project", str(project), "--config", str(config), "--report", str(report),
                "--event-catalog", str(event), "--script-table-catalog", str(tables),
                "--output", str(output),
            ]
            with mock.patch("tools.decomp_queue.load_modules", return_value={}):
                self.assertEqual(main(args), 0)
                written = output.read_text(encoding="utf-8")
                queue = json.loads(written)
                self.assertEqual(queue["schema_version"], 2)
                self.assertEqual(len(queue["inputs"]["files"]), 7)
                self.assertIn("command", queue["inputs"])
                self.assertTrue(queue["inputs"]["generated_at_utc"].endswith("+00:00"))
                for fingerprint in queue["inputs"]["files"]:
                    self.assertTrue(Path(fingerprint["path"]).is_absolute() is False)
                    self.assertIsInstance(fingerprint["size"], int)
                    self.assertIsInstance(fingerprint["mtime_ns"], int)
                    self.assertEqual(len(fingerprint["sha256"]), 64)
                self.assertEqual(main(args + ["--check"]), 0)
                event.write_text('{"events":[1]}\n', encoding="utf-8")
                self.assertEqual(main(args + ["--check"]), 1)
                self.assertEqual(output.read_text(encoding="utf-8"), written)
                # rollback/configure rewrite files with identical bytes and a new mtime: not a change
                event.write_text('{"events":[]}\n', encoding="utf-8")
                os.utime(event, (1_700_000_000, 1_700_000_000))
                self.assertEqual(main(args + ["--check"]), 0)
                config.write_text(config.read_text(encoding="utf-8"), encoding="utf-8")
                os.utime(config, (1_700_000_001, 1_700_000_001))
                self.assertEqual(main(args + ["--check"]), 0)

    def test_optional_catalogs_are_absent_from_generation_and_freshness_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            config = project / "config.yaml"
            config.write_text("main_module: {name: main, delinks: delinks.txt, symbols: symbols.txt, "
                              "relocations: relocs.txt}\n")
            for name in ("delinks.txt", "symbols.txt", "relocs.txt"):
                (project / name).write_text("synthetic\n")
            report = project / "report.json"
            report.write_text('{"units":[]}\n')
            output = project / "queue.json"
            args = ["--project", str(project), "--config", str(config), "--report", str(report),
                    "--output", str(output)]
            with mock.patch("tools.decomp_queue.load_modules", return_value={}):
                self.assertEqual(main(args), 0)
                queue = json.loads(output.read_text())
                self.assertEqual(len(queue["inputs"]["files"]), 5)
                self.assertEqual(queue["catalog_sources"], {"event_scripts": None, "script_tables": None})
                self.assertEqual(main(args + ["--check"]), 0)
                (project / "symbols.txt").write_text("synthetic changed\n")
                self.assertEqual(main(args + ["--check"]), 1)


if __name__ == "__main__":
    unittest.main()
