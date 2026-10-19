"""Synthetic ARM encodings only; no extracted instructions or title layouts."""
import contextlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import capstone
from tools import derive_abi_config as abi


# Small ARM instruction constructors, with invented register-frame layouts.
def add(dst, src, immediate):
    return 0xE2800000 | src << 16 | dst << 12 | immediate


def mov(dst, value):
    return 0xE3A00000 | dst << 12 | value


def load(dst, base, offset=0, half=False):
    if half:
        return 0xE1D000B0 | base << 16 | dst << 12 | (offset & 0xF0) << 4 | offset & 15
    return 0xE5900000 | base << 16 | dst << 12 | offset


def store(src, base, offset=0):
    return 0xE5800000 | base << 16 | src << 12 | offset


def function(words, address=0x10000000, module=-12):
    image = struct.pack('<' + 'I' * len(words), *words)
    decoder = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    decoder.detail = True
    return abi.Function(address, module, address, image, list(decoder.disasm(image, address)))


def saved_frame():
    # CPSR precedes the register array by a deliberately invented distance.
    return function([add(1, 0, 12), 0xE10F2000, 0xE4812008,
                     store(13, 1, 84), mov(0, 1), 0xE8817FFF,
                     add(0, 15, 16), store(0, 1, 80), mov(0, 0), 0xE12FFF1E])


def scheduler():
    words = [load(12, 15), mov(3, 0), load(12, 12), mov(2, 7),
             store(2, 12, 108), store(3, 12, 124), store(3, 12, 128),
             load(0, 12, 132), store(3, 12, 132),
             load(12, 15), store(3, 12, 16), store(3, 12, 20),
             load(12, 15), load(1, 12, 24, half=True), add(2, 12, 32),
             load(0, 1, 108, half=True), 0xE3500007, load(1, 1, 112),
             load(3, 12, 36), 0xE1A0E00F, 0xE12FFF13, store(1, 12, 28)]
    queue_index = len(words)
    words.extend([0x11000100, 0x11000200])
    for index, literal_index in [(0, queue_index), (9, queue_index), (12, queue_index + 1)]:
        words[index] = load(12, 15, (literal_index - index - 2) * 4)
    return function(words)


