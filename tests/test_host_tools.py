"""ROM-free tests of the Python tools' host-platform boundaries."""
import ctypes
import json
from pathlib import Path, PureWindowsPath
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from tools import host_tools, integration_ledger


class HostToolTests(unittest.TestCase):
    def test_virtualenv_and_executable_paths(self):
        root = Path("checkout")
        self.assertEqual(host_tools.venv_program(root, "python", windows=False),
                         root / ".venv/bin/python")
        windows_root = PureWindowsPath("C:/Games/DQ9")
        self.assertEqual(host_tools.venv_program(windows_root, "python", windows=True),
                         windows_root / ".venv/Scripts/python.exe")
        self.assertEqual(host_tools.venv_program(windows_root, "cmake", windows=True),
                         windows_root / ".venv/Scripts/cmake.exe")
        self.assertEqual(host_tools.executable_name("dq9", windows=True), "dq9.exe")
        self.assertEqual(host_tools.executable_name("dq9.EXE", windows=True), "dq9.EXE")
        self.assertEqual(host_tools.executable_name("dq9", windows=False), "dq9")

    def test_worktree_data_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            worktree = root / "build/wt-Port"
            worktree.mkdir(parents=True)
            (root / "ROM").mkdir()
            self.assertEqual(host_tools.data_root(root), root)
            self.assertEqual(host_tools.data_root(worktree), root)
            (worktree / "ROM").mkdir()
            self.assertEqual(host_tools.data_root(worktree), worktree)

    def test_exclusive_file_lock_and_release(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "integration.lock"
            with path.open("a+") as first, path.open("a+") as second:
                host_tools.lock_file(first)
                with self.assertRaises(OSError):
                    host_tools.lock_file(second)
                host_tools.lock_file(first, unlock=True)
                host_tools.lock_file(second)
                host_tools.lock_file(second, unlock=True)

    def test_windows_lock_uses_fixed_byte_region(self):
        windows_lock = mock.Mock()
        module = SimpleNamespace(locking=windows_lock, LK_NBLCK=2, LK_UNLCK=0)
        handle = mock.Mock()
        handle.fileno.return_value = 7
        with mock.patch.dict("sys.modules", msvcrt=module), mock.patch.object(host_tools.os, "name", "nt"):
            host_tools.lock_file(handle)
            host_tools.lock_file(handle, unlock=True)
        self.assertEqual(windows_lock.call_args_list, [mock.call(7, 2, 1), mock.call(7, 0, 1)])
        self.assertEqual(handle.seek.call_args_list, [mock.call(0), mock.call(0)])

    def test_linux_affinity_preserves_taskset_command(self):
        command = ["dq9", "--rom", "path with spaces.nds"]
        with mock.patch.object(host_tools.os, "name", "posix"), \
                mock.patch.object(host_tools.subprocess, "run") as run:
            host_tools.run_with_affinity(command, 5, check=True)
            self.assertEqual(run.call_args.args[0], ["taskset", "-c", "5", *command])
            host_tools.run_with_affinity(command, -1, check=True)
            self.assertEqual(run.call_args.args[0], command)
        self.assertEqual(command[0], "dq9")

    def test_windows_affinity_avoids_command_shell(self):
        command = ["C:/DQ9/dq9.exe", "--rom", "C:/ROM/a&b%game.nds"]
        process = mock.MagicMock(returncode=0)
        process.__enter__.return_value = process
        process.communicate.return_value = (b"out", b"err")
        with mock.patch.object(host_tools.os, "name", "nt"), \
                mock.patch.object(host_tools.subprocess, "Popen", return_value=process) as spawn, \
                mock.patch.object(host_tools, "_set_windows_affinity") as pin:
            result = host_tools.run_with_affinity(command, 5, check=True, stdout=subprocess.PIPE)
        spawn.assert_called_once_with(command, stdout=subprocess.PIPE)
        pin.assert_called_once_with(process, 5)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"out", b"err"))

    def test_windows_affinity_failure_reaps_child(self):
        process = mock.MagicMock()
        process.__enter__.return_value = process
        with mock.patch.object(host_tools.os, "name", "nt"), \
                mock.patch.object(host_tools.subprocess, "Popen", return_value=process), \
                mock.patch.object(host_tools, "_set_windows_affinity", side_effect=OSError("pin failed")):
            with self.assertRaisesRegex(OSError, "pin failed"):
                host_tools.run_with_affinity(["dq9.exe"], 5)
        process.kill.assert_called_once()
        process.wait.assert_called_once()
        process.communicate.assert_not_called()

    def test_windows_affinity_mask_is_pointer_sized(self):
        kernel = mock.Mock()
        kernel.SetProcessAffinityMask.return_value = 1
        with mock.patch.object(ctypes, "WinDLL", create=True, return_value=kernel):
            host_tools._set_windows_affinity(SimpleNamespace(_handle=123), 5)
        handle, mask = kernel.SetProcessAffinityMask.call_args.args
        self.assertEqual((handle.value, mask.value), (123, 1 << 5))
        self.assertEqual(kernel.SetProcessAffinityMask.argtypes[1], ctypes.c_size_t)
        with self.assertRaises(ValueError):
            host_tools._set_windows_affinity(SimpleNamespace(_handle=123), ctypes.sizeof(ctypes.c_size_t) * 8)

    def test_windows_process_discovery_handles_single_and_multiple_records(self):
        wanted = {"ProcessId": 42, "CommandLine": "python tools/integrate_batch.py author.json"}
        records = [wanted, {"ProcessId": 43, "CommandLine": None},
                   {"ProcessId": 44, "CommandLine": "python integration_ledger.py integrate_batch.py"}]
        for payload in (wanted, records, []):
            result = SimpleNamespace(stdout=json.dumps(payload))
            with mock.patch.object(integration_ledger.os, "name", "nt"), \
                    mock.patch.object(integration_ledger.subprocess, "run", return_value=result) as run:
                observed = integration_ledger.running_processes()
            expected = [] if payload == [] else [(42, wanted["CommandLine"])]
            self.assertEqual(observed, expected)
            self.assertEqual(run.call_args.args[0][0], "powershell.exe")
            self.assertEqual(run.call_args.kwargs["encoding"], "utf-8")
            self.assertTrue(run.call_args.kwargs["check"])


if __name__ == "__main__":
    unittest.main()
