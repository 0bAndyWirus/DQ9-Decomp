# Building DQ9-Decomp on Windows x64

The SDL host and Godot 4.5 GDExtension share the original native runtime. The cross-build uses Zig's MinGW-compatible Windows sysroot on Linux; native MSVC is a separate build path. A cross-link or Wine test is not a Windows hardware/gameplay test.

Targets are Windows 10/11 x64. Images, saves, generated game sources, captures, and game builds remain private. Game-host packages are local-only and must never be uploaded to CI, releases, or issue attachments.

## Source build kit on Windows (start here)

The Windows kit contains **source and scripts only: no game or binary is included**.
The first ROM run on Windows is unverified by the author. The `kit-smoke` CI job
(a `windows-latest` runner) builds and tests the ROM-free runtime using Zig and
packages the kit; it passed for v0.2.0. That is evidence for the toolchain and
runtime tests, not for Windows gameplay.

Requirements: Windows 10/11 x64, Python 3.10+ from
[python.org](https://www.python.org/downloads/windows/) with **Add python.exe to PATH**
enabled, internet access, about 5 GB free disk space, and your own legally obtained
European multilingual `.nds` dump. Extract the kit to a writable directory.
From Command Prompt in that directory:

```bat
BUILD-DQ9.bat "C:\path\game.nds"
PLAY-DQ9.bat
```

In PowerShell, use `.\BUILD-DQ9.bat` and `.\PLAY-DQ9.bat` instead. `PLAY-DQ9.bat` without arguments opens the launcher, which remembers your ROM path, language and options.
The language is optional (default `en`); `fr`, `de`, `es` and `it` are also supported.
For a fresh European-release save, choose **EEPROM 64 KB** on the launcher's
**ROM / SAVE → Save chip** control before Launch. The direct ROM/language form
uses the generated ABI configuration's chip rather than the launcher's saved choice.
The build creates `.venv`, installs the Python dependencies and Zig, downloads
ds-decomp with a pinned SHA-256 check and the official SDL2 SDK, then generates
and compiles code from your own dump. It can take 30–90 minutes; this Windows
build time has not been measured by the author. The launcher supplies the generated
`build\gen\abi.ini` and does not enable the debug tool.

In a source checkout, the launchers are `tools\windows\BUILD-DQ9.bat` and
`tools\windows\PLAY-DQ9.bat`. Alternatively, from the repository root:

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install PyYAML capstone unicorn cmake ninja ziglang==0.16.0
.\.venv\Scripts\python.exe tools\build_port.py --rom "C:\path\game.nds" --backup-type eeprom64k
```

The Windows-hosted flow pins `ziglang==0.16.0` because PyPI publishes no Windows wheel for
0.17.0, the version used by the Linux-hosted cross build below. `--dsd PATH` selects an existing ds-decomp executable instead of the pinned
download. `--sdk-deps DIR` selects the SDL SDK download directory (default
`build/win-deps`). All extracted data, generated sources, ABI metadata, game
binaries and saves remain local-only; never upload them or add them to the kit.
The executable embeds game-generated code and is not a redistributable release.

Protected overlay code is resolved from your own image with `tools/unwrap_protected_overlay.py`. This is a form of circumventing the cartridge's code protection; it works only on your own file and nothing derived from it is distributed. Whether that is permitted where you live is your responsibility; see [LEGAL.md](../LEGAL.md).

For a ROM-free runtime check using the same toolchain:

```powershell
.\.venv\Scripts\python.exe tools\build_port.py --runtime-only
```

This configures `build/win-zig` without generated game code, builds all runtime
targets and runs CTest with dummy SDL video/audio drivers. The independent SWI
formula checks need no external BIOS-reference source. `--build-dir DIR` selects
another build tree.

For support, run `COLLECT-DIAGNOSTICS.bat` in the kit folder (in a checkout:
`tools\windows\COLLECT-DIAGNOSTICS.bat`). It creates one
`diagnostics-<timestamp>.zip` beside the kit scripts, without building or running
the game. Python 3.10+ is required; no dependencies are installed. The ZIP contains
tool/package versions, OS/free disk, backup-chip and pin names, a launcher settings
summary (no ROM/save paths), binary existence/sizes only, script integrity hashes,
and at most the last 300 build-log lines. Home/profile and kit paths are replaced
with `<HOME>` and `<KIT>`. No ROM, save, generated code, executable, DLL, screenshot
or capture is attached. Review the text before sending it to support: build logs
can still contain custom paths outside the home/kit directories. The collector
returns 2 if an input/archive member violates its safety policy. On Linux, use
`python tools/collect_diagnostics.py --kit-dir /path/to/kit`.


Maintainers create the public source kit with
`python tools/package_windows_kit.py --tag TAG`. It archives `HEAD`, copies the
root launchers and instructions from the checkout, and audits the staged tree.
It warns about uncommitted tracked changes; commit the intended release first.
The source-aware audit permits HEAD-tracked original C++ sources, but still
rejects generated-source names and image/save/binary files. This public source
kit is different from the private binary packages described below.

## Build inputs

The kit's `tools/build_port.py` generates the game sources locally from your own dump. The lower-level cross-build and MSVC paths below take that separately generated directory through `DQ9_GENERATED_DIR` or the wrapper's `--gen` option.

Runtime-only builds omit `DQ9_GENERATED_DIR` and need no game image. The SWI differential CTest target uses independent formula checks by default. Its external reference-table mode requires a separately available reference-emulator source tree: configure `DQ9_BIOS_REFERENCE_SOURCE` with the path to public DeSmuME HLE `src/bios.cpp`. This is not a Nintendo BIOS and is not bundled. Wine uses its `Z:` mapping when reading the supplied external host path.

## Cross-build on Linux with Zig

Prerequisites: a local Python virtual environment, CMake 3.20+, Ninja, Git, curl, and tar. The wrapper uses `.venv/bin/cmake`:

```sh
.venv/bin/python -m pip install cmake ziglang==0.17.0
tools/build_windows.sh --gen /absolute/path/generated-code --jobs 6
```

The wrapper installs Zig if needed and uses the [official SDL2 MinGW development SDK](https://github.com/libsdl-org/SDL/releases/tag/release-2.30.12) under ignored `build/win-deps/`. `--deps DIR` or `--sdl-root DIR` selects an existing SDK. `DQ9_ZIG_PYTHON` selects the Zig interpreter; `--no-ccache` disables the optional compiler cache.

Outputs include the SDL executable, its SDL2 DLL, runtime test executables, and the session library under `build/win-sdl/`. Generated code retains the compiler options required for guest-pointer semantics; runtime code uses the selected Release settings. SDL2 is a separate DLL and must match the target architecture.

Direct configuration, using absolute SDK/generated paths:

```sh
.venv/bin/cmake -S . -B build/win-sdl -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-windows-zig.cmake \
  -DCMAKE_BUILD_TYPE=Release -DDQ9_BUILD_GODOT=OFF \
  -DDQ9_GENERATED_DIR="/absolute/path/generated-code" \
  -DDQ9_WINDOWS_SDL_ROOT="/absolute/path/SDL2-SDK" \
  -DSDL2_DIR="/absolute/path/SDL2-SDK/lib/cmake/SDL2" \
  -DDSR_SDL2_RUNTIME_DLL="/absolute/path/SDL2-SDK/bin/SDL2.dll"
.venv/bin/cmake --build build/win-sdl -j 6
```

For runtime-only configuration, omit the generated-directory option. Add `-DDQ9_BIOS_REFERENCE_SOURCE="/absolute/path/desmume/src/bios.cpp"` to exercise the external reference-table comparison; it requires that public HLE source even without game data.

## Godot Windows extension

In the Windows kit, `BUILD-GODOT.bat` does this for you: it downloads the pinned godot-cpp 4.5 bindings (SHA-256 verified) and the Godot 4.5 editor (SHA-512 verified), then builds the extension with the same Zig toolchain as the game (`python tools/build_port.py --godot-only --fetch-godot-editor --quiet`). `PLAY-GODOT.bat` starts it. These Windows steps are written but have not been run on a real Windows machine yet; see [Using the Godot version](USING_GODOT.md). The manual route below is for Linux cross-builds and custom setups.

Use matching external [godot-cpp 4.5 bindings](https://github.com/godotengine/godot-cpp/tree/godot-4.5-stable), selected through `GODOT_CPP_DIR`:

```sh
GODOT_CPP_DIR="/absolute/path/godot-cpp" \
  tools/build_windows.sh --both --gen /absolute/path/generated-code --jobs 6
# --godot-only selects just the extension and does not need SDL2.
```

The separate build tree is `build/win-godot`. The toolchain supplies Windows platform/architecture information; the wrapper uses `GODOTCPP_TARGET=template_release` and `GODOTCPP_SYSTEM_HEADERS=ON`.

The matching extension and session DLL are written to `godot/bin/`. `godot/dq9.gdextension` lists the Windows x86_64 libraries and session dependency. Do not mix DLLs built by different compilers/configurations or platforms.

To run locally, use the [Godot 4.5 Windows x64 editor](https://godotengine.org/download/archive/4.5-stable/), set `GODOT` to its executable, and open `godot\project.godot` or run `tools\run_godot.bat --menu`. Export templates are not needed to run the project in the editor.

A local standalone Windows export requires matching export templates and a Windows Desktop x86_64 preset. The tracked preset is Linux-only. Include the matching extension/session DLLs, but never an image or save; the resulting game export must remain private.

## Native MSVC build

Install Visual Studio 2022 Build Tools with Desktop development with C++, CMake, Ninja, Python, and Git. Use an x64 developer shell and an external vcpkg checkout with SDL2 installed. In PowerShell:

```powershell
& "$env:VCPKG_ROOT\vcpkg.exe" install sdl2:x64-windows
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install cmake
.\.venv\Scripts\cmake.exe -S . -B build/win-native -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows -DDQ9_BUILD_GODOT=OFF `
  "-DDSR_SDL2_RUNTIME_DLL=$env:VCPKG_ROOT/installed/x64-windows/bin/SDL2.dll"
.\.venv\Scripts\cmake.exe --build build/win-native -j 6
.\.venv\Scripts\ctest.exe --test-dir build/win-native --output-on-failure
```

This is runtime-only. External reference-table comparison additionally configures the separately available HLE source with `DQ9_BIOS_REFERENCE_SOURCE`; the default uses independent formula checks. A local game host configures the separately supplied generated-code directory. A Godot build uses a separate tree with `DQ9_BUILD_SDL=OFF`, `DQ9_BUILD_GODOT=ON`, matching bindings, and the same compiler/session configuration.

MSVC builds use the shared MSVC runtime; local machines without the development tools may need Microsoft's x64 Visual C++ Redistributable.

The Windows CI workflow builds/tests public runtime code only and uploads no artifacts. It makes the external public HLE test source available separately; no Nintendo BIOS or game image is used. CI success must be reported from an actual run, not inferred from its configuration.

## Local-only packaging and launch

```sh
tools/package_windows.sh             # local SDL + Godot builds required
tools/package_windows.sh --sdl-only  # local SDL-only package
```

The package script stages an explicit binary/resource whitelist under ignored `build/dist/`. Unknown files in an existing output directory cause refusal; choose a fresh `--out DIR` rather than deleting user data.

`tools/audit_package.py` is a heuristic filename/size/signature check, not proof that an archive contains no game data or is lawful to share. Even a package with no findings is local-only and must never be uploaded.

Launch from `<install-dir>` with `run_game.bat --menu` or `run_godot.bat --menu`, then select an existing local compatible image. Do not create a bundled image directory or copy an image beside the executable. Direct image selection uses `--rom` or `DQ9_ROM`; choose a writable save path separately. In a source checkout these wrappers live in `tools\`; PowerShell equivalents do not change system execution policy.

`DQ9_SAVE`, `DQ9_EXE`, and `GODOT` select local save/executable/editor paths. Default wrapper saves use the application-data directory. Keep diagnostic saves distinct from normal saves and out of any package. The Godot wrapper performs a headless editor import when needed; this requires a writable project directory and the real editor, not an export template.

`DQ9_ABI_CONFIG` selects caller-supplied local runtime metadata for both hosts; SDL also accepts `--abi-config PATH`. CPU sections use the fields in `runtime/include/dsr/abi_config.h`; supplied sections must be complete, including intentional zero fields. Omitting a CPU section disables its thread overrides with a diagnostic. Optional cartridge backup settings belong in that same private configuration. No title-specific ABI layout is bundled.

## Verification and limits

PE/import inspection verifies linkage and architecture, not execution. Optional Wine CTest checks are not native Windows or Godot-editor compatibility evidence. Cross-built DLLs cannot be exercised by Linux Python ctypes as Windows libraries.

On a Windows workstation, run runtime tests with the external reference source, then validate both hosts locally with suitable inputs: audio, controllers, touch, save persistence, scaling, fibers, and shutdown. Hardware and GPU-backend behavior require separate exercised checks. Keep all generated sources, captures, saves, dumps, and game binaries private.

As of 2026-10-12, native and cross-build configurations are present. This public guide asserts no full-game acceptance, target-hardware result, binary size, game coverage total, or performance guarantee.
