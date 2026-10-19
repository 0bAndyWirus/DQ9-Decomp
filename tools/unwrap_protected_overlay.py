#!/usr/bin/env python3
"""Derive resident and callable overlay images from the user's extracted code.

No cipher keys, title addresses, instruction signatures, or protection offsets
are supplied here. Capstone identifies constructor wrappers and argument-saving
call stubs; bounded Unicorn execution evaluates the user's own transform code,
including its literal loads, descriptor walks, key schedule, and branch fixes.
Only local overlay RAM and a private scratch stack are mapped. Derived images
and the rewritten manifest belong in the ignored build directory, never a release.
"""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
from capstone.arm import ARM_OP_IMM, ARM_OP_REG
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import arm_const
import yaml


class DerivationError(ValueError):
    """The input does not establish a unique, executable protection structure."""


def word(data: bytes, base: int, address: int) -> int:
    if address < base or address + 4 > base + len(data) or address & 3:
        raise DerivationError("descriptor or constructor pointer outside aligned overlay data")
    return struct.unpack_from('<I', data, address - base)[0]


def decoder() -> Cs:
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    return md


def stub_instructions(data: bytes, base: int, address: int):
    """Recognise save-arguments / decrypt / indirect-call / restore wrappers."""
    md = decoder()
    offset = address - base
    prefix = list(md.disasm(data[offset:offset+12], address))
    if len(prefix) != 3:
        return None
    a, b, c = prefix
    if (a.mnemonic, b.mnemonic, c.mnemonic) != ('push', 'push', 'pop'):
        return None
    if a.op_str != c.op_str or b.op_str != '{r0, r1, r2, r3}':
        return None
    instructions = []
    for ins in md.disasm(data[offset:offset+512], address):
        instructions.append(ins)
        if ins.mnemonic == 'bx' and ins.op_str == 'lr':
            break
    if len(instructions) < 6:
        return None
    indirect = [i for i in instructions if i.mnemonic == 'blx' and i.operands[0].type == ARM_OP_REG]
    direct = [i for i in instructions if i.mnemonic == 'bl' and i.operands[0].type == ARM_OP_IMM]
    if len(indirect) != 1 or len(direct) != 2 or not direct[0].address < indirect[0].address < direct[1].address:
        return None
    if instructions[-1].mnemonic != 'bx' or instructions[-1].op_str != 'lr':
        return None
    return instructions


class Guest:
    """Bounded, isolated evaluation of local ARM transform routines."""
    STACK = 0x10000000
    STACK_SIZE = 0x10000
    LIMIT = 2_000_000

    def __init__(self, data: bytes, base: int):
        self.base, self.size = base, len(data)
        lo = base & ~4095
        hi = (base + len(data) + 4095) & ~4095
        if not (hi <= self.STACK or lo >= self.STACK + self.STACK_SIZE):
            raise DerivationError('overlay overlaps the isolated scratch stack')
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.mem_map(lo, hi - lo)
        self.uc.mem_write(base, data)
        self.uc.mem_map(self.STACK, self.STACK_SIZE)
        self.md = decoder()
        self.writes = set()
        self.write_origins = {}
        self.calls = set()
        self.executed = set()
        self.error = None
        self.stop_at = None
        self.stopped = False
        self.uc.hook_add(UC_HOOK_CODE, self._code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._write)

    def _write(self, uc, access, address, size, value, user):
        if self.base <= address and address + size <= self.base + self.size:
            self.writes.update(range(address, address + size))
            pc = uc.reg_read(arm_const.UC_ARM_REG_PC)
            for at in range(address, address + size):
                self.write_origins.setdefault(at, set()).add(pc)
        elif not self.STACK <= address < address + size <= self.STACK + self.STACK_SIZE:
            self.error = 'transform writes outside overlay and scratch stack'
            uc.emu_stop()

    def _code(self, uc, address, size, user):
        if address == self.stop_at:
            self.stopped = True
            uc.emu_stop()
            return
        if not self.base <= address < self.base + self.size:
            self.error = 'transform branches outside its overlay'
            uc.emu_stop()
            return
        ins = next(self.md.disasm(bytes(uc.mem_read(address, size)), address), None)
        if ins is None:
            self.error = 'transform contains an undecodable ARM instruction'
            uc.emu_stop()
            return
        self.executed.add(address)
        if ins.mnemonic == 'bl' and ins.operands[0].type == ARM_OP_IMM:
            self.calls.add(ins.operands[0].imm)
        # Cache maintenance has no semantic effect on this isolated byte image.
        if ins.mnemonic == 'mcr' and ins.op_str.startswith('p15,') and ', c7,' in ins.op_str:
            uc.reg_write(arm_const.UC_ARM_REG_PC, address + size)

    def run(self, entry: int, stop_at: int | None = None):
        self.stop_at = self.STACK if stop_at is None else stop_at
        self.uc.reg_write(arm_const.UC_ARM_REG_SP, self.STACK + self.STACK_SIZE - 16)
        self.uc.reg_write(arm_const.UC_ARM_REG_LR, self.STACK)
        try:
            self.uc.emu_start(entry, self.stop_at if stop_at is None else self.STACK + 4, count=self.LIMIT)
        except UcError as exc:
            raise DerivationError(f'guest transform execution failed: {exc}') from exc
        if self.error:
            raise DerivationError(self.error)
        if self.uc.reg_read(arm_const.UC_ARM_REG_PC) != self.stop_at:
            raise DerivationError('guest transform did not reach its structural return/call boundary')

    def image(self) -> bytes:
        return bytes(self.uc.mem_read(self.base, self.size))


