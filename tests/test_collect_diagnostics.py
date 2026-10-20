import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from tools import collect_diagnostics as diagnostics
from tools.audit_package import KIT_BINARY_EXTENSIONS


class DiagnosticsTests(unittest.TestCase):
    def test_redacts_unicode_spaces_bang_and_windows_slashes(self):
        root = Path('/tmp/Élodie !/kit folder !').resolve()
        home = '/home/Élodie !'
        text = f'{root}/build {home}/file ' + r'C:\Users\Élodie !\file C:/Users/Élodie !/file'
        result = diagnostics.redact(text, root, [home, r'C:\Users\Élodie !'])
        self.assertEqual(result, '<KIT>/build <HOME>/file <HOME>\\file <HOME>/file')
        nested = diagnostics.redact_values({'path': str(root / 'build')}, root)
        self.assertEqual(json.loads(json.dumps(nested))['path'], '<KIT>' + os.sep + 'build')

    def test_refuses_shared_forbidden_suffixes_and_save_derivatives(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'out.zip'
            for name in [f'private{suffix.upper()}' for suffix in KIT_BINARY_EXTENSIONS] + [
                    'game.sav.bak', 'game.sav.tmp.42', 'game.dsv.bak', '../escape.txt']:
                with self.subTest(name=name), self.assertRaises(ValueError):
                    diagnostics.write_archive(output, {name: 'secret'})
            self.assertFalse(output.exists())
            with self.assertRaises(ValueError):
                diagnostics.write_archive(output, {'unexpected.txt': 'not allowlisted'})

    def test_main_refusal_is_exit_two_without_zip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(diagnostics, 'collect', return_value={'game.sav.bak': 'secret'}):
                self.assertEqual(diagnostics.main(['--kit-dir', str(root)]), 2)
            self.assertEqual(list(root.glob('*.zip')), [])

    def test_tail_is_last_300_lines(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / 'build-log.txt'
            log.write_text(''.join(f'{n}\n' for n in range(601)), encoding='utf-8')
            result = diagnostics.log_tail(log).splitlines()
            self.assertEqual(len(result), 300)
            self.assertEqual(result, [str(n) for n in range(301, 601)])

    def test_zip_allowlist_and_no_private_file_contents(self):
        with tempfile.TemporaryDirectory(prefix='diagnostics ! é ') as temporary:
            root = Path(temporary)
            (root / 'build/gen').mkdir(parents=True)
            (root / 'build/game/game').mkdir(parents=True)
            (root / 'godot/bin').mkdir(parents=True)
            (root / 'build/gen/abi.ini').write_text('[cartridge]\nbackup_type=eeprom64k\nsecret=PRIVATE\n')
            (root / 'build/build-log.txt').write_text(f'build in {root}\n', encoding='utf-8')
            for name in ['game.nds', 'save.sav.bak', 'build/gen/chunk.cpp',
                         'build/game/game/dq9.exe', 'godot/bin/session.dll', 'screenshot.png', 'inputs.txt']:
                (root / name).write_bytes(b'PRIVATE CONTENT DO NOT COLLECT')
            (root / 'script-paths.json').write_text('[]')
            with patch.object(diagnostics, 'command', return_value={'exit': 0, 'output': str(root)}), \
                    patch.dict(os.environ, {'XDG_CONFIG_HOME': str(root / 'config'), 'APPDATA': str(root / 'config')}):
                self.assertEqual(diagnostics.main(['--kit-dir', str(root)]), 0)
            [output] = root.glob('diagnostics-*.zip')
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(set(archive.namelist()), diagnostics.MEMBERS)
                combined = '\n'.join(archive.read(name).decode() for name in archive.namelist())
                self.assertNotIn('PRIVATE', combined)
                self.assertNotIn(str(root), combined)
                self.assertIn('<KIT>', combined)
                config = json.loads(archive.read('configuration.json'))
                self.assertEqual(config['abi_backup_type'], 'eeprom64k')
                binaries = json.loads(archive.read('binaries.json'))
                self.assertEqual(binaries['build/game/game/dq9.exe'],
                                 {'exists': True, 'size': len(b'PRIVATE CONTENT DO NOT COLLECT')})

    def test_only_tracked_manifest_scripts_are_hashed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'tools').mkdir()
            (root / 'tools/own.py').write_bytes(b'original script')
            (root / 'tools/untracked.py').write_bytes(b'user script')
            (root / 'script-paths.json').write_text('["tools/own.py"]')
            self.assertEqual(diagnostics.script_hashes(root), {
                'tools/own.py': hashlib.sha256(b'original script').hexdigest()})

    def test_launcher_summary_omits_paths_and_unknown_text(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / 'build/game/game/portable/config'
            config.mkdir(parents=True)
            (config.parent.parent / 'portable.txt').touch()
            (config / 'frontend.conf').write_text('rom_path=secret.nds\nsave_path=secret.sav\nlanguage=fr\nvolume=67\nunknown=SECRET\n')
            summary = diagnostics.configuration(root)['launcher']
            self.assertTrue(summary['portable'])
            self.assertEqual(summary['settings'], {'rom_path_configured': True,
                             'save_path_configured': True, 'language': 'fr', 'volume': 67})

    def test_invalid_log_encoding_refuses_instead_of_leaking_mangled_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / 'build-log.txt'
            log.write_bytes(b'C:\\Users\\\xe9lodie\\kit')
            with self.assertRaises(UnicodeError):
                diagnostics.log_tail(log)

    def test_symlink_log_is_refused(self):
        if os.name == 'nt':
            self.skipTest('Windows symlink privileges are not required by the kit')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'game.sav').write_text('secret')
            (root / 'build-log.txt').symlink_to(root / 'game.sav')
            with self.assertRaises(ValueError):
                diagnostics.log_tail(root / 'build-log.txt')


if __name__ == '__main__':
    unittest.main()
