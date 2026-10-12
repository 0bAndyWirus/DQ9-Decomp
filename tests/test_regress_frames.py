import contextlib
import io
import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from tools.regress_frames import _load_stages, main, run_regression


def _ppm(width: int, height: int, pixels: bytes) -> bytes:
    return b"P6\n%d %d\n255\n" % (width, height) + pixels


def _write_capture(directory: Path, frame: int, screen: str, pixels: bytes) -> None:
    """Encode 2x2 RGB pixels; PNG for oracle captures, PPM for native dumps."""
    path = directory / f"frame_{frame:04d}_{screen}.png" if directory.name == "oracle" else directory / f"frame_{frame:04d}_{screen}.ppm"
    if path.suffix == ".png":
        raw = b"".join(b"\0" + pixels[y * 6:(y + 1) * 6] for y in range(2))

        def chunk(kind, payload):
            return struct.pack(">I", len(payload)) + kind + payload + struct.pack(
                ">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

        ihdr = struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)
        path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                         + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    else:
        path.write_bytes(_ppm(2, 2, pixels))


class Runner:
    """Fake dq9 invocation: emits a fixed 2x2 dump per screen at every checkpoint."""

    def __init__(self, top=b"\x00\x00\x00" * 4, bottom=b"\xff\xff\xff" * 4):
        self.top = top
        self.bottom = bottom
        self.commands = []

    def __call__(self, config, dump_dir):
        self.commands.append((config, dump_dir))
        for stage in config["stages"]:
            _write_capture(dump_dir, stage["frame"], "top", self.top)
            _write_capture(dump_dir, stage["frame"], "bottom", self.bottom)


class RegressFramesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.capture = self.directory / "oracle"
        self.capture.mkdir()
        self.stages = self.directory / "stages.json"
        self.config = {
            "native": "native/dq9",
            "rom": "ROM/game.nds",
            "input_script": "inputs.txt",
            "oracle_dir": str(self.capture),
            "stages": [
                {"name": "exact", "frame": 1050, "top_tolerance": 0, "bottom_tolerance": 0},
                {"name": "loose", "frame": 1300, "top_tolerance": 100, "bottom_tolerance": 100},
            ],
        }
        for frame in (1050, 1300):
            _write_capture(self.capture, frame, "top", b"\x10\x10\x10" * 4)
            _write_capture(self.capture, frame, "bottom", b"\x10\x00\x00" * 4)

    def tearDown(self):
        self.temporary.cleanup()

    def _stage_file(self, **overrides) -> Path:
        for stage in self.config["stages"]:
            stage.update(overrides)
        self.stages.write_text(json.dumps(self.config))
        return self.stages

    def _run(self, arguments, runner=None):
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            status = main(arguments, runner=runner)
        return status, buffer.getvalue()

    def test_stage_file_requires_keys(self):
        file = self.directory / "broken.json"
        file.write_text('{"native": "x"}')
        with self.assertRaises(ValueError):
            _load_stages(file)
        file.write_text('{"native": "x", "rom": "y", "input_script": "z", "oracle_dir": "o", "stages": []}')
        with self.assertRaises(ValueError):
            _load_stages(file)

    def test_runner_captures_stage_differences(self):
        runner = Runner(top=b"\x10\x10\x10" * 4, bottom=b"\x10\x00\x00" * 4)
        results = run_regression(self.config, runner)
        self.assertEqual(results["exact"], {"top": 0, "bottom": 0})
        self.assertEqual(runner.commands[0][0], self.config)

    def test_pass_and_fail_against_tolerances(self):
        self._stage_file(top_tolerance=0, bottom_tolerance=0)  # identical: pass both stages
        runner = Runner(top=b"\x10\x10\x10" * 4, bottom=b"\x10\x00\x00" * 4)
        status, output = self._run([str(self.stages)], runner=runner)
        self.assertEqual(status, 0)
        self.assertIn("exact", output)
        self.assertIn("loose", output)
        self.assertIn("OK", output)

        self._stage_file(top_tolerance=0)  # black dumps vs \x10 oracle: fail
        status, output = self._run([str(self.stages)], runner=Runner())
        self.assertEqual(status, 1)

    def test_update_tolerances_records_baseline(self):
        stages = self._stage_file(top_tolerance=999, bottom_tolerance=999)
        runner = Runner(top=b"\x10\x10\x10" * 4, bottom=b"\x10\x00\x00" * 4)
        status, output = self._run(["--update-tolerances", str(stages)], runner=runner)
        self.assertEqual(status, 0)
        self.assertIn("exact: recorded top 0, bottom 0", output)
        saved = json.loads(stages.read_text())
        self.assertEqual(saved["stages"][0]["top_tolerance"], 0)
        self.assertEqual(saved["stages"][0]["bottom_tolerance"], 0)
        self.assertEqual(saved["stages"][1]["top_tolerance"], 0)



    def test_stage_schema_rejects_wrong_types_and_duplicate_names(self):
        for mutate, message in (
            (lambda cfg: cfg["stages"][0].update(frame="1050"), "frame must be a non-negative integer"),
            (lambda cfg: cfg["stages"][0].update(top_tolerance=True), "top_tolerance must be a non-negative integer"),
            (lambda cfg: cfg["stages"][1].update(name="exact"), "duplicate stage name"),
            (lambda cfg: cfg["stages"].__setitem__(1, None), "must be an object"),
        ):
            with self.subTest(message=message):
                config = json.loads(json.dumps(self.config))
                mutate(config)
                self.stages.write_text(json.dumps(config))
                with self.assertRaisesRegex(ValueError, message):
                    _load_stages(self.stages)

    def test_configured_dump_dir_is_used_and_retained(self):
        dump_dir = self.directory / "retained-dumps"
        self.config["dump_dir"] = str(dump_dir)
        runner = Runner(top=b"\x10\x10\x10" * 4, bottom=b"\x10\x00\x00" * 4)
        run_regression(self.config, runner)
        self.assertEqual(runner.commands[0][1], dump_dir)
        self.assertTrue((dump_dir / "frame_1050_top.ppm").is_file())

    def test_tolerance_updates_report_changes(self):
        stages = self._stage_file(top_tolerance=15, bottom_tolerance=20)
        runner = Runner(top=b"\x10\x10\x10" * 4, bottom=b"\x10\x00\x00" * 4)
        status, output = self._run(["--update-tolerances", str(stages)], runner=runner)
        self.assertEqual(status, 0)
        self.assertIn("changed top tolerance 15 -> 0", output)
if __name__ == "__main__":
    unittest.main()
