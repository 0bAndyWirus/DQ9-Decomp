#!/usr/bin/env python3
"""Build a local SDL host from an image you already own.

Everything this script produces is derived from your image. It stays under
`build/` and `extracted/` (both git-ignored) and must never be uploaded or shared.

Steps:
  1. optionally verify the image against a private fingerprint profile;
  2. extract the image and initialise the analysis workspace with ds-decomp (`dsd`);
  3. build the ARM9 manifest and resolve protected overlay code from your own image;
  4. discover ARM7 code from the extracted ARM7 binary;
  5. derive the thread-layer configuration `build/gen/abi.ini` from your own binaries;
  6. recompile both CPUs to host C++ (python -m recomp);
  7. configure and build the `dq9` executable (unless --generate-only).

Usage:
  python tools/build_port.py --rom PATH [--dsd PATH]
  python tools/build_port.py --runtime-only
  build/game/game/dq9 --rom PATH --abi-config build/gen/abi.ini [--language en|fr|de|es|it]

See docs/GETTING_STARTED.md for the verified status.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import importlib
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

try:
    from . import preflight
    from .host_tools import executable_name
except ImportError:
    import preflight
    from host_tools import executable_name

ROOT = Path(__file__).resolve().parent.parent
WORKSPACE = ROOT / "extracted" / "workspace"
GEN = ROOT / "build" / "gen"
DSD_RELEASE = "https://github.com/AetiasHax/ds-decomp/releases/download/v0.12.1"
DSD_SHA256 = {
    "dsd-linux-x86_64": "256edde804dd8e5d7bc4c0e4187f85244ee4cd1f42bb427502e01fda6af43ceb",
    "dsd-windows-x86_64.exe": "5917df26bffc05d4752276a2e0b1edd21bd6e920aabe1992be8a50efc9d10932",
}
# PyPI publishes no Windows wheel for ziglang 0.17.0; 0.16.0 is the newest release with a
# win_amd64 wheel. (Linux-hosted cross builds via tools/build_windows.sh use 0.17.0.)
ZIG_WINDOWS_REQUIREMENT = "ziglang==0.16.0"
BACKUP_TYPE_CHOICES = ("auto", "eeprom512b", "eeprom8k", "eeprom64k", "eeprom128k",
                       "flash256k", "flash512k", "flash1m", "flash8m")  # keep in sync with tools/derive_abi_config.py
WORKSPACE_MARKER = "rom.sha256"  # local-only fingerprint of the image the workspace was extracted from
SDL_VERSION = "2.30.12"
# Official release-2.30.12 MinGW SDK; verify before extracting executable code.
SDL_SHA256 = "dddafbf0705a8cbe1f97c69680886ce147ed4d38f1d04e03523681298e081b0f"
SDL_URL = (
    "https://github.com/libsdl-org/SDL/releases/download/"
    f"release-{SDL_VERSION}/SDL2-devel-{SDL_VERSION}-mingw.tar.gz"
)


def tool_environment(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = dict(os.environ)
    env["PATH"] = str(Path(sys.executable).parent) + os.pathsep + env.get("PATH", "")
    if os.name == "nt":
        env["DQ9_ZIG_PYTHON"] = sys.executable
    if extra:
        env.update(extra)
    return env


# Set by --quiet: command output goes to a log file and only step headers, summaries,
# a compile progress line and failures are shown.
_LOG: Path | None = None
_STEP = 0
_STEP_TOTAL = 0
_NINJA_PROGRESS = re.compile(r"^\[(\d+)/(\d+)\]")


def begin_steps(total: int) -> None:
    global _STEP, _STEP_TOTAL
    _STEP, _STEP_TOTAL = 0, total


def step(title: str) -> None:
    global _STEP
    _STEP += 1
    print(f"\n[{_STEP}/{_STEP_TOTAL}] {title}", flush=True)


def progress_bar(done: int, total: int, width: int = 30) -> str:
    """ASCII only: a legacy Windows console code page cannot print block characters."""
    filled = min(width, max(0, done * width // max(total, 1)))
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def banner(title: str, *lines: str) -> None:
    width = max(len(title), *(len(line) for line in lines), 56) + 4
    print("\n" + "=" * width)
    print(f"  {title}")
    for line in lines:
        print(f"  {line}")
    print("=" * width, flush=True)


def configure_log() -> Path:
    global _LOG
    _LOG = ROOT / "build" / "build-log.txt"
    _LOG.parent.mkdir(parents=True, exist_ok=True)
    _LOG.write_text("DQ9-Decomp build log (local file, may contain local paths; do not share publicly)\n", encoding="utf-8")
    return _LOG


def run(*cmd: object, cwd: Path = ROOT, env: dict[str, str] | None = None,
        summary: bool = False) -> None:
    args = [str(c) for c in cmd]
    progress = "--build" in args  # cmake --build: show ninja's [n/total] counter
    if _LOG is None:
        print("+ " + " ".join(args), flush=True)
        subprocess.run(args, cwd=cwd, env=tool_environment(env), check=True)
        return
    tail: collections.deque[str] = collections.deque(maxlen=40)
    last_text = ""
    last_shown = -1
    tty = sys.stdout.isatty()
    with _LOG.open("a", encoding="utf-8", errors="replace") as log:
        log.write("\n$ " + " ".join(args) + "\n")
        log.flush()
        proc = subprocess.Popen(args, cwd=cwd, env=tool_environment(env), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
        assert proc.stdout is not None
        for line in proc.stdout:
            log.write(line)
            text = line.rstrip("\r\n")
            tail.append(text)
            if text.strip():
                last_text = text.strip()
            match = _NINJA_PROGRESS.match(line) if progress else None
            if match:
                done, total = int(match.group(1)), int(match.group(2))
                percent = done * 100 // max(total, 1)
                bar = progress_bar(done, total)
                if tty:
                    print(f"\r    {bar} {percent:3d}%  {done}/{total}   ", end="", flush=True)
                elif percent // 10 != last_shown // 10:
                    print(f"    {bar} {percent:3d}%", flush=True)
                last_shown = percent
        code = proc.wait()
    if last_shown >= 0 and tty:
        print(flush=True)
    if code:
        print(f"\n    command failed (exit {code}). Last output:", file=sys.stderr)
        for text in tail:
            print("      " + text, file=sys.stderr)
        print(f"    Full log: {_LOG}", file=sys.stderr, flush=True)
        raise subprocess.CalledProcessError(code, args)
    if summary and last_text:
        print("    " + last_text, flush=True)


def fetch_dsd() -> Path:
    if os.name == "nt":
        asset = "dsd-windows-x86_64.exe"
    elif sys.platform.startswith("linux") and platform.machine().lower() in ("x86_64", "amd64"):
        asset = "dsd-linux-x86_64"
    else:
        raise SystemExit("Automatic dsd download supports Linux/Windows x64; supply --dsd PATH.")
    destination = ROOT / "build" / "tools" / asset
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists():
        print(f"Downloading {DSD_RELEASE}/{asset}", flush=True)
        temporary = destination.with_suffix(destination.suffix + ".download")
        try:
            urllib.request.urlretrieve(f"{DSD_RELEASE}/{asset}", temporary)
            verify_dsd(temporary, asset)
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
    verify_dsd(destination, asset)
    if os.name != "nt":
        destination.chmod(destination.stat().st_mode | 0o111)
    return destination


def verify_dsd(path: Path, asset: str) -> None:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != DSD_SHA256[asset]:
        raise SystemExit(f"SHA-256 mismatch for {path}; refusing to execute it.")


def extract_sdk(archive: Path, destination: Path) -> None:
    """Validate paths and links, including on Python without tarfile's data filter."""
    root = destination.resolve()
    with tarfile.open(archive, "r:gz") as sdk:
        for member in sdk.getmembers():
            target = (root / member.name).resolve()
            if not target.is_relative_to(root):
                raise ValueError(f"Unsafe SDK archive member: {member.name}")
            if member.issym() or member.islnk():
                base = target.parent if member.issym() else root
                if not (base / member.linkname).resolve().is_relative_to(root):
                    raise ValueError(f"Unsafe SDK archive link: {member.name}")
            elif not (member.isfile() or member.isdir()):
                raise ValueError(f"Unsupported SDK archive member: {member.name}")
        if hasattr(tarfile, "data_filter"):
            sdk.extractall(root, filter="data")
        else:
            sdk.extractall(root)


