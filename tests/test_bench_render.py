"""ROM-free checks for benchmark affinity and fresh-save handling."""
import argparse
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import bench_render


class BenchRenderTests(unittest.TestCase):
    def test_affinity_and_fresh_save(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source.sav"
            source.write_bytes(b"fresh save")
            calls = []

            def run(command, **kwargs):
                calls.append(command)
                save = Path(command[command.index("--save") + 1])
                self.assertEqual(save.read_bytes(), b"fresh save")
                save.write_bytes(b"guest modified save")
                self.assertEqual(command[command.index("--frames") + 1], "37")
                self.assertEqual(command[command.index("--rom") + 1], "invented.nds")
                self.assertEqual(command[command.index("--input-script") + 1], "invented.txt")
                self.assertEqual(command[command.index("--rtc-epoch") + 1], "0")
                self.assertEqual(command[command.index("--rtc-timezone-offset") + 1], "0")
                self.assertTrue(kwargs["check"])
                return subprocess.CompletedProcess(command, 0)

            with patch.object(bench_render.subprocess, "run", side_effect=run), \
                    patch.object(bench_render.time, "perf_counter", side_effect=[0, 2, 2, 4, 4, 6]):
                for cpu in (5, -1, None):
                    self.assertEqual(bench_render.run(
                        Path("dq9"), 37, cpu, image=Path("invented.nds"),
                        input_script=Path("invented.txt"), save=source), 2)
            self.assertEqual(calls[0][:4], ["taskset", "-c", "5", "dq9"])
            self.assertEqual(calls[1][0], "dq9")
            self.assertEqual(calls[2][0], "dq9")
            self.assertEqual(source.read_bytes(), b"fresh save")

    def test_scene_parser_uses_explicit_invented_values(self):
        self.assertEqual(bench_render.scene_range("sample:7:37"), ("sample", 7, 37))
        for value in ("sample", ":0:8", "sample:-1:8", "sample:8:8",
                      "sample:9:8", "sample:x:8", "sample:0:8:9"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                bench_render.scene_range(value)

    def test_cli_requires_all_local_inputs_and_a_scene(self):
        required = ["--image", "invented.nds", "--save", "invented.sav",
                    "--input-script", "invented.txt", "--scene", "sample:7:37"]
        for index in range(0, len(required), 2):
            with self.subTest(option=required[index]), \
                    patch("sys.stderr", new_callable=io.StringIO), \
                    self.assertRaises(SystemExit) as raised:
                bench_render.main(required[:index] + required[index + 2:])
            self.assertEqual(raised.exception.code, 2)

    def test_cli_times_only_caller_defined_ranges(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            paths = [directory / name for name in ("binary", "image", "save", "input")]
            for path in paths:
                path.touch()
            binary, image, save, script = paths
            output = directory / "results.txt"
            with patch.object(bench_render, "run", side_effect=[4.0, 1.0, 2.0]) as run, \
                    patch("sys.stdout", new_callable=io.StringIO):
                result = bench_render.main([
                    "--binary", str(binary), "--image", str(image), "--save", str(save),
                    "--input-script", str(script), "--scene", "sample:7:37",
                    "--scene", "other:0:14", "--cpu", "-1", "--out", str(output)])
            self.assertEqual(result, 0)
            self.assertEqual([call.args for call in run.call_args_list],
                             [(binary, 37, -1), (binary, 7, -1), (binary, 14, -1)])
            for call in run.call_args_list:
                self.assertEqual(call.kwargs, {"image": image, "save": save, "input_script": script})
            text = output.read_text()
            self.assertIn("sample", text)
            self.assertIn("other", text)
            self.assertIn("10.0", text)
            self.assertIn("7.0", text)

    def test_missing_local_input_has_clear_error(self):
        with patch("sys.stderr", new_callable=io.StringIO) as error:
            result = bench_render.main([
                "--binary", "/missing/invented-binary", "--image", "invented.nds",
                "--save", "invented.sav", "--input-script", "invented.txt",
                "--scene", "sample:7:37"])
        self.assertEqual(result, 2)
        self.assertIn("missing binary", error.getvalue())


if __name__ == "__main__":
    unittest.main()
