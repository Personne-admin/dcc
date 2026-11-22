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
        for name in ['dcc.exe', 'dccd.exe', 'dpm.exe', 'learndc.exe', 'ld.lld.exe']:
            (stage / 'bin' / name).write_bytes(b'fixture')
        (stage / 'lib').mkdir()
        for name in ['kernel32', 'ws2_32', 'advapi32', 'shell32', 'ntdll']:
            (stage / 'lib' / ('lib' + name + '.a')).write_bytes(b'fixture')
        for name in ['include/std.dc', 'include/libdcext.manifest', 'include/dpm/v1.dc', 'LICENSE', 'licenses/FTXUI-LICENSE']:
            target = stage / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('fixture')
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
            directory = xml.find(".//w:Property[@Id='INSTALLDIR']", NS)
            self.assertIsNotNone(directory)
            self.assertEqual(directory.attrib['Secure'], 'yes')
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
            self.assertIsNotNone(xml.find(".//w:File[@Name='dpm.exe']", NS))
            self.assertIsNotNone(xml.find(".//w:File[@Name='ld.lld.exe']", NS))

    def test_missing_server_payload_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
            root = Path(directory)
            (root / 'bin').mkdir()
            (root / 'bin/dcc.exe').write_bytes(b'fixture')
            result = subprocess.run(['python3', str(ROOT / 'mk/gen_wxs.py'), '--stage-dir', str(root),
                                     '--version', '0.6.0', '--output', str(root / 'dcc.wxs')], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('missing bin/dccd.exe', result.stderr)

    def test_missing_project_manager_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
            _, stage = self.generate(directory)
            (stage / 'bin/dpm.exe').unlink()
            result = subprocess.run(['python3', str(ROOT / 'mk/gen_wxs.py'), '--stage-dir', str(stage),
                                     '--version', '0.6.0', '--output', str(Path(directory) / 'missing.wxs')], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('missing bin/dpm.exe', result.stderr)

    def test_missing_learning_tool_or_license_is_rejected(self):
        for missing in ['bin/learndc.exe', 'licenses/FTXUI-LICENSE']:
            with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
                _, stage = self.generate(directory)
                (stage / missing).unlink()
                result = subprocess.run(['python3', str(ROOT / 'mk/gen_wxs.py'), '--stage-dir', str(stage),
                                         '--version', '0.6.0', '--output', str(Path(directory) / 'missing.wxs')], capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('missing ' + missing, result.stderr)

    def test_learndc_ships_beside_the_toolchain(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
            xml, _ = self.generate(directory)
            bin_dir = xml.find(".//w:Directory[@Id='d_bin']", NS)
            self.assertEqual(bin_dir.attrib['Name'], 'bin')
            names = {f.attrib['Name'] for f in xml.findall(".//w:DirectoryRef[@Id='d_bin']/w:Component/w:File", NS)}
            self.assertTrue({'dcc.exe', 'dccd.exe', 'dpm.exe', 'learndc.exe'} <= names)
            self.assertIsNotNone(xml.find(".//w:ComponentRef[@Id='c_bin_learndc_exe']", NS))
            self.assertIsNotNone(xml.find(".//w:File[@Name='FTXUI-LICENSE']", NS))

    def test_start_menu_shortcut_targets_learndc(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
            xml, _ = self.generate(directory)
            menu = xml.find(".//w:Directory[@Id='ProgramMenuFolder']/w:Directory[@Id='DccStartMenuFolder']", NS)
            self.assertEqual(menu.attrib['Name'], 'dcc')
            self.assertIsNotNone(xml.find(".//w:Directory[@Id='PersonalFolder']", NS))
            component = xml.find(".//w:DirectoryRef[@Id='DccStartMenuFolder']/w:Component[@Id='c_start_menu']", NS)
            self.assertRegex(component.attrib['Guid'], r'^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$')
            shortcut = component.find('w:Shortcut', NS)
            self.assertEqual(shortcut.attrib['Name'], 'Learn DC')
            self.assertEqual(shortcut.attrib['Target'], '[#f_bin_learndc_exe]')
            self.assertEqual(shortcut.attrib['WorkingDirectory'], 'PersonalFolder')
            self.assertNotIn('Arguments', shortcut.attrib)
            self.assertEqual(component.find('w:RemoveFolder', NS).attrib['On'], 'uninstall')
            key = component.find('w:RegistryValue', NS)
            self.assertEqual(key.attrib['KeyPath'], 'yes')
            self.assertEqual(key.attrib['Root'], 'HKLM')
            self.assertIsNotNone(xml.find(".//w:Feature/w:ComponentRef[@Id='c_start_menu']", NS))

    def test_path_entry_appends_the_installed_bin_directory(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
            xml, _ = self.generate(directory)
            component = xml.find(".//w:DirectoryRef[@Id='d_bin']/w:Component[@Id='c_path_entry']", NS)
            self.assertEqual(component.find('w:Condition', NS).text, 'ADDTOPATH = "1"')
            env = component.find('w:Environment', NS)
            self.assertEqual(env.attrib, {'Id': 'e_path', 'Name': 'PATH', 'Value': '[INSTALLDIR]bin', 'Action': 'set',
                                          'Part': 'last', 'System': 'yes', 'Permanent': 'yes'})
            self.assertEqual(component.find('w:RegistryValue', NS).attrib['KeyPath'], 'yes')
            self.assertIsNotNone(xml.find(".//w:Feature/w:ComponentRef[@Id='c_path_entry']", NS))
            self.assertEqual(xml.findall('.//w:Environment', NS), [env])

    def test_path_opt_out_property_is_remembered(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as directory:
            xml, _ = self.generate(directory)
            prop = xml.find(".//w:Property[@Id='ADDTOPATH']", NS)
            self.assertEqual(prop.attrib['Secure'], 'yes')
            self.assertNotIn('Value', prop.attrib)
            search = prop.find('w:RegistrySearch', NS)
            self.assertEqual((search.attrib['Root'], search.attrib['Key'], search.attrib['Name']), ('HKLM', 'Software\\dcc\\Installer', 'AddToPath'))
            remember = xml.find(".//w:Component[@Id='c_installer_options']/w:RegistryValue", NS)
            self.assertEqual((remember.attrib['Name'], remember.attrib['Value']), ('AddToPath', '[ADDTOPATH]'))
            for sequence in ['InstallExecuteSequence', 'InstallUISequence']:
                steps = {c.attrib['Action']: c for c in xml.findall(f'.//w:{sequence}/w:Custom', NS)}
                self.assertEqual((steps['SaveAddToPathCmd'].attrib['Before'], steps['SaveAddToPathCmd'].text), ('AppSearch', 'ADDTOPATH'))
                self.assertEqual((steps['RestoreAddToPathCmd'].attrib['After'], steps['RestoreAddToPathCmd'].text), ('AppSearch', 'ADDTOPATH_CMD'))
                self.assertEqual((steps['DefaultAddToPath'].attrib['After'], steps['DefaultAddToPath'].text), ('RestoreAddToPathCmd', 'NOT ADDTOPATH'))
            actions = {a.attrib['Id']: a.attrib for a in xml.findall('.//w:CustomAction', NS)}
            self.assertEqual(actions['DefaultAddToPath'], {'Id': 'DefaultAddToPath', 'Property': 'ADDTOPATH', 'Value': '1'})
            for attrs in actions.values():
                self.assertEqual(set(attrs), {'Id', 'Property', 'Value'})

    def test_new_components_keep_stable_guids(self):
        with tempfile.TemporaryDirectory(prefix='dcc-msi-') as first, tempfile.TemporaryDirectory(prefix='dcc-msi-') as second:
            a, _ = self.generate(first)
            b, _ = self.generate(second)
            for cid in ['c_path_entry', 'c_installer_options', 'c_start_menu']:
                guid = a.find(f".//w:Component[@Id='{cid}']", NS).attrib['Guid']
                self.assertNotEqual(guid, '*')
                self.assertEqual(guid, b.find(f".//w:Component[@Id='{cid}']", NS).attrib['Guid'])

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
