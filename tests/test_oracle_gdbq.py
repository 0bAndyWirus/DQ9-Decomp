import argparse
import contextlib
import io
import tempfile
import unittest
from subprocess import TimeoutExpired

from tools.oracle import gdbq

# Protocol parsing uses invented addresses rather than a title's memory layout.


class FakeProcess:
    def __init__(self, exited=False):
        self.pid = 123
        self.returncode = 0 if exited else None
        self.waits = 0

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        self.waits += 1
        self.returncode = 0
        return 0


class OracleGdbqTests(unittest.TestCase):
    def test_argument_helpers_parse_integer_and_memory_ranges(self):
        self.assertEqual(gdbq.integer("0x20"), 32)
        self.assertEqual(gdbq.parse_memory("0x08000000:16"), (0x08000000, 16))
        with self.assertRaises(argparse.ArgumentTypeError):
            gdbq.parse_memory("bad")

    def test_packet_checksum_is_rfc_serialized(self):
        self.assertEqual(gdbq.checksum(b"g"), ord("g"))
        self.assertEqual(gdbq.packet(b"g"), b"$g#67")

    def test_help_argument_exits_before_emulator_start(self):
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                gdbq.main(["--help"])
        self.assertEqual(raised.exception.code, 0)

    def test_cleanup_does_not_signal_a_confirmed_exit(self):
        process = FakeProcess(exited=True)
        calls = []
        gdbq.cleanup_process(process, kill_group=lambda *args: calls.append(args))
        self.assertEqual(calls, [])
        self.assertEqual(process.waits, 0)

    def test_cleanup_terminates_a_live_process_group_once(self):
        process = FakeProcess()
        calls = []
        gdbq.cleanup_process(process, kill_group=lambda *args: calls.append(args))
        self.assertEqual(calls, [(123, gdbq.signal.SIGTERM)])
        self.assertEqual(process.returncode, 0)
    def test_cleanup_escalates_only_after_timeout(self):
        class StubbornProcess(FakeProcess):
            def wait(self, timeout=None):
                self.waits += 1
                if timeout is not None:
                    raise TimeoutExpired("emulator", timeout)
                self.returncode = -9
                return self.returncode

        process = StubbornProcess()
        signals = []

        def kill_group(pid, signum):
            signals.append(signum)

        gdbq.cleanup_process(process, kill_group=kill_group)
        self.assertEqual(signals, [gdbq.signal.SIGTERM, gdbq.signal.SIGKILL])
        self.assertEqual(process.returncode, -9)


    def test_log_tail_is_bounded(self):
        with tempfile.TemporaryFile() as handle:
            handle.write(b"0123456789")
            self.assertEqual(gdbq.log_tail(handle, 4), "6789")


if __name__ == "__main__":
    unittest.main()
