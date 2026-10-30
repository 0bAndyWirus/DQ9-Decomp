# Contributing

Thanks for your interest in DQ9-Remastered, a research project on the European release of *Dragon Quest IX: Sentinels of the Starry Skies*. Read [LEGAL.md](LEGAL.md) and the [project charter](docs/PROJECT_CHARTER.md) first: the data boundary is the most important rule here.

## Welcome contributions

- Original tooling, runtime code and platform adapters.
- Tests built on **synthetic fixtures** that need no game.
- Documentation improvements.
- Reproducible bug fixes.

Open an issue before large or architectural changes. Check the [roadmap](docs/ROADMAP.md) and [architecture](docs/ARCHITECTURE.md) so your change fits the project's direction.

## What must never appear in a PR, issue or CI artifact

- ROMs, ROM bytes, hashes or fingerprints of game images, BIOS files or keys.
- Extracted or generated assets, graphics, audio, scripts, dialogue or text.
- Decompiled or disassembled game code, generated game sources, symbol or address dumps, traces or tables derived from the game.
- Saves, debugger dumps, screenshots or captures of the game.
- Compiled game-host builds or packages. Anything built with generated game code stays local.

Public bug reports must be sanitized: describe the problem with synthetic inputs where possible, and remove copyrighted content, local file paths, usernames and secrets before posting.

## Development setup and tests

Runtime-only setup and test commands (no game data needed; this does not build a playable game) are in the [README quick start](README.md#quick-start-runtime-and-tests-only) and [docs/BUILDING.md](docs/BUILDING.md):

```sh
cmake -S . -B build/runtime -DCMAKE_BUILD_TYPE=Debug
cmake --build build/runtime
ctest --test-dir build/runtime --output-on-failure

python3 -m unittest discover -s tests -v
```

Windows: [docs/BUILDING_WINDOWS.md](docs/BUILDING_WINDOWS.md). Tests that need local images, generated code, an external emulator or Godot are optional probes outside the default suites. Do not report skipped tests as passes.

The Python suite includes an offline Markdown path/heading-anchor check
(`python3 -m unittest discover -s tests -p test_docs_links.py -v`). It ignores
external URLs and fenced command examples; no network access or game inputs are
needed. Keep documentation links and CLI examples in sync with their targets.
Linux runtime and Python CI also run weekly. A newer run of the same pull
request cancels its superseded run; push and scheduled runs are not cancelled.

## Licensing

Contributions must be your own original work, and you must have the right to contribute them. They are licensed under the [MIT License](LICENSE), which covers original project code only, not the game or third-party works. Respect third-party licenses (see [NOTICE](NOTICE)).
