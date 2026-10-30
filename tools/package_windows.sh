#!/usr/bin/env bash
# Package only host binaries and tracked Godot resources: never ROMs/saves/dumps.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDL_BUILD="$ROOT/build/win-sdl"
OUTPUT="$ROOT/build/dist"
WITH_GODOT=1
RUNTIME_DIR=""
GODOT_CPP_DIR=${GODOT_CPP_DIR:-$HOME/Applications/godot/godot-cpp}
usage() {
  echo 'Usage: tools/package_windows.sh [--sdl-build DIR] [--out DIR] [--sdl-only] [--runtime-dir DIR]'
}
while (($#)); do
  case "$1" in
    --sdl-build|--out|--runtime-dir)
      (($# >= 2)) || { usage >&2; exit 2; }
      case "$1" in
        --sdl-build) SDL_BUILD=$2 ;;
        --out) OUTPUT=$2 ;;
        --runtime-dir) RUNTIME_DIR=$2 ;;
      esac
      shift 2 ;;
    --sdl-only) WITH_GODOT=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
"$ROOT/.venv/bin/python" - "$ROOT" "$SDL_BUILD" "$OUTPUT" "$WITH_GODOT" "$RUNTIME_DIR" "$GODOT_CPP_DIR" <<'PY'
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

root, build, output = map(pathlib.Path, sys.argv[1:4])
with_godot = sys.argv[4] == '1'
runtime_dir, bindings = map(pathlib.Path, sys.argv[5:7])
name = 'dq9-remastered-win64'
output.mkdir(parents=True, exist_ok=True)
# Staging never copies a user's ROM directory or any untracked project resource.
with tempfile.TemporaryDirectory(prefix='.windows-package-', dir=output) as scratch:
    stage = pathlib.Path(scratch) / name
    stage.mkdir()
    def copy(source, destination):
        if not source.is_file() or source.is_symlink():
            raise SystemExit(f'Missing required package input (or symlink): {source}')
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    for binary in ('dq9.exe', 'SDL2.dll'):
        copy(build / 'game' / binary, stage / binary)
    launchers = ['run_game.bat', 'run_game.ps1']
    if with_godot:
        launchers += ['run_godot.bat', 'run_godot.ps1']
        for binary in ('dq9_godot.windows.template_release.x86_64.dll', 'dsr_session.dll'):
            copy(root / 'godot' / 'bin' / binary, stage / 'godot' / 'bin' / binary)
        tracked = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z', '--', 'godot']).decode().split('\0')
        for path in filter(None, tracked):
            relative = pathlib.PurePosixPath(path)
            if path in ('godot/project.godot', 'godot/dq9.gdextension', 'godot/dq9.gdextension.uid') or (
                len(relative.parts) > 2 and relative.parts[1] in ('scenes', 'scripts', 'shaders')
                and relative.suffix in ('.gd', '.uid', '.tscn', '.gdshader', '.gdshaderinc')
            ):
                copy(root / path, stage / path)
        copy(bindings / 'LICENSE.md', stage / 'licenses' / 'godot-cpp.txt')
    for script in launchers:
        copy(root / 'tools' / script, stage / script)
    copy(root / 'packaging' / 'windows' / 'README-WINDOWS.txt', stage / 'README-WINDOWS.txt')
    copy(root / 'docs' / 'BUILDING_WINDOWS.md', stage / 'BUILDING_WINDOWS.md')
    # Zig statically links its C++ runtime. These are only needed for external
    # MinGW builds; never blindly copy an SDK directory containing extra files.
    if sys.argv[5]:
        for binary in ('libwinpthread-1.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libc++.dll', 'libc++abi.dll'):
            source = runtime_dir / binary
            if source.is_file():
                copy(source, stage / binary)
                if with_godot:
                    copy(source, stage / 'godot' / 'bin' / binary)
    # Include the SDL license from the exact SDK configured for this build.
    cache = (build / 'CMakeCache.txt').read_text()
    sdk = next((line.split('=', 1)[1] for line in cache.splitlines() if line.startswith('DSR_SDL2_RUNTIME_DLL:FILEPATH=')), None)
    if not sdk:
        raise SystemExit('CMakeCache.txt does not identify the SDL runtime DLL')
    copy(pathlib.Path(sdk).parents[2] / 'LICENSE.txt', stage / 'licenses' / 'SDL2.txt')
    try:
        import ziglang
    except ImportError:
        pass  # External MinGW/MSVC packages need not have Zig installed.
    else:
        zig = pathlib.Path(ziglang.__file__).parent
        for source, notice in ((zig / 'LICENSE', 'Zig.txt'),
                               (zig / 'lib/libcxx/LICENSE.TXT', 'libcxx.txt'),
                               (zig / 'lib/libcxxabi/LICENSE.TXT', 'libcxxabi.txt')):
            if source.is_file():
                copy(source, stage / 'licenses' / notice)
    files = list(stage.rglob('*'))
    forbidden = ('.nds', '.srl', '.sav', '.dsv', '.bin', '.cpp', '.manifest.json')
    if any(p.is_file() and p.name.lower().endswith(forbidden) for p in files):
        raise SystemExit('Refusing package containing a ROM, save, dump or generated source')
    subprocess.run([sys.executable, str(root / 'tools' / 'audit_package.py'), str(stage)], check=True)
    archive = pathlib.Path(scratch) / f'{name}.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for path in sorted(p for p in files if p.is_file()):
            bundle.write(path, pathlib.Path(name) / path.relative_to(stage))
    subprocess.run([sys.executable, str(root / 'tools' / 'audit_package.py'), str(archive)], check=True)
    destination = output / name
    if destination.exists():
        incoming = {p.relative_to(stage) for p in files if p.is_file()}
        unexpected = sorted(p.relative_to(destination) for p in destination.rglob('*')
                            if (p.is_file() or p.is_symlink()) and p.relative_to(destination) not in incoming)
        if unexpected:
            raise SystemExit(f'Existing package contains user/unknown files; choose --out DIR instead: {unexpected[0]}')
        shutil.rmtree(destination)
    shutil.move(str(stage), destination)
    shutil.move(str(archive), output / archive.name)
print(f'Package: {output / name}')
print(f'Zip: {output / (name + ".zip")}')
PY
