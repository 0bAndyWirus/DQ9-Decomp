#!/usr/bin/env python3
"""Derive a local thread ABI using decoded ARM behavior, never a signature database.

Only the runtime's documented context/IRQ contract is recognized. Ambiguous or
unobservable active fields are errors. Optional stack checks are disabled, and
unused math-layout fields are explicitly zero when math preservation is disabled.
Use --report build/gen/abi-report.json to inspect partial evidence on failure.
All outputs are private, image-derived data and must remain in ignored folders.
"""
from __future__ import annotations

import argparse
import configparser
import json
import sys
from dataclasses import dataclass
from pathlib import Path

import capstone
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG

ROOT = Path(__file__).resolve().parent.parent
FIELDS = (
    'save restore continuation irq_switch save_module restore_module irq_module '
    'thread_info alarm_queue irq_callback_lr context_cpsr_offset context_regs_offset '
    'context_pc_offset context_svc_sp_offset continuation_bias math_state_enabled '
    'context_math_offset math_div_numer_offset math_sqrt_param_offset '
    'math_div_control_offset math_sqrt_control_offset info_flag_offset '
    'info_active_offset info_list_offset info_callback_offset alarm_head_offset '
    'alarm_tail_offset ready_state state_offset list_next_offset alarm_next_offset '
    'alarm_blocked_queue_offset alarm_prev_offset stack_top_offset stack_bottom_offset'
).split()


class DerivationError(ValueError):
    """The image does not uniquely establish the supported ABI contract."""


def reg(ins, operand):
    return ins.reg_name(operand.reg) if operand.type == ARM_OP_REG else None


def immediate(ins, index):
    op = ins.operands[index]
    if op.type != ARM_OP_IMM:
        raise DerivationError('expected immediate operand')
    return op.imm


def memory(ins, base=None):
    operands = [o.mem for o in ins.operands if o.type == ARM_OP_MEM]
    if len(operands) != 1:
        raise DerivationError('expected one memory operand')
    mem = operands[0]
    if mem.index or (base is not None and ins.reg_name(mem.base) != base):
        raise DerivationError('unsupported indexed memory access')
    return mem.disp


def unique(items, role):
    if len(items) != 1:
        raise DerivationError(f'{role}: expected exactly one structural match, found {len(items)}')
    return items[0]


@dataclass
class Function:
    address: int
    module: int
    base: int
    image: bytes
    instructions: list

    def literal(self, ins):
        offset = ins.address + 8 + memory(ins, 'pc') - self.base
        if offset < 0 or offset + 4 > len(self.image):
            raise DerivationError('literal falls outside module image')
        return int.from_bytes(self.image[offset:offset + 4], 'little')


def load_functions(manifest_path, workspace):
    manifest = json.loads(manifest_path.read_text())
    functions = []
    for module in manifest['modules']:
        # ARM9 extraction images are authoritative; generated ARM7 images encode
        # autoload relocation and are explicitly named by the generated manifest.
        name = module['name']
        if manifest['cpu'] == 'arm9' and name in ('main', 'itcm', 'dtcm'):
            image_path = workspace / 'extract' / 'arm9' / ('arm9.bin' if name == 'main' else name + '.bin')
        else:
            image_path = Path(module['image'])
            if not image_path.is_absolute():
                image_path = manifest_path.parent / image_path
        image = image_path.read_bytes()
        for entry in module['functions']:
            if entry.get('thumb'):
                continue
            start = entry['addr'] - module['base']
            size = entry['size']
            if start < 0 or start + size > len(image):
                raise DerivationError('manifest function falls outside module image')
            decoder = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
            decoder.detail = True
            instructions = list(decoder.disasm(image[start:start + size], entry['addr']))
            functions.append(Function(entry['addr'], module['module_id'], module['base'], image, instructions))
    return functions


