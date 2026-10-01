#!/usr/bin/env python3
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REQUIRED = [
    "extension/package.json",
    "extension/out/extension.js",
    "extension/syntaxes/dcc.tmLanguage.json",
    "extension/syntaxes/dcc-dir.tmLanguage.json",
    "extension/syntaxes/dcc-test.tmLanguage.json",
    "extension/language-configuration.json",
    "extension.vsixmanifest",
    "[Content_Types].xml",
]
FORBIDDEN = re.compile(r"^extension/(src/|scripts/|.*\.vsix$|.*\.ts$(?<!\.d\.ts$))")


def main():
    vsix, version = Path(sys.argv[1]), sys.argv[2]
    errors = []
    with zipfile.ZipFile(vsix) as archive:
        names = set(archive.namelist())
        manifest = json.loads(archive.read("extension/package.json"))
        vsixmanifest = archive.read("extension.vsixmanifest").decode()
    for name in REQUIRED:
        if name not in names:
            errors.append("missing " + name)
    for name in sorted(names):
        if FORBIDDEN.match(name):
            errors.append("unexpected " + name)
    if manifest["version"] != version:
        errors.append("package.json version %s != %s" % (manifest["version"], version))
    m = re.search(r'<Identity [^>]*Version="([^"]+)"', vsixmanifest)
    if not m or m.group(1) != version:
        errors.append("vsixmanifest version %s != %s" % (m.group(1) if m else None, version))

    listed = subprocess.run(["npx", "--no-install", "vsce", "ls"], cwd=ROOT / "vscode", check=True,
                            capture_output=True, text=True).stdout.split()
    listed = ["LICENSE.txt" if name == "LICENSE" else name for name in listed]
    packaged = {n[len("extension/"):] for n in names if n.startswith("extension/")}
    if set(listed) != packaged:
        errors.append("vsce ls differs from archive: only listed %s, only packaged %s"
                      % (sorted(set(listed) - packaged)[:10], sorted(packaged - set(listed))[:10]))

    print("%s: %d entries, version %s, vsce ls %d files" % (vsix.name, len(names), manifest["version"], len(listed)))
    if errors:
        for error in errors:
            print("error: " + error, file=sys.stderr)
        sys.exit(1)
    print("vsix ok")


if __name__ == "__main__":
    main()
