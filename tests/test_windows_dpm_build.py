from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


def prerequisites(target, *arguments):
    result = subprocess.run(['make', '-qp', 'ENABLE_LLVM=0', *arguments], cwd=ROOT,
                            capture_output=True, text=True, timeout=120)
    for line in result.stdout.splitlines():
        if line.startswith(target + ':'):
            return line.split(':', 1)[1].split()
    raise AssertionError('missing target: ' + target)


class WindowsBuild(unittest.TestCase):
    def test_dpm_is_built_on_both_hosts(self):
        self.assertEqual(prerequisites('dpm'), ['compiler'])
        self.assertEqual(prerequisites('dpm', 'CROSS=windows'), ['compiler'])

    def test_dccd_schema_opt_out_is_preserved(self):
        self.assertIn('dpm', prerequisites('dccd'))
        self.assertNotIn('dpm', prerequisites('dccd', 'CROSS=windows'))
        self.assertNotIn('dpm', prerequisites('dccd', 'DCCD_DCON=0'))

    def test_utf8_manifest_is_shared_by_installed_tools(self):
        import xml.etree.ElementTree as ET
        manifest = ET.parse(ROOT / 'mk/windows.manifest').getroot()
        active = manifest.find('.//{http://schemas.microsoft.com/SMI/2019/WindowsSettings}activeCodePage')
        self.assertEqual(active.text, 'UTF-8')
        for tool in ('driver', 'dpm', 'dccd'):
            recipe = (ROOT / tool / 'GNUmakefile').read_text()
            self.assertIn('$(BINARY): $(WINDOWS_RESOURCE)', recipe)
            self.assertIn('$(TOPLEVEL)/mk/windows.manifest', recipe)


if __name__ == '__main__':
    unittest.main()
