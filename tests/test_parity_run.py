import contextlib
import io
import stat
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from tools.parity_run import main


def _png(pixels: bytes) -> bytes:
    raw = b"".join(b"\0" + pixels[y * 6:(y + 1) * 6] for y in range(2))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(
            ">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


# Fake dq9: parses --dump-dir/--dump-frames, writes one gray top dump (2x2, value 0x10) and
# a bottom dump whose last pixel is 0x20, or exits 7 when a `<stub>.fail` file exists.
STUB = r"""#!/bin/sh
[ -e "$0.fail" ] && exit 7
[ -e "$0.nocapture" ] && exit 0
while [ $# -gt 0 ]; do
  case "$1" in
    --dump-dir) dir="$2"; shift ;;
    --dump-frames) frames="$2"; shift ;;
  esac
  shift
done
for f in $(echo "$frames" | tr ',' ' '); do
  n=$(printf '%04d' "$f")
  printf 'P6\n2 2\n255\n\020\020\020\020\020\020\020\020\020\020\020\020' > "$dir/frame_${n}_top.ppm"
  printf 'P6\n2 2\n255\n\020\020\020\020\020\020\020\020\020\040\040\040' > "$dir/frame_${n}_bottom.ppm"
done
"""


class ParityRunTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.stub = self.directory / "dq9"
        self.stub.write_text(STUB)
        self.stub.chmod(self.stub.stat().st_mode | stat.S_IXUSR)
        self.oracle = self.directory / "oracle"
        self.oracle.mkdir()
        self.script = self.directory / "input.txt"
        self.script.write_text("0 0 -1 -1\n")
        # frame 10 has an oracle (identical gray); frame 20 has none.
        (self.oracle / "frame_0010_top.png").write_bytes(_png(b"\x10" * 12))
        (self.oracle / "frame_0010_bottom.png").write_bytes(_png(b"\x10" * 12))

    def tearDown(self):
        self.temporary.cleanup()

    def _run(self, *extra):
        out = self.directory / "out"
        buffer, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(buffer), contextlib.redirect_stderr(errors):
            status = main([
                "--native", str(self.stub), "--rom", "game.nds", "--native-script", str(self.script),
                "--frames", "21", "--dump-frames", "10,20", "--out", str(out), *extra,
            ])
        return status, buffer.getvalue(), errors.getvalue()

    def test_reports_counts_per_frame_and_screen(self):
        status, output, _ = self._run("--oracle-dir", str(self.oracle))
        self.assertEqual(status, 0)
        lines = output.splitlines()
        self.assertEqual(lines[0], "frame 10 top: 0/4 maxdelta 0 bbox None")
        self.assertEqual(lines[1], "frame 10 bottom: 1/4 maxdelta 16 bbox (1, 1, 1, 1)")
        self.assertEqual(lines[2], "frame 20 top: no oracle")
        self.assertEqual(lines[3], "frame 20 bottom: no oracle")

    def test_native_failure_exits_two(self):
        Path(str(self.stub) + ".fail").write_text("")
        status, output, errors = self._run("--oracle-dir", str(self.oracle))
        self.assertEqual(status, 2)
        self.assertEqual(output, "")
        self.assertIn("dq9 exited 7", errors)

    def test_reused_output_directory_does_not_report_stale_dumps(self):
        status, output, _ = self._run("--oracle-dir", str(self.oracle))
        self.assertEqual(status, 0)
        self.assertIn("frame 10 top: 0/4", output)
        Path(str(self.stub) + ".nocapture").write_text("")
        status, output, _ = self._run("--oracle-dir", str(self.oracle))
        self.assertEqual(status, 0)
        self.assertEqual(output.splitlines()[0], "frame 10 top: no native dump")
        self.assertEqual(output.splitlines()[1], "frame 10 bottom: no native dump")


    def test_strict_mode_fails_for_missing_captures_and_reports_summary(self):
        status, output, _ = self._run("--oracle-dir", str(self.oracle), "--strict")
        self.assertEqual(status, 1)
        self.assertIn("missing captures: native 0 [none]; oracle 2 [20/top, 20/bottom]", output)

    def test_missing_native_capture_is_reported_and_strict_fails(self):
        Path(str(self.stub) + ".nocapture").write_text("")
        status, output, _ = self._run("--oracle-dir", str(self.oracle), "--strict")
        self.assertEqual(status, 1)
        self.assertIn("missing captures: native 4 [10/top, 10/bottom, 20/top, 20/bottom]", output)

    def test_frame_list_rejects_empty_negative_and_duplicate_tokens(self):
        for frames in ("", "1,,2", "-1", "1,-2", "1,1"):
            with self.subTest(frames=frames), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    main(["--native-script", str(self.script), "--frames", "2", "--dump-frames", frames,
                          "--out", str(self.directory / "out"), "--native", str(self.stub), "--rom", "x"])
            self.assertEqual(raised.exception.code, 2)

if __name__ == "__main__":
    unittest.main()
