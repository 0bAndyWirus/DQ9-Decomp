"""Invented ARM programs only: no title bytes, keys, addresses, or layouts."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

from tools import unwrap_protected_overlay as unwrap


BASE = 0x10000


def branch(source, target):
    return 0xEB000000 | (((target-source-8)//4) & 0xFFFFFF)


def fixture(path, wrapped=False):
    image = bytearray(1024)
    def put(offset, *words):
        struct.pack_into('<'+'I'*len(words), image, offset, *words)
    # An invented argument-saving wrapper around a toy store-based decryptor.
    put(0, 0xE92D00F0, 0xE92D000F, 0xE8BD00F0,
        branch(12, 128), 0xE1A0C000, 0xE12FFF3C,
        branch(24, 192), 0xE12FFF1E)
    # Return the body pointer after writing an invented plaintext instruction.
    put(128, 0xE59F000C, 0xE59F100C, 0xE5801000, 0xE12FFF1E,
        0xE1A00000, BASE+512, 0xE3A00007)
    put(192, 0xE12FFF1E)
    put(512, 0xDEADBEEF)
    ctor_start = ctor_end = BASE+768
    if wrapped:
        put(0, 0)
        put(256, 0xE92D4000, 0xE28F0004, branch(264, 320), 0xE8BD8000)
        put(320, 0xE59F100C, 0xE59F200C, 0xE5812000, 0xE12FFF1E,
            0xE1A00000, BASE, 0xE92D00F0)
        put(768, BASE+256)
        ctor_end += 4
    path.write_bytes(image)
    module = dict(name='sample', module_id=7, base=BASE, image=str(path), functions=[],
                  data_ranges=[], code_ranges=[], labels=[], calls=[])
    metadata = dict(id=7, ctor_start=ctor_start, ctor_end=ctor_end)
    return module, metadata


class UnwrapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def test_resident_wrapper_executes_derived_decryptor(self):
        module, meta = fixture(self.root/'image.bin')
        resident, plain, stubs, protected, helpers = unwrap.unwrap_module(module, meta)
        self.assertEqual(protected, [(BASE+512, 4)])
        self.assertEqual(set(stubs), {BASE})
        self.assertEqual(stubs[BASE][-1].address+4-BASE, 32)
        self.assertEqual(unwrap.word(resident, BASE, BASE+512), 0xDEADBEEF)
        self.assertEqual(unwrap.word(plain, BASE, BASE+512), 0xE3A00007)
        self.assertEqual(helpers, {BASE+128, BASE+192})

    def test_constructor_unwraps_before_discovery(self):
        module, meta = fixture(self.root/'image.bin', wrapped=True)
        resident, plain, stubs, protected, helpers = unwrap.unwrap_module(module, meta)
        self.assertEqual(set(stubs), {BASE})
        self.assertEqual(unwrap.word(resident, BASE, BASE), 0xE92D00F0)
        self.assertIn(BASE+320, helpers)
        self.assertEqual(protected, [(BASE+512, 4)])

    def test_dsd_plaintext_is_not_decrypted_twice(self):
        module, meta = fixture(self.root/'image.bin')
        meta['dsprot'] = {'unencrypted': {'functions': [
            {'address': BASE+512, 'size': 4, 'encryption': {'keyed': 123}}]}}
        result = unwrap.unwrap_module(module, meta)
        self.assertEqual(result[1], Path(module['image']).read_bytes())

    def test_disagreeing_plaintext_metadata_fails(self):
        module, meta = fixture(self.root/'image.bin')
        meta['dsprot'] = {'unencrypted': {'functions': []}}
        with self.assertRaisesRegex(unwrap.DerivationError, 'metadata disagrees'):
            unwrap.unwrap_module(module, meta)

    def test_manifest_uses_discovered_entries_and_fresh_id(self):
        module, meta = fixture(self.root/'image.bin')
        manifest = {'modules': [module]}
        self.assertEqual(unwrap.rewrite_manifest(manifest, {'overlays':[meta]}, self.root/'out'), 1)
        self.assertEqual(len(manifest['modules']), 2)
        plain = manifest['modules'][-1]
        self.assertEqual(plain['module_id'], 8)
        self.assertEqual(plain['functions'][0]['size'], 4)
        self.assertEqual(module['functions'][0]['size'], 32)
        self.assertTrue(Path(plain['image']).exists())
        self.assertEqual(module['data_ranges'], [])

    def test_no_derived_wrappers_is_explicit_failure(self):
        image = self.root/'empty.bin'
        image.write_bytes(bytes(64))
        module = dict(name='empty', module_id=7, base=BASE, image=str(image), functions=[])
        meta = dict(id=7, ctor_start=BASE, ctor_end=BASE)
        with self.assertRaisesRegex(unwrap.DerivationError, 'no protected overlay'):
            unwrap.rewrite_manifest({'modules':[module]}, {'overlays':[meta]}, self.root/'out')

    def test_outside_pointer_fails_before_unpacking(self):
        with self.assertRaisesRegex(unwrap.DerivationError, 'outside aligned'):
            unwrap.word(bytes(4), BASE, BASE+4)

    def test_bounded_guest_reports_external_branch(self):
        image = struct.pack('<I', 0xE12FFF10) # bx r0 (unmapped, invented destination)
        with self.assertRaisesRegex(unwrap.DerivationError, 'execution failed|outside'):
            unwrap.Guest(image, BASE).run(BASE)

    def test_cli_rewrites_manifest_with_explicit_table(self):
        module, meta = fixture(self.root/'image.bin')
        manifest = self.root/'manifest.json'
        manifest.write_text(json.dumps({'modules':[module]}))
        table = self.root/'overlays.json'
        table.write_text(json.dumps({'overlays':[meta]}))
        self.assertEqual(unwrap.main(['--manifest', str(manifest), '--out', str(self.root/'out'),
                                      '--overlays', str(table)]), 0)
        self.assertEqual(len(json.loads(manifest.read_text())['modules']), 2)


if __name__ == '__main__':
    unittest.main()
