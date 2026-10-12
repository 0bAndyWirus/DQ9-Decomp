#!/usr/bin/env python3
"""Serialized integration runner: 20 manifests per `integrate_batch.py` call, then verification.

  integration_runner.py --list manifests.txt --out-dir build/.../run [--chunk 20]

Control files in --out-dir, checked between chunks only (a running chunk is never interrupted):
  STOP   finish the current chunk, record it, exit 4 without verification; rerun the same command
         to continue (the recorded chunks must be an exact prefix of the list).
  PAUSE  wait, checking every 30 s, until the file is removed.

A chunk that fails without per-manifest attribution is re-run as two halves, recursively (see
Runner.integrate); environment failures (lock held, pending journal, report baseline) stop the run.

After the last chunk it runs configure, `ninja -k 0`, `status.py --modules`, the unit tests and the
configured native build + ctest, and writes final.json. The native build requires generated code
and an existing build directory. Symbol-check exceptions require an explicit --baseline-config JSON
with exact `symbol_errors` and `followups` string lists. A `sha1` failure is tolerated only when
`rom_compare.py` proves that solely the secure-area/header CRC fields differ. The module gate
requires the configured count, and ctest is skipped (not counted) when the build failed.
Exit status: 0 all gates pass, 1 a gate failed, 2 bad input, 3 gates pass but some chunk failed, 4 stopped by
STOP, 5 environment failure.

Do not run it while another integration, configure or ninja is using the project workspace.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import subprocess
import sys
import time
from collections import deque
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


if __package__:
    from .host_tools import venv_program
    from .status import expected_module_count
else:
    from host_tools import venv_program
    from status import expected_module_count

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = ROOT / "workspace"
PY = venv_program(ROOT, "python")
EXIT_STOPPED = 4


def plan_chunks(manifests: Sequence[str], size: int) -> list[list[str]]:
    if size < 1:
        raise ValueError("chunk size must be positive")
    return [list(manifests[i:i + size]) for i in range(0, len(manifests), size)]


def verify_prefix(results: Sequence[dict], manifests: Sequence[str]) -> None:
    """Recorded chunks must be the exact leading part of the list, or a continuation would repeat or skip work."""
    done = [m for chunk in results for m in chunk["manifests"]]
    if done != list(manifests[:len(done)]):
        raise ValueError("recorded chunks are not an exact prefix of the manifest list")


# `rom_compare.py` exit 3 proves that only the secure-area/header CRC fields differ.
ROM_KNOWN_HEADER_ONLY = 3


@dataclass(frozen=True)
class BaselineConfig:
    symbol_errors: tuple[str, ...]
    followups: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if not isinstance(self.symbol_errors, (list, tuple)) or not self.symbol_errors:
            raise ValueError("baseline symbol_errors must be a nonempty string list")
        if not isinstance(self.followups, (list, tuple)):
            raise ValueError("baseline followups must be a string list")
        lines = (*self.symbol_errors, *self.followups)
        if any(not isinstance(line, str) or not line or line != line.strip() or "\n" in line or "\r" in line
               for line in lines):
            raise ValueError("baseline entries must be nonempty, single-line strings without surrounding whitespace")
        object.__setattr__(self, "symbol_errors", tuple(self.symbol_errors))
        object.__setattr__(self, "followups", tuple(self.followups))


def load_baseline_config(path: Path) -> BaselineConfig:
    data = json.loads(path.read_text())
    if not isinstance(data, dict) or set(data) - {"symbol_errors", "followups"}:
        raise ValueError("baseline config must contain only symbol_errors and optional followups")
    return BaselineConfig(data.get("symbol_errors"), data.get("followups", ()))




def classify_ninja(returncode: int, text: str, rom_exit: int | None = None,
                   baseline_config: BaselineConfig | None = None) -> tuple[str, list[str], list[str]]:
    """Accept only exact configured symbol diagnostics and proven header-only image differences.

    With no baseline configuration, every failed symbol check is a failure. Configured symbol errors
    and followups must match the complete diagnostic multiset; extra or missing lines are failures.
    """
    failed = re.findall(r"^FAILED: (.+?)\s*$", text, re.MULTILINE)
    errors = re.findall(r"^\[ERROR\]\s+(.+?)\s*$", text, re.MULTILINE)
    if returncode == 0:
        return ("FAIL" if failed or errors else "PASS"), failed, errors
    if not failed or not set(failed) <= {"check_symbols", "sha1"} or len(failed) != len(set(failed)):
        return "FAIL", failed, errors
    if "check_symbols" in failed:
        if baseline_config is None or Counter(errors) != Counter(
                (*baseline_config.symbol_errors, *baseline_config.followups)):
            return "FAIL", failed, errors
    if "check_symbols" not in failed and errors:
        return "FAIL", failed, errors
    if "sha1" in failed and rom_exit != ROM_KNOWN_HEADER_ONLY:
        return "FAIL", failed, errors
    return "KNOWN_BASELINE_FAIL", failed, errors


def modules_ok(returncode: int, text: str, expected_modules: int) -> bool:
    """Require one exact success line for the declared module count and an exit of zero."""
    lines = [line.strip() for line in text.splitlines() if line.startswith("modules:")]
    return (returncode == 0 and type(expected_modules) is int and expected_modules > 0
            and lines == [f"modules: {expected_modules} OK"])


def overall(final: dict, chunk_failures: Sequence[int]) -> tuple[str, int]:
    gates = [final[name]["status"] for name in
             ("configure", "ninja", "status_modules", "unittest", "cmake_build_game", "ctest_game")]
    if not all(status in ("PASS", "KNOWN_BASELINE_FAIL") for status in gates):
        return "FAIL", 1
    if chunk_failures:
        return "PASS_WITH_CHUNK_FAILURES", 3
    return ("PASS_WITH_KNOWN_BASELINE_FAIL" if "KNOWN_BASELINE_FAIL" in gates else "PASS"), 0


class FatalIntegration(RuntimeError):
    """An environment failure that retrying or splitting cannot fix; the run stops, nothing is recorded."""


_ERROR_LINE = re.compile(r"^integrate_batch\.py: (.*)$", re.MULTILINE)
FATAL_MARKERS = ("another integration holds", "pending integration journal", "workspace inconsistent",
                 "rollback failed", "objdiff report baseline", "no candidate manifests matched",
                 "pending rollback exists")
ALL_EXCLUDED = "no candidate units remain after exclusions"
EXIT_FATAL = 5


def failure_message(text: str) -> str:
    match = _ERROR_LINE.search(text)
    return match.group(1).strip() if match else ""


def failure_action(returncode: int, text: str) -> str:
    """`ok` (exit 0), `fatal` (environment problem), `final` (every manifest was individually excluded, so
    the outcome is already attributed) or `split` (rolled back without per-manifest attribution)."""
    if returncode == 0:
        return "ok"
    message = failure_message(text)
    if any(marker in message for marker in FATAL_MARKERS):
        return "fatal"
    if ALL_EXCLUDED in message:
        return "final"
    return "split"


def control_action(out_dir: Path) -> str:
    """`stop`, `pause` or `go`; STOP wins over PAUSE."""
    if (out_dir / "STOP").exists():
        return "stop"
    return "pause" if (out_dir / "PAUSE").exists() else "go"


class Runner:
    def __init__(self, manifests: Sequence[str], out_dir: Path, chunk: int, dsd: Path, wibo: Path,
                 *, project: Path = DEFAULT_PROJECT, rom: Path | None = None,
                 rebuilt_image: Path | None = None, expected_modules: int | None = None,
                 baseline_config: BaselineConfig | None = None):
        self.manifests = list(manifests)
        self.out_dir = out_dir
        self.size = chunk
        self.chunks = plan_chunks(manifests, chunk)
        self.dsd, self.wibo = dsd, wibo
        if expected_modules is not None and (type(expected_modules) is not int or expected_modules < 1):
            raise ValueError("expected modules must be a positive integer")
        self.project = project
        self.rom = rom
        self.rebuilt_image = rebuilt_image
        self.expected_modules = expected_modules
        self.baseline_config = baseline_config
        out_dir.mkdir(parents=True, exist_ok=True)
        self.results_path = out_dir / "results.json"

    def log(self, message: str) -> None:
        with (self.out_dir / "runner.log").open("a") as handle:
            handle.write(f"{dt.datetime.now().isoformat(timespec='seconds')} {message}\n")

    def run(self, command: Sequence[object], logfile: str, cwd: Path) -> tuple[int, str]:
        path = self.out_dir / logfile
        with path.open("w") as handle:
            code = subprocess.run([str(c) for c in command], cwd=cwd, stdout=handle, stderr=subprocess.STDOUT).returncode
        return code, path.read_text(errors="replace")

    def integrate(self) -> tuple[list[dict], bool]:
        """Run the remaining manifests; return (results, stopped).

        Resume is by consumed manifest count, not chunk index: the recorded chunks are an exact prefix of the
        list, the remainder is re-chunked with the current size, and chunk numbers continue from the last
        recorded one. Changing --chunk between runs therefore never repeats or skips a manifest.

        A chunk that fails without per-manifest attribution (retry limit, unisolated link/checksum failure, a
        crash) is rolled back by the integrator; it is then re-run as two halves, recursively, so one bad
        manifest cannot take a whole chunk with it. The failed attempt is kept as `chunkNNN.parentK.log` and
        never recorded; only leaf runs enter results.json, so recorded manifests stay an exact prefix.
        Environment failures (lock held, pending journal, report baseline) stop the run instead.
        """
        results = json.loads(self.results_path.read_text()) if self.results_path.exists() else []
        verify_prefix(results, self.manifests)
        consumed = sum(len(chunk["manifests"]) for chunk in results)
        queue = deque(plan_chunks(self.manifests[consumed:], self.size))
        self.log(f"{len(self.manifests)} manifests, {len(results) + len(queue)} chunks planned, {len(results)} already "
                 f"recorded ({consumed} manifests consumed), chunk size {self.size}")
        splits = 0
        while queue:
            number = len(results) + 1
            while (action := control_action(self.out_dir)) == "pause":
                self.log("PAUSE file present; waiting")
                time.sleep(30)
            if action == "stop":
                self.log(f"STOP file present; stopping before chunk {number}")
                return results, True
            chunk = queue.popleft()
            started = time.time()
            command = [PY, ROOT / "tools" / "integrate_batch.py", *chunk, "--max-retries", "12",
                       "--dsd", self.dsd, "-w", self.wibo, "--project", self.project]
            logname = f"chunk{number:03d}.log"
            code, text = self.run(command, logname, ROOT)
            seconds = round(time.time() - started, 1)
            action = failure_action(code, text)
            if action == "fatal":
                raise FatalIntegration(f"chunk {number} stopped on an environment failure: {failure_message(text)}")
            if action == "split" and len(chunk) > 1:
                splits += 1
                (self.out_dir / logname).replace(self.out_dir / f"chunk{number:03d}.parent{splits}.log")
                middle = len(chunk) // 2
                queue.extendleft([chunk[middle:], chunk[:middle]])
                self.log(f"chunk {number} ({len(chunk)} manifests) failed without attribution after {seconds}s: "
                         f"{failure_message(text)[:100]}; splitting into {middle} + {len(chunk) - middle}")
                continue
            results.append({"chunk": number, "manifests": chunk, "exit": code, "seconds": seconds})
            self.results_path.write_text(json.dumps(results, indent=1))
            self.log(f"chunk {number} exit {code} {seconds}s ({len(queue)} queued)")
        if control_action(self.out_dir) == "stop":  # requested while the last chunk ran: do not start verification
            self.log("STOP file present after the last chunk; skipping verification")
            return results, True
        return results, False

    def verify(self, results: Sequence[dict]) -> int:
        if self.rom is None or self.rebuilt_image is None:
            raise ValueError("verification requires reference and rebuilt image paths")
        expected_modules = (self.expected_modules if self.expected_modules is not None
                            else expected_module_count(self.project))
        failures = [r["chunk"] for r in results if r["exit"] != 0]
        final: dict = {"chunks": len(results), "chunks_nonzero": failures}

        def gate(name: str, code: int, ok: bool, **extra) -> None:
            final[name] = {"exit": code, "status": "PASS" if ok else "FAIL", **extra}

        code, _ = self.run([PY, self.project / "tools" / "configure.py", "eur", "--dsd", self.dsd, "-w", self.wibo],
                           "configure.log", self.project)
        gate("configure", code, code == 0)
        if final["configure"]["status"] == "PASS":
            code, text = self.run(["ninja", "-k", "0"], "ninja.log", self.project)
            rom_exit = None
            if "sha1" in re.findall(r"^FAILED: (.+?)\s*$", text, re.MULTILINE):
                rom_exit, _ = self.run([PY, ROOT / "tools" / "rom_compare.py", self.rom,
                                        self.rebuilt_image], "rom_compare.log", ROOT)
            status, failed, errors = classify_ninja(code, text, rom_exit, self.baseline_config)
            image = self.project / "build" / "eur" / "build" / "arm9.bin"
            final["ninja"] = {"exit": code, "status": status, "failed_targets": failed, "errors": errors[:5],
                              "rom_compare_exit": rom_exit, "arm9_image_present": image.exists()}
            code, text = self.run([PY, ROOT / "tools" / "status.py", "--modules", "--project", self.project,
                                   "--expected-modules", expected_modules], "status_modules.log", ROOT)
            gate("status_modules", code, modules_ok(code, text, expected_modules), expected_modules=expected_modules,
                 line=next((l for l in text.splitlines() if l.startswith("modules:")), None))
            code, _ = self.run([PY, "-m", "unittest", "discover", "-s", "tests"], "unittest.log", ROOT)
            gate("unittest", code, code == 0)
            code, _ = self.run([venv_program(ROOT, "cmake"), "--build", "build/game", "-j", "12"],
                               "cmake_build_game.log", ROOT)
            gate("cmake_build_game", code, code == 0)
            if code == 0:
                code, _ = self.run([venv_program(ROOT, "ctest"), "--test-dir", "build/game"], "ctest_game.log", ROOT)
                gate("ctest_game", code, code == 0)
            else:
                final["ctest_game"] = {"exit": None, "status": "SKIPPED_BUILD_FAILED"}
        else:
            for name in ("ninja", "status_modules", "unittest", "cmake_build_game", "ctest_game"):
                final[name] = {"exit": None, "status": "SKIPPED_CONFIGURE_FAILED"}
        final["overall"], status_code = overall(final, failures)
        (self.out_dir / "final.json").write_text(json.dumps(final, indent=1))
        self.log(f"DONE overall={final['overall']}")
        return status_code


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", type=Path, required=True, help="file with project-relative manifest paths, one per line")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--chunk", type=int, default=20)
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT, help="configured project workspace")
    parser.add_argument("--dsd", type=Path, help="default: <project>/dsd-0.12.1-linux-x86_64")
    parser.add_argument("--wibo", type=Path, help="default: <project>/wibo")
    parser.add_argument("--rom", type=Path, help="already-local reference image, required for verification")
    parser.add_argument("--rebuilt-image", type=Path, help="rebuilt image, required for verification")
    parser.add_argument("--baseline-config", type=Path, help="JSON with exact symbol_errors and followups lists")
    parser.add_argument("--expected-modules", type=int, help="default: count modules in the project ARM9 config")
    parser.add_argument("--no-verify", action="store_true", help="integrate only; skip the final gates")
    args = parser.parse_args(argv)
    try:
        if not args.no_verify and (args.rom is None or args.rebuilt_image is None):
            raise ValueError("--rom and --rebuilt-image are required unless --no-verify is set")
        project = args.project.resolve()
        baseline = load_baseline_config(args.baseline_config) if args.baseline_config else None
        manifests = [line.strip() for line in args.list.read_text().splitlines() if line.strip()]
        runner = Runner(manifests, args.out_dir, args.chunk,
                        (args.dsd or project / "dsd-0.12.1-linux-x86_64").resolve(),
                        (args.wibo or project / "wibo").resolve(), project=project,
                        rom=args.rom.resolve() if args.rom else None,
                        rebuilt_image=args.rebuilt_image.resolve() if args.rebuilt_image else None,
                        expected_modules=args.expected_modules, baseline_config=baseline)
        results, stopped = runner.integrate()
        return EXIT_STOPPED if stopped else (0 if args.no_verify else runner.verify(results))
    except FatalIntegration as error:
        print(f"integration_runner.py: {error}", file=sys.stderr)
        return EXIT_FATAL
    except (OSError, ValueError) as error:
        print(f"integration_runner.py: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