def context_save(fn):
    """Recognize CPSR + user register-frame serialization and saved PC."""
    ins = fn.instructions
    stores = [i for i in ins if i.mnemonic == 'stm' and len(i.operands) == 16]
    if len(stores) != 1 or not any(i.mnemonic == 'mrs' for i in ins):
        return None
    bulk = stores[0]
    cursor = reg(bulk, bulk.operands[0])
    if {reg(bulk, o) for o in bulk.operands[1:]} != {
        'r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'sb', 'sl', 'fp', 'ip', 'sp', 'lr'
    }:
        return None
    try:
        status = unique([i for i in ins if i.mnemonic == 'str' and i.writeback
                         and reg(i, i.operands[0]) == 'r2' and memory(i, cursor) == 0], 'CPSR store')
        regs_offset = immediate(status, 2)
        base = unique([i for i in ins if i.mnemonic == 'add'
                       and reg(i, i.operands[0]) == cursor
                       and reg(i, i.operands[1]) == 'r0' and i.address < status.address], 'context base')
        cpsr_offset = immediate(base, 2)
        saved_sp = unique([i for i in ins if i.mnemonic == 'str'
                           and reg(i, i.operands[0]) == 'sp'], 'supervisor SP')
        pc = unique([i for i in ins if i.mnemonic == 'add'
                     and reg(i, i.operands[1]) == 'pc'], 'saved continuation')
        pc_store = unique([i for i in ins if i.mnemonic == 'str'
                           and i.address > pc.address
                           and reg(i, i.operands[0]) == reg(pc, pc.operands[0])], 'continuation store')
        return dict(save=fn.address, save_module=fn.module,
                    context_cpsr_offset=cpsr_offset,
                    context_regs_offset=cpsr_offset + regs_offset,
                    context_pc_offset=cpsr_offset + regs_offset + memory(pc_store, cursor),
                    context_svc_sp_offset=cpsr_offset + regs_offset + memory(saved_sp, cursor),
                    continuation=pc.address + 8 + immediate(pc, 2))
    except DerivationError:
        return None


def context_restore(fn, saved):
    ins = fn.instructions
    returns = [i for i in ins if i.mnemonic == 'subs' and i.op_str.startswith('pc, lr,')]
    loads = [i for i in ins if i.mnemonic == 'ldm' and len(i.operands) == 16 and '^' in i.op_str]
    if len(returns) != 1 or len(loads) != 1:
        return None
    try:
        sp = unique([i for i in ins if i.mnemonic == 'ldr' and reg(i, i.operands[0]) == 'sp'], 'restore SP')
        lr = unique([i for i in ins if i.mnemonic == 'ldr' and reg(i, i.operands[0]) == 'lr'], 'restore PC')
        if (memory(sp, 'r0') + saved['context_regs_offset'] != saved['context_svc_sp_offset']
                or memory(lr, 'r0') + saved['context_regs_offset'] != saved['context_pc_offset']):
            return None
        return dict(restore=fn.address, restore_module=fn.module,
                    continuation_bias=immediate(returns[0], 2))
    except DerivationError:
        return None


