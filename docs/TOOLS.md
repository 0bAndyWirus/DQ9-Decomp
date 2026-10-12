# Tools reference

Exit codes are CLI process statuses. `0` means success unless a tool documents another result; tools without documented result codes generally use nonzero for an error. “Safe during active integration” means the tool does not mutate the EU workspace or `extracted/`; tools that read live EU state to generate output are marked **No** where an integration can make that result inconsistent.

| Script | Purpose | Exit-code convention | Safe during active integration? |
|---|---|---|---|
| `tools/align_hashes.py` | Align native and oracle hash timelines with a piecewise frame offset. | `0` success; nonzero on invalid input/error. | Yes |
| `tools/apply_source_defines.py` | Prepend the `#define` aliases `port_eu_unit.py` requires to candidate sources, rebuild the objects, keep the change only when the integrator preflight then accepts the candidate (otherwise restore source and object byte for byte). | `0` all accepted; `1` some not; `2` bad input or lock held. | No (edits candidate sources/objects; holds the integration lock) |
| `tools/arm7_analyze.py` | Discover ARM7 code and autoload regions from an extracted image. | `0` success; `2` analysis/input error. | Yes |
| `tools/asm_exact_run.py` | Generate candidate sources and verify through caller-supplied Python scripts selected with `--helper-main` / `DQ9_HELPER_MAIN` and `--helper-overlay` / `DQ9_HELPER_OVERLAY`; no helper-path defaults. See the command contract below. | `0` run completed (per-target outcomes in TSV); `2` invalid/missing helper; nonzero on other input/process errors. | No (reads workspace state and writes candidate sources/manifests) |
| `tools/audit_package.py` | Heuristically flag suspicious filenames, extensions, sizes, and generated-source signatures in local packages. It is not a content-completeness check or legal/safety certification. | `0` no heuristic findings; `1` flagged content; `2` unreadable input. | Yes |
| `tools/author_targets.py` | Build author work batches from a fresh decomp queue, excluding functions already covered by a candidate or integrated manifest, the project skip list and (optionally) previously parked functions; reads the arm/thumb mode from the DSD symbols. | `0` written; `1` queue stale (`--require-fresh`); `2` bad input. | Yes (writes only its output JSON) |
| `tools/bench_render.py` | Measure headless speed for explicitly supplied repeatable `--scene NAME:START:END` ranges using required `--image`, `--save`, and `--input-script`. Each timing run starts with a fresh copy of the supplied save. | `0` measured; `2` missing input or invalid CLI; nonzero on runtime error. | Yes |
| `tools/build_deck.sh` | Build the Steam Deck bundle in the Steam Runtime SDK container. | `0` success; otherwise build/container/smoke failure status. | Yes |
| `tools/build_both.sh` | Configure/build the SDL app (`build/game-ui`) and Godot GDExtension (`build/godot-ext`); `--test` runs each selected host's tests. Prints both launch commands. | `0` selected builds/tests pass; nonzero on configure/build/test failure or invalid options. | Yes |
| `tools/godot_smoke.sh` | Compare 3,000 (default) Godot GDExtension frames with native `dq9`, then test pause/step, realtime audio, and clean shutdown. | `0` all checks pass; nonzero on parity/runtime failure. | Yes |
| `godot/tests/audio_run.gd` | Run a 90-second realtime Godot audio measurement; logs per-second emulated FPS, ring fill, generator queue range/average, playback skips, and host frame time. Use a copied save because the session flushes on exit. | `0` completed measurement; nonzero on setup/runtime error. | Yes |
| `tools/build_windows.sh` | Cross-build Windows x64 SDL and/or Godot hosts with Zig and the official SDL2 MinGW SDK; `--test` uses optional Wine. | `0` success; nonzero on configure/build/test failure. | Yes |
| `tools/package_windows.sh` | Stage a local-only Windows host package from an explicit binary/resource whitelist, then heuristically audit the directory and archive. Never upload the result. | `0` staged with no heuristic findings; nonzero on missing files, audit or packaging failure. | Yes |
| `tools/check_coverage.py` | Compare executed guest PCs and instruction modes with a recompiler manifest. | `0` covered; `1` uncovered PCs/mode mismatches; `2` error. | Yes |
| `tools/clean_build.py` | Remove regenerable probe artifacts from the ignored `build/` directory. | `0` dry run or applied cleanup succeeds; nonzero on error. | No |
| `tools/compare_frames.py` | Compare same-size RGB screen captures. | `0` identical; `1` different; `2` error. | Yes |
| `tools/compare_hashes.py` | Compare per-frame screen-hash files and report divergence ranges. | `0` comparison completes; `2` invalid/unreadable input. | Yes |
| `tools/decomp_queue.py` | Build a prioritized queue of unmatched EU ARM9 functions from an Objdiff report. | `0` generated or fresh; `1` `--check` found changed inputs; nonzero on error. | No |
| `tools/debug_bundle.py` | List, show, replay (`--interactive` resumes at the note with a fresh writable save), or export an in-game debug issue bundle. | `0` success; nonzero on invalid bundle, failed replay, or I/O error. | Yes |
| `tools/find_eu_matches.py` | Find compiled ARM ELF functions in raw module images with relocations masked. | `0` success; `2` error. | Yes |
| `tools/ghidra_query.py` | Run read-only Ghidra queries or import/name a cached module using PyGhidra. | `0` success (or non-strict incomplete import); `1` strict import failures; `2` error. | Yes |
| `tools/ghidra/import_module.py` | Ghidra postScript to apply DSD function/label names, analyze, and save an import. | Runs under Ghidra; emits a JSON result rather than selecting a process status. | Yes |
| `tools/ghidra/query_module.py` | Read-only Ghidra postScript for bounded module queries. | Runs under Ghidra; emits query JSON; query errors are raised to the wrapper. | Yes |
| `tools/integrate_batch.py` | Integrate candidate manifest batches and commit after module/report checks. | `0` success; `1` integration error. | No |
| `tools/integrate_candidates.py` | Preflight and integrate compiled candidate units into the EU DSD project. | `0` success/no eligible work; `1` integration error. | No |
| `tools/integration_ledger.py` | Summarize integration runs, retry lists, ETA/watch state, and candidate health. | `0` success; `2` report/input error. | Yes |
| `tools/integration_runner.py` | Run serialized manifest chunks and local verification gates. `--project` selects the caller's workspace; module expectations are derived from config or `--expected-modules`. `--baseline-config` supplies exact approved diagnostic exceptions; without it, checks are strict. Verification inputs use `--rom` and `--rebuilt-image`. | `0` all gates pass; `1` gate failed; `2` bad input; `3` gates pass with chunk failures; `4` stopped by STOP; `5` environment failure. | No |
| `tools/inventory_nds.py` | Report NDS image structure without extracting files or assets. | `0` inventory completed; `2` input error. | Yes |
| `tools/manifest_nds_code.py` | Emit metadata and hashes for local NDS executable modules without extracting them. | `0` manifest completed; `2` input error. | Yes |
| `tools/parity_run.py` | Run the native build with an input script and report frame differences against oracle PNGs. | `0` completed; `1` strict mode found missing captures; `2` native-run error. | Yes |
| `tools/port_eu_unit.py` | Plan or apply DSD config edits that let a compiled unit link as `complete`. | `0` plan/apply success; `1` conflicts or unapproved definitions; `2` error. | No |
| `tools/prepare_dsd_config.py` | Adapt ds-rom extraction metadata for the pinned dsd 0.12.x schema. | `0` config prepared; `2` error. | No |
| `tools/propose_eu_symbols.py` | Propose EU function addresses from raw USA images or compiled ELF objects. | `0` proposals emitted; `2` error. | Yes |
| `tools/queue_freshness.py` | Check whether a generated decomp queue is older than its inputs. A newer input whose size and sha256 equal the queue's recorded fingerprint (`inputs.files`) counts as unchanged, because rollback and `configure` rewrite config files with identical bytes and a new mtime; queues without fingerprints are judged by mtime. | `0` fresh; `1` stale; `2` unknown/unreadable inputs. | Yes |
| `tools/regress_frames.py` | Run native frame regression checks against oracle checkpoints. | `0` pass; `1` regression; nonzero on run/configuration error. | Yes |
| `tools/native_trace_reduce.py` | Reduce a native `DSR_TRACE` row stream (stdin) to unique ARM9 `module_id addr` entries plus a `.meta` with last frame. | `0` success; `2` usage error. | No |
| `tools/refresh_candidates.py` | Recompile candidates with a stale or missing object using the `mwcc` ninja rule's flags, and with `--verify` classify them with the integrator's preflight. | `0` all refreshed (and verified); `1` compile/verify failures; `2` bad input or journal present. | No (writes objects under `build/scratch`; refuses while a journal exists) |
| `tools/rom_compare.py` | Compare a rebuilt NDS image with its reference and classify differences. | `0` identical; `1` other difference/size mismatch; `2` unreadable; `3` only known CRC fields differ. | Yes |
| `tools/scan_upstream_eu_matches.py` | Compile USA upstream units and locate their functions in extracted EU images. | `0` scan completed; `1` compile/scan failures; `2` error. | No |
| `tools/small_function_candidates.py` | Generate conservative C++ candidates for small ARM functions and exact-match them. | `0` no candidate failures; `1` candidate mismatch; `2` error. | Yes |
| `tools/status.py` | Show one-screen project status for report totals, module checksums, and pending state. | `0` report/status succeeds; nonzero on invalid state or command error. | No |
| `tools/stress_recompiler.py` | Generate seeded short ARMv5TE/Thumb-1 sequences and focused instruction forms, compile emitted C++, and compare registers (including user-bank SP/LR), NZCVQ, return PC and memory with Unicorn. `--seed`, `--sequences` and `--states` reproduce/expand a run. No ROM required. | `0` tests pass; `1` differential/test failure; `2` invalid options or unavailable Python dependencies. | Yes |
| `tools/stretch_input.py` | Stretch events and merge overlaps in an input script. | `0` success; nonzero on invalid input. | Yes |
| `tools/symdiff.py` | Compare one function symbol between target and base objects with objdiff. | `0` exact match; `1` mismatch; `2` tool error. | Yes |
| `tools/validate_hashes.py` | Validate per-frame screen-hash files before comparing them. | `0` valid; `1` problems; `2` unreadable file. | Yes |
| `tools/verify_reference_rom.py` | Check an already locally available image selected with `--rom` or `DQ9_ROM` against a caller-supplied private `--profile` JSON. No title fingerprints are bundled. | `0` supplied profile matches; `1` mismatch; `2` invalid/unreadable input or profile. | Yes |
| `tools/oracle/gdbq.py` | Run patched DeSmuME under its GDB stub and emit JSON at CPU breakpoints. | `0` success; `1` debugger/oracle failure. | Yes |
| `tools/oracle/install_call_targets.py` | Install original call-target glue into caller-local emulator source using required `--source` and `--anchors` JSON regex patterns; no emulator statements or anchor defaults are bundled. See `tools/oracle/README.md`. | `0` installed/already installed; `2` input or anchor error. | Yes (edits only the supplied local emulator source) |
| `tools/oracle/run_oracle.py` | Drive the patched local DeSmuME CLI as a deterministic capture oracle. | `0` capture completed; nonzero on oracle/process error. | Yes |

