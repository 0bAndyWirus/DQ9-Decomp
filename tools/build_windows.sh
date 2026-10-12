#!/usr/bin/env bash
# Cross-build Windows x64 on Linux using Zig's bundled MinGW sysroot.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PYTHON="$ROOT/.venv/bin/python"
CMAKE="$ROOT/.venv/bin/cmake"
GEN="${DQ9_GEN:-}"
DEPS="$ROOT/build/win-deps"
SDL_ROOT=""
JOBS=${JOBS:-6}
MODE=sdl
DO_TEST=0
USE_CCACHE=1
GODOT_CPP_DIR=${GODOT_CPP_DIR:-$HOME/Applications/godot/godot-cpp}
SDL_VERSION=2.30.12
ZIG_VERSION=0.17.0

usage() {
  cat <<EOF
Usage: tools/build_windows.sh [--sdl-only | --godot-only | --both]
       [--gen DIR] [--jobs N] [--deps DIR] [--sdl-root DIR] [--no-ccache] [--test]

Separately supplied generated sources are required: --gen DIR or DQ9_GEN.
Requires the repository .venv, CMake, Ninja, curl and tar. Installs ziglang into
.venv if absent and downloads the official SDL2 MinGW SDK into ignored build/.
--test requires Wine on PATH; no Windows execution is claimed without it.
Environment: DQ9_GEN, GODOT_CPP_DIR (Godot 4.5 bindings), JOBS (default 6), DQ9_ZIG_PYTHON.
Output: build/win-sdl/game/dq9.exe and/or godot/bin/*windows*.dll.
EOF
}
while (($#)); do
  case "$1" in
    --sdl-only) MODE=sdl; shift ;;
    --godot-only) MODE=godot; shift ;;
    --both) MODE=both; shift ;;
    --gen|--jobs|--deps|--sdl-root)
      (($# >= 2)) || { usage >&2; exit 2; }
      case "$1" in
        --gen) GEN=$2 ;;
        --jobs) JOBS=$2 ;;
        --deps) DEPS=$2 ;;
        --sdl-root) SDL_ROOT=$2 ;;
      esac
      shift 2 ;;
    --no-ccache) USE_CCACHE=0; shift ;;
    --test) DO_TEST=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "build_windows.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
  esac
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "--jobs must be a positive integer" >&2; exit 2; }
[[ -n "$GEN" ]] || { echo "build_windows.sh: separately supplied generated sources are required; use --gen DIR or DQ9_GEN" >&2; exit 2; }
for cpu in arm9 arm7; do
  [[ -f "$GEN/$cpu/fn_table.cpp" ]] || { echo "Missing $GEN/$cpu/fn_table.cpp in the supplied generated-code directory" >&2; exit 1; }
done
[[ -x "$PYTHON" && -x "$CMAKE" ]] || { echo "Create .venv and install cmake first (see docs/BUILDING_WINDOWS.md)" >&2; exit 1; }
export DQ9_ZIG_PYTHON=${DQ9_ZIG_PYTHON:-$PYTHON}
if ! "$DQ9_ZIG_PYTHON" -c 'import ziglang' 2>/dev/null; then
  "$DQ9_ZIG_PYTHON" -m pip install "ziglang==$ZIG_VERSION"
fi
zig_version=$("$DQ9_ZIG_PYTHON" -m ziglang version)
printf 'Zig %s\n' "$zig_version"
GEN=$(cd "$GEN" && pwd)
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/toolchains/x86_64-windows-zig.cmake"
  "-DPython3_EXECUTABLE=$PYTHON"
  "-DDQ9_GENERATED_DIR=$GEN")
if ((USE_CCACHE)) && command -v ccache >/dev/null; then
  export CCACHE_BASEDIR="$ROOT" CCACHE_NOHASHDIR=1
  # Include the real Zig version, not only the unchanged wrapper, in cache keys.
  export CCACHE_COMPILERCHECK="string:ziglang-$zig_version"
  export CCACHE_EXTRAFILES="$ROOT/cmake/toolchains/zig-cc.sh:$ROOT/cmake/toolchains/zig-cxx.sh"
  common+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
else
  common+=(-DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=)
fi
if ((DO_TEST)) && ! command -v wine64 >/dev/null && ! command -v wine >/dev/null; then
  echo "--test needs Wine; cross-built PE executables cannot run directly on Linux" >&2
  exit 1
fi
if [[ "$MODE" != godot ]]; then
  if [[ -z "$SDL_ROOT" ]]; then
    mkdir -p "$DEPS"
    SDL_ROOT="$DEPS/SDL2-$SDL_VERSION/x86_64-w64-mingw32"
    if [[ ! -f "$SDL_ROOT/bin/SDL2.dll" ]]; then
      archive="$DEPS/SDL2-devel-$SDL_VERSION-mingw.tar.gz"
      curl -fL --retry 2 "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-devel-$SDL_VERSION-mingw.tar.gz" -o "$archive"
      tar -xzf "$archive" -C "$DEPS"
    fi
  fi
  SDL_ROOT=$(cd "$SDL_ROOT" && pwd)
  "$CMAKE" -S "$ROOT" -B "$ROOT/build/win-sdl" "${common[@]}" \
    -DDQ9_BUILD_SDL=ON -DDQ9_BUILD_GODOT=OFF \
    "-DDQ9_WINDOWS_SDL_ROOT=$SDL_ROOT" "-DSDL2_DIR=$SDL_ROOT/lib/cmake/SDL2" \
    "-DDSR_SDL2_RUNTIME_DLL=$SDL_ROOT/bin/SDL2.dll"
  "$CMAKE" --build "$ROOT/build/win-sdl" -j "$JOBS"
  if ((DO_TEST)); then "$ROOT/.venv/bin/ctest" --test-dir "$ROOT/build/win-sdl" --output-on-failure; fi
fi
if [[ "$MODE" != sdl ]]; then
  [[ -f "$GODOT_CPP_DIR/CMakeLists.txt" ]] || { echo "Missing Godot 4.5 godot-cpp checkout: $GODOT_CPP_DIR" >&2; exit 1; }
  "$CMAKE" -S "$ROOT" -B "$ROOT/build/win-godot" "${common[@]}" \
    -DDQ9_BUILD_SDL=OFF -DDQ9_BUILD_GODOT=ON \
    "-DGODOT_CPP_DIR=$GODOT_CPP_DIR" -DGODOTCPP_TARGET=template_release \
    -DGODOTCPP_SYSTEM_HEADERS=ON
  "$CMAKE" --build "$ROOT/build/win-godot" -j "$JOBS"
  if ((DO_TEST)); then "$ROOT/.venv/bin/ctest" --test-dir "$ROOT/build/win-godot" --output-on-failure; fi
fi
if [[ "$MODE" == sdl ]]; then
  printf '\nPackage SDL: %s/tools/package_windows.sh --sdl-only\n' "$ROOT"
elif [[ "$MODE" == both ]]; then
  printf '\nPackage: %s/tools/package_windows.sh\n' "$ROOT"
else
  printf '\nWindows launcher: tools\\run_godot.bat --menu (set GODOT to the Windows editor)\n'
fi
