from pathlib import Path
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NS = {'w': 'http://schemas.microsoft.com/wix/2006/wi'}


class InstallerDiscovery(unittest.TestCase):
    def generate(self, directory):
        root = Path(directory)
        stage = root / 'Program Files mock' / 'dcc'
        (stage / 'bin').mkdir(parents=True)
        for name in ['dcc.exe', 'dccd.exe']:
            (stage / 'bin' / name).write_bytes(b'fixture')
        output = root / 'dcc.wxs'
        result = subprocess.run(['python3', str(ROOT / 'mk/gen_wxs.py'), '--stage-dir', str(stage),
                                 '--version', '0.6.0', '--output', str(output)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return ET.parse(output).getroot(), stage

    def test_installer_records_resolved_install_location(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
            xml, _ = self.generate(directory)
            action = xml.find(".//w:CustomAction[@Id='SetInstallLocation']", NS)
            self.assertIsNotNone(action)
            self.assertEqual(action.attrib['Property'], 'ARPINSTALLLOCATION')
            self.assertEqual(action.attrib['Value'], '[INSTALLDIR]')
            sequence = xml.find(".//w:InstallExecuteSequence/w:Custom[@Action='SetInstallLocation']", NS)
            self.assertEqual(sequence.attrib['After'], 'CostFinalize')

    def test_product_identity_and_payload_match_extension(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
            xml, _ = self.generate(directory)
            product = xml.find('w:Product', NS)
            self.assertIn(product.attrib['UpgradeCode'], (ROOT / 'vscode/src/windowsInstallation.ts').read_text())
            self.assertEqual(product.find('w:Package', NS).attrib['InstallScope'], 'perMachine')
            self.assertIsNotNone(product.find('w:MajorUpgrade', NS))
            component = xml.find(".//w:Component[@Id='c_bin_dccd_exe']", NS)
            self.assertEqual(component.find('w:File', NS).attrib['Name'], 'dccd.exe')
            self.assertEqual(component.find('w:File', NS).attrib['KeyPath'], 'yes')
            self.assertIn(component.attrib['Id'], (ROOT / 'vscode/src/windowsInstallation.ts').read_text())
            self.assertIsNotNone(xml.find(".//w:ComponentRef[@Id='c_bin_dccd_exe']", NS))
            self.assertIsNotNone(xml.find(".//w:File[@Name='dcc.exe']", NS))

    def test_missing_server_payload_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
            root = Path(directory)
            (root / 'bin').mkdir()
            (root / 'bin/dcc.exe').write_bytes(b'fixture')
            result = subprocess.run(['python3', str(ROOT / 'mk/gen_wxs.py'), '--stage-dir', str(root),
                                     '--version', '0.6.0', '--output', str(root / 'dcc.wxs')], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('missing bin/dccd.exe', result.stderr)

    def test_unicode_staging_and_uninstall_metadata(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
            root = Path(directory) / 'installation \u5de5\u5177'
            root.mkdir()
            xml, _ = self.generate(root)
            self.assertIn('\u5de5\u5177', xml.find(".//w:File[@Name='dccd.exe']", NS).attrib['Source'])
            sequence = xml.find(".//w:InstallExecuteSequence/w:Custom[@Action='SetInstallLocation']", NS)
            self.assertEqual(sequence.text, 'NOT REMOVE~="ALL"')


if __name__ == '__main__':
    unittest.main()
