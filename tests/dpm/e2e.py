import os
import shutil
import subprocess
import sys
import tempfile

DPM = os.path.abspath(sys.argv[1])
DCC = os.path.abspath(sys.argv[2])
ROOT = tempfile.mkdtemp(prefix="dpm_e2e_")
FAILURES = []
CHECKS = 0

ENV = dict(os.environ)
ENV.update({
    "DPM_HOME": os.path.join(ROOT, "home"),
    "DCC": DCC,
    "DPM_ALLOW_FILE_URLS": "1",
    "HOME": ROOT,
    "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_CONFIG_SYSTEM": "/dev/null",
    "GIT_AUTHOR_NAME": "t",
    "GIT_AUTHOR_EMAIL": "t@example.invalid",
    "GIT_COMMITTER_NAME": "t",
    "GIT_COMMITTER_EMAIL": "t@example.invalid",
})
ENV.pop("DPM_OFFLINE", None)
ENV.pop("DPM_SCHEMA_ROOT", None)


def check(ok, label, detail=""):
    global CHECKS
    CHECKS += 1
    if not ok:
        FAILURES.append(label)
        print("FAIL  " + label)
        if detail:
            print(detail)


def dpm(cwd, *args, env=None, timeout=300):
    e = dict(ENV)
    if env:
        e.update(env)
    r = subprocess.run([DPM, *args], cwd=cwd, env=e, capture_output=True, text=True, timeout=timeout)
    return r


def detail(r):
    return "exit %d\nstdout:\n%s\nstderr:\n%s" % (r.returncode, r.stdout, r.stderr)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="") as f:
        f.write(text)


def read(path):
    with open(path, newline="") as f:
        return f.read()


