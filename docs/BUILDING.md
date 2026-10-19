# Building DQ9-Decomp

The SDL host and Godot 4.5 GDExtension share the original native runtime. This repository distributes sources and tests only: no game data, generated game sources, or binaries. A local build-from-your-own-copy script (`tools/build_port.py`) is provided but currently yields a host that does not boot; see [Getting started](GETTING_STARTED.md). A game host needs a separately supplied generated-code directory.

For Windows native/cross-builds, see [Building on Windows](BUILDING_WINDOWS.md). Build outputs, saves, captures, and packages stay local; game-host packages (which include code generated from the game) must never be uploaded or distributed.

## Runtime-only prerequisites

- A C++20 compiler, CMake 3.20+, and Ninja or Make.
- SDL2 development files for the default SDL/runtime-test configuration; pkg-config is used when available.
- Python 3.10+ for tools and tests. Use a local virtual environment with PyYAML and Capstone for the tools; Unicorn is needed for the optional differential tests.
- For the SWI differential test's external reference-table mode, a separately available reference-emulator source tree. `runtime/core/CMakeLists.txt` accepts `DQ9_BIOS_REFERENCE_SOURCE` pointing to public DeSmuME HLE `src/bios.cpp`. This is not a Nintendo BIOS and is not bundled. Without that option, the public test uses independently computed formula checks instead.

Without `DQ9_GENERATED_DIR`, CMake builds runtime libraries/tests, not a playable game:

```sh
cmake -S . -B build/runtime -DCMAKE_BUILD_TYPE=Debug
cmake --build build/runtime
ctest --test-dir build/runtime --output-on-failure
```

Runtime-only CTest does not need game data. External reference-table comparisons still require the locally available HLE source described above; enable them with `-DDQ9_BIOS_REFERENCE_SOURCE="/absolute/path/desmume/src/bios.cpp"` when configuring. Tests that depend on local images, generated code, an external emulator, or Godot have additional prerequisites; do not represent skipped/unavailable tests as passes. Successful runtime tests do not establish complete game behavior.

## Build a local SDL host

Select the separately supplied generated-code directory at configure time:

```sh
cmake -S . -B build/game-ui -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDQ9_BUILD_SDL=ON -DDQ9_BUILD_GODOT=OFF \
  -DDQ9_GENERATED_DIR="/absolute/path/generated-code"
cmake --build build/game-ui --target dq9
```

`tools/build_port.py` can produce that directory, but the result does not yet reach any game output (blank screens; cause not yet identified). Keep it and the resulting executable private. Optional ccache/compiler/linker choices are local development settings, not performance guarantees.

## Godot prerequisites and build

Use the Godot 4.5 editor/runner and matching external `godot-cpp` bindings, tag `godot-4.5-stable`. Neither is bundled. Set `GODOT` to the editor executable and `GODOT_CPP_DIR` to the bindings checkout. See the [Godot 4.5 archive](https://godotengine.org/download/archive/4.5-stable/) and [binding sources](https://github.com/godotengine/godot-cpp/tree/godot-4.5-stable) for these development dependencies.

```sh
cmake -S . -B build/godot-ext -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDQ9_BUILD_SDL=OFF -DDQ9_BUILD_GODOT=ON \
  -DDQ9_GENERATED_DIR="/absolute/path/generated-code" \
  -DGODOT_CPP_DIR="$GODOT_CPP_DIR"
cmake --build build/godot-ext
```

The extension build updates `godot/bin/`. On Linux, Godot bindings and `dsr_session` must use the same shared C++ runtime; mixing static/shared runtime copies can break locale/stream operations. Windows toolchain behavior is covered in the Windows guide.

`tools/build_both.sh` is a convenience wrapper for already prepared local host inputs. It selects separate SDL/Godot build trees; `--sdl-only`, `--godot-only`, and `--test` choose hosts/checks. It is not a game generator. Consult its help for the current path options.

## Run with an existing local image

Select an already locally available compatible image via `--rom` or `DQ9_ROM`. Select a writable save path separately. Do not copy an image into the project or package.

Title-specific runtime metadata is also caller-supplied. `DQ9_ABI_CONFIG` selects a local INI file for both hosts; SDL additionally accepts `--abi-config PATH`. The public field names are defined in `runtime/include/dsr/abi_config.h`. A supplied CPU section must include every field, including intentional zeros; omitting a CPU section disables its guest-thread overrides with a diagnostic. Optional cartridge backup settings use the same file. No title ABI layout or values are shipped.

```sh
build/game-ui/game/dq9 --rom "$DQ9_ROM" --save "$DQ9_SAVE"
# SDL path/settings panel:
build/game-ui/game/dq9 --launcher
# Godot path/settings panel:
"$GODOT" --path godot -- --menu
# Godot direct start:
"$GODOT" --path godot -- --rom "$DQ9_ROM" --save "$DQ9_SAVE" --autostart
```

The source wrappers `tools/run_sdl.sh --menu` and `tools/run_godot.sh --menu` also open the host panels. A missing image or unavailable native extension is an error, not a substitute game.

Godot keyboard defaults are A/B = Z/X, X/Y = A/S, Start/Select = Enter/Backspace, D-pad = arrows, and L/R = E/Q. Click the displayed bottom screen for touch. In Merged Overlay, Alt + left drag moves the bottom screen and Alt + right drag resizes it. F11 toggles fullscreen; F10 or Escape opens pause/debug. Settings/rebinding remain host-only and do not alter simulation.

Layouts preserve each screen's native 4:3 aspect ratio. Nearest filtering is the default; smooth filtering is an explicit choice. Language/settings changes are applied on start/restart. Use distinct writable save paths for diagnostic runs to preserve normal saves.

## Host and rendering tests

Godot software/host tests require the editor, matching extension, and any test-specific local inputs. Rendered shader tests require a real rendering backend; Godot `--headless` alone has no usable viewport output. The original synthetic native rendering fixtures are enabled with `DQ9_GPU_SCENE_TESTS=ON` when configuring the extension.

```sh
"$GODOT" --headless --path godot --script res://tests/test_logic.gd
"$GODOT" --headless --path godot --script res://tests/test_issue_store.gd
xvfb-run -a "$GODOT" --path godot --script res://tests/gpu3d_poc.gd -- \
  "$PWD/build/gpu3d-poc"
```

Graphical host QA additionally needs an X server and `xdotool`. `test_end_to_end.gd` takes caller-supplied `--qa-frames` and `--qa-checkpoints` along with local image/save/input/output paths. GPU scene/visible probes likewise require explicit local frame/checkpoint selections; no game-specific route is shipped in these instructions. See [shader notes](GPU3D_SHADER_NOTES.md) and [launcher/debug privacy rules](LAUNCHER_AND_DEBUG.md).

Keep all gameplay screenshots, saves, texture/memory dumps, audio, input captures, and debug bundles private. Redact a tooling issue description before posting; never attach complete bundles publicly.

## Local-only export

`godot/export_presets.cfg` defines a Linux/X11 preset. With matching Godot 4.5 export templates installed, a local export can be created through Project → Export or:

```sh
"$GODOT" --headless --path godot \
  --export-release "Linux/X11" build/dq9-godot.x86_64
```

The matching GDExtension must be available as a project resource. A local export still selects an existing image and writable save; it must not bundle either. Exports and compiled game packages are local-only and must never be uploaded, including to CI, releases, or public issue attachments.
