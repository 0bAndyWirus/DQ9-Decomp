"""Integration-ledger checks using invented logs, counts, paths and modules."""

import json
import os
import tempfile
import unittest
from pathlib import Path

from tools.integration_ledger import (
    build_ledger,
    categorize,
    estimate_eta,
    failure_kind,
    health_scan,
    load_run,
    parse_chunk_log,
    relative_manifest,
    write_retry_lists,
)

SYNTHETIC_PROJECT = "/synthetic/project"
GOOD_LOG = f"""snapshot: {SYNTHETIC_PROJECT}/build/scratch/snapshots/pre-batch-1.tar.gz
Batch integration summary:
  Integrated: 2 unit(s), 2 function(s)
  Skipped: 1 unit(s), 1 function(s)
    skipped {SYNTHETIC_PROJECT}/src/_candidates/AuthorA/c.json: {SYNTHETIC_PROJECT}/src/_candidates/AuthorA/c.json: candidate source is newer than its object
  Matched: 12 functions, 512 bytes, 8/16 units
  Excluded manifests: 1
    {SYNTHETIC_PROJECT}/src/_candidates/AuthorA/b.json: arm9 link failure isolated by manifest bisection
"""
FAILED_LOG = """snapshot: x
integrate_batch.py: apply failed for synthetic_bad.json (exit 2): port_eu_unit.py: .text: no member matches the target modules
"""


def _manifest(root: Path, author: str, stem: str, module: str = "ov123", *, header: str | None = None,
              stale: bool = False) -> Path:
    base = root / "src/_candidates" / author
    base.mkdir(parents=True, exist_ok=True)
    source = base / f"{stem}.cpp"
    source.write_text(f'#include "{header}"\nint f(){{return 1;}}\n' if header else "int f(){return 1;}\n")
    obj = root / "build/scratch" / author / f"{stem}.o"
    obj.parent.mkdir(parents=True, exist_ok=True)
    obj.write_bytes(b"o")
    manifest = base / f"{stem}.json"
    manifest.write_text(json.dumps({"module": module, "functions": [{"name": "f", "address": "0x08000000"}]}))
    now = os.stat(obj).st_mtime_ns
    os.utime(source, ns=(now + (10**9 if stale else -10**9),) * 2)
    return manifest


class ParseTests(unittest.TestCase):
    def test_summary_log_yields_skipped_excluded_and_match(self):
        log = parse_chunk_log(GOOD_LOG)
        self.assertEqual(log.integrated_units, 2)
        self.assertEqual(log.matched, (12, 512, 8, 16))
        self.assertEqual(list(log.skipped), ["src/_candidates/AuthorA/c.json"])
        self.assertIn("newer than its object", log.skipped["src/_candidates/AuthorA/c.json"])
        self.assertEqual(list(log.excluded), ["src/_candidates/AuthorA/b.json"])
        self.assertIsNone(log.failure)

    def test_skipped_lines_are_not_mistaken_for_exclusions(self):
        text = (f"  Excluded manifests: 0\n    skipped {SYNTHETIC_PROJECT}/src/_candidates/A/x.json: why\n")
        log = parse_chunk_log(text)
        self.assertEqual(log.excluded, {})
        self.assertEqual(list(log.skipped), ["src/_candidates/A/x.json"])

    def test_failure_line_is_captured_and_classified(self):
        log = parse_chunk_log(FAILED_LOG)
        self.assertEqual(failure_kind(log.failure), "apply-failed:synthetic_bad.json")
        self.assertEqual(failure_kind("retry limit reached: build/eur/arm9.o failed"), "retry-limit")
        self.assertEqual(failure_kind("could not isolate arm9 link failure"), "unisolated")
        self.assertEqual(failure_kind(None), "unknown")

    def test_categories(self):
        self.assertEqual(categorize("candidate source is newer than its object"), "stale-object")
        self.assertEqual(categorize("author-local header types.h conflicts with different src/x"), "header-conflict")
        self.assertEqual(categorize("arm9 link failure isolated by manifest bisection"), "link-bisection")
        self.assertEqual(categorize("something new"), "other")

    def test_relative_manifest_strips_any_prefix(self):
        self.assertEqual(relative_manifest(f"{SYNTHETIC_PROJECT}/src/_candidates/A/x.json"), "src/_candidates/A/x.json")
        self.assertEqual(relative_manifest("src/_candidates/A/x.json"), "src/_candidates/A/x.json")