def windows_cmake_options(deps: Path, sdl: bool = True) -> list[str]:
    if os.name != "nt":
        return []
    try:
        importlib.import_module("ziglang")
    except ImportError:
        run(sys.executable, "-m", "pip", "install", ZIG_WINDOWS_REQUIREMENT)
    options = [f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake/toolchains/x86_64-windows-zig.cmake'}"]
    if sdl:
        deps = deps.resolve()
        root = deps / f"SDL2-{SDL_VERSION}" / "x86_64-w64-mingw32"
        if not (root / "bin" / "SDL2.dll").is_file():
            deps.mkdir(parents=True, exist_ok=True)
            archive = deps / f"SDL2-devel-{SDL_VERSION}-mingw.tar.gz"
            download_verified(SDL_URL, archive, "sha256", SDL_SHA256)
            extract_sdk(archive, deps)
        options += [
            f"-DDQ9_WINDOWS_SDL_ROOT={root}",
            f"-DSDL2_DIR={root / 'lib/cmake/SDL2'}",
            f"-DDSR_SDL2_RUNTIME_DLL={root / 'bin/SDL2.dll'}",
        ]
    return [*options, f"-DPython3_EXECUTABLE={sys.executable}"]


GODOT_CPP_TAG = "godot-4.5-stable"
GODOT_CPP_URL = f"https://github.com/godotengine/godot-cpp/archive/refs/tags/{GODOT_CPP_TAG}.zip"
GODOT_CPP_SHA256 = "0316ea168750b5f4f1caea6457657a6c64241f890f78e2f042aee4be2b1085bf"
GODOT_EDITOR_ASSET = "Godot_v4.5-stable_win64.exe.zip"
GODOT_EDITOR_URL = f"https://github.com/godotengine/godot-builds/releases/download/4.5-stable/{GODOT_EDITOR_ASSET}"
GODOT_EDITOR_SHA512 = (
    "e8ef4867466bb01a2b762c4ec404f54266b40a5cbba0efa95e3ab325cd7d349b"
    "5dc11259e1c1525195080dc82294ff28044f7ac4acad833e4b054b4257e9aa1a"
)
GODOT_EDITOR_EXE = "Godot_v4.5-stable_win64.exe"


def download_verified(url: str, destination: Path, algorithm: str, digest: str) -> None:
    """Download once; never keep or use a file whose digest differs from the pinned one."""
    if destination.is_file() and hashlib.new(algorithm, destination.read_bytes()).hexdigest() == digest:
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".download")
    try:
        print(f"    downloading {destination.name}", flush=True)
        urllib.request.urlretrieve(url, temporary)
        if hashlib.new(algorithm, temporary.read_bytes()).hexdigest() != digest:
            raise SystemExit(f"{algorithm.upper()} mismatch for {url}; refusing to use it.")
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def extract_zip(archive: Path, destination: Path) -> None:
    """Refuse members escaping the destination and symbolic links."""
    root = destination.resolve()
    with zipfile.ZipFile(archive) as bundle:
        for info in bundle.infolist():
            if not (root / info.filename).resolve().is_relative_to(root):
                raise ValueError(f"Unsafe archive member: {info.filename}")
            if (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError(f"Unsupported archive link: {info.filename}")
        bundle.extractall(root)


def ensure_godot_cpp(deps: Path) -> Path:
    deps = deps.resolve()
    target = deps / f"godot-cpp-{GODOT_CPP_TAG}"
    if not (target / "CMakeLists.txt").is_file():
        archive = deps / f"godot-cpp-{GODOT_CPP_TAG}.zip"
        download_verified(GODOT_CPP_URL, archive, "sha256", GODOT_CPP_SHA256)
        extract_zip(archive, deps)
    return target


def ensure_godot_editor() -> Path:
    directory = ROOT / "build" / "tools" / "godot"
    exe = directory / GODOT_EDITOR_EXE
    if not exe.is_file():
        archive = directory / GODOT_EDITOR_ASSET
        download_verified(GODOT_EDITOR_URL, archive, "sha512", GODOT_EDITOR_SHA512)
        extract_zip(archive, directory)
    return exe


def godot_step_count(fetch_editor: bool) -> int:
    return 1 + (1 if fetch_editor and os.name == "nt" else 0)


def build_godot(args: argparse.Namespace, cmake: list[str]) -> list[Path]:
    """Build the Godot GDExtension into godot/bin from the already generated code."""
    if not (GEN / "abi.ini").is_file():
        raise SystemExit(f"Generated code not found in {GEN}. Build the game first (BUILD-DQ9.bat).")
    step("Building the Godot extension (downloads the pinned godot-cpp 4.5 bindings)")
    bindings = ensure_godot_cpp(args.sdk_deps)
    build = ROOT / "build" / ("win-godot" if os.name == "nt" else "godot")
    windows = ["-DGODOTCPP_TARGET=template_release", "-DGODOTCPP_SYSTEM_HEADERS=ON"] if os.name == "nt" else []
    run(*cmake, "-S", ROOT, "-B", build, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.config}",
        f"-DDQ9_GENERATED_DIR={GEN}", "-DDQ9_BUILD_SDL=OFF", "-DDQ9_BUILD_GODOT=ON",
        f"-DGODOT_CPP_DIR={bindings}", *windows, *windows_cmake_options(args.sdk_deps, sdl=False))
    build_with_retry(cmake, build)
    names = (["dq9_godot.windows.template_release.x86_64.dll", "dsr_session.dll"] if os.name == "nt"
             else ["libdq9_godot.so"])
    outputs = [ROOT / "godot" / "bin" / name for name in names]
    missing = [path for path in outputs if not path.is_file()]
    if missing:
        raise SystemExit("Godot build finished but did not produce: " + ", ".join(str(m) for m in missing))
    if args.fetch_godot_editor:
        if os.name == "nt":
            step("Downloading the Godot 4.5 editor (checksum verified)")
            outputs.append(ensure_godot_editor())
        else:
            print("    --fetch-godot-editor only applies to Windows; install Godot 4.5 yourself.")
    return outputs