def git(cwd, *args):
    r = subprocess.run(["git", *args], cwd=cwd, env=ENV, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


def make_lib_repo(name, versions):
    path = os.path.join(ROOT, "repos", name)
    os.makedirs(path)
    git(path, "init", "-q", "--initial-branch=main")
    for version, answer in versions:
        write(os.path.join(path, "project.dcon"), 'dpm::v1::Project { name = "%s", version = "%s" }\n' % (name, version))
        write(os.path.join(path, "src", name, "lib.dc"), "module %s::lib;\n\npublic i32 answer()\n{\n    return %d;\n}\n" % (name, answer))
        git(path, "add", "-A")
        git(path, "commit", "-q", "-m", version)
        git(path, "tag", "v" + version)
    return path


def new_app(name, body="return 0;"):
    parent = os.path.join(ROOT, "apps")
    os.makedirs(parent, exist_ok=True)
    r = dpm(parent, "init", name)
    check(r.returncode == 0, "init " + name, detail(r))
    app = os.path.join(parent, name)
    write(os.path.join(app, "src", "main.dc"), "module main;\n\npublic i32 main()\n{\n    %s\n}\n" % body)
    return app


def run_app(app, *args):
    return dpm(app, "run", *args)


def scenario_basic():
    app = new_app("basic", "return 7;")
    check(os.path.isfile(os.path.join(app, "project.dcon")), "init writes project.dcon")
    check(".dpm/" in read(os.path.join(app, ".gitignore")), "init writes .gitignore")
    again = dpm(app, "init")
    check(again.returncode == 2, "init refuses to overwrite", detail(again))
    r = dpm(app, "build")
    check(r.returncode == 0, "build", detail(r))
    check(os.path.isfile(os.path.join(app, ".dpm/out/debug/host/basic")), "build output path")
    check(os.path.isfile(os.path.join(app, "compile_commands.json")), "compile_commands.json merged")
    r = run_app(app)
    check(r.returncode == 7, "run propagates the exit code", detail(r))
    r = dpm(app, "build", "--profile", "release")
    check(r.returncode == 0 and os.path.isfile(os.path.join(app, ".dpm/out/release/host/basic")), "release profile output", detail(r))
    r = dpm(app, "build", "--profile", "nope")
    check(r.returncode == 2, "unknown profile is a usage error", detail(r))
    r = dpm(app, "build", "missing")
    check(r.returncode == 2, "unknown product is a usage error", detail(r))
    r = dpm(app, "run", "--target", "x86_64-elf")
    check(r.returncode == 2 and "runner" in r.stderr, "run refuses foreign targets", detail(r))
    r = dpm(app, "run", "--runner", "true")
    check(r.returncode == 0, "run with a runner", detail(r))
    r = dpm(app, "clean")
    check(r.returncode == 0 and not os.path.exists(os.path.join(app, ".dpm/out")), "clean", detail(r))
    broken = new_app("broken", "return missing_name;")
    r = dpm(broken, "build")
    check(r.returncode == 1, "a compile error exits 1", detail(r))
    check("missing_name" in r.stdout + r.stderr, "compiler diagnostics are passed through", detail(r))


def scenario_run_args():
    app = new_app("args", "return 3;")
    r = run_app(app, "--", "a", "b")
    check(r.returncode == 3, "run accepts arguments", detail(r))


def scenario_path_dependency():
    parent = os.path.join(ROOT, "paths")
    os.makedirs(parent)
    r = dpm(parent, "init", "util", "--lib")
    check(r.returncode == 0, "init --lib", detail(r))
    check(os.path.isfile(os.path.join(parent, "util", "src", "util", "lib.dc")), "library layout")
    app = os.path.join(parent, "app")
    r = dpm(parent, "init", "app")
    write(os.path.join(app, "src", "main.dc"), "module main;\nimport util::lib;\n\npublic i32 main()\n{\n    return util::lib::answer();\n}\n")
    r = dpm(app, "add", "path:../util")
    check(r.returncode == 0, "add a path dependency", detail(r))
    check('location = "path:../util"' in read(os.path.join(app, "project.dcon")), "add edits project.dcon")
    check(os.path.isfile(os.path.join(app, "project.lock.dcon")), "add writes the lock")
    r = run_app(app)
    check(r.returncode == 42, "run uses the path dependency", detail(r))
    r = dpm(app, "tree")
    check(r.returncode == 0 and "path:../util@0.1.0" in r.stdout, "tree", detail(r))
    r = dpm(app, "why", "path:../util")
    check(r.returncode == 0 and "path:../util" in r.stdout, "why", detail(r))
    r = dpm(app, "add", "path:../util")
    check(r.returncode != 0, "adding twice fails", detail(r))
    r = dpm(app, "remove", "path:../util")
    check(r.returncode == 0 and "path:../util" not in read(os.path.join(app, "project.dcon")), "remove", detail(r))
    r = dpm(app, "build")
    check(r.returncode == 1, "building without the dependency fails", detail(r))


def scenario_git_dependency():
    repo = make_lib_repo("gitlib", [("1.0.0", 10), ("1.1.0", 11)])
    app = new_app("gitapp")
    write(os.path.join(app, "src", "main.dc"), "module main;\nimport gitlib::lib;\n\npublic i32 main()\n{\n    return gitlib::lib::answer();\n}\n")
    location = "git+file://" + repo
    r = dpm(app, "add", location)
    check(r.returncode == 0, "add a git dependency", detail(r))
    manifest = read(os.path.join(app, "project.dcon"))
    check('version = "1.1.0"' in manifest, "add picks the latest version", manifest)
    r = run_app(app)
    check(r.returncode == 11, "run uses the git dependency", detail(r))
    lock = read(os.path.join(app, "project.lock.dcon"))
    check("commit = " in lock and "hash = \"sha256:" in lock, "lock records commit and hash")
    check(dpm(app, "build", "--locked").returncode == 0, "--locked passes with a current lock")

    git(repo, "tag", "-f", "v1.1.0", "HEAD~1")
    check(dpm(app, "build", "--locked", "--offline").returncode == 0, "a moved tag does not affect a locked build")

    write(os.path.join(repo, "src", "gitlib", "lib.dc"), "module gitlib::lib;\n\npublic i32 answer()\n{\n    return 12;\n}\n")
    write(os.path.join(repo, "project.dcon"), 'dpm::v1::Project { name = "gitlib", version = "1.2.0" }\n')
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "1.2.0")
    git(repo, "tag", "v1.2.0")
    r = dpm(app, "update", "--dry-run")
    check(r.returncode == 0 and "1.1.0 -> 1.2.0" in r.stdout, "update --dry-run", detail(r))
    check('version = "1.1.0"' in read(os.path.join(app, "project.dcon")), "dry run changes nothing")
    r = dpm(app, "update")
    check(r.returncode == 0 and 'version = "1.2.0"' in read(os.path.join(app, "project.dcon")), "update raises the minimum", detail(r))
    r = run_app(app)
    check(r.returncode == 12, "run after update", detail(r))
    r = dpm(app, "update")
    check("up to date" in r.stdout, "update when current", detail(r))

    manifest = read(os.path.join(app, "project.dcon"))
    write(os.path.join(app, "project.dcon"), manifest.replace('"1.2.0"', '"1.0.0"'))
    r = dpm(app, "build", "--locked")
    check(r.returncode == 4, "a stale lock fails --locked with 4", detail(r))
    r = dpm(app, "lock")
    check(r.returncode == 0, "lock re-derives", detail(r))
    check(dpm(app, "lock", "--check").returncode == 0, "lock --check passes when current")


