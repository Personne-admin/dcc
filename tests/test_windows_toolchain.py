from pathlib import Path
import hashlib
import importlib.util
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('stage_windows', ROOT / 'mk/stage_windows_toolchain.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class LinkerPayload(unittest.TestCase):
    def fixture(self, root, extra=None):
        archive = root / 'tools.zip'
        with zipfile.ZipFile(archive, 'w') as package:
            for name, content in {'sdk/bin/ld.lld.exe': b'linker', 'sdk/bin/Runtime.dll': b'runtime',
                                  'sdk/other/bin/runtime.dll': b'other architecture', 'sdk/bin/clang.exe': b'compiler',
                                  'sdk/LICENSE.TXT': b'license', **(extra or {})}.items():
                package.writestr(name, content)
        libs = root / 'sysroot/x86_64-w64-mingw32/lib'
        libs.mkdir(parents=True)
        for name in module.IMPORT_LIBS:
            (libs / ('lib' + name + '.a')).write_bytes(b'archive')
        return archive, hashlib.sha256(archive.read_bytes()).hexdigest(), root / 'stage', root / 'sysroot'

    def test_only_linker_and_required_dlls_are_staged(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(Path(directory))
            with patch.object(module, 'imports', side_effect=lambda path, _: ['runtime.dll', 'KERNEL32.dll'] if path.name == 'ld.lld.exe' else ['ucrtbase.dll']):
                module.stage(*args)
            stage = args[2]
            self.assertEqual(sorted(p.name for p in (stage / 'bin').iterdir()), ['Runtime.dll', 'ld.lld.exe'])
            self.assertEqual((stage / 'bin/Runtime.dll').read_bytes(), b'runtime')
            self.assertTrue((stage / 'share/licenses/llvm-mingw/LICENSE.TXT').is_file())
            self.assertTrue((stage / 'lib/libkernel32.a').is_file())

    def test_checksum_failure_does_not_touch_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            archive, _, stage, sysroot = self.fixture(Path(directory))
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                module.stage(archive, '0' * 64, stage, sysroot)
            self.assertFalse(stage.exists())

    def test_unknown_runtime_dependency_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(Path(directory))
            with patch.object(module, 'imports', return_value=['missing.dll']):
                with self.assertRaisesRegex(ValueError, 'unbundled'):
                    module.stage(*args)

    def test_archive_traversal_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(Path(directory), {'sdk/../outside': b'bad'})
            with self.assertRaisesRegex(ValueError, 'unsafe'):
                module.stage(*args)


if __name__ == '__main__':
    unittest.main()
