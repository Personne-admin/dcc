import hashlib
import http.server
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading

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
    "DPM_REGISTRY": "",
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
    for key, value in (env or {}).items():
        if value is None:
            e.pop(key, None)
        else:
            e[key] = value
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


def make_pkg_repo(dirname, name, versions, deps="", body="", extra=""):
    path = os.path.join(ROOT, "repos", dirname)
    os.makedirs(path)
    git(path, "init", "-q", "--initial-branch=main")
    for version, answer in versions:
        write(os.path.join(path, "project.dcon"), 'dpm::v1::Project { name = "%s", version = "%s"%s%s }\n' % (name, version, deps, extra))
        write(os.path.join(path, "src", name, "lib.dc"), "module %s::lib;\n\n%s\npublic i32 answer()\n{\n    return %d;\n}\n" % (name, body, answer))
        git(path, "add", "-A")
        git(path, "commit", "-q", "-m", version)
        git(path, "tag", "v" + version)
    return path


HASHES = {}


def tree_hash(repo, version, registry=""):
    key = (repo, version)
    if key not in HASHES:
        app = new_app("hash%d" % len(HASHES))
        write(os.path.join(app, "project.dcon"),
              'dpm::v1::Project { name = "probe", version = "0.1.0", dependencies = { { location = "git+file://%s", version = "%s" } } }\n' % (repo, version))
        r = dpm(app, "lock", env={"DPM_REGISTRY": registry})
        check(r.returncode == 0, "probe lock for " + repo, detail(r))
        HASHES[key] = re.search(r'hash = "(sha256:[0-9a-f]{64})"', read(os.path.join(app, "project.lock.dcon"))).group(1)
    return HASHES[key]


class Registry:
    def __init__(self, name):
        self.dir = os.path.join(ROOT, "registries", name)
        os.makedirs(self.dir)
        write(os.path.join(self.dir, "registry.dcon"), 'dpm::v1::RegistryConfig { name = "%s" }\n' % name)
        self.url = "file://" + self.dir

    def add_alias(self, alias, repo, versions):
        entries = []
        for item in versions:
            version = item["version"]
            commit = item.get("commit") or git(repo, "rev-parse", "v%s^{commit}" % version)
            digest = item.get("hash") or tree_hash(repo, version, self.url)
            deps = ", ".join('{ location = "%s", version = "%s" }' % d for d in item.get("deps", []))
            text = '{ version = "%s", source = "git+file://%s", commit = "%s", hash = "%s"' % (version, repo, commit, digest)
            if deps:
                text += ", dependencies = { %s }" % deps
            if item.get("yanked"):
                text += ", yanked = true"
            entries.append(text + " }")
        write(os.path.join(self.dir, "aliases", alias[:2], alias + ".dcon"),
              'dpm::v1::RegistryAlias { alias = "%s", location = "git+file://%s", versions = { %s } }\n' % (alias, repo, ", ".join(entries)))


def import_main(module, call="answer"):
    return "module main;\nimport %s::lib;\n\npublic i32 main()\n{\n    return %s::lib::%s();\n}\n" % (module, module, call)