def irq_layout(fn):
    """Follow the alarm wake loop and ready-list scan, not function names."""
    ins = fn.instructions
    if not ins or ins[0].mnemonic != 'ldr':
        return None
    try:
        queue = fn.literal(ins[0])
        state_store = unique([i for i in ins if i.mnemonic == 'str'
                             and reg(i, i.operands[0]) == 'r2'
                             and any(o.type == ARM_OP_MEM and i.reg_name(o.mem.base) == 'ip' for o in i.operands)], 'wake state')
        state = memory(state_store, 'ip')
        zeros = [i for i in ins if i.mnemonic == 'str' and reg(i, i.operands[0]) == 'r3'
                 and any(o.type == ARM_OP_MEM and i.reg_name(o.mem.base) == 'ip' for o in i.operands)]
        if len(zeros) != 5:
            return None
        next_load = unique([i for i in ins if i.mnemonic == 'ldr'
                            and reg(i, i.operands[0]) == 'r0' and memory(i) == memory(zeros[2], 'ip')], 'alarm next')
        ready = unique([i for i in ins if i.mnemonic == 'ldrh'
                        and reg(i, i.operands[0]) == 'r0' and memory(i, 'r1') == state], 'ready state')
        link = unique([i for i in ins if i.mnemonic == 'ldr'
                       and reg(i, i.operands[0]) == 'r1'
                       and any(o.type == ARM_OP_MEM and i.reg_name(o.mem.base) == 'r1' for o in i.operands)], 'ready link')
        callback = unique([i for i in ins if i.mnemonic == 'ldr' and reg(i, i.operands[0]) == 'r3'], 'callback')
        active = unique([i for i in ins if i.mnemonic == 'str' and reg(i, i.operands[0]) == 'r1'
                         and any(o.type == ARM_OP_MEM and i.reg_name(o.mem.base) == 'ip' for o in i.operands)], 'active context')
        literals = [fn.literal(i) for i in ins if i.mnemonic == 'ldr'
                    and any(o.type == ARM_OP_MEM and i.reg_name(o.mem.base) == 'pc' for o in i.operands)
                    and i.address < ready.address]
        info = unique(list(set(literals) - {queue}), 'thread info')
        flag = unique([i for i in ins if i.mnemonic == 'ldrh' and reg(i, i.operands[0]) == 'r1'], 'reschedule flag')
        head = unique([i for i in ins if i.mnemonic == 'add' and reg(i, i.operands[1]) == 'ip'], 'ready head')
        wake_value = unique([i for i in ins if i.mnemonic == 'mov' and reg(i, i.operands[0]) == 'r2'
                             and i.address < state_store.address], 'wake value')
        compare = unique([i for i in ins if i.mnemonic == 'cmp' and reg(i, i.operands[0]) == 'r0'
                          and ready.address < i.address < link.address], 'ready comparison')
        if immediate(compare, 1) != immediate(wake_value, 1):
            return None
        if not any(i.mnemonic == 'bx' and reg(i, i.operands[0]) == 'r3'
                   and callback.address < i.address < active.address for i in ins):
            return None
        return dict(irq_switch=fn.address, irq_module=fn.module, alarm_queue=queue,
                    thread_info=info, state_offset=state, ready_state=immediate(compare, 1),
                    list_next_offset=memory(link, 'r1'),
                    alarm_blocked_queue_offset=memory(zeros[0], 'ip'),
                    alarm_prev_offset=memory(zeros[1], 'ip'), alarm_next_offset=memory(next_load, 'ip'),
                    alarm_head_offset=memory(zeros[3], 'ip'), alarm_tail_offset=memory(zeros[4], 'ip'),
                    info_flag_offset=memory(flag, 'ip'), info_list_offset=immediate(head, 2),
                    info_active_offset=memory(active, 'ip'), info_callback_offset=memory(callback, 'ip'),
                    irq_callback_lr=active.address)
    except (DerivationError, IndexError):
        return None


