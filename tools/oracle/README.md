# DQ9-Decomp local capture oracle

These Python tools drive a caller-supplied DeSmuME CLI with capture hooks or a
GDB-enabled CLI. The installer carries no emulator source; anchors are supplied
locally. Input images, symbol exports, and captures are not included. Keep local
inputs, metadata, and generated output in an ignored directory such as `build/`.
Use `--binary` to select your local executable.

## Deterministic captures

The capture executable must implement the `DS_ORACLE_*` hooks used by
`run_oracle.py`. Example invocation:

```sh
.venv/bin/python tools/oracle/run_oracle.py \
  --rom build/local/input.nds --binary build/local-oracle/desmume-cli \
  --frames 100 --snapshots 0,25,50,99 \
  --input build/local/inputs.json --output build/oracle-capture
```

The driver reads the ARM9 entry from the standard ROM header field at offset
`0x24`; `--entry ADDRESS` explicitly overrides it. That entry is always included
in the trace whitelist. `--symbols`, `--arm9-manifest`, and `--arm7-manifest` add
caller-supplied function entries. Symbols are optional; manifests default to
`build/gen/arm9.manifest.json` and `build/gen/arm7.manifest.json`. Missing optional
inputs are listed in the run summary. `--trace-limit` caps function-entry rows.

Input JSON is an array of frame-indexed states. This is an invented example:

```json
[
  {"frame": 0, "buttons": ["START"]},
  {"frame": 10, "buttons": ["A"], "touch": [128, 96]}
]
```

Buttons and touch apply only to the specified frame; omitted frames release
all input. Supported buttons are `A`, `B`, `SELECT`, `START`, `RIGHT`, `LEFT`,
`UP`, `DOWN`, `R`, `L`, `X`, and `Y`. Touch coordinates are bottom-screen pixels
in `[0,255] x [0,191]`.

`--snapshots` accepts comma-separated indices and inclusive `first-last` or
`first-last/step` ranges, all within `0..frames-1`. Its default is
`0,100,300,599`, so shorter captures must supply their own list. `--no-ram`
omits RAM/register snapshots. `--hashes FILE` records per-frame screen hashes;
`--coverage` records unique executed addresses and instruction modes.

### Output formats

- `frame_NNNN_top.png`, `frame_NNNN_bottom.png`: native 256x192 RGB captures.
- `frame_NNNN_mainram.bin`: the emulator's 16 MiB ARM9 main-memory backing store.
- `frame_NNNN_io.bin`: 64 KiB ARM9 register bytes followed by 64 KiB ARM7 register bytes.
- `calls.txt`: `frame cpu pc r0 r1 r2 r3 sp lr`. Frame is decimal, CPU is `0`
  (ARM9) or `1` (ARM7), and other fields are hexadecimal without `0x`. These are
  entry-address observations, not proof that a branch instruction was executed.
- `entries.txt`, `inputs.txt`: generated control files.
- `pcs_arm9.txt`, `pcs_arm7.txt`: optional sorted `addr_hex mode` rows; mode is
  `a` for ARM or `t` for Thumb.
- Hash output: `frame top_fnv1a64 bottom_fnv1a64`, calculated over RGB screen bytes.
- `--call-targets FILE`: with the corresponding call-target hook installed,
  decoder-derived `cpu addr mode bytes32 source frame` rows. CPU and frame are
  decimal; addresses and live target fingerprints are hexadecimal. The hook
  observes taken calls rather than the entry whitelist.

Captures and traces describe only the supplied input and exercised execution.
For frame comparisons, use the same inputs and emulator configuration; loading
or animation timing can differ even when rendering is equivalent.

### Local call-target hook installation

`install_call_targets.py` adds the original `call_targets.h` glue to a locally
supplied emulator source file. Supply both the source and a private anchor file:

```sh
.venv/bin/python tools/oracle/install_call_targets.py \
  --source build/local-oracle/src/armcpu.cpp --anchors build/local/oracle-anchors.json
```

The JSON object must have exactly four keys: `include`, `trace`, `arm`, and
`thumb`. Each value is a Python regular-expression string matching exactly one
complete source line; matching uses `re.MULTILINE`. JSON backslashes must be
escaped. Derive these patterns from your local emulator version; no defaults
are provided. Patterns must be nonempty and their matches must not overlap.

The installer preserves each matched line and appends the header after
`include`, call classification/source state after `trace`, and call recording
after `arm` and `thumb`, preserving the matched indentation. Choose `trace`
at the existing instruction-entry trace point and the execution anchors just
after ARM and Thumb instruction execution, where the glue's CPU and frame
variables are available. A missing or ambiguous anchor aborts without changing
the source. Repeating installation with the same header is a no-op; a different
header path is rejected. Rebuild your local emulator after installation.


## GDB probes

`gdbq.py` starts a GDB-enabled CLI with `--start-paused`, uses `xvfb-run` when
available, and otherwise selects SDL's dummy video driver. All addresses in the
following example are synthetic demonstration values, not executable locations
in any supplied image. Replace them with addresses from your own metadata.

```sh
.venv/bin/python tools/oracle/gdbq.py \
  --rom build/local/input.nds --binary build/local-oracle/desmume-cli-gdb \
  --cpu arm9 --port 3333 --break 0x08000000,0x08000010 \
  --regs --mem 0x09000000:16
```

`--break` takes comma-separated addresses. `--watch-write ADDR:LENGTH` adds a
write watchpoint, `--mem ADDR:LENGTH` is repeatable, and `--max-hits` limits stops.
Each hit is a JSON line with the stopped address, stop reply, optional registers,
and requested memory as hex. The emulator is terminated after the probe. This
live probe does not apply frame-indexed JSON input scripts.

## ARM7 analysis metadata

`tools/arm7_analyze.py` requires `--base`, `--entry`, and
`--autoload-callback`, either explicitly or through caller-local `--metadata`
JSON. Explicit CLI values override metadata. The tool locates autoload
parameters structurally in the supplied image; it does not encode routine
identities, extra roots, or startup data ranges.

Optional metadata fields are `extra_roots` (objects with `address`, optional
`thumb`, and optional `reason`), `data_ranges` (objects with `start`, exclusive
`end`, and optional `reason`), and `routine_names` (an address-to-name mapping).
Addresses may be JSON integers or integer strings. An address's low bit also
selects Thumb state for roots. The Python `analyze_image` API requires the three
layout values and accepts `extra_seeds`, `data_ranges`, and `routine_names`.
Autoload module names are derived from their destinations; module IDs start at
`-5` and follow descriptor order, with `-4` reserved for the main image.
