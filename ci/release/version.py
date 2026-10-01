#!/usr/bin/env python3
import argparse
import json
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TAG_RE = re.compile(r"^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
DRY_RUN_VERSION = "0.0.0-dryrun"


class VersionError(Exception):
    pass


def numeric(version):
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)", version)
    if not m:
        raise VersionError("not a version: " + version)
    return tuple(int(part) for part in m.groups())


def resolve(tag, dry_run, floor):
    if dry_run:
        if tag:
            raise VersionError("--dry-run does not take a tag")
        version = DRY_RUN_VERSION
    else:
        if not tag:
            raise VersionError("a release needs a tag vX.Y.Z")
        m = TAG_RE.match(tag)
        if not m:
            raise VersionError("tag %r is not vX.Y.Z with decimal components" % tag)
        version = tag[1:]
        if numeric(version) < numeric(floor):
            raise VersionError("tag %s is lower than vscode/package.json version %s" % (tag, floor))
    core = ".".join(str(part) for part in numeric(version))
    suffix = version[len(core):]
    return {
        "DCC_VERSION": version,
        "MSI_VERSION": core,
        "DEB_VERSION": core + ("~" + suffix[1:] if suffix else ""),
        "VSIX_VERSION": version,
    }


def package_json_version(root):
    return json.loads((root / "vscode/package.json").read_text())["version"]


def cmd_resolve(args):
    versions = resolve(args.tag, args.dry_run, package_json_version(ROOT))
    for key, value in versions.items():
        print("%s=%s" % (key, value))


def rewrite_json(path, update):
    data = json.loads(path.read_text())
    update(data)
    path.write_text(json.dumps(data, indent=4, ensure_ascii=False) + "\n")


def cmd_inject(args):
    version = args.version

    def package(data):
        data["version"] = version

    def lock(data):
        data["version"] = version
        data["packages"][""]["version"] = version

    rewrite_json(ROOT / "vscode/package.json", package)
    rewrite_json(ROOT / "vscode/package-lock.json", lock)
    print("injected %s into vscode/package.json and vscode/package-lock.json" % version)


def run(cmd, **kwargs):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kwargs).stdout


def cmd_verify(args):
    versions = dict(line.split("=", 1) for line in Path(args.versions).read_text().split())
    out = Path(args.out)
    commit = run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"]).strip()
    errors = []

    def expect(what, actual, wanted):
        status = "ok" if actual == wanted else "MISMATCH"
        print("%-28s %-40s %s" % (what, actual, status))
        if actual != wanted:
            errors.append("%s: got %r, want %r" % (what, actual, wanted))

    deb = out / ("dcc_%s_amd64.deb" % versions["DEB_VERSION"])
    msi = out / ("dcc-%s-x86_64.msi" % versions["DCC_VERSION"])
    vsix = out / ("dcc-vscode-%s.vsix" % versions["VSIX_VERSION"])
    for path in (deb, msi, vsix):
        expect("artifact " + path.name, "present" if path.is_file() else "missing", "present")
    if errors:
        raise VersionError("; ".join(errors))

    expect("deb Version", run(["dpkg-deb", "-f", str(deb), "Version"]).strip(), versions["DEB_VERSION"])
    with tempfile.TemporaryDirectory() as tmp:
        run(["dpkg-deb", "-x", str(deb), tmp])
        expect("deb dcc --version", run([tmp + "/usr/bin/dcc", "--version"]).strip(),
               "dcc %s (%s)" % (versions["DCC_VERSION"], commit))

    props = run(["msiinfo", "export", str(msi), "Property"])
    product = dict(line.split("\t", 1) for line in props.splitlines() if "\t" in line)
    expect("msi ProductVersion", product.get("ProductVersion", "").strip(), versions["MSI_VERSION"])

    with zipfile.ZipFile(vsix) as archive:
        manifest = json.loads(archive.read("extension/package.json"))
    expect("vsix package.json version", manifest["version"], versions["VSIX_VERSION"])

    if args.windows_version:
        expect("dcc.exe --version", Path(args.windows_version).read_text().strip(),
               "dcc %s (%s)" % (versions["DCC_VERSION"], commit))

    if errors:
        raise VersionError("; ".join(errors))
    print("all artifact versions agree")


def cmd_selftest(args):
    cases = [
        ("v0.4.0", False, "0.4.0", True),
        ("v1.2.3", False, "0.4.0", True),
        ("v0.3.9", False, "0.4.0", False),
        ("v0.4.0-rc1", False, "0.4.0", False),
        ("0.4.0", False, "0.4.0", False),
        ("v01.4.0", False, "0.4.0", False),
        ("", True, "0.4.0", True),
        ("v0.4.0", True, "0.4.0", False),
        ("", False, "0.4.0", False),
    ]
    for tag, dry, floor, ok in cases:
        try:
            resolve(tag, dry, floor)
            passed = ok
        except VersionError:
            passed = not ok
        print("%-14s dry_run=%-5s floor=%s %s" % (tag or "(none)", dry, floor, "ok" if passed else "FAIL"))
        if not passed:
            sys.exit(1)
    assert resolve("", True, "0.4.0")["DEB_VERSION"] == "0.0.0~dryrun"
    assert resolve("", True, "0.4.0")["MSI_VERSION"] == "0.0.0"
    assert resolve("v0.4.0", False, "0.4.0") == {"DCC_VERSION": "0.4.0", "MSI_VERSION": "0.4.0", "DEB_VERSION": "0.4.0", "VSIX_VERSION": "0.4.0"}
    print("version selftest passed")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("resolve")
    p.add_argument("--tag", default="")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(func=cmd_resolve)
    p = sub.add_parser("inject")
    p.add_argument("version")
    p.set_defaults(func=cmd_inject)
    p = sub.add_parser("verify")
    p.add_argument("--versions", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--windows-version", default="")
    p.set_defaults(func=cmd_verify)
    p = sub.add_parser("selftest")
    p.set_defaults(func=cmd_selftest)
    args = parser.parse_args()
    try:
        args.func(args)
    except VersionError as error:
        print("version check failed: %s" % error, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
