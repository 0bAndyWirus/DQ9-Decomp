import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import author_targets as at

# Invented exclusions and addresses, independent of any image.
SYNTHETIC_SKIP = at.SkipList(
    functions=frozenset({("ov011", 0x2000), ("ov020", 0x3000)}),
    ranges=(("main", 0x1000, 0x1100),),
    name_prefixes=("synthetic_init_",),
)


def entry(module, address, name=None, *, rank=1, match=None, size=8, unit="_dsd_gap@x_1"):
    return {"function_name": name or f"synthetic_{module}_{address:08x}", "module": module, "address": address,
            "size": size, "unit": unit, "gap_object": unit, "fuzzy_match_percent": match, "priority_rank": rank,
            "hot_path": {"is_hot": rank == 1}, "difficulty": {"score": 0.5}}


class ForbiddenTests(unittest.TestCase):
    def test_supplied_exclusions_are_shared_with_integration(self):
        self.assertIsNotNone(at.is_forbidden("main", 0x1010, "synthetic_a", SYNTHETIC_SKIP))
        self.assertIsNotNone(at.is_forbidden("main", 0x4000, "synthetic_init_a", SYNTHETIC_SKIP))
        for module, address in sorted(SYNTHETIC_SKIP.functions):
            self.assertIsNotNone(at.is_forbidden(module, address, "f", SYNTHETIC_SKIP))
            self.assertIsNotNone(at.is_forbidden(module, address | 1, "f", SYNTHETIC_SKIP))
        self.assertIsNone(at.is_forbidden("main", 0x1100, "f", SYNTHETIC_SKIP))
        self.assertIsNone(at.is_forbidden("ov011", 0x2010, "f", SYNTHETIC_SKIP))
        self.assertIsNone(at.is_forbidden("ov017", 0x3000, "f", SYNTHETIC_SKIP))
        self.assertIsNone(at.is_forbidden("main", 0x1010, "synthetic_init_a"))


class BuildTargetsTests(unittest.TestCase):
    def test_matched_claimed_forbidden_and_parked_are_dropped_with_counts(self):
        entries = [entry("ov017", 0x100, rank=5), entry("ov017", 0x110, match=100.0),
                   entry("ov017", 0x120), entry("ov017", 0x130), entry("main", 0x1010),
                   entry("ov017", 0x140, "synthetic_init_x"), {"function_name": "broken"}]
        targets, counts = at.build_targets(entries, claimed={("ov017", 0x120)}, modes={},
                                           parked={("ov017", 0x130)}, skip_list=SYNTHETIC_SKIP)
        self.assertEqual([t["addr"] for t in targets], [0x100])
        self.assertEqual({k: counts[k] for k in ("matched", "claimed", "forbidden", "parked", "no_module")},
                         {"matched": 1, "claimed": 1, "forbidden": 2, "parked": 1, "no_module": 1})

    def test_parked_functions_are_appended_only_on_request(self):
        entries = [entry("ov017", 0x100), entry("ov017", 0x130)]
        targets, _ = at.build_targets(entries, set(), {}, {("ov017", 0x130)}, include_parked=True)
        self.assertEqual([t["addr"] for t in targets], [0x100, 0x130])

    def test_modules_follow_their_best_priority_and_addresses_ascend_inside_a_module(self):
        entries = [entry("ov003", 0x300, rank=9), entry("ov003", 0x200, rank=50),
                   entry("ov017", 0x900, rank=2), entry("ov017", 0x400, rank=40)]
        targets, _ = at.build_targets(entries, set(), {})
        self.assertEqual([(t["module"], t["addr"]) for t in targets],
                         [("ov017", 0x400), ("ov017", 0x900), ("ov003", 0x200), ("ov003", 0x300)])

    def test_mode_comes_from_symbols_and_defaults_to_arm(self):
        entries = [entry("ov017", 0x101), entry("ov017", 0x200)]
        targets, _ = at.build_targets(entries, set(), {("ov017", 0x100): "thumb"})
        self.assertEqual({t["addr"]: t["mode"] for t in targets}, {0x101: "thumb", 0x200: "arm"})


class BatchTests(unittest.TestCase):
    def test_batches_have_the_requested_size_numbering_and_author_names(self):
        targets = [{"addr": i} for i in range(45)]
        payload = at.make_batches(targets, 20, "AuthorNext", 3)
        self.assertEqual([len(b["targets"]) for b in payload["batches"]], [20, 20, 5])
        self.assertEqual([b["author"] for b in payload["batches"]], ["AuthorNext0001", "AuthorNext0002", "AuthorNext0003"])
        self.assertEqual((payload["targetFunctions"], payload["concurrency"], payload["assignmentLimit"]), (45, 3, 20))
        with self.assertRaises(ValueError):
            at.make_batches(targets, 0, "A", 1)