def constructor_entries(data: bytes, base: int, metadata: dict) -> list[int]:
    start, end = metadata['ctor_start'], metadata['ctor_end']
    if start == end:
        return []
    entries = [word(data, base, at) for at in range(start, end, 4)]
    return [a for a in entries if a not in (0, 0xFFFFFFFF)]


def unwrap_module(module: dict, metadata: dict):
    base = module['base']
    original = Path(module['image']).read_bytes()
    resident = original
    def find_stubs(image):
        return {base + offset: instructions for offset in range(0, len(image) - 12, 4)
                if (instructions := stub_instructions(image, base, base + offset)) is not None}
    stubs = find_stubs(resident)
    helpers = set()
    constructors = constructor_entries(original, base, metadata)
    if not stubs:
        # Only evaluate constructors whose first basic block forms an inline-list
        # wrapper. Do not execute arbitrary application static initialisers.
        wrappers = []
        targets = set()
        for entry in constructors:
            code = list(decoder().disasm(original[entry-base:entry-base+16], entry))
            if len(code) != 4:
                continue
            a, b, c, d = code
            if (a.mnemonic in ('push', 'stmdb') and 'lr' in a.op_str
                    and b.mnemonic == 'add' and b.op_str.startswith('r0, pc,')
                    and c.mnemonic == 'bl' and c.operands[0].type == ARM_OP_IMM
                    and d.mnemonic in ('pop', 'ldm') and 'pc' in d.op_str):
                wrappers.append(entry)
                targets.add(c.operands[0].imm)
        if not wrappers:
            return None
        if len(targets) != 1:
            raise DerivationError('constructor wrappers disagree on their transform routine')
        guest = Guest(original, base)
        for entry in wrappers:
            guest.run(entry)
        resident = guest.image()
        helpers.update(guest.calls)
        stubs = find_stubs(resident)
        if not stubs:
            raise DerivationError('constructor transforms produced no recognised call stubs')
    plain = bytearray(resident)
    protected = []
    for address, instructions in stubs.items():
        call = next(i for i in instructions if i.mnemonic == 'blx' and i.operands[0].type == ARM_OP_REG)
        guest = Guest(resident, base)
        guest.run(address, call.address)
        reg_name = call.reg_name(call.operands[0].reg).upper()
        register = getattr(arm_const, 'UC_ARM_REG_' + reg_name, None)
        if register is None:
            raise DerivationError('unsupported indirect-call register')
        target = guest.uc.reg_read(register)
        if target not in guest.writes:
            raise DerivationError('decryptor target is not the start of a written function')
        end = target
        while end in guest.writes:
            end += 1
        if target & 3 or (end-target) & 3 or not base <= target < end <= base+len(resident):
            raise DerivationError('decryptor produced an invalid ARM function interval')
        stub_pcs = {i.address for i in instructions}
        if any(not target <= at < end and not origins <= stub_pcs
               for at, origins in guest.write_origins.items()):
            raise DerivationError('decryptor wrote an ambiguous function interval')
        plain[target-base:end-base] = guest.image()[target-base:end-base]
        protected.append((target, end-target))
        helpers.update(guest.calls)
        helpers.update(i.operands[0].imm for i in instructions
                       if i.mnemonic == 'bl' and i.operands[0].type == ARM_OP_IMM)
    if len(set(protected)) != len(protected):
        raise DerivationError('multiple wrappers resolve to the same protected function')
    unencrypted = metadata.get('dsprot', {}).get('unencrypted')
    if unencrypted is not None:
        declared = sorted((f['address'], f['size']) for f in unencrypted['functions']
                          if isinstance(f.get('encryption'), dict) and 'keyed' in f['encryption'])
        if declared != sorted(protected):
            raise DerivationError('DSD plaintext metadata disagrees with executed wrapper intervals')
        # DSD already evaluated both protection layers during extraction. The
        # execution above establishes boundaries, not a second decryption pass.
        plain = bytearray(original)
    return resident, bytes(plain), stubs, sorted(protected), helpers


