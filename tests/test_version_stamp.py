import os
from pathlib import Path
import resource
import subprocess


root = Path(__file__).resolve().parents[1]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, CXX="clang++", CC="clang", AR="ar")
revision = subprocess.check_output(
    ["git", "rev-parse", "--short", "HEAD"], cwd=root, text=True
).strip()
binary = root / "build/bin/dcc"
tracked = [binary, root / "build/pcm/dcc.cli.pcm", root / "build/obj/dcc.cli.pcm.o"]
unrelated = root / "build/obj/dcc.ir.lower.pcm.o"


def build(stamp, release=None):
    arguments = ["make", "driver", "-j2", f"DCC_GIT_HASH={stamp}", "DCC_WRAPPER=timeout 60"]
    if release is not None:
        arguments.append(f"DCC_VERSION={release}")
    result = subprocess.run(
        arguments,
        cwd=root, env=env, capture_output=True, text=True, timeout=300,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def version():
    return subprocess.check_output([binary, "--version"], text=True, timeout=60).strip()


def times(paths):
    return [path.stat().st_mtime_ns for path in paths]


try:
    build(revision)
    assert version().endswith(f"({revision})"), version()
    original_version = version()
    expected_release = env.get("DCC_VERSION") or "0.6.0-dev"
    assert original_version == f"dcc {expected_release} ({revision})", original_version
    old_times = times(tracked)
    unrelated_time = times([unrelated])
    build("version-stamp-regression")
    assert version().endswith("(version-stamp-regression)"), version()
    assert all(a < b for a, b in zip(old_times, times(tracked)))
    assert times([unrelated]) == unrelated_time
    stable_times = times(tracked)
    build("version-stamp-regression")
    assert times(tracked) == stable_times
    assert times([unrelated]) == unrelated_time
    build(revision, "9.8.7-test")
    assert version() == f"dcc 9.8.7-test ({revision})", version()
    stable_times = times(tracked)
    build(revision, "9.8.7-test")
    assert times(tracked) == stable_times
    assert times([unrelated]) == unrelated_time
finally:
    build(revision)

assert version().endswith(f"({revision})"), version()
assert version() == original_version, version()
print("PASS development version, incremental revision/version overrides, unchanged builds, and restoration")