class WorkspaceReadingTests(unittest.TestCase):
    def test_claims_include_pending_candidates_integrated_units_and_variant_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            for relative, module, address in (("src/_candidates/A/a.json", "ov017", "0x100"),
                                              ("src/Recovered/ov017/b.json", "ov017", "0x200"),
                                              ("src/Recovered/ov017/_h0123456789/c.json", "ov017", "0x301")):
                path = project / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps({"module": module, "functions": [{"name": "f", "address": address}]}))
            (project / "src/_candidates/A/bad.json").write_text("not json")
            self.assertEqual(at.claimed_functions(project), {("ov017", 0x100), ("ov017", 0x200), ("ov017", 0x300)})

    def test_modes_are_read_from_the_symbols_files(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            main = project / "config/eur/arm9/symbols.txt"
            main.parent.mkdir(parents=True)
            main.write_text("synthetic_a kind:function(arm,size=0x10) addr:0x1000\n"
                            "synthetic_b kind:function(thumb,size=0x8) addr:0x1011 local\n"
                            "synthetic_c kind:data(any) addr:0x2000\n")
            self.assertEqual(at.load_modes(project, ["main", "ov999"]),
                             {("main", 0x1000): "arm", ("main", 0x1010): "thumb"})

    # Invented records exercising the attempt-history input shape.
    HISTORY = [{"identity": "ov031:12288", "attempts": [
        {"module": "ov031", "addr": 0x3000, "name": "synthetic_a", "size": 8, "status": "parked"},
        {"module": "ov031", "addr": 0x3001, "name": "synthetic_a", "size": 8, "status": "parked"}]},
        {"identity": "main:16384", "attempts": [{"module": "main", "addr": 0x4000, "name": "f"}]}]

    def test_parked_history_with_attempt_lists_is_flattened_not_silently_empty(self):
        self.assertEqual(at.parked_keys(self.HISTORY), {("ov031", 0x3000), ("main", 0x4000)})

    def test_flat_records_and_the_unresolved_wrapper_are_accepted(self):
        flat = [{"module": "ov031", "addr": 0x3000}, {"module": "main", "address": "0x1000"}]
        self.assertEqual(at.parked_keys(flat), {("ov031", 0x3000), ("main", 0x1000)})
        self.assertEqual(at.parked_keys({"unresolved": flat}), at.parked_keys(flat))
        self.assertEqual(at.parked_keys([]), set())

    def test_unrecognized_parked_shapes_are_rejected_instead_of_read_as_empty(self):
        for bad in ({"queue": []}, "text", 5, [{"identity": "x", "attempts": []}], [{"identity": "x"}],
                    [{"attempts": "no"}], [{"module": "ov1"}], [None]):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                at.parked_keys(bad)

    def test_parked_functions_are_kept_out_of_the_fresh_list_using_synthetic_history(self):
        entries = [entry("ov031", 0x3000, rank=3), entry("ov031", 0x3100, rank=4)]
        targets, counts = at.build_targets(entries, set(), {}, at.parked_keys(self.HISTORY))
        self.assertEqual([t["addr"] for t in targets], [0x3100])
        self.assertEqual(counts["parked"], 1)

    def test_cli_applies_private_skip_list_from_argument_and_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "proj"
            project.mkdir()
            queue = root / "queue.json"
            queue.write_text(json.dumps({"queue": [entry("main", 0x1010), entry("main", 0x1200)]}))
            policy = root / "skip.json"
            policy.write_text(json.dumps({"ranges": [{"module": "main", "start": "0x1000", "end": "0x1100"}]}))
            out = root / "out.json"
            base = ["--project", str(project), "--queue", str(queue), "--output", str(out)]
            for explicit in (True, False):
                with self.subTest(explicit=explicit), patch.dict(os.environ, {"DQ9_SKIP_LIST": str(policy)}):
                    options = [*base, "--skip-list", str(policy)] if explicit else base
                    self.assertEqual(at.main(options), 0)
                result = json.loads(out.read_text())
                self.assertEqual(result["source"]["counts"]["forbidden"], 1)
                self.assertEqual(result["batches"][0]["targets"][0]["addr"], 0x1200)
            with patch.dict(os.environ, {}, clear=True), patch("sys.stderr", new_callable=io.StringIO) as errors:
                self.assertEqual(at.main(base), 0)
            self.assertIn("empty skip list", errors.getvalue())
            self.assertEqual(json.loads(out.read_text())["source"]["counts"]["targets"], 2)

    def test_cli_refuses_a_malformed_parked_file_and_a_pending_journal_and_failed_gates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "proj"
            project.mkdir()
            queue = root / "queue.json"
            queue.write_text(json.dumps({"queue": [entry("ov017", 0x100)]}))
            out = root / "out.json"
            base = ["--project", str(project), "--queue", str(queue), "--output", str(out)]
            parked = root / "parked.json"
            parked.write_text(json.dumps({"something": "else"}))
            self.assertEqual(at.main([*base, "--parked", str(parked)]), 2)
            self.assertFalse(out.exists())
            final = root / "final.json"
            for overall, expected in (("PASS_WITH_CHUNK_FAILURES", 0), ("PASS_WITH_KNOWN_BASELINE_FAIL", 0),
                                      ("FAIL", 1), (None, 1)):
                final.write_text(json.dumps({"overall": overall}))
                out.unlink(missing_ok=True)
                self.assertEqual(at.main([*base, "--require-gates", str(final)]), expected, overall)
                self.assertEqual(out.exists(), expected == 0)
            self.assertEqual(at.main([*base, "--require-gates", str(root / "missing.json")]), 1)
            out.unlink(missing_ok=True)
            journal = project / at.JOURNAL
            journal.parent.mkdir(parents=True)
            journal.write_text("{}")
            self.assertEqual(at.main(base), 1)
            self.assertFalse(out.exists())

    def test_cli_writes_the_file_and_rejects_missing_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "proj"
            project.mkdir()
            queue = root / "queue.json"
            queue.write_text(json.dumps({"queue": [entry("ov017", 0x100), entry("ov017", 0x110)]}))
            out = root / "out.json"
            self.assertEqual(at.main(["--project", str(project), "--queue", str(queue), "--output", str(out),
                                      "--limit", "1"]), 0)
            self.assertEqual(len(json.loads(out.read_text())["batches"]), 2)
            self.assertEqual(at.main(["--project", str(project), "--queue", str(root / "none.json"),
                                      "--output", str(out)]), 2)


if __name__ == "__main__":
    unittest.main()