def rewrite_manifest(manifest: dict, metadata: dict, out: Path) -> int:
    by_id = {m['id']: m for m in metadata['overlays']}
    derived = []
    count = 0
    used_ids = {m['module_id'] for m in manifest['modules']}
    for module in manifest['modules']:
        if module['module_id'] not in by_id:
            continue
        result = unwrap_module(module, by_id[module['module_id']])
        if result is None:
            continue
        resident, plain, stubs, protected, helpers = result
        out.mkdir(parents=True, exist_ok=True)
        name = module['name']
        resident_path, plain_path = out / f'{name}_resident.bin', out / f'{name}_plain.bin'
        resident_path.write_bytes(resident)
        plain_path.write_bytes(plain)
        entries = {f['addr']: f for f in module['functions']}
        for address, instructions in stubs.items():
            entries[address] = dict(addr=address, thumb=False, size=instructions[-1].address+instructions[-1].size-address,
                                    name=f'{name}_stub_{address:x}')
        starts = sorted(set(entries) | helpers)
        for address in helpers:
            if not module['base'] <= address < module['base']+len(resident):
                raise DerivationError('helper target outside overlay')
            end = next((s for s in starts if s > address), module['base']+len(resident))
            entries.setdefault(address, dict(addr=address, thumb=False, size=end-address, name=f'{name}_helper_{address:x}'))
        module.update(image=str(resident_path.resolve()), functions=sorted(entries.values(), key=lambda f:f['addr']), data_ranges=[])
        module['code_ranges'] = [[f['addr'], f['addr']+f['size']] for f in module['functions']]
        new_id = max(used_ids)+1
        used_ids.add(new_id)
        derived.append(dict(name=name+'plain', module_id=new_id, base=module['base'], image=str(plain_path.resolve()),
                            functions=[dict(addr=a, thumb=False, size=n, name=f'{name}_protected_{a:x}') for a,n in protected],
                            labels=[], data_ranges=[], code_ranges=[[a,a+n] for a,n in protected], calls=[]))
        count += len(protected)
    if not derived:
        raise DerivationError('no protected overlay wrappers could be derived from extracted images')
    manifest['modules'].extend(derived)
    return count


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--overlays', type=Path, help='DSD-derived overlay table (default: beside module images)')
    args = parser.parse_args(argv)
    try:
        manifest = json.loads(args.manifest.read_text())
        paths = {Path(m['image']).parent / 'arm9_overlays.yaml' for m in manifest['modules'] if m['module_id'] >= 0}
        tables = [p for p in paths if p.exists()] if args.overlays is None else [args.overlays]
        if len(tables) != 1:
            raise DerivationError('supply --overlays: cannot uniquely locate the extracted overlay table')
        count = rewrite_manifest(manifest, yaml.safe_load(tables[0].read_text()), args.out)
        args.manifest.write_text(json.dumps(manifest, indent=1)+'\n')
    except (DerivationError, OSError, KeyError, ValueError) as exc:
        parser.exit(1, f'protected overlay derivation failed: {exc}\n')
    print(f'protected overlays: {count} callable functions derived; manifest updated')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
