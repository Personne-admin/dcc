#!/usr/bin/env python3
import argparse
import hashlib
import os
import sys
import xml.sax.saxutils as sx

UPGRADE_CODE = "BC06D996-A013-4321-9967-7BCEDBBD011B"
PATH_COMPONENT_GUID = "6F1C2B7E-3D4A-4E8B-9A51-2C7D0E9F4B13"
OPTIONS_COMPONENT_GUID = "0B8E5D21-7A3C-4F96-8E12-5D9C4A7B3E60"
SHORTCUT_COMPONENT_GUID = "A4D7F3C2-9B1E-4C58-B6A0-3E2F8D1C7A95"
INSTALLER_KEY = r"Software\dcc\Installer"
SHORTCUT_NAME = "Learn DC"


def safe_id(prefix: str, rel_path: str) -> str:
    cleaned = "".join(c if c.isalnum() else "_" for c in rel_path)
    ident = f"{prefix}_{cleaned}"
    if len(ident) > 70:
        digest = hashlib.sha1(rel_path.encode()).hexdigest()[:8]
        ident = f"{prefix}_{digest}_{cleaned[-40:]}"

    return ident


def walk_stage(stage_dir: str):
    files_by_dir: dict[str, list[tuple[str, str, str, str]]] = {}

    def walk(abs_dir: str, rel_dir: str, dir_id: str) -> str:
        xml_parts = []
        for name in sorted(os.listdir(abs_dir)):
            abs_path = os.path.join(abs_dir, name)
            rel_path = os.path.join(rel_dir, name) if rel_dir else name
            if os.path.isdir(abs_path):
                child_id = safe_id("d", rel_path)
                child_xml = walk(abs_path, rel_path, child_id)
                xml_parts.append(f'<Directory Id="{child_id}" Name="{sx.escape(name)}">{child_xml}</Directory>')
            else:
                comp_id = safe_id("c", rel_path)
                file_id = safe_id("f", rel_path)
                files_by_dir.setdefault(dir_id, []).append((comp_id, file_id, abs_path, name))

        return "".join(xml_parts)

    tree_xml = walk(stage_dir, "", "INSTALLDIR")
    return tree_xml, files_by_dir


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--stage-dir", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--product-name", default="dcc")
    p.add_argument("--manufacturer", default="dcc project")
    args = p.parse_args()

    stage_dir = os.path.abspath(args.stage_dir)
    if not os.path.isdir(stage_dir):
        print(f"error: stage dir does not exist: {stage_dir}", file=sys.stderr)
        sys.exit(1)

    for name in ("dcc.exe", "dccd.exe", "dpm.exe", "learndc.exe", "ld.lld.exe"):
        if not os.path.isfile(os.path.join(stage_dir, "bin", name)):
            p.error(f"MSI payload is missing bin/{name}")

    for name in ("kernel32", "ws2_32", "advapi32", "shell32"):
        if not os.path.isfile(os.path.join(stage_dir, "lib", "lib" + name + ".a")):
            p.error(f"MSI payload is missing lib/lib{name}.a")

    for name in ("include/std.dc", "include/libdcext.manifest", "include/dpm/v1.dc", "LICENSE", "licenses/FTXUI-LICENSE"):
        if not os.path.isfile(os.path.join(stage_dir, name)):
            p.error(f"MSI payload is missing {name}")

    tree_xml, files_by_dir = walk_stage(stage_dir)

    directory_refs = []
    component_refs = []
    for dir_id, files in sorted(files_by_dir.items()):
        comps = []
        for comp_id, file_id, abs_path, name in files:
            comps.append(
                f'<Component Id="{comp_id}" Guid="*">'
                f'<File Id="{file_id}" Name="{sx.escape(name)}" Source="{sx.escape(abs_path)}" KeyPath="yes" />'
                f"</Component>"
            )
            component_refs.append(f'      <ComponentRef Id="{comp_id}" />')
        directory_refs.append(f'<DirectoryRef Id="{dir_id}">{"".join(comps)}</DirectoryRef>')

    learndc_file_id = safe_id("f", os.path.join("bin", "learndc.exe"))
    bin_dir_id = safe_id("d", "bin")
    directory_refs.append(
        f'<DirectoryRef Id="{bin_dir_id}">'
        f'<Component Id="c_path_entry" Guid="{PATH_COMPONENT_GUID}">'
        f'<Condition>ADDTOPATH = "1"</Condition>'
        f'<RegistryValue Root="HKLM" Key="{INSTALLER_KEY}" Name="PathEntry" Type="string" Value="[INSTALLDIR]bin" KeyPath="yes" />'
        f'<Environment Id="e_path" Name="PATH" Value="[INSTALLDIR]bin" Action="set" Part="last" System="yes" Permanent="no" />'
        f"</Component>"
        f'<Component Id="c_installer_options" Guid="{OPTIONS_COMPONENT_GUID}">'
        f'<RegistryValue Root="HKLM" Key="{INSTALLER_KEY}" Name="AddToPath" Type="string" Value="[ADDTOPATH]" KeyPath="yes" />'
        f"</Component>"
        f"</DirectoryRef>"
    )
    directory_refs.append(
        f'<DirectoryRef Id="DccStartMenuFolder">'
        f'<Component Id="c_start_menu" Guid="{SHORTCUT_COMPONENT_GUID}">'
        f'<Shortcut Id="s_learndc" Name="{SHORTCUT_NAME}" Description="Learn the DC language with interactive exercises" '
        f'Target="[#{learndc_file_id}]" WorkingDirectory="PersonalFolder" />'
        f'<RemoveFolder Id="r_start_menu" On="uninstall" />'
        f'<RegistryValue Root="HKLM" Key="{INSTALLER_KEY}" Name="StartMenuShortcut" Type="integer" Value="1" KeyPath="yes" />'
        f"</Component>"
        f"</DirectoryRef>"
    )
    component_refs.extend(f'      <ComponentRef Id="{c}" />' for c in ("c_path_entry", "c_installer_options", "c_start_menu"))

    directory_refs_xml = "\n    ".join(directory_refs)
    component_refs_xml = "\n".join(component_refs)

    wxs = f"""<?xml version="1.0" encoding="utf-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="*" Name="{sx.escape(args.product_name)}" Language="1033" Version="{sx.escape(args.version)}"
           Manufacturer="{sx.escape(args.manufacturer)}" UpgradeCode="{UPGRADE_CODE}">
    <Package InstallerVersion="450" Compressed="yes" InstallScope="perMachine" />
    <MajorUpgrade DowngradeErrorMessage="A newer version of [ProductName] is already installed." />
    <Media Id="1" Cabinet="dcc.cab" EmbedCab="yes" />
    <Property Id="INSTALLDIR" Secure="yes" />
    <Property Id="ADDTOPATH" Secure="yes">
      <RegistrySearch Id="AddToPathRemembered" Root="HKLM" Key="{INSTALLER_KEY}" Name="AddToPath" Type="raw" />
    </Property>
    <Property Id="ADDTOPATH_CMD" Secure="yes" />
    <CustomAction Id="SetInstallLocation" Property="ARPINSTALLLOCATION" Value="[INSTALLDIR]" />
    <CustomAction Id="SaveAddToPathCmd" Property="ADDTOPATH_CMD" Value="[ADDTOPATH]" />
    <CustomAction Id="RestoreAddToPathCmd" Property="ADDTOPATH" Value="[ADDTOPATH_CMD]" />
    <CustomAction Id="DefaultAddToPath" Property="ADDTOPATH" Value="1" />
    <InstallExecuteSequence>
      <Custom Action="SaveAddToPathCmd" Before="AppSearch">ADDTOPATH</Custom>
      <Custom Action="RestoreAddToPathCmd" After="AppSearch">ADDTOPATH_CMD</Custom>
      <Custom Action="DefaultAddToPath" After="RestoreAddToPathCmd">NOT ADDTOPATH</Custom>
      <Custom Action="SetInstallLocation" After="CostFinalize">NOT REMOVE~="ALL"</Custom>
    </InstallExecuteSequence>
    <InstallUISequence>
      <Custom Action="SaveAddToPathCmd" Before="AppSearch">ADDTOPATH</Custom>
      <Custom Action="RestoreAddToPathCmd" After="AppSearch">ADDTOPATH_CMD</Custom>
      <Custom Action="DefaultAddToPath" After="RestoreAddToPathCmd">NOT ADDTOPATH</Custom>
    </InstallUISequence>

    <Directory Id="TARGETDIR" Name="SourceDir">
      <Directory Id="ProgramFiles64Folder">
        <Directory Id="INSTALLDIR" Name="dcc">{tree_xml}</Directory>
      </Directory>
      <Directory Id="ProgramMenuFolder">
        <Directory Id="DccStartMenuFolder" Name="dcc" />
      </Directory>
      <Directory Id="PersonalFolder" />
    </Directory>

    {directory_refs_xml}

    <Feature Id="MainFeature" Title="dcc toolchain" Level="1">
{component_refs_xml}
    </Feature>
  </Product>
</Wix>
"""

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(wxs)

    total_files = sum(len(v) for v in files_by_dir.values())
    print(f"  WXS      {total_files} files -> {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
