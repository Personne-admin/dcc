#!/usr/bin/env python3
import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GROUPS = [
    ("feat", "Features"),
    ("fix", "Fixes"),
    ("refactor", "Refactoring"),
    ("test", "Tests"),
    ("docs", "Documentation"),
    ("chore", "Chores"),
]
SUBJECT_RE = re.compile(r"^(?P<kind>[a-z]+)(?:\((?P<scope>[^)]*)\))?(?P<breaking>!)?: (?P<message>.+)$")


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout


def previous_tag(rev, exclude):
    tags = git("tag", "--merged", rev, "--list", "v[0-9]*.[0-9]*.[0-9]*", "--sort=-v:refname").split()
    tags = [t for t in tags if re.fullmatch(r"v\d+\.\d+\.\d+", t) and t != exclude]
    return tags[0] if tags else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--rev", default="HEAD")
    parser.add_argument("--tag", default="")
    args = parser.parse_args()

    prev = previous_tag(args.rev, args.tag)
    span = "%s..%s" % (prev, args.rev) if prev else args.rev
    subjects = git("log", "--first-parent", "--format=%s", span).splitlines()

    grouped = {kind: [] for kind, _ in GROUPS}
    other = []
    breaking = []
    for subject in subjects:
        m = SUBJECT_RE.match(subject)
        if m and m.group("kind") in grouped:
            scope = m.group("scope")
            entry = ("**%s:** " % scope if scope else "") + m.group("message")
            (breaking if m.group("breaking") else grouped[m.group("kind")]).append(entry)
        else:
            other.append(subject)

    lines = ["# dcc %s" % args.version, ""]
    lines.append("Changes since %s (%d commits)." % (prev, len(subjects)) if prev else "All changes (%d commits)." % len(subjects))
    if breaking:
        lines += ["", "## Breaking changes", ""] + ["- " + entry for entry in breaking]
    for kind, title in GROUPS:
        if grouped[kind]:
            lines += ["", "## " + title, ""] + ["- " + entry for entry in grouped[kind]]
    if other:
        lines += ["", "## Other", ""] + ["- " + entry for entry in other]
    lines += ["", "## Artifacts", "",
              "- `dcc_*_amd64.deb`: Ubuntu 24.04+ and Debian trixie+ (`apt install ./dcc_*_amd64.deb`)",
              "- `dcc-*-x86_64.msi`: Windows x64 installer",
              "- `dcc-vscode-*.vsix`: VS Code extension (`code --install-extension dcc-vscode-*.vsix`)",
              "- `SHA256SUMS`: checksums of the above"]
    print("\n".join(lines))


if __name__ == "__main__":
    main()