def scenario_offline_and_cache():
    repo = make_lib_repo("offlib", [("1.0.0", 5)])
    app = new_app("offapp")
    write(os.path.join(app, "src", "main.dc"), "module main;\nimport offlib::lib;\n\npublic i32 main()\n{\n    return offlib::lib::answer();\n}\n")
    check(dpm(app, "add", "git+file://" + repo).returncode == 0, "add for offline test")
    check(dpm(app, "build", "--offline").returncode == 0, "offline build with a populated cache")
    r = dpm(app, "cache", "verify")
    check(r.returncode == 0, "cache verify", detail(r))
    cache = dpm(app, "cache", "path").stdout.strip()
    src = os.path.join(cache, "src")
    entry = os.path.join(src, os.listdir(src)[0])
    victim = os.path.join(entry, "project.dcon")
    os.chmod(entry, 0o755)
    os.chmod(victim, 0o644)
    write(victim, "tampered")
    r = dpm(app, "cache", "verify")
    check(r.returncode == 4, "a tampered tree is detected", detail(r))
    check(dpm(app, "cache", "clean", "--all").returncode == 0, "cache clean --all")
    r = dpm(app, "build", "--offline")
    check(r.returncode == 5 and "not in cache" in r.stderr, "offline build without a cache exits 5", detail(r))
    r = dpm(app, "fetch")
    check(r.returncode == 0, "fetch repopulates the cache", detail(r))
    check(dpm(app, "build", "--offline").returncode == 0, "offline build after fetch")


def scenario_tests():
    app = new_app("tested")
    write(os.path.join(app, "tests", "good.dc"), "module good;\n\npublic i32 main()\n{\n    return 0;\n}\n")
    write(os.path.join(app, "tests", "bad.dc"), "module bad;\n\npublic i32 main()\n{\n    return 3;\n}\n")
    r = dpm(app, "test")
    check(r.returncode == 1 and "1 passed, 1 failed" in r.stdout, "dpm test counts", detail(r))
    check("ok   good" in r.stdout and "FAIL bad" in r.stdout, "dpm test lists outcomes", detail(r))
    r = dpm(app, "test", "good")
    check(r.returncode == 0 and "1 passed, 0 failed" in r.stdout, "dpm test NAME", detail(r))
    r = dpm(app, "test", "-j", "4")
    check(r.returncode == 1, "dpm test -j", detail(r))
    r = dpm(app, "test", "nothing")
    check(r.returncode == 2, "unknown test name", detail(r))
    write(os.path.join(app, "tests", "hang.dc"), "module hang;\n\npublic i32 main()\n{\n    while (true) {}\n}\n")
    r = dpm(app, "test", "hang", "--timeout", "2")
    check(r.returncode == 1 and "timed out" in r.stdout, "test timeout", detail(r))