The Ghidra postScripts and oracle drivers are original project glue; external tool sources, game images, generated game code, and analysis outputs remain local-only. This is a component reference, not an end-to-end game-generation workflow. Game hosts require a separately supplied generated-code directory.

Title-specific policies are caller inputs. `--skip-list` / `DQ9_SKIP_LIST` supplies private JSON with `functions` (`module`, `address`), `ranges` (`module`, `start`, `end`), and `name_prefixes`; an absent list is empty and produces a warning. Integration, candidate refresh, and target selection use this shared policy rather than shipping game-specific exclusions. `port_eu_unit.py --keep-configured-image` is repeatable and defaults to no preserved configured images. ARM7 discovery requires caller-supplied load, entry, and callback metadata.

The reference-image profile contains `size`, `product_code`, `revision`, `sha1`, `md5`, and `crc32` expectations. Profile values, local verification hashes, analysis addresses, and generated reports must remain private. Package tools create local-only results: a clean heuristic scan is never permission to upload them.

The stress generator defaults to 192 random sequences per instruction set plus
focused shifter, conditional, DSP/saturation, memory, block-transfer, PC and
interworking specimens, each with 64 initial states. Run
`.venv/bin/python tools/stress_recompiler.py --seed 0xa9467e57 --states 64`.
Unicorn does not implement DS unaligned transfers or ARM register-shifted PC+12 /
the selected PC+12 store behavior: the harness documents those reference
adaptations explicitly. The latter substitutes an unused register containing
PC+12 before Unicorn execution; it does not derive expected values from emitted
code. See [GBATEK's ARM ALU PC rules](https://problemkaputt.de/gbatek-arm-opcodes-data-processing-alu.htm).
ARMv5-unpredictable LDM writeback with the base in its register list, LDM^
writeback, and ARMv6-only halfword T forms are excluded; defined ARM/Thumb
base-in-list forms are included. Thumb-1 has high-register ADD/MOV/CMP, but no
high-register SUB encoding.

Focused regressions cover encoding-owned S-bit flags for ARM ADC/SBC/RSC,
PC+12 for both register-shifted operands and explicit shift aliases, word-aligned
Thumb ADR plus its decoded offset, and callable secondary entries for local BL
subroutines beyond the caller's return.

`asm_exact_run.py` runs helpers with the current Python interpreter from the EU
workspace. The main helper receives `verify STEM` with `DQ_AUTHOR` set to
`--author`; the overlay/ITCM helper receives `verify STEM --write` with
`MEDB_AUTHOR` set. CLI helper paths override their corresponding environment
variables. Only helpers required by the selected shard need be supplied. A
helper must exit `0` only for a 100% relocation-aware, contiguous, correctly
placed match and write `src/_candidates/<AUTHOR>/<STEM>.json` on success.
The runner retains sources only when both the successful exit and manifest are
present; verification does not integrate candidates.