def math_layout(save_fn, functions):
    """Symbolically track hardware-to-context transfers in the save helper."""
    prefix = []
    for ins in save_fn.instructions:
        if ins.mnemonic == 'mrs':
            break
        prefix.append(ins)
    calls = [i for i in prefix if i.mnemonic in ('bl', 'blx')]
    if not calls:
        return {'math_state_enabled': 0}
    call = unique(calls, 'math-state save helper')
    setup = unique([i for i in prefix if i.mnemonic == 'add' and reg(i, i.operands[0]) == 'r0'
                    and reg(i, i.operands[1]) == 'r0'], 'math context base')
    literal = unique([i for i in prefix if i.mnemonic == 'ldr'], 'math helper literal')
    target = save_fn.literal(literal)
    helper = unique([f for f in functions if f.module == save_fn.module and f.address == target], 'math helper')
    pointers = {'r0': ('context', 0)}
    values = {}
    transfers = {}
    # Generic Nintendo DS division/square-root register addresses, not guest ABI data.
    hardware = {0x04000290: 'math_div_numer_offset', 0x040002B8: 'math_sqrt_param_offset',
                0x04000280: 'math_div_control_offset', 0x040002B0: 'math_sqrt_control_offset'}
    for ins in helper.instructions:
        ops = ins.operands
        if ins.mnemonic == 'ldr' and len(ops) > 1 and ops[1].type == ARM_OP_MEM and ins.reg_name(ops[1].mem.base) == 'pc':
            pointers[reg(ins, ops[0])] = ('hardware', helper.literal(ins))
        elif ins.mnemonic == 'add' and reg(ins, ops[1]) in pointers:
            kind, off = pointers[reg(ins, ops[1])]
            pointers[reg(ins, ops[0])] = (kind, off + immediate(ins, 2))
        elif ins.mnemonic in ('ldm', 'stm') and reg(ins, ops[0]) in pointers:
            base = reg(ins, ops[0]); kind, off = pointers[base]
            for index, op in enumerate(ops[1:]):
                register = reg(ins, op)
                if ins.mnemonic == 'ldm' and kind == 'hardware':
                    values[register] = off + index * 4
                elif ins.mnemonic == 'stm' and kind == 'context' and register in values:
                    transfers[values[register]] = off + index * 4
            if ins.writeback:
                pointers[base] = (kind, off + (len(ops) - 1) * 4)
        elif ins.mnemonic in ('ldrh', 'strh'):
            mem = next((o.mem for o in ops if o.type == ARM_OP_MEM), None)
            if mem and ins.reg_name(mem.base) in pointers:
                kind, off = pointers[ins.reg_name(mem.base)]
                register = reg(ins, ops[0])
                if ins.mnemonic == 'ldrh' and kind == 'hardware':
                    values[register] = off + mem.disp
                elif ins.mnemonic == 'strh' and kind == 'context' and register in values:
                    transfers[values[register]] = off + mem.disp
        if ins.mnemonic == 'bx':
            break
    if not all(address in transfers for address in hardware):
        raise DerivationError('math helper does not establish every hardware-state field')
    return dict(math_state_enabled=1, context_math_offset=immediate(setup, 2),
                **{name: transfers[address] for address, name in hardware.items()})


def derive(functions):
    saves = [(f, result) for f in functions if (result := context_save(f)) is not None]
    save_fn, result = unique(saves, 'context save')
    restores = [r for f in functions if (r := context_restore(f, result)) is not None]
    result.update(unique(restores, 'context restore'))
    result.update(unique([r for f in functions if (r := irq_layout(f)) is not None], 'IRQ scheduler'))
    result.update(math_layout(save_fn, functions))
    # These are explicit host policies, not inferred guest structure offsets.
    result.update(stack_top_offset=0, stack_bottom_offset=0)
    if result['math_state_enabled'] == 0:
        result.update(context_math_offset=0, math_div_numer_offset=0,
                      math_sqrt_param_offset=0, math_div_control_offset=0,
                      math_sqrt_control_offset=0)
    missing = {field: 'required active field could not be derived'
               for field in FIELDS if field not in result}
    return result, missing


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--workspace', type=Path, default=ROOT / 'extracted' / 'workspace')
    parser.add_argument('--arm9-manifest', type=Path, default=ROOT / 'build/gen/arm9.manifest.json')
    parser.add_argument('--arm7-manifest', type=Path, default=ROOT / 'build/gen/arm7.manifest.json')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/gen/abi.ini')
    parser.add_argument('--report', type=Path, help='optional private JSON evidence, including unresolved fields')
    args = parser.parse_args(argv)
    report = {}
    try:
        for cpu in ('arm9', 'arm7'):
            values, unresolved = derive(load_functions(getattr(args, cpu + '_manifest'), args.workspace))
            report[cpu] = dict(values=values, unresolved=unresolved)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + '\n')
        unresolved = [f'{cpu}.{field}: {reason}' for cpu, evidence in report.items()
                      for field, reason in evidence['unresolved'].items()]
        if unresolved:
            raise DerivationError('cannot derive a complete ABI (no INI written):\n  ' + '\n  '.join(unresolved))
        config = configparser.ConfigParser()
        for cpu, evidence in report.items():
            config[cpu] = {key: str(evidence['values'][key]) for key in FIELDS}
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open('w') as stream:
            stream.write('; Host policy: stack_top_offset = stack_bottom_offset = 0 disables stack checks.\n')
            stream.write('; With math_state_enabled = 0, all unused math-layout offsets are explicitly 0.\n\n')
            config.write(stream)
        print(f'--abi-config {args.out.resolve()}')
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f'ABI derivation failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
