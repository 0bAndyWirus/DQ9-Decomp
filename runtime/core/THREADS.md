# DQ9-Decomp runtime thread overrides

## Caller-supplied ABI

The runtime contains no title-specific thread entry addresses or structure layouts.
`MachineConfig::guest_abi` supplies an optional `ThreadAbi` for each CPU. An
unconfigured CPU keeps its generated guest routines; startup logs that its thread
overrides are disabled, and the IRQ seam does not inspect guest thread structures.

The executable accepts `--abi-config PATH` or `DQ9_ABI_CONFIG`. The convenience
`Session(SessionConfig)` constructor accepts `abi_config_path`, falling back to
`DQ9_ABI_CONFIG` when that path is empty. This is an optional, caller-supplied INI
file, not a file distributed with the project. An omitted path or missing file
disables overrides with a startup log; an existing unreadable or invalid file is
an error.

The file supports `[arm9]`, `[arm7]`, and `[cartridge]` sections. Omit a CPU section
to leave its overrides disabled. In a CPU section every `ThreadAbi` field declared
in `runtime/include/dsr/abi_config.h` is required, including fields deliberately
set to zero. Unsigned fields accept decimal or `0x` hexadecimal values; module
fields accept signed decimal values. Blank lines and `#`/`;` comments are allowed.
Duplicate sections, duplicate keys, unknown keys, and incomplete layouts are errors.

The supplied fields cover:

- Save, restore, and IRQ-switch entries and their generated-table module numbers;
  the saved continuation and callback return address.
- Context CPSR, general-register array, continuation, supervisor SP, and optional
  math-state offsets, plus the continuation bias.
- Thread-information flag, active-context, ready-list, and callback offsets.
- Alarm queue head/tail, thread state, list links, alarm links, and optional stack
  bounds. `ready_state` supplies the runnable-state value.

`[cartridge]` accepts only `backup_type`: `auto`, `eeprom512b`, `eeprom8k`,
`eeprom64k`, `eeprom128k`, `flash256k`, `flash512k`, `flash1m`, or `flash8m`.
`auto` infers capacity from a supported existing save size and otherwise defaults
to flash. There is no cartridge-title signature database.

## Supported override contract

Ordinary ready/blocked scheduling, priorities, and mutexes remain guest code.
The save primitive serializes the guest register frame, associates the running
host fiber with its context, and returns zero. Its saved return value is one;
the saved registers also retain the register-array write cursor and CPSR in the
expected argument registers. General registers use the user/system bank, with
supervisor SP stored separately. The restore primitive resumes a parked fiber
only if its saved continuation and guest SP still match. A reinitialized context
starts a fresh fiber at its saved entry, with its saved LR serving as the exit
procedure. Restoring a saved continuation after its fiber is gone is fatal.

When `math_state_enabled` is one, configured context fields preserve the divider
and square-root register state. The IRQ override wakes alarm waiters, scans the
ready list, calls an optional switch callback, and updates the active context.
IRQ return restores the interrupted CPU frame before parking the outgoing fiber
and resuming the selected context. Reset clears per-CPU fiber/context records.
Destroying a machine restores the generated function slots it replaced, so a
later unconfigured session does not inherit thread overrides.

The synthetic tests use invented addresses and distinct layouts for both CPUs.
They exercise IRQ handoff, repeated yields, termination, reinitialization,
register preservation, and disabled/CPU-selective override installation.

## Stack diagnostics

A zero `stack_top_offset`/`stack_bottom_offset` pair disables bounds checks.
Otherwise saved SP must lie in its inclusive bounds, and distinct live stack
ranges must not overlap. Checks run in debug builds; release builds enable them
with `DSR_CHECK_THREAD_STACKS=1`. Failures print the frame, CPU, event, guest PC,
fiber, context, SP, and stack ranges before aborting.

`DSR_TRACE_THREADS=1` logs context save/restore, IRQ switch, and fiber park/resume
with guest contexts, SP, PC, and host fiber identity. It does not change the ABI.