class ContextPatterns(unittest.TestCase):
    def test_save_derives_nondefault_layout(self):
        fn = saved_frame()
        result = abi.context_save(fn)
        self.assertEqual(result['save'], fn.address)
        self.assertEqual(result['save_module'], -12)
        self.assertEqual(result['context_cpsr_offset'], 12)
        self.assertEqual(result['context_regs_offset'], 20)
        self.assertEqual(result['context_pc_offset'], 100)
        self.assertEqual(result['context_svc_sp_offset'], 104)
        self.assertEqual(result['continuation'], fn.address + 48)

    def test_restore_requires_matching_layout(self):
        saved = abi.context_save(saved_frame())
        restore = function([load(13, 0, 84), load(14, 0, 80),
                            0xE8D07FFF, 0xE25EF008], address=0x12000000)
        result = abi.context_restore(restore, saved)
        self.assertEqual(result['restore'], restore.address)
        self.assertEqual(result['continuation_bias'], 8)
        self.assertIsNone(abi.context_restore(restore, dict(saved, context_pc_offset=99)))

    def test_unrelated_and_ambiguous_functions_fail(self):
        self.assertIsNone(abi.context_save(function([mov(0, 0), 0xE12FFF1E])))
        with self.assertRaisesRegex(abi.DerivationError, 'found 2'):
            abi.derive([saved_frame(), saved_frame()])

    def test_scheduler_derives_invented_values(self):
        fn = scheduler()
        result = abi.irq_layout(fn)
        self.assertIsNotNone(result)
        for field, value in dict(alarm_queue=0x11000100, thread_info=0x11000200,
                                 state_offset=108, ready_state=7, list_next_offset=112,
                                 alarm_blocked_queue_offset=124, alarm_prev_offset=128,
                                 alarm_next_offset=132, alarm_head_offset=16, alarm_tail_offset=20,
                                 info_flag_offset=24, info_list_offset=32, info_active_offset=28,
                                 info_callback_offset=36, irq_callback_lr=fn.address + 84).items():
            self.assertEqual(result[field], value, field)

    def test_disabled_math_does_not_invent_offsets(self):
        self.assertEqual(abi.math_layout(saved_frame(), []), {'math_state_enabled': 0})

    def test_derive_applies_only_explicit_disabled_feature_policies(self):
        fn = saved_frame()
        for enabled in (0, 1):
            math_fields = dict(math_state_enabled=enabled)
            if enabled:
                math_fields.update(context_math_offset=120, math_div_numer_offset=8,
                                   math_sqrt_param_offset=24, math_div_control_offset=36,
                                   math_sqrt_control_offset=42)
            with patch.object(abi, 'context_restore', return_value={}), patch.object(
                abi, 'irq_layout', return_value={}
            ), patch.object(abi, 'math_layout', return_value=math_fields):
                result, missing = abi.derive([fn])
            self.assertEqual(result['stack_top_offset'], 0)
            self.assertEqual(result['stack_bottom_offset'], 0)
            for field in ('context_math_offset', 'math_div_numer_offset',
                          'math_sqrt_param_offset', 'math_div_control_offset',
                          'math_sqrt_control_offset'):
                self.assertEqual(result[field], math_fields[field] if enabled else 0)
                self.assertNotIn(field, missing)
            self.assertIn('irq_switch', missing)

    def test_successful_output_documents_policy(self):
        values = {field: 0 for field in abi.FIELDS}
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'abi.ini'
            with patch.object(abi, 'load_functions', return_value=[]), patch.object(
                abi, 'derive', return_value=(values, {})
            ), contextlib.redirect_stdout(io.StringIO()) as output:
                result = abi.main(['--out', str(out)])
            self.assertEqual(result, 0)
            self.assertIn('disables stack checks', out.read_text())
            self.assertIn('unused math-layout offsets are explicitly 0', out.read_text())
            self.assertIn(str(out.resolve()), output.getvalue())

    def test_unmapped_literal_is_rejected(self):
        fn = function([load(0, 15, 100)])
        with self.assertRaisesRegex(abi.DerivationError, 'outside module'):
            fn.literal(fn.instructions[0])


class CommandContract(unittest.TestCase):
    def test_failure_reports_missing_fields_without_ini(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            out, report = root / 'abi.ini', root / 'report.json'
            with patch.object(abi, 'load_functions', return_value=[]), patch.object(
                abi, 'derive', return_value=({'save': 123}, {'stack_top_offset': 'unobservable policy'})
            ), contextlib.redirect_stderr(io.StringIO()) as error:
                result = abi.main(['--out', str(out), '--report', str(report)])
            self.assertEqual(result, 1)
            self.assertFalse(out.exists())
            self.assertIn('arm9.stack_top_offset', error.getvalue())
            self.assertEqual(json.loads(report.read_text())['arm7']['values']['save'], 123)

    def test_failure_does_not_replace_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'abi.ini'
            out.write_text('previous configuration')
            with patch.object(abi, 'load_functions', side_effect=abi.DerivationError('missing pattern')):
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(abi.main(['--out', str(out)]), 1)
            self.assertEqual(out.read_text(), 'previous configuration')

    def test_manifest_names_do_not_supply_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / 'synthetic.bin'
            image.write_bytes(struct.pack('<I', mov(0, 0)))
            manifest = root / 'manifest.json'
            manifest.write_text(json.dumps({'cpu': 'arm7', 'modules': [
                {'name': 'invented', 'module_id': -17, 'base': 0x10000000, 'image': str(image),
                 'functions': [{'addr': 0x10000000, 'size': 4, 'thumb': False, 'name': 'SaveContext'}]}
            ]}))
            functions = abi.load_functions(manifest, root)
            self.assertEqual(len(functions), 1)
            with self.assertRaisesRegex(abi.DerivationError, 'context save'):
                abi.derive(functions)


if __name__ == '__main__':
    unittest.main()