def scenario_registry_file():
    repo = make_lib_repo("regpkg", [("1.0.0", 10), ("1.1.0", 11), ("1.2.0", 12), ("2.0.0-rc.1", 20)])
    reg = Registry("main")
    reg.add_alias("regpkg", repo, [{"version": "1.0.0"}, {"version": "1.1.0", "yanked": True}, {"version": "1.2.0"}, {"version": "2.0.0-rc.1"}])
    env = {"DPM_REGISTRY": reg.url}
    app = new_app("regapp")
    write(os.path.join(app, "src", "main.dc"), import_main("regpkg"))
    r = dpm(app, "add", "regpkg", env=env)
    check(r.returncode == 0, "add an alias", detail(r))
    manifest = read(os.path.join(app, "project.dcon"))
    check('location = "regpkg"' in manifest and 'version = "1.2.0"' in manifest, "add picks the latest release that is neither yanked nor a pre-release", manifest)
    r = dpm(app, "run", env=env)
    check(r.returncode == 12, "run uses the alias dependency", detail(r))
    lock = read(os.path.join(app, "project.lock.dcon"))
    check('location = "git+file://%s"' % repo in lock, "the lock records the canonical location", lock)
    check(git(repo, "rev-parse", "v1.2.0^{commit}") in lock and tree_hash(repo, "1.2.0") in lock, "the lock records the registry's commit and hash", lock)
    check('"regpkg"' not in lock.replace("regpkg", "x", 0) or 'location = "regpkg"' not in lock, "the lock holds no alias")
    r = dpm(app, "build", "--locked", env={"DPM_REGISTRY": "file://" + os.path.join(ROOT, "nowhere")})
    check(r.returncode == 0, "a current lock never reads the registry", detail(r))
    r = dpm(app, "tree", env=env)
    check(r.returncode == 0 and "git+file://%s@1.2.0" % repo in r.stdout, "tree shows the canonical location", detail(r))

    before = read(os.path.join(app, "project.lock.dcon"))
    bad = tree_hash(repo, "1.0.0")
    entry = os.path.join(reg.dir, "aliases", "re", "regpkg.dcon")
    original = read(entry)
    good = tree_hash(repo, "1.2.0")
    write(entry, original.replace('hash = "%s"' % good, 'hash = "%s"' % bad))
    r = dpm(app, "lock", env=env)
    check(r.returncode == 0 and "differs from the lock" in r.stderr, "a registry entry that disagrees with the lock only warns", detail(r))
    check(read(os.path.join(app, "project.lock.dcon")) == before, "the lock's pins win over the registry")
    check(dpm(app, "build", env=env).returncode == 0, "the build still uses the locked hash")
    os.remove(os.path.join(app, "project.lock.dcon"))
    r = dpm(app, "lock", env=env)
    check(r.returncode == 4 and ("mismatch" in r.stderr or "contains version" in r.stderr), "a registry hash of other content is fatal when nothing is locked", detail(r))
    check(not os.path.exists(os.path.join(app, "project.lock.dcon")), "no lock is written for an unverified entry")
    write(entry, original)
    r = dpm(app, "lock", env=env)
    check(r.returncode == 0 and read(os.path.join(app, "project.lock.dcon")) == before, "the corrected registry resolves to the same lock", detail(r))

    write(entry, original.replace('hash = "%s"' % good, 'hash = "sha256:%s"' % ("f" * 64)))
    os.remove(os.path.join(app, "project.lock.dcon"))
    r = dpm(app, "lock", env=env)
    check(r.returncode == 4 and "mismatch" in r.stderr, "a registry hash that matches nothing is fatal", detail(r))
    check(not os.path.exists(os.path.join(app, "project.lock.dcon")), "no lock is written for a hash that matches nothing")
    write(entry, original)
    check(dpm(app, "lock", env=env).returncode == 0, "the corrected registry resolves again")

    wrong = git(repo, "rev-parse", "v1.0.0^{commit}")
    write(entry, original.replace(git(repo, "rev-parse", "v1.2.0^{commit}"), wrong))
    os.remove(os.path.join(app, "project.lock.dcon"))
    r = dpm(app, "lock", env=env)
    check(r.returncode == 4 and "mismatch" in r.stderr, "a registry commit paired with a cached hash is verified, not trusted", detail(r))
    check(not os.path.exists(os.path.join(app, "project.lock.dcon")), "no lock is written for a commit that does not produce the hash")
    write(entry, original)
    check(dpm(app, "lock", env=env).returncode == 0, "the corrected registry resolves again after a wrong commit")

    app2 = new_app("regyank")
    write(os.path.join(app2, "src", "main.dc"), import_main("regpkg"))
    r = dpm(app2, "add", "regpkg@1.1.0", env=env)
    check(r.returncode == 0 and "yanked" in r.stderr, "a yanked minimum is replaced with a warning", detail(r))
    check('version = "1.2.0"' in read(os.path.join(app2, "project.lock.dcon")), "the next release in the series is locked")
    check(dpm(app2, "run", env=env).returncode == 12, "the replacement builds")

    r = dpm(app2, "add", "nothere", env=env)
    check(r.returncode == 4 and "not in any registry" in r.stderr, "an unknown alias is a resolution error", detail(r))
    r = dpm(app2, "add", "nothere@1.0.0", env=env)
    check(r.returncode == 4 and "not in any registry" in r.stderr, "an unknown alias with a version", detail(r))
    r = dpm(app2, "add", "regpkg", env={"DPM_REGISTRY": "file://" + os.path.join(ROOT, "nowhere")})
    check(r.returncode == 4 and "registry.dcon" in r.stderr, "a missing registry is reported", detail(r))
    r = dpm(app2, "add", "regpkg", env={"DPM_REGISTRY": ""})
    check(r.returncode != 0 and "registry" in r.stderr, "no registry means aliases cannot be resolved", detail(r))
    r = dpm(app2, "add", "regpkg", env={"DPM_ALLOW_FILE_URLS": None, "DPM_REGISTRY": reg.url})
    check(r.returncode == 3 and "file://" in r.stderr, "file registries need the test opt-in", detail(r))

    git(repo, "checkout", "-q", "main")
    write(os.path.join(repo, "project.dcon"), 'dpm::v1::Project { name = "regpkg", version = "1.3.0" }\n')
    write(os.path.join(repo, "src", "regpkg", "lib.dc"), "module regpkg::lib;\n\npublic i32 answer()\n{\n    return 13;\n}\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "1.3.0")
    git(repo, "tag", "v1.3.0")
    reg.add_alias("regpkg", repo, [{"version": "1.0.0"}, {"version": "1.1.0", "yanked": True}, {"version": "1.2.0"}, {"version": "1.3.0"}, {"version": "2.0.0-rc.1"}])
    r = dpm(app, "update", "--dry-run", env=env)
    check(r.returncode == 0 and "1.2.0 -> 1.3.0" in r.stdout, "update finds the newer registry release", detail(r))
    r = dpm(app, "update", env=env)
    check(r.returncode == 0 and dpm(app, "run", env=env).returncode == 13, "update raises an alias dependency", detail(r))


def scenario_registry_dependencies():
    util = make_pkg_repo("utilpkg", "utilpkg", [("1.0.0", 5)])
    mid = make_pkg_repo("midpkg", "midpkg", [("1.0.0", 0)], deps=', dependencies = { { location = "utilpkg", version = "1.0.0" } }',
                        body="import utilpkg::lib;")
    write(os.path.join(mid, "src", "midpkg", "lib.dc"),
          "module midpkg::lib;\nimport utilpkg::lib;\n\npublic i32 answer()\n{\n    return utilpkg::lib::answer() + 1;\n}\n")
    git(mid, "add", "-A")
    git(mid, "commit", "-q", "-m", "fix")
    git(mid, "tag", "-f", "v1.0.0")
    liar = make_pkg_repo("liarpkg", "liarpkg", [("1.0.0", 1)])
    reg = Registry("deps")
    reg.add_alias("utilpkg", util, [{"version": "1.0.0"}])
    reg.add_alias("midpkg", mid, [{"version": "1.0.0", "deps": [("utilpkg", "1.0.0")]}])
    reg.add_alias("liarpkg", liar, [{"version": "1.0.0", "deps": [("utilpkg", "1.0.0")]}])
    env = {"DPM_REGISTRY": reg.url}
    app = new_app("depsapp")
    write(os.path.join(app, "src", "main.dc"), import_main("midpkg"))
    r = dpm(app, "add", "midpkg", env=env)
    check(r.returncode == 0, "add a package whose requirements come from the registry summary", detail(r))
    lock = read(os.path.join(app, "project.lock.dcon"))
    check(lock.count("version = ") == 2 and "git+file://%s" % util in lock, "the transitive alias is locked", lock)
    r = dpm(app, "run", env=env)
    check(r.returncode == 6, "run builds the transitive alias dependency", detail(r))
    r = dpm(app, "why", "git+file://" + util, env=env)
    check(r.returncode == 0 and "midpkg" in r.stdout.replace("git+file://" + mid, "midpkg"), "why explains the transitive package", detail(r))

    liar_app = new_app("liarapp")
    r = dpm(liar_app, "add", "liarpkg", env=env)
    check(r.returncode == 4 and "differ from the lock" in r.stderr, "a registry summary that does not match the manifest is refused", detail(r))
    check("liarpkg" not in read(os.path.join(liar_app, "project.dcon")), "a refused add leaves the manifest alone")
    check(not os.path.exists(os.path.join(liar_app, "project.lock.dcon")), "a refused add writes no lock")

    one = make_pkg_repo("same_a", "same", [("1.0.0", 1)])
    two = make_pkg_repo("same_b", "same", [("1.0.0", 2)])
    reg.add_alias("samea", one, [{"version": "1.0.0"}])
    reg.add_alias("sameb", two, [{"version": "1.0.0"}])
    clash = new_app("clashapp")
    check(dpm(clash, "add", "samea", env=env).returncode == 0, "first namespace owner")
    r = dpm(clash, "add", "sameb", env=env)
    check(r.returncode == 4 and "both provide the namespace 'same'" in r.stderr, "two registry packages with one name are refused", detail(r))


def scenario_registry_configuration():
    repo = make_pkg_repo("cfgpkg", "cfgpkg", [("1.0.0", 4)])
    reg = Registry("cfg")
    reg.add_alias("cfgpkg", repo, [{"version": "1.0.0"}])
    app = new_app("cfgapp")
    write(os.path.join(app, "src", "main.dc"), import_main("cfgpkg"))
    project = read(os.path.join(app, "project.dcon"))
    write(os.path.join(app, "project.dcon"), project.replace('version = "0.1.0",', 'version = "0.1.0",\n    registry = "%s",' % reg.url, 1))
    r = dpm(app, "add", "cfgpkg", env={"DPM_REGISTRY": None})
    check(r.returncode == 0, "the project's registry is used", detail(r))
    r = dpm(app, "run", env={"DPM_REGISTRY": None})
    check(r.returncode == 4, "run with the project registry", detail(r))
    other = Registry("cfg_empty")
    r = dpm(app, "lock", env={"DPM_REGISTRY": other.url})
    check(r.returncode == 4, "the environment overrides the project registry", detail(r))

    config_dir = os.path.join(ROOT, "config")
    os.makedirs(config_dir)
    config = os.path.join(config_dir, "config.dcon")
    write(config, 'dpm::v1::UserConfig { registries = { "%s" } }\n' % reg.url)
    app2 = new_app("cfgapp2")
    write(os.path.join(app2, "src", "main.dc"), import_main("cfgpkg"))
    r = dpm(app2, "add", "cfgpkg", env={"DPM_REGISTRY": None, "DPM_CONFIG": config})
    check(r.returncode == 0, "the user configuration supplies registries", detail(r))
    write(config, 'dpm::v1::UserConfig { registries = { "http://example.com/reg" } }\n')
    r = dpm(app2, "lock", env={"DPM_REGISTRY": None, "DPM_CONFIG": config})
    check(r.returncode == 3 and "config.dcon" in r.stderr, "a plain http registry in the user configuration is refused", detail(r))
    write(config, 'dpm::v1::UserConfig { registries = { "https://127.0.0.1:1/reg" }, offline = true }\n')
    r = dpm(app2, "build", env={"DPM_REGISTRY": None, "DPM_CONFIG": config})
    check(r.returncode == 0, "offline in the user configuration with a current lock needs no registry", detail(r))
    app3 = new_app("cfgapp3")
    r = dpm(app3, "add", "cfgpkg", env={"DPM_REGISTRY": None, "DPM_CONFIG": config})
    check(r.returncode == 5 and "not in cache" in r.stderr, "offline in the user configuration forbids the network", detail(r))
    write(config, "this is not a config")
    r = dpm(app3, "lock", env={"DPM_REGISTRY": None, "DPM_CONFIG": config})
    check(r.returncode == 3, "an invalid user configuration is a manifest error", detail(r))


class RegistryServer:
    def __init__(self, root):
        self.root = root
        self.requests = []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                path = self.path
                status = outer.respond(self, path)
                outer.requests.append((path, status))

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.port

    def respond(self, handler, path):
        if path.startswith("/redir/"):
            handler.send_response(302)
            handler.send_header("Location", self.base + "/reg/" + path[len("/redir/"):])
            handler.end_headers()
            return 302
        if path.startswith("/big/") and path != "/big/registry.dcon":
            body = b"x" * (300 * 1024)
        else:
            rel = path[len("/big/"):] if path.startswith("/big/") else path[len("/reg/"):] if path.startswith("/reg/") else None
            full = os.path.join(self.root, rel) if rel else None
            if not full or not os.path.isfile(full):
                handler.send_response(404)
                handler.send_header("Content-Length", "0")
                handler.end_headers()
                return 404
            body = open(full, "rb").read()
        etag = '"%s"' % hashlib.sha1(body).hexdigest()
        if handler.headers.get("If-None-Match") == etag:
            handler.send_response(304)
            handler.send_header("ETag", etag)
            handler.end_headers()
            return 304
        handler.send_response(200)
        handler.send_header("ETag", etag)
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)
        return 200

    def count(self, status=None, prefix=""):
        return len([1 for p, s in self.requests if p.startswith(prefix) and (status is None or s == status)])

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def scenario_registry_http():
    repo = make_pkg_repo("httppkg", "httppkg", [("1.0.0", 21), ("1.1.0", 22)])
    reg = Registry("http")
    reg.add_alias("httppkg", repo, [{"version": "1.0.0"}])
    server = RegistryServer(reg.dir)
    try:
        url = server.base + "/reg"
        env = {"DPM_REGISTRY": url}
        app = new_app("httpapp")
        write(os.path.join(app, "src", "main.dc"), import_main("httppkg"))
        r = dpm(app, "add", "httppkg", env=env)
        check(r.returncode == 3 and "https" in r.stderr and server.count() == 0, "plain http registries need the loopback opt-in", detail(r))
        env["DPM_ALLOW_INSECURE_LOOPBACK"] = "1"
        r = dpm(app, "add", "httppkg", env=env)
        check(r.returncode == 0, "add through an http registry", detail(r))
        check(server.count(200, "/reg/registry.dcon") == 1 and server.count(200, "/reg/aliases/ht/httppkg.dcon") == 1, "the descriptor and the record are fetched once", str(server.requests))
        r = dpm(app, "run", env=env)
        check(r.returncode == 21, "run with an http registry dependency", detail(r))
        seen = len(server.requests)
        r = dpm(app, "lock", env=env)
        check(r.returncode == 0 and len(server.requests) == seen, "fresh cached files are not requested again", str(server.requests))
        before = read(os.path.join(app, "project.lock.dcon"))
        r = dpm(app, "lock", "--refresh", env=env)
        check(r.returncode == 0 and server.count(304) == 2, "--refresh revalidates with ETag and gets 304", str(server.requests) + detail(r))
        check(read(os.path.join(app, "project.lock.dcon")) == before, "a revalidated lock is unchanged")
        reg.add_alias("httppkg", repo, [{"version": "1.0.0"}, {"version": "1.1.0"}])
        r = dpm(app, "update", "--dry-run", env=env)
        check(r.returncode == 0 and "up to date" in r.stdout, "the cached record is used within the freshness window", detail(r))
        r = dpm(app, "update", "--dry-run", "--refresh", env=env)
        check(r.returncode == 0 and "1.0.0 -> 1.1.0" in r.stdout and server.count(200, "/reg/aliases") == 2, "--refresh picks up a changed record", detail(r) + str(server.requests))
        r = dpm(app, "add", "nothere", env=env)
        check(r.returncode == 4 and "not in any registry" in r.stderr, "a 404 is an unknown alias", detail(r))
        seen = len(server.requests)
        r = dpm(app, "add", "nothere", env=env)
        check(r.returncode == 4 and len(server.requests) == seen, "a missing alias is remembered for the freshness window", str(server.requests))

        r = dpm(app, "add", "httppkg", env={**env, "DPM_REGISTRY": server.base + "/redir"})
        check(r.returncode == 5 and "cannot fetch" in r.stderr, "a redirect to plain http is refused", detail(r))
        r = dpm(app, "add", "bigpkg", env={**env, "DPM_REGISTRY": server.base + "/big"})
        check(r.returncode == 5 and "size" in r.stderr.lower(), "an oversized response is refused", detail(r))
        cache = dpm(app, "cache", "path", env=env).stdout.strip()
        found = []
        for dirpath, _, files in os.walk(os.path.join(cache, "registry")):
            found += [f for f in files if f == "bigpkg.dcon"]
        check(not found, "an oversized response is not cached")
    finally:
        server.stop()
    r = dpm(app, "lock", "--offline", env=env)
    check(r.returncode == 0, "cached registry files serve an offline resolution", detail(r))
    r = dpm(app, "lock", "--refresh", env=env)
    check(r.returncode == 0 and "using the cached copy" in r.stderr, "an unreachable registry falls back to the cached copy", detail(r))
    r = dpm(app, "add", "fresh_alias", env=env)
    check(r.returncode == 5, "an alias that was never cached fails when the registry is unreachable", detail(r))
    r = dpm(app, "add", "fresh_alias", "--offline", env=env)
    check(r.returncode == 5 and "not in cache" in r.stderr, "offline mode names the missing cache entry", detail(r))


try:
    scenario_basic()
    scenario_run_args()
    scenario_path_dependency()
    scenario_git_dependency()
    scenario_offline_and_cache()
    scenario_registry_file()
    scenario_registry_dependencies()
    scenario_registry_configuration()
    scenario_registry_http()
    scenario_tests()
    scenario_formatting_and_determinism()
    scenario_misc()
finally:
    shutil.rmtree(ROOT, ignore_errors=True)

print("%d checks, %d failed" % (CHECKS, len(FAILURES)))
sys.exit(1 if FAILURES else 0)
