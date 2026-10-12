DQ9-Decomp - Windows x64

Requires Windows 10/11 x64 (system Universal CRT).
This file accompanies locally created packages, not a public binary release.
Game-host packages (they include code generated from the game) are local-only and must never be uploaded or distributed, including
through CI, releases, or public issue attachments. This source project ships
no binaries or generated game sources. Keep all session data private.

A local package contains native host binaries, SDL2, launchers and (for the full
package) a Godot project with its Windows GDExtension. It does not include an
image, BIOS, saves, game assets, generated source files, or Godot itself.
Cross-compilation alone does not establish Windows gameplay compatibility.

1. Use a writable <install-dir> for the complete local package. Keep SDL2.dll
   beside dq9.exe.
2. Select an already locally available compatible image with --rom or DQ9_ROM.
   There is no required image folder or filename. From <install-dir>, run:
     run_game.bat --rom "%DQ9_ROM%"
   Or run run_game.bat --menu and select the image's existing local path.
3. An optional language precedes other options, for example:
     run_game.bat fr --rom "%DQ9_ROM%"
   Supported languages are en, fr, de, es and it. Other native options are
   forwarded, for example --headless. Explicit --rom takes precedence over
   DQ9_ROM; explicit --save takes precedence over DQ9_SAVE.
4. Saves default to %APPDATA%\dq9-decomp\dq9.sav, not to <install-dir>.
   Use --save or DQ9_SAVE to override the save path and keep a backup.

DQ9_ABI_CONFIG selects caller-supplied local runtime metadata for both hosts;
SDL also accepts --abi-config PATH. No title-specific ABI layout or values
are supplied. Keep that configuration local alongside other private inputs.

Godot (only included in the full local package):
- Use a locally installed Godot 4.5 stable Windows x64 editor from
  godotengine.org. Set GODOT to its executable.
- From <install-dir>, run:
    run_godot.bat --rom "%DQ9_ROM%"
  Or use run_godot.bat --menu to open settings and Browse for a local image.
- Saves default to %APPDATA%\dq9-decomp\godot.sav. The same --rom/DQ9_ROM
  and --save/DQ9_SAVE selection rules apply.
- Keep godot\bin\dq9_godot.windows.template_release.x86_64.dll and
  dsr_session.dll together. A release extension works in both the editor
  and a release export.
- Export templates are not bundled. For a local standalone Godot executable,
  open godot\project.godot, install the matching 4.5 templates, add Windows
  Desktop x86_64 in Project > Export, and include the GDExtension DLLs.
  Keep the resulting package local; do not upload it.

The Zig build statically links its C++ runtime; no MinGW runtime installation
is needed. Native MSVC builds may require Microsoft's x64 Visual C++
Redistributable.
A generated-game build requires a separately supplied generated-code directory.
For local build configuration and verification limits,
see docs/BUILDING_WINDOWS.md in the source project. Third-party license notices
are in licenses\ in the local package.
