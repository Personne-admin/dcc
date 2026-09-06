#!/usr/bin/env python3
import argparse
import hashlib
import os
import sys
import xml.sax.saxutils as sx

# persistent code used by windows to know that the same program is being updated.
UPGRADE_CODE = "BC06D996-A013-4321-9967-7BCEDBBD011B"


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

    directory_refs_xml = "\n    ".join(directory_refs)
    component_refs_xml = "\n".join(component_refs)

    wxs = f"""<?xml version="1.0" encoding="utf-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="*" Name="{sx.escape(args.product_name)}" Language="1033" Version="{sx.escape(args.version)}"
           Manufacturer="{sx.escape(args.manufacturer)}" UpgradeCode="{UPGRADE_CODE}">
    <Package InstallerVersion="450" Compressed="yes" InstallScope="perMachine" />
    <MajorUpgrade DowngradeErrorMessage="A newer version of [ProductName] is already installed." />
    <Media Id="1" Cabinet="dcc.cab" EmbedCab="yes" />

    <Directory Id="TARGETDIR" Name="SourceDir">
      <Directory Id="ProgramFiles64Folder">
        <Directory Id="INSTALLDIR" Name="dcc">{tree_xml}</Directory>
      </Directory>
    </Directory>

    {directory_refs_xml}

    <Feature Id="MainFeature" Title="dcc toolchain" Level="1">
{component_refs_xml}
    </Feature>
  </Product>
</Wix>
"""

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w") as f:
        f.write(wxs)

    total_files = sum(len(v) for v in files_by_dir.values())
    print(f"  WXS      {total_files} files -> {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