def build_with_retry(cmake: list[str], build_dir: Path, *targets: str) -> None:
    """cmake --build; on Windows retry once serially, as it sometimes cannot link a freshly written .exe
    ("Access is denied" / "file in use" while a scanner holds it). Real errors fail the retry too."""
    command = [*cmake, "--build", build_dir, *(["--target", *targets] if targets else [])]
    try:
        run(*command)
    except subprocess.CalledProcessError:
        if os.name != "nt":
            raise
        print("build failed; retrying once with a single job", flush=True)
        run(*command, "--parallel", "1")


def ctest_executable() -> str:
    local = Path(sys.executable).parent / executable_name("ctest")
    if local.is_file():
        return str(local)
    located = shutil.which("ctest", path=tool_environment()["PATH"])
    if not located:
        raise SystemExit("ctest not found; install cmake with this Python interpreter.")
    return located


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


WORKSPACE_PARTS = ("extract", "config", "build")  # what build_port.py itself creates in a workspace


def clear_workspace(workspace: Path, rom: Path, generated: Path) -> None:
    """Deletes only the directories this script created, never a user directory.

    --workspace is caller-supplied, so it is resolved and refused when it is the repository, one of
    its ancestors, or contains the image, and a custom workspace is only cleared when it carries
    this tool's fingerprint file. Only the extract/config/build subdirectories and the fingerprint
    are removed (errors are raised, not hidden). The generated-code directory is only
    cleared when it is the build/gen directory of this checkout.
    """
    workspace, rom = workspace.resolve(), rom.resolve()
    if workspace == ROOT or workspace in ROOT.parents or workspace.parent == workspace:
        raise SystemExit(f"Refusing to clear {workspace}: it is the repository or one of its parent directories.")
    if rom == workspace or workspace in rom.parents:
        raise SystemExit(f"Refusing to clear {workspace}: it contains the image {rom}.")
    clear_generated = generated.resolve() == (ROOT / "build" / "gen").resolve() and generated.is_dir()
    if clear_generated and (rom == generated.resolve() or generated.resolve() in rom.parents):
        raise SystemExit(f"Refusing to clear {generated}: it contains the image {rom}. Move the image elsewhere.")
    # (All refusals above and below happen before anything is deleted.)
    # Generic folder names prove nothing: only this checkout's default workspace, or one carrying
    # our fingerprint file, is known to be ours. Anything else that already has such folders is
    # somebody else's project; point at a new, empty directory instead.
    owned = workspace == WORKSPACE.resolve() or (workspace / WORKSPACE_MARKER).is_file()
    existing = [part for part in WORKSPACE_PARTS if (workspace / part).is_dir()]
    if existing and not owned:
        raise SystemExit(
            f"Refusing to clear {workspace}: it has {', '.join(existing)} folders that this tool did not create "
            f"(no {WORKSPACE_MARKER} fingerprint, and it is not the default workspace).\n"
            f"Use a new, empty directory for --workspace instead.")
    for part in existing:
        shutil.rmtree(workspace / part)
    (workspace / WORKSPACE_MARKER).unlink(missing_ok=True)
    if clear_generated:
        shutil.rmtree(generated)  # stale chunks from the old image must not be compiled


