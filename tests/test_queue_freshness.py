import json
import os
import tempfile
import unittest
from pathlib import Path

from tools.queue_freshness import dependencies, main, missing_inputs, newer_than, recorded_catalogs, required_inputs


def _touch(path: Path, mtime: float, text: str = "x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    os.utime(path, (mtime, mtime))
    return path


class Fixture:
    """A project plus catalogs, all dated `mtime`; the queue records catalog paths in `catalog_sources`."""

    def __init__(self, root: Path, mtime: float = 100):
        self.root = root
        self.project = root / "proj"
        _touch(self.project / "build/eur/report.json", mtime)
        _touch(self.project / "config/eur/arm9/config.yaml", mtime)
        _touch(self.project / "config/eur/arm9/delinks.txt", mtime)
        _touch(self.project / "config/eur/arm9/unrelated.txt", mtime + 1000)  # not a dependency
        self.events = _touch(root / "events.json", mtime)
        self.tables = _touch(root / "tables.json", mtime)

    def queue(self, mtime: float, *, record: bool = True) -> Path:
        sources = {"event_scripts": str(self.events), "script_tables": str(self.tables)} if record else {}
        return _touch(self.root / "queue.json", mtime, json.dumps({"catalog_sources": sources}))

    def run(self, queue: Path, *extra: str) -> int:
        return main(["--queue", str(queue), "--project", str(self.project), *extra])


class DependencyTests(unittest.TestCase):
    def test_dependencies_are_inputs_plus_dsd_config_files_only(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            inputs = required_inputs(fixture.project, None, fixture.events, fixture.tables)
            names = sorted(path.name for path in dependencies(fixture.project, inputs))
            self.assertEqual(names, ["config.yaml", "delinks.txt", "events.json", "report.json", "tables.json"])

    def test_catalog_paths_are_taken_from_the_queue(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            self.assertEqual(recorded_catalogs(fixture.queue(200), fixture.project),
                             {"event_scripts": fixture.events, "script_tables": fixture.tables})
            self.assertEqual(recorded_catalogs(fixture.queue(200, record=False), fixture.project), {})

    def test_unrecorded_optional_catalogs_are_not_required_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            inputs = required_inputs(fixture.project, None, None, None)
            self.assertEqual(set(inputs), {"report", "arm9 config"})
            self.assertEqual(sorted(path.name for path in dependencies(fixture.project, inputs)),
                             ["config.yaml", "delinks.txt", "report.json"])

    def test_relative_catalog_paths_resolve_from_the_project(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            queue = _touch(fixture.root / "queue.json", 200, json.dumps({
                "catalog_sources": {
                    "event_scripts": os.path.relpath(fixture.events, fixture.project),
                    "script_tables": os.path.relpath(fixture.tables, fixture.project),
                },
            }))
            catalogs = recorded_catalogs(queue, fixture.project)
            self.assertEqual(catalogs["event_scripts"].resolve(), fixture.events)
            self.assertEqual(catalogs["script_tables"].resolve(), fixture.tables)
            self.assertEqual(fixture.run(queue), 0)

    def test_newer_dependencies_are_listed_most_recent_first(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            queue = fixture.queue(50)
            os.utime(fixture.project / "build/eur/report.json", (500, 500))
            inputs = required_inputs(fixture.project, None, fixture.events, fixture.tables)
            stale = newer_than(queue, dependencies(fixture.project, inputs))
            self.assertEqual(stale[0][0].name, "report.json")
            self.assertEqual(stale[0][1], 450)

    def test_missing_inputs_name_each_missing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            (fixture.project / "build/eur/report.json").unlink()
            inputs = required_inputs(fixture.project, None, fixture.events, fixture.tables)
            self.assertEqual(len(missing_inputs(inputs)), 1)
            self.assertTrue(missing_inputs(inputs)[0].startswith("report:"))


class ExitCodeTests(unittest.TestCase):
    def test_fresh_when_everything_is_older_than_the_queue(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            self.assertEqual(fixture.run(fixture.queue(200)), 0)

    def test_stale_when_the_report_config_or_a_catalog_is_newer(self):
        for target in ("proj/build/eur/report.json", "proj/config/eur/arm9/delinks.txt",
                       "events.json", "tables.json"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                fixture = Fixture(Path(directory))
                queue = fixture.queue(200)
                os.utime(Path(directory) / target, (300, 300))
                self.assertEqual(fixture.run(queue), 1)

    def test_missing_report_with_config_present_is_unknown_not_fresh(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            queue = fixture.queue(200)
            (fixture.project / "build/eur/report.json").unlink()
            self.assertEqual(fixture.run(queue), 2)

    def test_missing_catalog_is_unknown_even_if_everything_else_is_older(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            queue = fixture.queue(200)
            fixture.events.unlink()
            self.assertEqual(fixture.run(queue), 2)

    def test_absent_unrecorded_catalogs_do_not_make_the_queue_unknown(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            queue = fixture.queue(200, record=False)
            fixture.events.unlink()
            fixture.tables.unlink()
            self.assertEqual(fixture.run(queue), 0)
            os.utime(fixture.project / "config/eur/arm9/delinks.txt", (300, 300))
            self.assertEqual(fixture.run(queue), 1)

    def test_explicit_missing_optional_catalog_is_unknown(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            queue = fixture.queue(200, record=False)
            self.assertEqual(fixture.run(queue, "--event-catalog", str(fixture.root / "missing.json")), 2)

    def test_cli_override_replaces_the_recorded_catalog_path(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            queue = fixture.queue(200)
            newer = _touch(Path(directory) / "other-events.json", 900)
            self.assertEqual(fixture.run(queue, "--event-catalog", str(newer)), 1)

    def test_missing_queue_is_unknown(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            self.assertEqual(fixture.run(Path(directory) / "none.json"), 2)

    def _fingerprinted_queue(self, fixture, mtime):
        """A schema-2 queue whose `inputs.files` fingerprint every dependency (paths relative to the project)."""
        import hashlib
        files = []
        for path in dependencies(fixture.project, required_inputs(fixture.project, None, fixture.events, fixture.tables)):
            data = path.read_bytes()
            files.append({"path": os.path.relpath(path, fixture.project), "size": len(data),
                          "mtime_ns": 1, "sha256": hashlib.sha256(data).hexdigest()})
        sources = {"event_scripts": str(fixture.events), "script_tables": str(fixture.tables)}
        return _touch(fixture.root / "queue.json", mtime,
                      json.dumps({"catalog_sources": sources, "inputs": {"files": files}}))

    def test_a_rewrite_with_identical_bytes_keeps_the_queue_fresh_but_a_real_change_does_not(self):
        # rollback/configure rewrite config files with the same content and a new mtime
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            queue = self._fingerprinted_queue(fixture, 200)
            for name in ("config/eur/arm9/delinks.txt", "config/eur/arm9/config.yaml"):
                path = fixture.project / name
                path.write_text("x")  # same bytes as the fixture wrote
                os.utime(path, (900, 900))
            self.assertEqual(fixture.run(queue), 0)
            (fixture.project / "config/eur/arm9/delinks.txt").write_text("changed")
            os.utime(fixture.project / "config/eur/arm9/delinks.txt", (900, 900))
            self.assertEqual(fixture.run(queue), 1)

    def test_a_queue_without_fingerprints_still_judges_by_mtime(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(Path(directory))
            queue = fixture.queue(200)
            os.utime(fixture.project / "config/eur/arm9/delinks.txt", (900, 900))
            self.assertEqual(fixture.run(queue), 1)


if __name__ == "__main__":
    unittest.main()