def scenario_formatting_and_determinism():
    repo = make_lib_repo("fmtlib", [("1.0.0", 1)])
    other = make_lib_repo("fmtother", [("2.0.0", 2)])
    original = (
        "dpm::v1::Project {\r\n"
        "\tname = \"fmt\",\r\n"
        "\tversion = \"0.1.0\",\r\n"
        "\t// the libraries\r\n"
        "\tdependencies = {\r\n"
        "\t\t{ location = \"git+file://%s\", version = \"1.0.0\" }, // first\r\n"
        "\t},\r\n"
        "\tproducts = { { name = \"fmt\", entry = \"src/main.dc\" } },\r\n"
        "}\r\n" % repo
    )
    app = os.path.join(ROOT, "fmt")
    write(os.path.join(app, "project.dcon"), original)
    write(os.path.join(app, "src", "main.dc"), "module main;\n\npublic i32 main()\n{\n    return 0;\n}\n")
    r = dpm(app, "add", "git+file://" + other)
    check(r.returncode == 0, "add keeps working on CRLF manifests", detail(r))
    edited = read(os.path.join(app, "project.dcon"))
    expected = original.replace(
        "// first\r\n\t},",
        "// first\r\n\t\t{ location = \"git+file://%s\", version = \"2.0.0\" },\r\n\t}," % other,
    )
    check(edited == expected, "add preserves comments, tabs and CRLF byte for byte", edited)
    lock_a = read(os.path.join(app, "project.lock.dcon"))
    r = dpm(app, "remove", "git+file://" + other)
    check(r.returncode == 0 and read(os.path.join(app, "project.dcon")) == original, "remove restores the original bytes", detail(r))

    copy = os.path.join(ROOT, "fmt2")
    shutil.copytree(app, copy)
    os.remove(os.path.join(copy, "project.lock.dcon"))
    check(dpm(copy, "lock").returncode == 0, "second lock")
    check(read(os.path.join(copy, "project.lock.dcon")) == read(os.path.join(app, "project.lock.dcon")), "locks from two directories are byte identical")
    del lock_a


def scenario_misc():
    app = new_app("misc")
    r = dpm(app, "check", "project.dcon")
    check(r.returncode == 0, "check", detail(r))
    dpm(app, "lock")
    r = dpm(app, "check", "project.lock.dcon")
    check(r.returncode == 0, "check validates lock files", detail(r))
    r = dpm(app, "frobnicate")
    check(r.returncode == 2, "unknown command", detail(r))
    r = dpm(app, "build", "--bogus")
    check(r.returncode == 2, "unknown option", detail(r))
    lib = os.path.join(ROOT, "apps", "nolib")
    dpm(os.path.dirname(lib), "init", "nolib", "--lib")
    r = dpm(lib, "build")
    check(r.returncode == 2 and "no products" in r.stderr, "a library has nothing to build", detail(r))
    r = dpm(ROOT, "build", "--manifest-path", os.path.join(app, "project.dcon"))
    check(r.returncode == 0, "--manifest-path", detail(r))


try:
    scenario_basic()
    scenario_run_args()
    scenario_path_dependency()
    scenario_git_dependency()
    scenario_offline_and_cache()
    scenario_tests()
    scenario_formatting_and_determinism()
    scenario_misc()
finally:
    shutil.rmtree(ROOT, ignore_errors=True)

print("%d checks, %d failed" % (CHECKS, len(FAILURES)))
sys.exit(1 if FAILURES else 0)