class LedgerTests(unittest.TestCase):
    def _run_dir(self, root: Path, name: str, chunks: list[tuple[list[str], int, str]]) -> Path:
        directory = root / name
        directory.mkdir(parents=True)
        results = []
        for number, (manifests, code, log) in enumerate(chunks, 1):
            (directory / f"chunk{number:03d}.log").write_text(log)
            results.append({"chunk": number, "manifests": manifests, "exit": code, "seconds": 100.0})
        (directory / "results.json").write_text(json.dumps(results))
        (directory / "runner.log").write_text(f"{sum(len(c[0]) for c in chunks)} manifests, {len(chunks)} chunks, 0 already recorded\n")
        return directory

    def test_outcomes_per_state_and_later_run_overrides_earlier(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            a, b, c, d = (f"src/_candidates/AuthorA/{n}.json" for n in "abcd")
            first = self._run_dir(root, "pass1", [([a, b, c], 0, GOOD_LOG), ([d], 1, FAILED_LOG)])
            second = self._run_dir(root, "pass2", [([d], 0, "  Integrated: 1 unit(s), 1 function(s)\n")])
            ledger = build_ledger([load_run(first)])
            self.assertEqual(ledger[a]["state"], "integrated")
            self.assertEqual(ledger[b]["category"], "link-bisection")
            self.assertEqual(ledger[c]["category"], "stale-object")
            self.assertEqual(ledger[d]["state"], "chunk-failed")
            self.assertEqual(ledger[d]["category"], "chunk-failed:apply-failed")
            ledger = build_ledger([load_run(first), load_run(second)])
            self.assertEqual(ledger[d]["state"], "integrated")
            self.assertEqual(ledger[d]["run"], "pass2")

    def _reconcile_fixture(self, root: Path):
        project = root / "proj"
        a = _manifest(project, "AuthorA", "a").relative_to(project).as_posix()  # logged integrated, still on disk
        b = "src/_candidates/AuthorA/b.json"  # logged excluded, absent from disk
        e = "src/_candidates/AuthorA/e.json"  # logged integrated, absent, copy under src/Recovered
        f = "src/_candidates/AuthorA/f.json"  # logged integrated, absent, no copy anywhere
        (project / "src/Recovered/ov123").mkdir(parents=True)
        (project / "src/Recovered/ov123/e.json").write_text("{}")
        run = self._run_dir(root, "pass1", [([a, b, e, f], 0, GOOD_LOG)])
        return project, load_run(run), (a, b, e, f)

    def test_an_absent_candidate_is_never_promoted_to_integrated_without_logged_success(self):
        with tempfile.TemporaryDirectory() as directory:
            project, run, (a, b, e, f) = self._reconcile_fixture(Path(directory))
            ledger = build_ledger([run], project)
            self.assertEqual(ledger[a]["state"], "integrated-unconfirmed")
            self.assertEqual(ledger[b]["state"], "unknown-absent")  # logged exclusion, not a success
            self.assertEqual(ledger[e]["state"], "integrated")
            self.assertEqual(ledger[f]["state"], "integrated-unconfirmed")

    def test_an_in_flight_transaction_disables_workspace_cross_checks_and_retry_lists(self):
        with tempfile.TemporaryDirectory() as directory:
            project, run, (a, b, e, f) = self._reconcile_fixture(Path(directory))
            journal = project / "build/scratch/integrate_candidates_rollback.json"
            journal.parent.mkdir(parents=True, exist_ok=True)
            journal.write_text("{}")
            ledger = build_ledger([run], project)
            self.assertTrue(all(entry["fs"] == "in-flight" for entry in ledger.values()))
            self.assertEqual(ledger[b]["state"], "excluded")  # logged outcome preserved
            self.assertEqual(ledger[a]["state"], "integrated")
            with self.assertRaises(ValueError):
                write_retry_lists(ledger, Path(directory) / "out", project)

    def test_a_unit_integrated_into_a_variant_directory_counts_as_integrated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "proj"
            variant = project / "src/Recovered/ov123/_h0123456789"
            variant.mkdir(parents=True)
            (variant / "odd.json").write_text("{}")
            manifest = "src/_candidates/AuthorB/odd.json"  # moved away by the integrator
            run = self._run_dir(root, "pass1", [([manifest], 0, "  Integrated: 1 unit(s), 1 function(s)\n")])
            self.assertEqual(build_ledger([load_run(run)], project)[manifest]["state"], "integrated")

    def test_retry_lists_keep_only_pending_manifests_on_disk(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "proj"
            kept = _manifest(project, "AuthorA", "c").relative_to(project).as_posix()
            gone = "src/_candidates/AuthorA/gone.json"
            ledger = {
                kept: {"state": "skipped", "category": "stale-object"},
                gone: {"state": "skipped", "category": "stale-object"},
                "src/_candidates/AuthorA/ok.json": {"state": "integrated", "category": "integrated"},
            }
            counts = write_retry_lists(ledger, root / "out", project)
            self.assertEqual((root / "out/retry-stale-object.txt").read_text().split(), [kept])
            self.assertEqual(counts["retry-all.txt"], 1)


class EtaTests(unittest.TestCase):
    def test_slow_chunks_raise_the_expected_eta_above_the_optimistic_one(self):
        seconds = [100.0] * 8 + [1000.0, 1000.0]
        eta = estimate_eta(seconds, 10)
        self.assertEqual(eta["optimistic"], 1000.0)
        self.assertAlmostEqual(eta["expected"], 10 * (100 * 0.8 + 1000 * 0.2))
        self.assertGreater(eta["pessimistic"], eta["optimistic"])

    def test_nothing_remaining_means_zero(self):
        self.assertEqual(estimate_eta([100.0], 0)["expected"], 0.0)


class HealthTests(unittest.TestCase):
    def test_scan_reports_stale_objects_and_conflicting_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            ok_one = _manifest(project, "AuthorA", "one", header="types.h")
            (ok_one.parent / "types.h").write_text("// A\n")
            ok_two = _manifest(project, "AuthorB", "two", header="types.h")
            (ok_two.parent / "types.h").write_text("// B\n")
            ok_three = _manifest(project, "AuthorB", "three", header="types.h")
            stale = _manifest(project, "AuthorC", "old", stale=True)
            report = health_scan(project)
            self.assertEqual(report["problems"], {"stale-object": [stale.relative_to(project).as_posix()]})
            self.assertEqual(report["with_local_headers"], 3)
            self.assertEqual(len(report["header_conflicts"]), 1)
            conflict = report["header_conflicts"][0]
            self.assertEqual((conflict["module"], conflict["header"]), ("ov123", "types.h"))
            self.assertEqual(report["manifests_needing_variant_dir"], 1)
            self.assertEqual(conflict["minority_manifests"], [ok_one.relative_to(project).as_posix()])
            self.assertEqual(report["scanned"], 4)

    def test_candidate_header_that_differs_from_the_module_copy_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            clashing = _manifest(project, "AuthorA", "clash", header="types.h")
            (clashing.parent / "types.h").write_text("// mine\n")
            same = _manifest(project, "AuthorB", "same", header="abi.h")
            (same.parent / "abi.h").write_text("// shared\n")
            recovered = project / "src/Recovered/ov123"
            recovered.mkdir(parents=True)
            (recovered / "types.h").write_text("// already integrated variant\n")
            (recovered / "abi.h").write_text("// shared\n")
            report = health_scan(project)
            self.assertEqual(report["conflicts_with_existing_headers"],
                             [{"module": "ov123", "header": "types.h",
                               "manifests": [clashing.relative_to(project).as_posix()]}])
            self.assertEqual(report["manifests_needing_variant_dir"], 1)
            self.assertEqual(report["healthy"], 2)


if __name__ == "__main__":
    unittest.main()
