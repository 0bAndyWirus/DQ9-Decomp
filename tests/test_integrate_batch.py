"""Batch integration tests with invented guest addresses, modules and report counts."""

import dataclasses
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import integrate_candidates as candidates_tool
from tools import integrate_batch

PANIC = "panicked at objdiff-core/src/arch/arm.rs:130"


_REAL_CHECK = integrate_batch._require_consistent_workspace
_REAL_SNAPSHOT = integrate_batch._snapshot_workspace


class IntegrateBatchTests(unittest.TestCase):
    def setUp(self):
        patcher = patch.object(integrate_batch, "_require_consistent_workspace")
        patcher.start()
        self.addCleanup(patcher.stop)
        snap = patch.object(integrate_batch, "_snapshot_workspace", return_value=Path("snap"))
        snap.start()
        self.addCleanup(snap.stop)

    def test_snapshot_keeps_only_latest_archives_of_config_and_src(self):
        import tarfile
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "config").mkdir()
            (root / "config" / "a.txt").write_text("a", encoding="utf-8")
            (root / "src").mkdir()
            (root / "src" / "b.cpp").write_text("b", encoding="utf-8")
            (root / "build").mkdir()
            (root / "build" / "junk").write_text("x", encoding="utf-8")
            snaps = root / "build" / "scratch" / "snapshots"
            snaps.mkdir(parents=True)
            for index in range(4):
                (snaps / f"pre-batch-2000010{index}-000000.tar.gz").write_bytes(b"old")
            target = _REAL_SNAPSHOT(root, keep=2)
            self.assertEqual(len(list(snaps.glob("pre-batch-*.tar.gz"))), 2)
            with tarfile.open(target) as archive:
                self.assertEqual(sorted(archive.getnames()), ["config", "config/a.txt", "src", "src/b.cpp"])

    def test_consistency_check_rejects_configured_sources_that_are_missing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src/Recovered/ov120").mkdir(parents=True)
            (root / "src/Recovered/ov120/present.cpp").write_text("x", encoding="utf-8")
            plan = "wibo mwccarm.exe -c src/Recovered/ov120/present.cpp -o x\nwibo mwccarm.exe -c src/Recovered/ov120/gone.cpp -o x\n"
            with patch.object(integrate_batch, "_run", return_value=_result(0, plan, "")):
                with self.assertRaisesRegex(integrate_batch.BatchError, "1 configured source"):
                    _REAL_CHECK(root)

    def _candidate(self, root, *, author="Author", stem="unit", module="ov123", symbol=None, address=0x1000):
        manifest = root / "src" / "_candidates" / author / f"{stem}.json"
        source = root / "src" / "_candidates" / author / f"{stem}.cpp"
        obj = root / "build" / "scratch" / author / f"{stem}.o"
        source.parent.mkdir(parents=True, exist_ok=True)
        obj.parent.mkdir(parents=True, exist_ok=True)
        source.write_text(f"int {stem}() {{ return 1; }}\n", encoding="utf-8")
        obj.write_bytes(b"object")
        functions = [{"name": symbol or stem, "address": hex(address)}]
        manifest.write_text(json.dumps({
            "file": f"src/_candidates/{author}/{stem}.cpp",
            "object": f"build/scratch/{author}/{stem}.o",
            "module": module,
            "functions": functions,
        }), encoding="utf-8")
        candidate = candidates_tool.load_candidate(manifest, root)
        return candidate, source, obj, manifest, functions

    def _fake_driver(self, project, *, bad_stems=(), panic_stems=(), report_metrics=None,
                     link_failures=0, apply_failures=()):
        """Fake subprocess driver: apply/configure/link/check/report against a temp project."""
        applied: list[str] = []
        report_count = 0
        link_count = 0

        def write_report():
            report = project / "build" / "eur" / "report.json"
            report.parent.mkdir(parents=True, exist_ok=True)
            payload = report_metrics or {"measures": {"matched_functions": 12,
                                                      "matched_code": 128,
                                                      "complete_units": 2,
                                                      "total_units": 4}}
            report.write_text(json.dumps(payload), encoding="utf-8")
        def driver(command, cwd=None, **kwargs):
            nonlocal report_count, link_count
            joined = " ".join(command)
            if "port_eu_unit.py" in joined:
                if "--apply" in command:
                    applied.append(command[2])
                    if any(command[2].endswith(f"/{stem}.o") for stem in apply_failures):
                        return _result(1, "", ".text: no member matches the target modules")
                address = command[command.index("--text-address") + 1]
                return _result(0, json.dumps({"sections": {".text": {"start": address}},
                                              "conflicts": [], "source_defines": []}), "")
            if "configure.py" in joined:
                return _result(0, "configured", "")
            if "build/eur/arm9.o" in command:
                link_count += 1
                if link_count <= len(bad_stems):
                    return _result(1, "", f"Multiply-defined symbols: sym_{bad_stems[link_count - 1]}\n")
                if link_count <= len(bad_stems) + link_failures:
                    return _result(1, "", "")
                return _result(0, "linked", "")
            if "check_modules" in command[-1]:
                return _result(0, "Check module main: OK\n", "")
            if "report" in command[-1]:
                report_count += 1
                if report_count == 1 and panic_stems:
                    return _result(2, "", PANIC + "\\n")
                write_report()
                return _result(0, "report", "")
            return _result(0, "", "")

        return driver, applied

    # --- success path -----------------------------------------------------
    def test_success_integrates_configures_checks_reports_and_commits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate, source, _, _, _ = self._candidate(root, stem="unit")
            driver, _ = self._fake_driver(root)
            with patch("tools.integrate_batch.subprocess.run", side_effect=driver), \
                 patch("tools.integrate_candidates.subprocess.run", side_effect=driver):
                code = integrate_batch.run_batch([str(candidate.manifest)], root, root / "dsd", root / "wibo")
            self.assertEqual(code, 0)
            self.assertTrue(candidate.destination.is_file())
            self.assertFalse(source.exists())
            self.assertFalse((root / "build/scratch/integrate_candidates_rollback.json").exists())
            self.assertTrue((root / "build/eur/report.json").is_file())

    # --- multiply-defined exclusion / retry -------------------------------
    def test_multiply_defined_excludes_offender_and_integrates_rest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            good, good_src, _, good_manifest, _ = self._candidate(root, stem="good", symbol="sym_good")
            bad, bad_src, _, bad_manifest, _ = self._candidate(root, author="Other", stem="bad",
                                                               symbol="sym_bad", address=0x1100)
            driver, applied = self._fake_driver(root, bad_stems=("bad",))
            with patch("tools.integrate_batch.subprocess.run", side_effect=driver), \
                 patch("tools.integrate_candidates.subprocess.run", side_effect=driver), \
                 patch("builtins.print") as mock_print:
                code = integrate_batch.run_batch([str(good.manifest), str(bad.manifest)],
                                                 root, root / "dsd", root / "wibo")
            self.assertEqual(code, 0)
            # The bad unit was applied once, rolled back, and never applied again.
            self.assertEqual(sum(obj.endswith("/bad.o") for obj in applied), 1)
            self.assertTrue(good.destination.is_file())
            self.assertFalse(good_src.exists())
            self.assertTrue(bad_src.is_file())
            self.assertTrue(bad_manifest.is_file())
            self.assertFalse(bad.destination.exists())
            output = "\n".join(str(call.args[0]) for call in mock_print.call_args_list)
            self.assertIn("multiply-defined: sym_bad", output)
            self.assertIn(str(bad_manifest), output)
            self.assertFalse((root / "build/scratch/integrate_candidates_rollback.json").exists())

    # --- report panic bisection ------------------------------------------
    def test_report_panic_bisects_and_excludes_offender(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = ["alpha", "bravo", "charlie"]
            made = [self._candidate(root, author=f"A{i}", stem=name, address=0x1000 + 0x100 * i)
                    for i, name in enumerate(names)]
            driver, _ = self._fake_driver(root, panic_stems=("charlie",))
            with patch("tools.integrate_batch.subprocess.run", side_effect=driver), \
                 patch("tools.integrate_candidates.subprocess.run", side_effect=driver), \
                 patch.object(integrate_batch, "_probe_report",
                              side_effect=lambda items, *_: any(item.manifest.stem == "charlie" for item in items)), \
                 patch("builtins.print") as mock_print:
                code = integrate_batch.run_batch([str(item[0].manifest) for item in made],
                                                 root, root / "dsd", root / "wibo")
            self.assertEqual(code, 0)
            output = "\n".join(str(call.args[0]) for call in mock_print.call_args_list)
            self.assertIn("objdiff report panic isolated by manifest bisection", output)
            self.assertIn("charlie", output)
            for name, item in zip(names, made):
                if name == "charlie":
                    self.assertTrue(item[1].is_file())  # excluded source untouched
                else:
                    self.assertTrue(item[0].destination.is_file())
            self.assertFalse((root / "build/scratch/integrate_candidates_rollback.json").exists())

    def test_report_panic_bisection_rejects_failing_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(integrate_batch, "_stage", return_value=_result(0, "", "")), \
                 patch.object(integrate_batch, "_run",
                              return_value=_result(2, "", PANIC)):
                with self.assertRaisesRegex(integrate_batch.BatchError, "baseline failed"):
                    integrate_batch._bisect_panic(
                        [], root, root / "report.json", root / "dsd", root / "wibo")

    def test_report_probe_ignores_panic_text_on_successful_command(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = self._candidate(root, stem="unit")[0]
            with patch.object(integrate_batch, "_stage", return_value=_result(0, "", "")), \
                 patch.object(integrate_batch, "_run",
                              return_value=_result(0, PANIC, "")), \
                 patch.object(candidates_tool, "apply_candidates"), \
                 patch.object(candidates_tool, "rollback_candidates"):
                self.assertFalse(integrate_batch._probe_report(
                    [candidate], root, root / "report.json", root / "dsd", root / "wibo"))


    def test_thumb_halfword_unit_end_rounds_up_when_two_bytes_are_gap(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = self._candidate(root, address=0x1001)[0]
            with patch.object(candidates_tool, "function_sizes",
                              return_value={0x1000: 0x12, 0x1014: 4}), \
                 patch.object(integrate_batch, "_can_pad", return_value=True):
                self.assertEqual(integrate_batch._planned_thumb_unit_end(candidate, root), 0x1014)

    def _project_with_tail(self, root: Path, tail: bytes):
        arm9 = root / "config/eur/arm9"
        arm9.mkdir(parents=True)
        (arm9 / "config.yaml").write_text(
            "main_module:\n  name: main\n  object: ../../../build/eur/build/arm9.bin\n  delinks: delinks.txt\n"
            "  symbols: symbols.txt\n  relocations: relocs.txt\nautoloads: []\noverlays: []\n")
        (arm9 / "delinks.txt").write_text("    .text start:0x1000 end:0x1100 kind:code\n")
        (arm9 / "symbols.txt").write_text("f kind:function(thumb,size=0x12) addr:0x1001\n")
        (arm9 / "relocs.txt").write_text("")
        (root / "build/eur/build").mkdir(parents=True)
        (root / "build/eur/build/arm9.bin").write_bytes(bytes(0x12) + tail + bytes(0xec))

    def test_halfword_unit_with_a_nonzero_padding_tail_is_rejected_by_the_gate_like_the_applier(self):
        for tail, expected in ((b"\x00\x00", 0x1014), (b"\x00\x01", None)):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self._project_with_tail(root, tail)
                candidate = self._candidate(root, address=0x1001)[0]
                candidate = dataclasses.replace(candidate, module="main")
                with patch.object(candidates_tool, "function_sizes", return_value={0x1000: 0x12}):
                    self.assertEqual(integrate_batch._planned_thumb_unit_end(candidate, root), expected, tail)

    def test_thumb_halfword_unit_without_two_byte_gap_is_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = self._candidate(root, address=0x1001)[0]
            with patch.object(candidates_tool, "function_sizes",
                              return_value={0x1000: 0x12, 0x1012: 2}):
                self.assertIsNone(integrate_batch._planned_thumb_unit_end(candidate, root))

    def _abuts(self, symbols: str, header: str, size: int = 0x40):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "symbols.txt").write_text(symbols)
            (root / "delinks.txt").write_text(header + "\nsrc/X.cpp:\n    complete\n")
            candidate = self._candidate(root, address=0x1000)[0]
            with patch.object(candidates_tool, "function_sizes", return_value={0x1000: size}), \
                 patch.object(candidates_tool, "_symbols_path", return_value=root / "symbols.txt"):
                return integrate_batch._abuts_data_symbol(candidate, root)

    HEADER = "    .text start:0x1000 end:0x2000 kind:code align:32\n"

    def test_unit_ending_right_before_a_bss_or_data_symbol_inside_text_is_rejected(self):
        for kind in ("bss", "data(any)"):
            self.assertTrue(self._abuts(f"f kind:function(arm,size=0x40) addr:0x1000\nd kind:{kind} addr:0x1040\n",
                                        self.HEADER), kind)

    def test_unit_followed_by_a_function_or_closing_the_text_section_is_accepted(self):
        followed = "f kind:function(arm,size=0x40) addr:0x1000\ng kind:function(arm,size=0x8) addr:0x1040\n"
        self.assertFalse(self._abuts(followed, self.HEADER))
        closing = "f kind:function(arm,size=0x40) addr:0x1000\nd kind:data(any) addr:0x1040\n"
        self.assertFalse(self._abuts(closing, "    .text start:0x1000 end:0x1040 kind:code align:32\n"))

    # Invented status records retain the DSD parser's public diagnostic format.
    SYNTHETIC_CHECK_OUTPUT = (
        "[INFO ] Check ARM9 main: checksum failed\n[INFO ] Check ITCM: OK\n[INFO ] Check DTCM: OK\n"
        "[INFO ] Check overlay 121: checksum failed\n[INFO ] Check overlay 122: OK\n"
        "[INFO ] Check overlay 124: checksum failed\n[INFO ] Check overlay 125: checksum failed\n"
        "[INFO ] Check overlay 126: OK\nError: Some module(s) didn't pass the checks.\n")

    def test_failed_modules_are_read_from_dsd_output_format(self):
        self.assertEqual(integrate_batch._failed_checksum_modules(self.SYNTHETIC_CHECK_OUTPUT),
                         {"main", "ov121", "ov124", "ov125"})
        self.assertEqual(integrate_batch._failed_checksum_modules("[INFO ] Check ITCM: checksum failed\n"), {"itcm"})
        self.assertEqual(integrate_batch._failed_checksum_modules("[INFO ] Check DTCM: checksum failed\n"), {"dtcm"})
        self.assertEqual(integrate_batch._failed_checksum_modules("Check overlay 5: OK\n"), set())

    def test_only_failed_modules_that_hold_batch_candidates_are_returned(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            items = [self._candidate(root, author="A", stem="a", module="ov124", address=0x1000)[0],
                     self._candidate(root, author="B", stem="b", module="ov122", address=0x2000)[0],
                     self._candidate(root, author="C", stem="c", module="main", address=0x3000)[0]]
            self.assertEqual(integrate_batch._checksum_modules(self.SYNTHETIC_CHECK_OUTPUT, items), {"ov124", "main"})
            self.assertEqual(integrate_batch._checksum_modules("nothing failed\n", items), set())

    def test_checksum_failure_bisects_only_candidates_in_failed_module(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main = self._candidate(root, author="Main", stem="mainunit", module="main",
                                   address=0x1000)
            good = self._candidate(root, author="Good", stem="good", module="ov127",
                                   address=0x1100)
            bad = self._candidate(root, author="Bad", stem="bad", module="ov128",
                                  address=0x1200)
            checks = 0

            def driver(command, cwd=None, **kwargs):
                nonlocal checks
                joined = " ".join(command)
                if "port_eu_unit.py" in joined:
                    address = command[command.index("--text-address") + 1]
                    return _result(0, json.dumps({"sections": {".text": {"start": address}},
                                                  "conflicts": [], "source_defines": []}), "")
                if "configure.py" in joined:
                    return _result(0, "configured", "")
                if "build/eur/arm9.o" in command:
                    return _result(0, "linked", "")
                if "check_modules" in command[-1]:
                    checks += 1
                    if checks == 1:
                        return _result(1, "", "[INFO ] Check overlay 128: checksum failed")
                    return _result(0, "[INFO ] Check overlay 128: OK", "")
                if "report" in command[-1]:
                    report = root / "build/eur/report.json"
                    report.parent.mkdir(parents=True, exist_ok=True)
                    report.write_text(json.dumps({"measures": {"matched_functions": 1,
                        "matched_code": 1, "complete_units": 1, "total_units": 1}}))
                    return _result(0, "report", "")
                return _result(0, "", "")

            with patch("tools.integrate_batch.subprocess.run", side_effect=driver), \
                 patch("tools.integrate_candidates.subprocess.run", side_effect=driver), \
                 patch.object(integrate_batch, "_probe_modules",
                              side_effect=lambda items, *_: any(
                                  item.manifest.stem == "bad" for item in items)), \
                 patch("builtins.print") as mock_print:
                code = integrate_batch.run_batch(
                    [str(item[0].manifest) for item in (main, good, bad)],
                    root, root / "dsd", root / "wibo")
            self.assertEqual(code, 0)
            output = "\n".join(str(call.args[0]) for call in mock_print.call_args_list)
            self.assertIn("module checksum failure isolated by check_modules", output)
            self.assertIn(str(bad[3]), output)
            self.assertNotIn(str(main[3]), output)
            self.assertNotIn(str(good[3]), output)

    def test_unknown_link_failure_bisects_and_excludes_only_offender(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            good = self._candidate(root, author="A", stem="good", address=0x1000)
            bad = self._candidate(root, author="B", stem="bad", address=0x1100)
            driver, _ = self._fake_driver(root, link_failures=1)
            def probe(items, *_):
                return any(item.manifest.stem == "bad" for item in items)
            with patch("tools.integrate_batch.subprocess.run", side_effect=driver), \
                 patch("tools.integrate_candidates.subprocess.run", side_effect=driver), \
                 patch.object(integrate_batch, "_probe_link", side_effect=probe), \
                 patch("builtins.print") as mock_print:
                code = integrate_batch.run_batch(
                    [str(good[0].manifest), str(bad[0].manifest)], root, root / "dsd", root / "wibo")
            self.assertEqual(code, 0)
            self.assertTrue(good[0].destination.is_file())
            self.assertTrue(bad[1].is_file())
            output = "\n".join(str(call.args[0]) for call in mock_print.call_args_list)
            self.assertIn("arm9 link failure isolated by manifest bisection", output)
            self.assertIn(str(bad[3]), output)
            self.assertNotIn(str(good[3]), output)
    def test_apply_failure_excludes_named_manifest_not_batch_tail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            good = self._candidate(root, author="A", stem="good", address=0x1000)
            bad = self._candidate(root, author="B", stem="bad", address=0x1100)
            last = self._candidate(root, author="C", stem="last", address=0x1200)
            driver, applied = self._fake_driver(root, apply_failures=("bad",))
            with patch("tools.integrate_batch.subprocess.run", side_effect=driver), \
                 patch("tools.integrate_candidates.subprocess.run", side_effect=driver), \
                 patch("builtins.print") as mock_print:
                code = integrate_batch.run_batch(
                    [str(good[3]), str(bad[3]), str(last[3])], root, root / "dsd", root / "wibo")
            self.assertEqual(code, 0)
            self.assertTrue(good[0].destination.is_file())
            self.assertTrue(last[0].destination.is_file())
            self.assertTrue(bad[1].is_file())
            self.assertEqual(sum(obj.endswith("/bad.o") for obj in applied), 1)
            output = "\n".join(str(call.args[0]) for call in mock_print.call_args_list)
            self.assertIn(str(bad[3]), output)
            self.assertIn(".text: no member matches the target modules", output)
            self.assertNotIn(str(last[3]), output)

    def test_preflight_named_failure_skips_only_that_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            good = self._candidate(root, author="A", stem="good", address=0x1000)
            bad = self._candidate(root, author="B", stem="bad", address=0x1100)
            last = self._candidate(root, author="C", stem="last", address=0x1200)
            driver, _ = self._fake_driver(root)

            def preflight(items, _project):
                if items[0].manifest.name == bad[3].name:
                    raise candidates_tool.CandidateError(
                        f"preflight rejected candidate set: {bad[3].name}: .text: no member matches the target modules")

            with patch("tools.integrate_batch.subprocess.run", side_effect=driver), \
                 patch("tools.integrate_candidates.subprocess.run", side_effect=driver), \
                 patch.object(candidates_tool, "preflight", side_effect=preflight), \
                 patch("builtins.print") as mock_print:
                code = integrate_batch.run_batch(
                    [str(good[3]), str(bad[3]), str(last[3])], root, root / "dsd", root / "wibo")
            self.assertEqual(code, 0)
            self.assertTrue(good[0].destination.is_file())
            self.assertTrue(last[0].destination.is_file())
            self.assertTrue(bad[1].is_file())
            output = "\n".join(str(call.args[0]) for call in mock_print.call_args_list)
            self.assertIn(str(bad[3]), output)
            self.assertIn(".text: no member matches the target modules", output)
            self.assertNotIn(str(good[3]), output)
            self.assertNotIn(str(last[3]), output)


    def test_unknown_link_bisection_reports_interacting_pair(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            items = [self._candidate(root, author=f"A{i}", stem=name, address=0x1000 + i * 0x100)[0]
                     for i, name in enumerate(("one", "two", "three", "four"))]
            def probe(subset, *_):
                stems = {item.manifest.stem for item in subset}
                return {"two", "four"} <= stems
            with patch.object(integrate_batch, "_probe_link", side_effect=probe):
                offenders = integrate_batch._bisect_link(items, root, root / "dsd", root / "wibo")
            self.assertEqual({item.manifest.stem for item in offenders}, {"two", "four"})

    def test_large_combination_only_failure_is_left_unisolated_after_a_bounded_number_of_probes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            items = [self._candidate(root, author=f"A{i}", stem=f"unit{i:02d}", address=0x1000 + i * 0x100)[0]
                     for i in range(20)]
            probes = []

            def probe(subset, *_):
                probes.append(len(subset))
                return {"unit03", "unit17"} <= {item.manifest.stem for item in subset}  # only the pair fails

            with patch.object(integrate_batch, "_probe_link", side_effect=probe):
                offenders = integrate_batch._bisect_link(items, root, root / "dsd", root / "wibo")
            self.assertEqual(offenders, [])  # not isolated: the runner re-runs the chunk as halves
            self.assertEqual(probes, [10, 10])  # one halving, no 100-pair scan

    def test_pair_scan_still_runs_for_small_sets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            items = [self._candidate(root, author=f"A{i}", stem=f"unit{i}", address=0x1000 + i * 0x100)[0]
                     for i in range(8)]
            probes = []

            def probe(subset, *_):
                probes.append(len(subset))
                return {"unit1", "unit6"} <= {item.manifest.stem for item in subset}

            with patch.object(integrate_batch, "_probe_link", side_effect=probe):
                offenders = integrate_batch._bisect_link(items, root, root / "dsd", root / "wibo")
            self.assertEqual({item.manifest.stem for item in offenders}, {"unit1", "unit6"})
            self.assertLessEqual(len(probes), 2 + integrate_batch.MAX_PAIR_PROBES)

    # --- retry limit / rollback safety ------------------------------------
    def test_retry_limit_exhausted_rolls_back_and_reports_exclusions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            one, src1, _, manifest1, _ = self._candidate(root, stem="one", symbol="sym_one", address=0x1000)
            two, src2, _, manifest2, _ = self._candidate(root, author="Other", stem="two",
                                                         symbol="sym_two", address=0x1100)
            driver, _ = self._fake_driver(root, bad_stems=("one", "two"))
            with patch("tools.integrate_batch.subprocess.run", side_effect=driver), \
                 patch("tools.integrate_candidates.subprocess.run", side_effect=driver), \
                 patch("builtins.print") as mock_print:
                with self.assertRaises(integrate_batch.BatchError) as ctx:
                    integrate_batch.run_batch([str(one.manifest), str(two.manifest)],
                                              root, root / "dsd", root / "wibo", max_retries=1)
            self.assertIn("retry limit reached", str(ctx.exception))
            output = "\n".join(str(call.args[0]) for call in mock_print.call_args_list)
            self.assertIn("multiply-defined", output)
            self.assertIn(str(manifest1), output)
            self.assertIn(str(manifest2), output)
            # Nothing committed: sources and manifests restored in place.
            self.assertTrue(src1.is_file())
            self.assertTrue(src2.is_file())
            self.assertTrue(manifest1.is_file())
            self.assertFalse(one.destination.exists())
            self.assertFalse(two.destination.exists())
            self.assertFalse((root / "build/scratch/integrate_candidates_rollback.json").exists())

    # --- dry run ----------------------------------------------------------
    def test_dry_run_preflights_without_mutating(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate, source, _, _, _ = self._candidate(root, stem="unit")
            driver, applied = self._fake_driver(root)
            with patch("tools.integrate_batch.subprocess.run") as batch_run, \
                 patch("tools.integrate_candidates.subprocess.run", side_effect=driver) as candidates_run:
                code = integrate_batch.run_batch([str(candidate.manifest)], root, root / "dsd",
                                                 root / "wibo", dry_run=True)
            self.assertEqual(code, 0)
            self.assertEqual(batch_run.call_count, 0)  # only the preflight plan call ran
            self.assertEqual(candidates_run.call_count, 1)
            self.assertNotIn("--apply", candidates_run.call_args.args[0])
            self.assertEqual(applied, [])
            self.assertTrue(source.is_file())
            self.assertFalse(candidate.destination.exists())
            self.assertFalse((root / "build/eur/report.json").exists())

    # --- report freshness gate -------------------------------------------
    def test_stale_report_fails_check_and_excludes_after_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate, source, _, manifest, _ = self._candidate(root, stem="unit")

            def stale_driver(command, cwd=None, **kwargs):
                joined = " ".join(command)
                if "port_eu_unit.py" in joined:
                    return _result(0, json.dumps({"sections": {".text": {"start": "0x1000"}},
                                                  "conflicts": [], "source_defines": []}), "") \
                        if "--apply" not in command else _result(0, "{}", "")
                if "configure.py" in joined:
                    return _result(0, "configured", "")
                if "check_modules" in command[-1]:
                    return _result(0, "OK", "")
                if "report" in command[-1]:
                    return _result(0, "report", "")  # claims success but writes no report
                return _result(0, "", "")

            with patch("tools.integrate_batch.subprocess.run", side_effect=stale_driver), \
                 patch("tools.integrate_candidates.subprocess.run", side_effect=stale_driver), \
                 patch("builtins.print") as mock_print:
                with self.assertRaises(integrate_batch.BatchError) as ctx:
                    integrate_batch.run_batch([str(candidate.manifest)], root, root / "dsd",
                                              root / "wibo", max_retries=0)
            self.assertIn("report did not produce a readable report.json", str(ctx.exception))
            output = "\n".join(str(call.args[0]) for call in mock_print.call_args_list)
            self.assertIn(str(manifest), output)
            self.assertTrue(source.is_file())  # rolled back, not committed
            self.assertFalse((root / "build/scratch/integrate_candidates_rollback.json").exists())

    def _report_payload(self) -> str:
        return json.dumps({"measures": {"matched_functions": 0, "matched_code": 0,
                                        "complete_units": 1, "total_units": 2}})

    def test_report_written_with_a_coarse_or_equal_timestamp_is_accepted(self):
        # File timestamps come from a coarse clock: a report generated right now can carry an mtime
        # below the wall clock sampled before the command, or equal to the previous report's mtime.
        for stamp in (1_000_000_000, "previous"):
            with self.subTest(stamp=stamp), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                report = root / "build" / "eur" / "report.json"
                report.parent.mkdir(parents=True)
                report.write_text("{}", encoding="utf-8")
                previous = report.stat().st_mtime_ns
                when = previous if stamp == "previous" else stamp * 10**9

                def regenerate(command, cwd=None, **kwargs):
                    report.write_text(self._report_payload(), encoding="utf-8")
                    os.utime(report, ns=(when, when))
                    return _result(0, "", "")

                with patch.object(integrate_batch, "_run", side_effect=regenerate):
                    payload = integrate_batch._fresh_report(root, report)
                self.assertEqual(payload["measures"]["complete_units"], 1)

    def test_old_report_is_never_returned_when_the_command_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "build" / "eur" / "report.json"
            report.parent.mkdir(parents=True)
            report.write_text(self._report_payload(), encoding="utf-8")  # valid but stale
            with patch.object(integrate_batch, "_run", return_value=_result(0, "", "")):
                with self.assertRaisesRegex(integrate_batch.BatchError, "did not produce a readable report"):
                    integrate_batch._fresh_report(root, report)

    def test_a_subset_that_cannot_be_applied_is_inconclusive_not_fatal_in_every_probe(self):
        # The full batch applied, but a bisection subset can fail to apply (a placement can depend on a
        # neighbour that is not in the subset). That must neither abort the chunk nor blame the subset.
        error = candidates_tool.CandidateError("apply failed for b.json (exit 2): .text: no member matches")
        probes = (
            lambda items: integrate_batch._probe_link(items, Path("p"), Path("d"), Path("w")),
            lambda items: integrate_batch._probe_modules(items, Path("p"), Path("d"), Path("w")),
            lambda items: integrate_batch._probe_report(items, Path("p"), Path("r.json"), Path("d"), Path("w")),
        )
        for probe in probes:
            with self.subTest(probe=probe), \
                    patch.object(candidates_tool, "apply_candidates", side_effect=error), \
                    patch.object(candidates_tool, "rollback_candidates") as rollback, \
                    patch.object(integrate_batch, "_stage") as stage, \
                    patch.object(integrate_batch, "_run") as run:
                self.assertFalse(probe(["x"]))
                rollback.assert_not_called()  # nothing was applied, apply_candidates already rolled back
                stage.assert_not_called()
                run.assert_not_called()

    def test_bisection_survives_an_inapplicable_half_and_still_isolates_the_offender(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            items = [self._candidate(root, stem=name, address=0x1000 + 0x10 * index)[0]
                     for index, name in enumerate("abcd")]
            names = lambda subset: "".join(sorted(c.manifest.stem for c in subset))

            def apply(subset, project):
                if names(subset) == "ab":  # this half cannot be placed on its own
                    raise candidates_tool.CandidateError("apply failed for a.json (exit 2)")
                applied.append(names(subset))

            def stage(project, dsd, wibo, name):
                link_fails = "d" in applied[-1] and name == "build/eur/arm9.o"
                return _result(1 if link_fails else 0, "", "")

            applied = []
            with patch.object(candidates_tool, "apply_candidates", side_effect=apply), \
                    patch.object(candidates_tool, "rollback_candidates"), \
                    patch.object(integrate_batch, "_stage", side_effect=stage):
                offenders = integrate_batch._bisect_link(items, Path("p"), Path("d"), Path("w"))
            self.assertEqual(names(offenders), "d")

    def test_chunk_must_be_a_positive_integer(self):
        for value in ("0", "-5"):
            with self.subTest(chunk=value), patch.object(integrate_batch, "run_batch") as run_batch:
                self.assertEqual(integrate_batch.main(["x.json", "--chunk", value]), 1)
                run_batch.assert_not_called()

    def test_second_integration_is_refused_while_lock_is_held(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with integrate_batch.WorkspaceLock(root):
                with self.assertRaises(integrate_batch.BatchError):
                    with integrate_batch.WorkspaceLock(root):
                        pass
            with integrate_batch.WorkspaceLock(root):
                pass

    # --- CLI plumbing ------------------------------------------------------
    def test_main_no_matches_returns_error(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(integrate_batch.BatchError):
                integrate_batch.run_batch(["src/_candidates/**/*.json"], root, root / "dsd", root / "wibo")


def _result(code, out, err):
    from types import SimpleNamespace
    return SimpleNamespace(returncode=code, stdout=out, stderr=err)


if __name__ == "__main__":
    unittest.main()
