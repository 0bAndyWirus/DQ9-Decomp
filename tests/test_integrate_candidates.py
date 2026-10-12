import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools.integrate_candidates import (
    CandidateError,
    SkipList,
    apply_candidates,
    finalize_candidates,
    load_candidate,
    load_skip_list,
    main,
    preflight,
    preflight_eligible,
    rollback_candidates,
    select_candidates,
)


class IntegrateCandidatesTests(unittest.TestCase):
    def _candidate(self, root, *, author="Author", stem="unit", file=None, object_path=None, module="ov017", functions=None):
        manifest = root / "src" / "_candidates" / author / f"{stem}.json"
        source = root / "src" / "_candidates" / author / f"{stem}.cpp"
        obj = root / "build" / "scratch" / author / f"{stem}.o"
        source.parent.mkdir(parents=True, exist_ok=True)
        obj.parent.mkdir(parents=True, exist_ok=True)
        source.write_text("int unit() { return 1; }\n", encoding="utf-8")
        obj.write_bytes(b"object")
        data = {
            "file": file or f"src/_candidates/{author}/{stem}.cpp",
            "object": object_path or f"build/scratch/{author}/{stem}.o",
            "module": module,
            "functions": functions if functions is not None else [{"name": "unit", "address": "0x1000"}],
        }
        manifest.write_text(json.dumps(data), encoding="utf-8")
        return manifest, source, obj

    def test_killed_apply_is_recovered_from_write_ahead_journal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first, first_source, _ = self._candidate(root, stem="one")
            second, second_source, _ = self._candidate(root, stem="two")
            config = root / "config" / "eur" / "delinks.txt"
            config.parent.mkdir(parents=True)
            config.write_text("original\n", encoding="utf-8")
            script = (
                "import os, signal, sys\n"
                "from pathlib import Path\n"
                "from types import SimpleNamespace\n"
                "from unittest.mock import patch\n"
                "import tools.integrate_candidates as ic\n"
                "root = Path(sys.argv[1])\n"
                "cands = [ic.load_candidate(root / 'src/_candidates/Author' / f'{n}.json', root) for n in ('one', 'two')]\n"
                "calls = []\n"
                "def fake_run(cmd, **kw):\n"
                "    calls.append(cmd)\n"
                "    (root / 'config/eur/delinks.txt').write_text('edited %d\\n' % len(calls))\n"
                "    if len(calls) == 2:\n"
                "        os.kill(os.getpid(), signal.SIGKILL)\n"
                "    return SimpleNamespace(returncode=0, stdout='', stderr='')\n"
                "with patch.object(ic.subprocess, 'run', fake_run):\n"
                "    ic.apply_candidates(cands, root)\n"
            )
            result = subprocess.run([sys.executable, "-c", script, str(root)],
                                    cwd=Path(__file__).resolve().parents[1], capture_output=True, text=True)
            self.assertEqual(result.returncode, -9, result.stderr)
            self.assertFalse(first_source.exists())
            self.assertEqual(config.read_text(encoding="utf-8"), "edited 2\n")
            rollback_candidates(root)
            self.assertTrue(first_source.exists())
            self.assertTrue(second_source.exists())
            self.assertTrue(first.exists() and second.exists())
            self.assertEqual(config.read_text(encoding="utf-8"), "original\n")

    def test_load_candidate_validates_paths_and_maps_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, source, obj = self._candidate(root)
            candidate = load_candidate(manifest, root)
            self.assertEqual(candidate.source, source.resolve())
            self.assertEqual(candidate.object, obj.resolve())
            self.assertEqual(candidate.destination, (root / "src/Recovered/ov017/unit.cpp").resolve())

    def test_load_candidate_rejects_author_path_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, _ = self._candidate(root, file="src/_candidates/Elsewhere/unit.cpp")
            with self.assertRaisesRegex(CandidateError, "file must be"):
                load_candidate(manifest, root)

    def test_load_candidate_requires_nonempty_function_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, _ = self._candidate(root, functions=[{"name": "", "address": "0x1000"}])
            with self.assertRaisesRegex(CandidateError, "function 0"):
                load_candidate(manifest, root)

    def test_legacy_manifest_without_object_and_with_symbol_keys_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, source, obj = self._candidate(root)
            data = json.loads(manifest.read_text(encoding="utf-8"))
            del data["object"]
            data["functions"] = [{"symbol": "unit", "address": "0x1000"}]
            manifest.write_text(json.dumps(data), encoding="utf-8")
            candidate = load_candidate(manifest, root)
            self.assertEqual(candidate.object, obj.resolve())  # location fixed by convention
            self.assertEqual(candidate.functions, ({"name": "unit", "address": "0x1000"},))

    def test_a_wrong_object_path_or_a_function_without_any_name_is_still_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, _ = self._candidate(root, object_path="build/scratch/Elsewhere/unit.o")
            with self.assertRaisesRegex(CandidateError, "object must be"):
                load_candidate(manifest, root)
            manifest, _, _ = self._candidate(root, functions=[{"address": "0x1000"}])
            with self.assertRaisesRegex(CandidateError, "function 0"):
                load_candidate(manifest, root)
            manifest, _, _ = self._candidate(root, functions=[{"symbol": 7, "address": "0x1000"}])
            with self.assertRaisesRegex(CandidateError, "function 0"):
                load_candidate(manifest, root)

    def test_preflight_rejects_conflicts_without_applying(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, _ = self._candidate(root)
            candidate = load_candidate(manifest, root)
            response = SimpleNamespace(returncode=0, stdout=json.dumps({"sections": {".text": {"start": "0x1000"}}, "conflicts": ["overlap"], "source_defines": []}), stderr="")
            with patch("tools.integrate_candidates.subprocess.run", return_value=response) as run:
                with self.assertRaisesRegex(CandidateError, "conflicts"):
                    preflight([candidate], root)
            self.assertNotIn("--apply", run.call_args.args[0])

    def test_preflight_uses_earliest_function_address(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, _ = self._candidate(
                root, functions=[{"name": "later", "address": "0x2000"}, {"name": "earlier", "address": "0x1000"}])
            candidate = load_candidate(manifest, root)
            response = SimpleNamespace(returncode=0, stdout=json.dumps(
                {"sections": {".text": {"start": "0x1000"}}, "conflicts": [], "source_defines": []}), stderr="")
            with patch("tools.integrate_candidates.subprocess.run", return_value=response) as run:
                preflight([candidate], root)
            command = run.call_args.args[0]
            self.assertEqual(command[command.index("--text-address") + 1], "0x1000")

    def test_select_candidates_skips_missing_objects_and_newer_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing, _, missing_object = self._candidate(root, stem="missing")
            missing_object.unlink()
            stale, stale_source, stale_object = self._candidate(root, stem="stale")
            os.utime(stale_object, ns=(1_000_000_000, 1_000_000_000))
            os.utime(stale_source, ns=(2_000_000_000, 2_000_000_000))
            candidates, skipped = select_candidates([missing, stale], root)
            self.assertEqual(candidates, [])
            self.assertEqual(len(skipped), 2)
            self.assertIn("object does not exist", skipped[0][1])
            self.assertIn("source is newer", skipped[1][1])

    def test_preflight_eligible_skips_only_failed_candidates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            invalid_manifest, _, _ = self._candidate(
                root, stem="invalid", functions=[{"name": "invalid", "address": "0x1000"}])
            valid_manifest, _, _ = self._candidate(
                root, author="Other", stem="valid", functions=[{"name": "valid", "address": "0x2000"}])
            invalid = load_candidate(invalid_manifest, root)
            valid = load_candidate(valid_manifest, root)
            rejected = SimpleNamespace(returncode=0, stdout=json.dumps(
                {"sections": {".text": {"start": "0x1000"}}, "conflicts": ["overlap"],
                 "source_defines": []}), stderr="")
            accepted = SimpleNamespace(returncode=0, stdout=json.dumps(
                {"sections": {".text": {"start": "0x2000"}}, "conflicts": [],
                 "source_defines": []}), stderr="")
            with patch("tools.integrate_candidates.subprocess.run", side_effect=[rejected, accepted]):
                candidates, skipped = preflight_eligible([invalid, valid], root)
            self.assertEqual(candidates, [valid])
            self.assertEqual(len(skipped), 1)
            self.assertIn("conflicts", skipped[0][1])



    def test_duplicate_module_address_is_skipped_before_preflight(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest1, _, _ = self._candidate(root)
            manifest2, _, _ = self._candidate(root, author="Other", stem="other")
            with patch("tools.integrate_candidates.subprocess.run") as run:
                self.assertEqual(main([str(manifest1), str(manifest2), "--project", str(root)]), 0)
            run.assert_not_called()

    def test_select_candidates_requires_functions_to_abut(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            symbols = root / "config" / "eur" / "arm9" / "overlays" / "ov017" / "symbols.txt"
            symbols.parent.mkdir(parents=True)
            symbols.write_text(
                "a kind:function(arm,size=0x8) addr:0x1000\n"
                "b kind:function(thumb,size=0x6) addr:0x1008\n"
                "c kind:function(arm,size=0x4) addr:0x1010\n"
                "d kind:function(arm,size=0x4) addr:0x1100\n",
                encoding="utf-8",
            )
            contiguous, _, _ = self._candidate(root, stem="contiguous", functions=[
                {"name": "a", "address": "0x1000"},
                {"name": "b", "address": "0x1009"},
                {"name": "c", "address": "0x1010"},
            ])
            scattered, _, _ = self._candidate(root, stem="scattered", functions=[
                {"name": "c", "address": "0x1010"},
                {"name": "d", "address": "0x1100"},
            ])
            candidates, skipped = select_candidates([contiguous, scattered], root)
            self.assertEqual([candidate.manifest for candidate in candidates], [contiguous.resolve()])
            self.assertEqual([path for path, _ in skipped], [scattered.resolve()])

    def test_select_candidates_skips_functions_already_delinked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            module_dir = root / "config" / "eur" / "arm9" / "overlays" / "ov017"
            module_dir.mkdir(parents=True)
            (module_dir / "symbols.txt").write_text("", encoding="utf-8")
            # Module-level section lines precede units and are not ownership.
            (module_dir / "delinks.txt").write_text(
                "    .text start:0x00000000 end:0x00010000 kind:code align:32\n\n"
                "src/Owned.cpp:\n    complete\n    .text start:0x00001000 end:0x00001010\n",
                encoding="utf-8",
            )
            owned, _, _ = self._candidate(root, stem="owned", functions=[{"name": "a", "address": "0x1008"}])
            free, _, _ = self._candidate(root, stem="free", functions=[{"name": "b", "address": "0x1010"}])
            candidates, skipped = select_candidates([owned, free], root)
            self.assertEqual([candidate.manifest for candidate in candidates], [free.resolve()])
            self.assertEqual([path for path, _ in skipped], [owned.resolve()])

    def test_apply_preflights_then_applies_and_moves_source_and_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, source, _ = self._candidate(root)
            candidate = load_candidate(manifest, root)
            plan = SimpleNamespace(returncode=0, stdout=json.dumps({"sections": {".text": {"start": "0x1000"}}, "conflicts": [], "source_defines": []}), stderr="")
            applied = SimpleNamespace(returncode=0, stdout="{}", stderr="")
            with patch("tools.integrate_candidates.subprocess.run", side_effect=[plan, applied]) as run:
                preflight([candidate], root)
                apply_candidates([candidate], root)
            self.assertFalse(source.exists())
            self.assertFalse(manifest.exists())
            self.assertTrue(candidate.destination.is_file())
            self.assertTrue(candidate.manifest_destination.is_file())
            self.assertEqual(run.call_args_list[0].args[0][3], "src/Recovered/ov017/unit.cpp")
            self.assertEqual(run.call_args_list[1].args[0][3], "src/Recovered/ov017/unit.cpp")
            self.assertNotIn("--apply", run.call_args_list[0].args[0])
            self.assertIn("--apply", run.call_args_list[1].args[0])

    def test_later_apply_failure_restores_config_sources_and_manifests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest1, source1, _ = self._candidate(root)
            manifest2, source2, _ = self._candidate(root, author="Other", stem="second")
            candidate1 = load_candidate(manifest1, root)
            candidate2 = load_candidate(manifest2, root)
            config = root / "config/eur/arm9/symbols.txt"
            config.parent.mkdir(parents=True)
            config.write_text("before\n", encoding="utf-8")
            calls = 0

            def apply_side_effect(*args, **kwargs):
                nonlocal calls
                calls += 1
                if calls == 1:
                    config.write_text("changed\n", encoding="utf-8")
                    return SimpleNamespace(returncode=0, stdout="{}", stderr="")
                return SimpleNamespace(returncode=1, stdout="", stderr="rejected")

            with patch("tools.integrate_candidates.subprocess.run", side_effect=apply_side_effect):
                with self.assertRaisesRegex(CandidateError, "apply failed"):
                    apply_candidates([candidate1, candidate2], root)
            self.assertEqual(config.read_text(encoding="utf-8"), "before\n")
            self.assertTrue(source1.is_file())
            self.assertTrue(source2.is_file())
            self.assertTrue(manifest1.is_file())
            self.assertTrue(manifest2.is_file())
            self.assertFalse(candidate1.destination.exists())
            self.assertFalse(candidate1.manifest_destination.exists())

    def test_saved_rollback_restores_config_source_and_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, source, _ = self._candidate(root)
            candidate = load_candidate(manifest, root)
            config = root / "config/eur/arm9/symbols.txt"
            config.parent.mkdir(parents=True)
            config.write_text("before\n", encoding="utf-8")

            def apply_side_effect(*args, **kwargs):
                config.write_text("after\n", encoding="utf-8")
                return SimpleNamespace(returncode=0, stdout="{}", stderr="")

            with patch("tools.integrate_candidates.subprocess.run", side_effect=apply_side_effect):
                apply_candidates([candidate], root)
            self.assertTrue((root / "build/scratch/integrate_candidates_rollback.json").is_file())
            rollback_candidates(root)
            self.assertEqual(config.read_text(encoding="utf-8"), "before\n")
            self.assertTrue(source.is_file())
            self.assertTrue(manifest.is_file())
            self.assertFalse(candidate.destination.exists())
            self.assertFalse(candidate.manifest_destination.exists())
            self.assertFalse((root / "build/scratch/integrate_candidates_rollback.json").exists())

    def test_author_local_headers_travel_with_source_and_are_removed_on_rollback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, source, obj = self._candidate(root)
            author_dir = source.parent
            (author_dir / "support.h").write_text('#include "nested.h"\n', encoding="utf-8")
            (author_dir / "nested.h").write_text("// nested\n", encoding="utf-8")
            source.write_text('#include "support.h"\n#include "system.h"\nint unit() { return 1; }\n',
                              encoding="utf-8")
            os.utime(obj)
            candidate = load_candidate(manifest, root)
            with patch("tools.integrate_candidates.subprocess.run",
                       return_value=SimpleNamespace(returncode=0, stdout="{}", stderr="")):
                apply_candidates([candidate], root)
            destination_dir = candidate.destination.parent
            self.assertEqual((destination_dir / "support.h").read_text(encoding="utf-8"), '#include "nested.h"\n')
            self.assertTrue((destination_dir / "nested.h").is_file())
            self.assertFalse((destination_dir / "system.h").exists())
            rollback_candidates(root)
            self.assertFalse((destination_dir / "support.h").exists())
            self.assertFalse((destination_dir / "nested.h").exists())
            self.assertTrue(source.is_file())

    def _with_header(self, root, author, stem, address, content, header="types.h"):
        manifest, source, obj = self._candidate(root, author=author, stem=stem, module="ov017",
                                                functions=[{"name": stem, "address": hex(address)}])
        (source.parent / header).write_text(content, encoding="utf-8")
        source.write_text(f'#include "{header}"\nint {stem}() {{ return 1; }}\n', encoding="utf-8")
        os.utime(obj)
        return manifest, source

    def test_header_differing_from_the_module_copy_moves_the_unit_to_a_variant_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, source = self._with_header(root, "A", "unit", 0x1000, "// mine\n")
            existing = root / "src/Recovered/ov017/types.h"
            existing.parent.mkdir(parents=True)
            existing.write_text("// other\n", encoding="utf-8")
            candidates, skipped = select_candidates([manifest], root)
            self.assertEqual(skipped, [])
            (candidate,) = candidates
            variant = candidate.destination.parent
            self.assertEqual(variant.parent, root / "src/Recovered/ov017")
            self.assertTrue(variant.name.startswith("_h"))
            with patch("tools.integrate_candidates.subprocess.run",
                       return_value=SimpleNamespace(returncode=0, stdout="{}", stderr="")):
                apply_candidates(candidates, root)
            self.assertEqual((variant / "types.h").read_text(encoding="utf-8"), "// mine\n")
            self.assertEqual(existing.read_text(encoding="utf-8"), "// other\n")  # untouched
            self.assertTrue(candidate.destination.is_file())
            rollback_candidates(root)
            self.assertFalse(variant.exists())  # created by this apply, removed once empty
            self.assertEqual(existing.read_text(encoding="utf-8"), "// other\n")
            self.assertTrue(source.is_file())

    def test_supplied_skip_list_functions_ranges_and_prefixes_are_refused(self):
        # Invented policy: no addresses or names originate in an image.
        skip_list = SkipList(
            functions=frozenset({("main", 0x2000), ("ov011", 0x3000)}),
            ranges=(("main", 0x1000, 0x1100),),
            name_prefixes=("synthetic_init_",),
        )
        cases = [("main", "0x2000", "synthetic_a"), ("main", "0x2001", "synthetic_thumb"),
                 ("ov011", "0x3000", "synthetic_b"), ("main", "0x1010", "synthetic_range"),
                 ("main", "0x4000", "synthetic_init_x"), ("ov017", "0x3000", "ok_name")]
        for module, address, name in cases:
            with self.subTest(module=module, address=address), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                manifest, _, _ = self._candidate(root, module=module, functions=[{"name": name, "address": address}])
                with patch("tools.integrate_candidates.contiguity_error", return_value=None):
                    chosen, skipped = select_candidates([manifest], root, skip_list)
                if name == "ok_name":
                    self.assertEqual((len(chosen), skipped), (1, []))
                else:
                    self.assertEqual(chosen, [])
                    self.assertIn(name, skipped[0][1])
                    self.assertIn("skip list" if name != "synthetic_range" else "skip-list", skipped[0][1])

    def test_one_forbidden_function_refuses_the_whole_multi_function_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, _ = self._candidate(root, module="ov011", functions=[
                {"name": "fine", "address": "0x3010"}, {"name": "bad", "address": "0x3000"}])
            with patch("tools.integrate_candidates.contiguity_error", return_value=None):
                chosen, skipped = select_candidates([manifest], root, SkipList(frozenset({("ov011", 0x3000)})))
            self.assertEqual(chosen, [])
            self.assertIn("bad", skipped[0][1])

    def test_skip_list_loads_explicit_file_or_environment_and_warns_when_absent(self):
        with tempfile.TemporaryDirectory() as directory:
            policy = Path(directory) / "skip.json"
            policy.write_text(json.dumps({
                "functions": [{"module": "ov011", "address": "0x3001"}],
                "ranges": [{"module": "main", "start": "0x1000", "end": "0x1100"}],
                "name_prefixes": ["synthetic_init_"],
            }))
            expected = SkipList(frozenset({("ov011", 0x3000)}),
                                (("main", 0x1000, 0x1100),), ("synthetic_init_",))
            self.assertEqual(load_skip_list(policy), expected)
            with patch.dict(os.environ, {"DQ9_SKIP_LIST": str(policy)}):
                self.assertEqual(load_skip_list(), expected)
            policy.write_text(json.dumps([{"module": "main", "address": 0x2000}]))
            self.assertEqual(load_skip_list(policy), SkipList(frozenset({("main", 0x2000)})))
            with patch.dict(os.environ, {}, clear=True), patch("sys.stderr", new_callable=io.StringIO) as errors:
                self.assertEqual(load_skip_list(), SkipList())
            self.assertIn("empty skip list", errors.getvalue())

    def test_skip_list_rejects_invalid_configuration(self):
        invalid = [None, 7, {"functions": {}}, {"functions": [None]},
                   {"functions": [{"module": "bad", "address": "0x1000"}]},
                   {"functions": [{"module": "main", "address": True}]},
                   {"ranges": [{"module": "main", "start": 0x1100, "end": 0x1000}]},
                   {"name_prefixes": [""]}]
        with tempfile.TemporaryDirectory() as directory:
            policy = Path(directory) / "skip.json"
            for data in invalid:
                with self.subTest(data=data):
                    policy.write_text(json.dumps(data))
                    with self.assertRaisesRegex(CandidateError, "cannot load skip list"):
                        load_skip_list(policy)

    def test_cli_applies_explicit_skip_list_before_preflight(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, _ = self._candidate(root, functions=[{"name": "synthetic_a", "address": "0x1000"}])
            policy = root / "skip.json"
            policy.write_text(json.dumps([{"module": "ov017", "address": "0x1000"}]))
            with patch("tools.integrate_candidates.preflight") as check:
                self.assertEqual(main(["--project", str(root), "--skip-list", str(policy), str(manifest)]), 0)
            check.assert_not_called()

    def test_batch_units_with_different_same_named_headers_all_integrate_the_minority_in_a_variant(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            majority = [self._with_header(root, "A", f"m{i}", 0x1000 + 0x10 * i, "// common\n")[0] for i in range(2)]
            odd = self._with_header(root, "B", "odd", 0x2000, "// different\n")[0]
            twin = self._with_header(root, "C", "twin", 0x3000, "// different\n")[0]
            candidates, skipped = select_candidates([*majority, odd, twin], root)
            self.assertEqual(skipped, [])
            by_name = {c.manifest.stem: c for c in candidates}
            self.assertEqual(by_name["m0"].destination.parent, root / "src/Recovered/ov017")
            self.assertEqual(by_name["m1"].destination.parent, root / "src/Recovered/ov017")
            variants = {by_name["odd"].destination.parent, by_name["twin"].destination.parent}
            self.assertEqual(len(variants), 1)  # identical header sets share one variant directory
            (variant,) = variants
            self.assertTrue(variant.name.startswith("_h"))
            with patch("tools.integrate_candidates.subprocess.run",
                       return_value=SimpleNamespace(returncode=0, stdout="{}", stderr="")):
                apply_candidates(candidates, root)
            self.assertEqual((root / "src/Recovered/ov017/types.h").read_text(encoding="utf-8"), "// common\n")
            self.assertEqual((variant / "types.h").read_text(encoding="utf-8"), "// different\n")
            for name in ("odd", "twin"):
                self.assertTrue((variant / f"{name}.cpp").is_file())

    def test_identical_header_already_in_module_is_not_recopied_or_removed_by_rollback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, source, obj = self._candidate(root)
            (source.parent / "support.h").write_text("// same\n", encoding="utf-8")
            source.write_text('#include "support.h"\n', encoding="utf-8")
            os.utime(obj)
            existing = root / "src/Recovered/ov017/support.h"
            existing.parent.mkdir(parents=True)
            existing.write_text("// same\n", encoding="utf-8")
            candidate = load_candidate(manifest, root)
            self.assertEqual(candidate.headers, ())
            with patch("tools.integrate_candidates.subprocess.run",
                       return_value=SimpleNamespace(returncode=0, stdout="{}", stderr="")):
                apply_candidates([candidate], root)
            rollback_candidates(root)
            self.assertTrue(existing.is_file())

    def test_finalize_discards_rollback_state_without_reverting_integration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, source, _ = self._candidate(root)
            candidate = load_candidate(manifest, root)
            with patch("tools.integrate_candidates.subprocess.run", return_value=SimpleNamespace(
                    returncode=0, stdout="{}", stderr="")):
                apply_candidates([candidate], root)
            rollback_file = root / "build/scratch/integrate_candidates_rollback.json"
            self.assertTrue(rollback_file.is_file())
            finalize_candidates(root)
            self.assertFalse(rollback_file.exists())
            self.assertFalse(source.exists())
            self.assertFalse(manifest.exists())
            self.assertTrue(candidate.destination.is_file())
            self.assertTrue(candidate.manifest_destination.is_file())

if __name__ == "__main__":
    unittest.main()
