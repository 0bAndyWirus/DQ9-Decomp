import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.integration_runner import (
    BaselineConfig,
    EXIT_STOPPED,
    FatalIntegration,
    expected_module_count,
    failure_action,
    load_baseline_config,
    main,
    ROM_KNOWN_HEADER_ONLY,
    Runner,
    classify_ninja,
    control_action,
    modules_ok,
    overall,
    plan_chunks,
    verify_prefix,
)

# Synthetic diagnostics: no application symbols or image-derived evidence.
BASELINE = BaselineConfig(("Synthetic label missing",), ("Synthetic lookup exhausted",))
KNOWN_NINJA = ("FAILED: check_symbols \n[ERROR] Synthetic label missing\n"
               "[ERROR]   Synthetic lookup exhausted\nninja: build stopped\n")


class GateTests(unittest.TestCase):
    def test_symbol_failures_are_strict_by_default_and_exact_when_configured(self):
        self.assertEqual(classify_ninja(0, "")[0], "PASS")
        self.assertEqual(classify_ninja(1, KNOWN_NINJA)[0], "FAIL")
        self.assertEqual(classify_ninja(1, KNOWN_NINJA, baseline_config=BASELINE)[0], "KNOWN_BASELINE_FAIL")
        for text in (
            KNOWN_NINJA.replace("Synthetic label missing", "Unrelated label missing"),
            KNOWN_NINJA.replace("Synthetic label missing", "Prefix Synthetic label missing suffix"),
            KNOWN_NINJA.replace("[ERROR]   Synthetic lookup exhausted\n", ""),
            KNOWN_NINJA + "[ERROR] Synthetic label missing\n",
            KNOWN_NINJA + "[ERROR] something else\n",
            KNOWN_NINJA + "FAILED: build/native.o \n",
            KNOWN_NINJA + "FAILED: check_symbols \n",
            "ninja: error: loading 'build.ninja'\n",
        ):
            with self.subTest(text=text):
                self.assertEqual(classify_ninja(1, text, baseline_config=BASELINE)[0], "FAIL")
        self.assertEqual(classify_ninja(0, KNOWN_NINJA, baseline_config=BASELINE)[0], "FAIL")

    def test_multiple_configured_errors_require_the_complete_multiset(self):
        baseline = BaselineConfig(("Synthetic first missing", "Synthetic second missing"))
        text = "FAILED: check_symbols\n[ERROR] Synthetic second missing\n[ERROR] Synthetic first missing\n"
        self.assertEqual(classify_ninja(1, text, baseline_config=baseline)[0], "KNOWN_BASELINE_FAIL")
        self.assertEqual(classify_ninja(1, text.replace("[ERROR] Synthetic second missing\n", ""),
                                        baseline_config=baseline)[0], "FAIL")

    def test_a_sha1_failure_is_tolerated_only_with_proven_header_only_differences(self):
        both = KNOWN_NINJA + "FAILED: sha1 \n"
        self.assertEqual(classify_ninja(1, both, ROM_KNOWN_HEADER_ONLY, BASELINE)[0], "KNOWN_BASELINE_FAIL")
        self.assertEqual(classify_ninja(1, both, ROM_KNOWN_HEADER_ONLY)[0], "FAIL")
        for rom_exit in (1, None, 2, 0):
            with self.subTest(rom_exit=rom_exit):
                self.assertEqual(classify_ninja(1, both, rom_exit, BASELINE)[0], "FAIL")
        self.assertEqual(classify_ninja(1, "FAILED: sha1 \n", ROM_KNOWN_HEADER_ONLY)[0], "KNOWN_BASELINE_FAIL")
        self.assertEqual(classify_ninja(1, "FAILED: sha1 \n[ERROR] x\n", ROM_KNOWN_HEADER_ONLY)[0], "FAIL")
        self.assertEqual(classify_ninja(1, both + "FAILED: build/src/x.ctx.cpp \n",
                                        ROM_KNOWN_HEADER_ONLY, BASELINE)[0], "FAIL")

    def test_modules_gate_uses_the_dynamic_expected_count_and_exit_zero(self):
        for count in (1, 4, 9):
            with self.subTest(count=count):
                line = f"modules: {count} OK\n"
                self.assertTrue(modules_ok(0, "report: x\n" + line, count))
                self.assertFalse(modules_ok(0, line, count + 1))
                self.assertFalse(modules_ok(1, line, count))
                self.assertFalse(modules_ok(0, line + line, count))
                self.assertFalse(modules_ok(0, line.rstrip() + "  <-- EXPECTED MORE\n", count))
        self.assertFalse(modules_ok(0, "modules: skipped, integration running\n", 4))
        self.assertFalse(modules_ok(0, "modules: 0 OK\n", 0))

    def _final(self, **overrides):
        final = {name: {"status": "PASS"} for name in
                 ("configure", "ninja", "status_modules", "unittest", "cmake_build_game", "ctest_game")}
        for name, status in overrides.items():
            final[name] = {"status": status}
        return final

    def test_overall_status_and_exit_codes(self):
        self.assertEqual(overall(self._final(), []), ("PASS", 0))
        self.assertEqual(overall(self._final(ninja="KNOWN_BASELINE_FAIL"), []), ("PASS_WITH_KNOWN_BASELINE_FAIL", 0))
        self.assertEqual(overall(self._final(), [44]), ("PASS_WITH_CHUNK_FAILURES", 3))
        self.assertEqual(overall(self._final(status_modules="FAIL"), []), ("FAIL", 1))
        self.assertEqual(overall(self._final(cmake_build_game="FAIL", ctest_game="SKIPPED_BUILD_FAILED"), [])[1], 1)

    def test_baseline_json_loads_explicit_error_and_followup_lists(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "baseline.json"
            path.write_text(json.dumps({"symbol_errors": list(BASELINE.symbol_errors),
                                        "followups": list(BASELINE.followups)}))
            baseline = load_baseline_config(path)
            self.assertEqual(classify_ninja(1, KNOWN_NINJA, baseline_config=baseline)[0], "KNOWN_BASELINE_FAIL")
            for data in ({}, [], {"symbol_errors": []}, {"symbol_errors": "Synthetic label missing"},
                         {"symbol_errors": [""]}, {"symbol_errors": ["Synthetic\nmultiline"]},
                         {"symbol_errors": ["Synthetic label missing"], "followups": None},
                         {"symbol_errors": ["Synthetic label missing"], "unknown": []}):
                with self.subTest(data=data):
                    path.write_text(json.dumps(data))
                    with self.assertRaises(ValueError):
                        load_baseline_config(path)

    def test_module_count_is_derived_from_project_config(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            path = project / "config" / "eur" / "arm9" / "config.yaml"
            path.parent.mkdir(parents=True)
            path.write_text("main_module: {name: synthetic-main}\n"
                            "autoloads: [{name: synthetic-autoload}]\n"
                            "overlays: [{name: synthetic-one}, {name: synthetic-two}]\n")
            self.assertEqual(expected_module_count(project), 4)
            path.write_text("main_module: {name: synthetic-main}\n")
            self.assertEqual(expected_module_count(project), 1)
            path.write_text("overlays: []\n")
            with self.assertRaises(ValueError):
                expected_module_count(project)


class VerificationTests(unittest.TestCase):
    def test_runner_propagates_baseline_images_project_and_expected_modules(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            runner = Runner([], project / "run", 2, Path("/dsd"), Path("/wibo"),
                            project=project, rom=project / "reference.nds",
                            rebuilt_image=project / "rebuilt.nds", expected_modules=4,
                            baseline_config=BASELINE)
            commands = {}

            def fake_run(self, command, logfile, cwd):
                commands[logfile] = (command, cwd)
                return {"ninja.log": (1, KNOWN_NINJA + "FAILED: sha1\n"),
                        "rom_compare.log": (ROM_KNOWN_HEADER_ONLY, ""),
                        "status_modules.log": (0, "modules: 4 OK\n")}.get(logfile, (0, ""))

            with patch.object(Runner, "run", fake_run):
                self.assertEqual(runner.verify([]), 0)
            final = json.loads((runner.out_dir / "final.json").read_text())
            self.assertEqual(final["ninja"]["status"], "KNOWN_BASELINE_FAIL")
            self.assertEqual(final["status_modules"]["expected_modules"], 4)
            self.assertEqual(commands["ninja.log"][1], project)
            self.assertIn(project, commands["status_modules.log"][0])
            self.assertIn(4, commands["status_modules.log"][0])
            self.assertEqual(commands["rom_compare.log"][0][-2:],
                             [project / "reference.nds", project / "rebuilt.nds"])

    def test_runner_without_baseline_does_not_allow_symbol_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            runner = Runner([], project / "run", 2, Path("/dsd"), Path("/wibo"),
                            project=project, rom=project / "reference.nds",
                            rebuilt_image=project / "rebuilt.nds")

            def fake_run(self, command, logfile, cwd):
                return {"ninja.log": (1, KNOWN_NINJA),
                        "status_modules.log": (0, "modules: 6 OK\n")}.get(logfile, (0, ""))

            with patch.object(Runner, "run", fake_run), patch(
                    "tools.integration_runner.expected_module_count", return_value=6) as count:
                self.assertEqual(runner.verify([]), 1)
            count.assert_called_once_with(project)
            final = json.loads((runner.out_dir / "final.json").read_text())
            self.assertEqual(final["ninja"]["status"], "FAIL")
            self.assertEqual(final["status_modules"]["status"], "PASS")

    def test_cli_passes_the_optional_baseline_and_explicit_module_count(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            manifests = project / "manifests.txt"
            manifests.write_text("")
            baseline = project / "baseline.json"
            baseline.write_text(json.dumps({"symbol_errors": list(BASELINE.symbol_errors),
                                            "followups": list(BASELINE.followups)}))
            captured = []

            def fake_integrate(self):
                captured.append(self)
                return [], False

            with patch.object(Runner, "integrate", fake_integrate), patch.object(Runner, "verify", return_value=0):
                self.assertEqual(main(["--list", str(manifests), "--out-dir", str(project / "run"),
                                       "--project", str(project), "--rom", str(project / "reference.nds"),
                                       "--rebuilt-image", str(project / "rebuilt.nds"),
                                       "--baseline-config", str(baseline), "--expected-modules", "4"]), 0)
            self.assertEqual(captured[0].expected_modules, 4)
            self.assertEqual(captured[0].project, project)
            self.assertEqual(classify_ninja(1, KNOWN_NINJA, baseline_config=captured[0].baseline_config)[0],
                             "KNOWN_BASELINE_FAIL")


class PlanningTests(unittest.TestCase):
    def test_chunks_cover_the_list_in_order(self):
        self.assertEqual(plan_chunks(list("abcde"), 2), [["a", "b"], ["c", "d"], ["e"]])
        with self.assertRaises(ValueError):
            plan_chunks(["a"], 0)

    def test_prefix_check_rejects_reordered_or_foreign_history(self):
        manifests = ["a", "b", "c", "d"]
        verify_prefix([{"manifests": ["a", "b"]}], manifests)
        with self.assertRaises(ValueError):
            verify_prefix([{"manifests": ["b", "a"]}], manifests)
        with self.assertRaises(ValueError):
            verify_prefix([{"manifests": ["a", "x"]}], manifests)


class ControlFileTests(unittest.TestCase):
    def test_stop_beats_pause_and_absence_means_go(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            self.assertEqual(control_action(out), "go")
            (out / "PAUSE").write_text("")
            self.assertEqual(control_action(out), "pause")
            (out / "STOP").write_text("")
            self.assertEqual(control_action(out), "stop")

    def test_stop_file_ends_the_run_after_the_chunk_in_progress_and_resume_continues(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            manifests = [f"src/_candidates/A/{n}.json" for n in "abcde"]
            runner = Runner(manifests, out, 2, Path("/dsd"), Path("/wibo"))
            commands = []

            first_phase = [True]
            def fake_run(self, command, logfile, cwd):
                commands.append([str(c) for c in command])
                if first_phase[0] and len(commands) == 1:
                    (out / "STOP").write_text("")  # operator asks for a stop while chunk 1 runs
                return 0, ""

            with patch.object(Runner, "run", fake_run):
                results, stopped = runner.integrate()
            self.assertTrue(stopped)
            self.assertEqual([r["chunk"] for r in results], [1])
            self.assertEqual(len(commands), 1)
            self.assertIn("src/_candidates/A/a.json", commands[0])
            self.assertEqual(json.loads((out / "results.json").read_text())[0]["exit"], 0)
            self.assertEqual(EXIT_STOPPED, 4)

            (out / "STOP").unlink()
            commands.clear()
            first_phase[0] = False
            with patch.object(Runner, "run", fake_run):
                results, stopped = Runner(manifests, out, 2, Path("/dsd"), Path("/wibo")).integrate()
            self.assertFalse(stopped)
            self.assertEqual([r["chunk"] for r in results], [1, 2, 3])
            self.assertEqual(len(commands), 2)  # chunk 1 was not repeated
            self.assertNotIn("src/_candidates/A/a.json", " ".join(commands[0]))

    def _splitting_run(self, out: Path, bad: set[str], calls: list, message: str = "retry limit reached: boom"):
        def fake_run(self, command, logfile, cwd):
            chunk = [c for c in map(str, command) if c.endswith(".json")]
            calls.append(chunk)
            failed = any(name in bad for name in chunk)
            text = f"integrate_batch.py: {message}\n" if failed else "  Integrated: ok\n"
            (out / logfile).write_text(text)
            return (1 if failed else 0), text
        return fake_run

    def test_a_chunk_failing_without_attribution_is_split_so_one_bad_manifest_loses_only_itself(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            manifests = [f"src/_candidates/A/{i}.json" for i in range(8)]
            calls = []
            bad = {"src/_candidates/A/5.json"}
            with patch.object(Runner, "run", self._splitting_run(out, bad, calls)):
                results, stopped = Runner(manifests, out, 8, Path("/d"), Path("/w")).integrate()
            self.assertFalse(stopped)
            self.assertEqual([m for r in results for m in r["manifests"]], manifests)  # each exactly once, in order
            self.assertEqual([r["chunk"] for r in results], list(range(1, len(results) + 1)))
            failed = [r for r in results if r["exit"] != 0]
            self.assertEqual([r["manifests"] for r in failed], [["src/_candidates/A/5.json"]])
            self.assertEqual(sum(len(r["manifests"]) for r in results if r["exit"] == 0), 7)
            self.assertLessEqual(len(calls), 2 * 3 + 1)  # bisection depth, not one run per manifest each time
            self.assertTrue(list(out.glob("chunk001.parent*.log")))
            verify_prefix(results, manifests)
            verify_prefix(json.loads((out / "results.json").read_text()), manifests)

    def test_a_chunk_that_already_attributed_every_exclusion_is_recorded_not_split(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            manifests = ["src/_candidates/A/a.json", "src/_candidates/A/b.json"]
            calls = []
            run = self._splitting_run(out, set(manifests), calls,
                                      message="retry limit reached: no candidate units remain after exclusions")
            with patch.object(Runner, "run", run):
                results, _ = Runner(manifests, out, 2, Path("/d"), Path("/w")).integrate()
            self.assertEqual(len(calls), 1)
            self.assertEqual([(r["manifests"], r["exit"]) for r in results], [(manifests, 1)])

    def test_environment_failure_stops_the_run_and_records_nothing(self):
        for message in ("another integration holds /x/lock; wait for it to finish",
                        "pending integration journal /x; run integrate_candidates.py --rollback first",
                        "objdiff report baseline failed before candidate bisection (exit 1)"):
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                out = Path(directory)
                manifests = ["src/_candidates/A/a.json", "src/_candidates/A/b.json"]
                calls = []
                with patch.object(Runner, "run", self._splitting_run(out, set(manifests), calls, message=message)):
                    with self.assertRaises(FatalIntegration):
                        Runner(manifests, out, 2, Path("/d"), Path("/w")).integrate()
                self.assertEqual(len(calls), 1)  # no splitting, no retries
                self.assertFalse((out / "results.json").exists())

    def test_failure_action_matrix(self):
        self.assertEqual(failure_action(0, ""), "ok")
        self.assertEqual(failure_action(1, "integrate_batch.py: could not isolate arm9 link failure: x\n"), "split")
        self.assertEqual(failure_action(1, "Traceback (most recent call last):\n"), "split")  # crash, no message
        self.assertEqual(failure_action(1, "integrate_batch.py: apply failed for a.json (exit 2): x\n"), "split")
        self.assertEqual(failure_action(1, "integrate_batch.py: retry limit reached: no candidate units remain "
                                           "after exclusions\n"), "final")

    def test_changing_the_chunk_size_between_runs_never_repeats_or_skips_a_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            manifests = [f"src/_candidates/A/{i:02d}.json" for i in range(25)]
            seen = []

            def fake_run(self, command, logfile, cwd):
                seen.append((logfile, [c for c in map(str, command) if c.endswith(".json")]))
                if len(seen) == 1:
                    (out / "STOP").write_text("")
                return 0, ""

            with patch.object(Runner, "run", fake_run):
                Runner(manifests, out, 20, Path("/d"), Path("/w")).integrate()
            (out / "STOP").unlink()
            with patch.object(Runner, "run", fake_run):
                results, stopped = Runner(manifests, out, 10, Path("/d"), Path("/w")).integrate()
            self.assertFalse(stopped)
            self.assertEqual([m for _, chunk in seen for m in chunk], manifests)  # each exactly once, in order
            self.assertEqual([c["chunk"] for c in results], [1, 2])
            self.assertEqual([len(c["manifests"]) for c in results], [20, 5])
            self.assertEqual([name for name, _ in seen], ["chunk001.log", "chunk002.log"])

    def test_stop_requested_during_the_last_chunk_skips_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            manifests = ["src/_candidates/A/a.json", "src/_candidates/A/b.json"]

            def fake_run(self, command, logfile, cwd):
                (out / "STOP").write_text("")  # arrives while the only chunk runs
                return 0, ""

            with patch.object(Runner, "run", fake_run):
                results, stopped = Runner(manifests, out, 20, Path("/d"), Path("/w")).integrate()
            self.assertTrue(stopped)
            self.assertEqual(len(results), 1)
            (out / "STOP").unlink()
            with patch.object(Runner, "run", fake_run):
                results, stopped = Runner(manifests, out, 20, Path("/d"), Path("/w")).integrate()
            self.assertFalse(stopped)  # nothing left, no STOP file: verification may proceed
            self.assertEqual(len(results), 1)


if __name__ == "__main__":
    unittest.main()