def ensure_workspace_matches_rom(workspace: Path, rom: Path, rebuild: bool, generated: Path) -> str:
    """Refuse to rebuild from a workspace that was extracted from a different image.

    The extraction step is skipped when a workspace already exists, and copied module images are
    never overwritten, so building image B after image A would silently recompile A. The image
    fingerprint is kept next to the extracted data (git-ignored, local only). Returns the digest.
    """
    digest = file_sha256(rom)
    if rebuild:
        clear_workspace(workspace, rom, generated)
    elif (workspace / "config" / "arm9" / "config.yaml").exists():
        marker = workspace / WORKSPACE_MARKER
        recorded = marker.read_text(encoding="utf-8").strip() if marker.is_file() else ""
        if recorded != digest:
            reason = ("was extracted from a different image" if recorded
                      else "has no record of the image it was extracted from")
            raise SystemExit(
                f"The existing workspace {workspace} {reason}.\n"
                f"Build this image from a clean workspace with:\n"
                f"  python tools/build_port.py --rebuild-workspace --rom \"{rom}\"")
    return digest


def launch_hint(exe: Path, rom: Path, abi: Path) -> str:
    args = [str(exe), "--rom", str(rom), "--abi-config", str(abi), "--language", "en"]
    if os.name == "nt":
        quoted = ['"' + arg.replace("`", "``").replace("$", "`$").replace('"', '`"') + '"' for arg in args]
        return "& " + " ".join(quoted)
    return shlex.join(args)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--rom", type=Path, help="your own image (required unless --runtime-only)")
    p.add_argument("--dsd", type=Path, help="ds-decomp executable (default: verified pinned download)")
    p.add_argument("--profile", type=Path, help="private fingerprint profile JSON for verify_reference_rom.py")
    p.add_argument("--workspace", type=Path, default=WORKSPACE)
    p.add_argument("--build-dir", type=Path, help="default: build/game, or build/win-zig with --runtime-only")
    p.add_argument("--sdk-deps", type=Path, default=ROOT / "build" / "win-deps")
    p.add_argument("--runtime-only", action="store_true", help="build/test the public runtime without any image")
    # Release: the same generated code is ~7% faster and the executable ~10x smaller than with
    # RelWithDebInfo (outputs identical); pass --config RelWithDebInfo when you need symbols.
    p.add_argument("--config", default="Release")
    p.add_argument("--generate-only", action="store_true", help="stop after generating C++")
    p.add_argument("--backup-type", choices=BACKUP_TYPE_CHOICES, default="auto",
                   help="cartridge backup chip written to the local abi.ini (default auto: inferred from an existing "
                        "save file, otherwise flash512k). Use eeprom64k for the European release.")
    p.add_argument("--rebuild-workspace", action="store_true",
                   help="discard the extracted workspace and generated code first (use when switching to a different image)")
    p.add_argument("--check", action="store_true", help="only report whether this machine and image are ready, then exit")
    p.add_argument("--quiet", action="store_true",
                   help="short step-by-step output; full command output goes to build/build-log.txt")
    p.add_argument("--godot", action="store_true", help="after the game, also build the Godot extension")
    p.add_argument("--godot-only", action="store_true",
                   help="build only the Godot extension from already generated code (no image needed)")
    p.add_argument("--fetch-godot-editor", action="store_true",
                   help="Windows: with --godot/--godot-only, download the pinned Godot 4.5 editor into build/tools/godot")
    args = p.parse_args(argv)
    if not args.runtime_only and not args.godot_only and args.rom is None:
        p.error("--rom is required unless --runtime-only or --godot-only is used")
    if args.quiet:
        configure_log()
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(errors="replace")
            sys.stderr.reconfigure(errors="replace")
    if args.godot_only and (args.runtime_only or args.generate_only or args.godot):
        p.error("--godot-only cannot be combined with --runtime-only, --generate-only or --godot")
    if args.runtime_only and args.generate_only:
        p.error("--runtime-only and --generate-only cannot be combined")
    args.build_dir = args.build_dir or ROOT / "build" / ("win-zig" if args.runtime_only else "game")
    if args.godot_only:
        pass
    elif not args.runtime_only:
        begin_steps(5 + (godot_step_count(args.fetch_godot_editor) if args.godot else 0))
        step("Checking your PC and the ROM")
        # Fail in seconds, not after the extraction step, when the image or tools are unusable.
        ready = preflight.report(preflight.run_checks(args.rom, ROOT / "build"))
        if args.check:
            return 0 if ready else 1
        if not ready:
            print("Fix the FAIL lines above and run again.", file=sys.stderr)
            return 1
    elif args.check:
        p.error("--check needs an image; it cannot be combined with --runtime-only")
    cmake = [sys.executable, "-m", "cmake"]
    if args.godot_only:
        begin_steps(godot_step_count(args.fetch_godot_editor))
        outputs = build_godot(args, cmake)
        banner("Godot extension ready", *(str(path) for path in outputs),
               "Start it with PLAY-GODOT.bat (Windows) or tools/run_godot.sh.")
        return 0
    if args.runtime_only:
        options = windows_cmake_options(args.sdk_deps)
        run(*cmake, "-S", ROOT, "-B", args.build_dir, "-G", "Ninja",
            f"-DCMAKE_BUILD_TYPE={args.config}", "-UDQ9_GENERATED_DIR",
            "-DDQ9_BUILD_SDL=ON", "-DDQ9_BUILD_GODOT=OFF", *options)
        build_with_retry(cmake, args.build_dir)
        run(ctest_executable(), "--test-dir", args.build_dir, "--output-on-failure",
            env={"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy", "SDL_RENDER_DRIVER": "software"})
        return 0
    step("Extracting the ROM with ds-decomp")
    args.dsd = args.dsd.resolve() if args.dsd else fetch_dsd()

    rom = args.rom.resolve()
    py = sys.executable
    tools = ROOT / "tools"
    rom_digest = ensure_workspace_matches_rom(args.workspace, rom, args.rebuild_workspace, GEN)

    if args.profile:
        run(py, tools / "verify_reference_rom.py", "--rom", rom, "--profile", args.profile)

    extract = args.workspace / "extract"
    config_dir = args.workspace / "config"
    build_dir = args.workspace / "build"
    config = config_dir / "arm9" / "config.yaml"
    if not config.exists():
        extract.mkdir(parents=True, exist_ok=True)
        run(args.dsd, "rom", "extract", "--rom", rom, "--output-path", extract)
        run(py, tools / "prepare_dsd_config.py", extract / "config.yaml", extract / "config-dsd.yaml")
        run(args.dsd, "init", "--rom-config", extract / "config-dsd.yaml",
            "--output-path", config_dir, "--build-path", build_dir)
        (args.workspace / WORKSPACE_MARKER).write_text(rom_digest + "\n", encoding="utf-8")

    images = build_dir / "build"
    images.mkdir(parents=True, exist_ok=True)
    pairs = [(extract / "arm9" / n, images / n) for n in ("arm9.bin", "itcm.bin", "dtcm.bin")]
    pairs += [(f, images / f"arm9_{f.name}") for f in sorted((extract / "arm9_overlays").glob("ov*.bin"))]
    for src, dst in pairs:
        if src.exists() and not dst.exists():
            dst.write_bytes(src.read_bytes())
    # The manifest builder reads per-module load addresses from these YAML files
    # next to the images; without them every non-main module gets base 0.
    for name in ("arm9.yaml", "itcm.yaml", "dtcm.yaml"):
        shutil.copyfile(extract / "arm9" / name, images / name)
    overlays_yaml = (extract / "arm9_overlays" / "overlays.yaml").read_text(encoding="utf-8")
    (images / "arm9_overlays.yaml").write_text(
        re.sub(r"file_name: ov(\d+)\.bin", r"file_name: arm9_ov\1.bin", overlays_yaml), encoding="utf-8")

    step("Analysing the game code")
    GEN.mkdir(parents=True, exist_ok=True)
    manifest9 = GEN / "arm9.manifest.json"
    run(py, "-m", "recomp.dsd_manifest", "--config", config, "--out", manifest9, summary=True)
    run(py, tools / "unwrap_protected_overlay.py", "--manifest", manifest9, "--out", GEN, summary=True)

    # DSD writes the ARM7 load address, entry and autoload callback while extracting.
    import yaml  # imported late so tools/preflight.py can report a missing PyYAML first

    arm7_meta = yaml.safe_load((extract / "arm7" / "arm7.yaml").read_text(encoding="utf-8"))
    for key in ("base_address", "entry_function", "autoload_callback"):
        if key not in arm7_meta:
            raise SystemExit(f"{extract / 'arm7' / 'arm7.yaml'} lacks {key}")
    run(py, tools / "arm7_analyze.py", "--arm7", extract / "arm7" / "arm7.bin", "--out", GEN,
        "--base", hex(arm7_meta["base_address"]), "--entry", hex(arm7_meta["entry_function"]),
        "--autoload-callback", hex(arm7_meta["autoload_callback"]), summary=True)

    abi = GEN / "abi.ini"
    run(py, tools / "derive_abi_config.py", "--workspace", args.workspace, "--out", abi, "--report", GEN / "abi-report.json",
        "--backup-type", args.backup_type)

    step("Translating ARM9 and ARM7 code to C++ (a few minutes)")
    run(py, "-m", "recomp", "--manifest", manifest9, "--out", GEN / "arm9", summary=True)
    run(py, "-m", "recomp", "--manifest", GEN / "arm7.manifest.json", "--out", GEN / "arm7", summary=True)
    if args.generate_only:
        print(f"generated C++ in {GEN}")
        return 0

    step("Compiling dq9.exe (the long step; only changed files rebuild next time)")
    run(*cmake, "-S", ROOT, "-B", args.build_dir, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.config}",
        f"-DDQ9_GENERATED_DIR={GEN}", *windows_cmake_options(args.sdk_deps))
    build_with_retry(cmake, args.build_dir, "dq9")
    exe = args.build_dir / "game" / executable_name("dq9")
    if args.godot:
        build_godot(args, cmake)
    if args.quiet:
        banner("Build complete - keep every generated file private",
               f"Game: {exe}", f"Log:  {_LOG}")
    else:
        print(f"built {exe} (local only; never distribute)")
        print("run: " + launch_hint(exe, rom, abi))
    return 0


if __name__ == "__main__":
    sys.exit(main())
