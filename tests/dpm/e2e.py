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
    odd = dpm(os.path.dirname(app), "init", "oddname", "--lib", "--name", "../escape")
    check(odd.returncode == 2 and "not usable" in odd.stderr, "init rejects a name that is not a path-safe identifier", detail(odd))
    for reserved in ("std", "core", "assert", "dcext", "dpm"):
        odd = dpm(os.path.dirname(app), "init", "res_" + reserved, "--name", reserved)
        check(odd.returncode == 2 and "cannot be a package name" in odd.stderr and not os.path.exists(os.path.join(os.path.dirname(app), "res_" + reserved, "project.dcon")),
              "init refuses the reserved name " + reserved, detail(odd))
        odd = dpm(os.path.dirname(app), "new", "newres_" + reserved, "--name", reserved)
        check(odd.returncode == 2 and "namespace of a library" in odd.stderr and not os.path.exists(os.path.join(os.path.dirname(app), "newres_" + reserved)),
              "new refuses the reserved name " + reserved, detail(odd))
    odd = dpm(os.path.dirname(app), "init", "oddname2", "--name", "1abc")
    check(odd.returncode == 2 and "not usable" in odd.stderr, "init rejects a name that is not an identifier", detail(odd))
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


def scenario_modules():
    app = new_app("modules", "return util::count(5);")
    write(os.path.join(app, "src", "main.dc"), "module main;\n\nimport util;\n\npublic i32 main()\n{\n    return util::count(5);\n}\n")
    write(os.path.join(app, "src", "util.dc"), "module util;\n\nvolatile i32 sink;\n\npublic i32 count(i32 n)\n{\n    i32 i = 0;\n    while i < n {\n        sink = i;\n        i = i + 1;\n    }\n    return sink;\n}\n")
    r = dpm(app, "run")
    check(r.returncode == 4, "a debug build links the project's other modules", detail(r))
    r = dpm(app, "run", "--profile", "release")
    check(r.returncode == 4, "and so does a release build", detail(r))
    dpm(app, "clean")
    r = dpm(app, "build", "-v")
    check("-fwhole-program" in r.stdout + r.stderr, "the debug profile compiles the whole import graph", detail(r))


def scenario_new():
    sample = os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates", "sample")
    parent = os.path.join(ROOT, "new")
    os.makedirs(parent)
    r = dpm(parent, "template", "check", sample)
    check(r.returncode == 0 and "sample: ok (7 files, 4 variables)" in r.stdout and r.stderr == "", "template check accepts the sample", detail(r))
    r = dpm(parent, "template", "info", "path:" + sample)
    check(r.returncode == 0 and "owner (string, required): Who owns the project" in r.stdout and "answer (integer, default \"42\", 0 to 255)" in r.stdout, "template info lists the variables", detail(r))
    r = dpm(parent, "template", "info", "builtin:library")
    check(r.returncode == 0 and "template library" in r.stdout, "template info describes a built-in template", detail(r))

    r = dpm(parent, "new", "made", "--template", "path:" + sample, "--set", "owner=me", "--set", "title=My Title")
    made = os.path.join(parent, "made")
    check(r.returncode == 0 and "created made from sample (7 files)" in r.stdout and "cd made && dpm run" in r.stdout, "new generates the sample", detail(r))
    check(read(os.path.join(made, "README.md")) == "# My Title\n\nLevel low, owned by me.\n@@name@@ stays as it is.\n", "placeholders are replaced and @@@@ is an escape")
    check(read(os.path.join(made, "docs", "made.md")) == "documentation for made\n", "a placeholder in a file name is replaced")
    check(not os.path.exists(os.path.join(made, "skip")), "excluded files are not generated")
    with open(os.path.join(made, "data", "blob.bin"), "rb") as f:
        check(f.read() == b"raw @@name@@ \0 bytes\n", "a file without substitution is copied byte for byte")
    check(os.access(os.path.join(made, "scripts", "run.sh"), os.X_OK) and not os.access(os.path.join(made, "README.md"), os.X_OK), "the executable bit is kept")
    check(read(os.path.join(made, ".gitignore")) == ".dpm/\ncompile_commands.json\n", "dot files are generated")
    r = dpm(made, "run")
    check(r.returncode == 42, "the generated project builds and runs", detail(r))
    leftovers = [n for n in os.listdir(parent) if n.startswith(".dpm-new")]
    check(leftovers == [], "no staging directory is left behind")

    r = dpm(parent, "new", "seven", "--template", "path:" + sample, "--set", "owner=me", "--set", "answer=7", "--name", "sev")
    check(r.returncode == 0 and dpm(os.path.join(parent, "seven"), "run").returncode == 7, "--set and --name are applied", detail(r))
    check('name = "sev"' in read(os.path.join(parent, "seven", "project.dcon")), "--name sets the package name")

    before = set(os.listdir(parent))
    for label, args in (
        ("a required variable is missing", ["--set", "title=x"]),
        ("an integer is out of range", ["--set", "owner=me", "--set", "answer=300"]),
        ("a choice is not one of the choices", ["--set", "owner=me", "--set", "level=mid"]),
        ("a string has a character that is not allowed", ["--set", "owner=me", '--set', 'title=a"b']),
        ("a string is too long", ["--set", "owner=me", "--set", "title=" + "x" * 41]),
        ("a variable is unknown", ["--set", "owner=me", "--set", "nope=1"]),
        ("a variable is set twice", ["--set", "owner=me", "--set", "owner=you"]),
        ("--set has no value", ["--set", "owner"]),
        ("the built-in dir cannot be set", ["--set", "owner=me", "--set", "dir=x"]),
        ("the name is not an identifier", ["--set", "owner=me", "--name", "1bad"]),
        ("--name and --set name disagree", ["--set", "owner=me", "--name", "a", "--set", "name=b"]),
    ):
        r = dpm(parent, "new", "refused", "--template", "path:" + sample, *args)
        check(r.returncode == 2 and not os.path.exists(os.path.join(parent, "refused")), "usage error, nothing created: " + label, detail(r))
    check(set(os.listdir(parent)) == before, "refused runs created nothing")

    r = dpm(parent, "new", "lib1", "--lib")
    check(r.returncode == 0 and os.path.isfile(os.path.join(parent, "lib1", "src", "lib1", "lib.dc")), "the built-in library template", detail(r))
    r = dpm(parent, "new", "x", "--template", "path:" + sample, "--lib", "--set", "owner=me")
    check(r.returncode == 2 and "--lib" in r.stderr, "--lib applies to built-in templates only", detail(r))
    r = dpm(parent, "new", "x", "--template", "builtin:nope")
    check(r.returncode == 2 and "no built-in template" in r.stderr, "an unknown built-in template", detail(r))
    r = dpm(parent, "new", "x", "--template", "sample")
    check(r.returncode == 4 and "no registry is configured" in r.stderr, "a bare word is a registry template name", detail(r))
    r = dpm(parent, "new", "x", "--template", "Not_A_Name")
    check(r.returncode == 2 and "not a built-in template" in r.stderr, "a word that is no template name is not looked up", detail(r))
    version = subprocess.run([DPM, "--version"], capture_output=True, text=True, env=ENV)
    dcc_version = subprocess.run([DCC, "--version"], capture_output=True, text=True, env=ENV).stdout.split()[1]
    check(version.returncode == 0 and re.fullmatch(r"dpm %s \(\S+\)\n" % re.escape(dcc_version), version.stdout) is not None, "dpm --version prints the toolchain version", version.stdout)

    def versioned(name, field, value):
        root = os.path.join(ROOT, "versioned", name)
        write(os.path.join(root, "template.dcon"), 'dpm::v1::Template { name = "ver", %s = "%s" }\n' % (field, value))
        write(os.path.join(root, "files", "a.txt"), "x\n")
        return root

    for field, tool in (("requires_dpm", "dpm"), ("requires_dcc", "dcc")):
        newer = versioned(field + "-newer", field, "99.0.0")
        r = dpm(parent, "new", "tooold_" + field, "--template", "path:" + newer)
        check(r.returncode == 1 and "requires %s 99.0.0" % tool in r.stderr and not os.path.exists(os.path.join(parent, "tooold_" + field)), "a template needing a newer %s is refused" % tool, detail(r))
        r = dpm(parent, "template", "info", "path:" + newer)
        check(r.returncode == 0 and "needs %s 99.0.0 or newer" % tool in r.stdout, "template info shows the %s requirement" % tool, detail(r))
        okay = versioned(field + "-ok", field, dcc_version)
        r = dpm(parent, "new", "fine_" + field, "--template", "path:" + okay)
        check(r.returncode == 0, "a template needing exactly this %s is accepted" % tool, detail(r))

    shutil.copytree(sample, os.path.join(parent, "tpl"))
    r = dpm(parent, "new", "x", "--template", "tpl")
    check(r.returncode == 2 and "write ./tpl or path:tpl" in r.stderr, "a relative directory needs ./ or path:", detail(r))
    r = dpm(parent, "new", "viadot", "--template", "./tpl", "--set", "owner=me")
    check(r.returncode == 0, "./DIR is a local template", detail(r))
    r = dpm(parent, "new", "viapath", "--template", "path:tpl", "--set", "owner=me")
    check(r.returncode == 0, "path:DIR is a local template", detail(r))

    busy = os.path.join(parent, "busy")
    write(os.path.join(busy, "mine.txt"), "mine")
    r = dpm(parent, "new", "busy", "--template", "path:" + sample, "--set", "owner=me")
    check(r.returncode == 1 and "not empty" in r.stderr and os.listdir(busy) == ["mine.txt"], "a non-empty directory is refused and untouched", detail(r))
    empty = os.path.join(parent, "empty")
    os.makedirs(os.path.join(empty, ".git"))
    r = dpm(parent, "new", "empty", "--template", "path:" + sample, "--set", "owner=me")
    check(r.returncode == 0 and os.path.isdir(os.path.join(empty, ".git")) and os.path.isfile(os.path.join(empty, "project.dcon")), "an empty directory with .git is filled", detail(r))
    here = os.path.join(parent, "here")
    os.makedirs(here)
    r = dpm(here, "new", "--template", "path:" + sample, "--set", "owner=me")
    check(r.returncode == 0 and 'name = "here"' in read(os.path.join(here, "project.dcon")), "no DIR means the current directory, named after it", detail(r))

    def broken(name, files, template=None, links=()):
        root = os.path.join(ROOT, "broken", name)
        write(os.path.join(root, "template.dcon"), template or 'dpm::v1::Template { name = "bad", substitute = { "*.md" } }\n')
        for rel, text in files.items():
            write(os.path.join(root, "files", rel), text)
        for rel, target in links:
            os.symlink(target, os.path.join(root, "files", rel))
        return root

    for label, root in (
        ("an unknown placeholder", broken("unknown", {"a.md": "one\n@@nope@@\n"})),
        ("a symbolic link", broken("link", {"a.md": "x"}, links=[("l", "a.md")])),
        ("a missing template.dcon", os.path.join(ROOT, "broken", "none")),
        ("an invalid template.dcon", broken("invalid", {}, template="dpm::v1::Template { name = 1 }\n")),
        ("a case collision", broken("case", {"Readme": "a", "README": "b"})),
        ("a generated project.dcon that is not valid", broken("project", {"project.dcon": "dpm::v1::Project { name = \"@@name@@\", version = \"nope\" }\n"},
                                                          template='dpm::v1::Template { name = "bad", substitute = { "project.dcon" } }\n')),
    ):
        os.makedirs(root, exist_ok=True)
        r = dpm(parent, "new", "never", "--template", "path:" + root)
        check(r.returncode == 3 and not os.path.exists(os.path.join(parent, "never")), "template error, nothing created: " + label, detail(r))
        r = dpm(parent, "template", "check", root)
        check(r.returncode == 3, "template check fails: " + label, detail(r))
    r = dpm(parent, "template", "check", broken("unused", {"a.md": "x"}, template='dpm::v1::Template { name = "bad", variables = { { name = "idle", default_value = "x" } }, exclude = { "gone/**" } }\n'))
    check(r.returncode == 0 and "variable 'idle' is not used" in r.stderr and "'gone/**' matches no file" in r.stderr, "template check warns without failing", detail(r))
    r = dpm(parent, "template", "check", broken("gitdir", {".git/config": "x"}))
    check(r.returncode == 0, "a .git directory at the template root is ignored", detail(r))


def make_template_repo(name, tags, branches=()):
    sample = os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates", "sample")
    path = os.path.join(ROOT, "repos", name)
    os.makedirs(path)
    git(path, "init", "-q", "--initial-branch=main")
    for tag, title in tags:
        shutil.rmtree(os.path.join(path, "files"), ignore_errors=True)
        shutil.copytree(sample, path, dirs_exist_ok=True)
        text = read(os.path.join(path, "template.dcon")).replace("Sample Title", title)
        write(os.path.join(path, "template.dcon"), text)
        git(path, "add", "-A")
        git(path, "commit", "-q", "-m", title)
        if tag:
            git(path, "tag", tag)
    for branch, title in branches:
        git(path, "checkout", "-q", "-b", branch)
        text = read(os.path.join(path, "template.dcon"))
        write(os.path.join(path, "template.dcon"), re.sub(r'default_value = "[^"]*", max_length = 40', 'default_value = "%s", max_length = 40' % title, text))
        git(path, "commit", "-q", "-am", title)
        git(path, "checkout", "-q", "main")
    return path


FETCH_LINE = re.compile(r"template (\S+) commit ([0-9a-f]{40}) hash (sha256:[0-9a-f]{64})")


def scenario_new_fetch():
    parent = os.path.join(ROOT, "newfetch")
    os.makedirs(parent)
    repo = make_template_repo("tpl", [("v1.0.0", "First Edition"), ("v1.1.0", "Second Edition")], branches=[("dev", "Dev Edition")])
    url = "git+file://" + repo
    r = dpm(parent, "new", "latest", "--template", url, "--set", "owner=me")
    m = FETCH_LINE.search(r.stdout)
    head_tag = git(repo, "rev-parse", "v1.1.0^{commit}")
    check(r.returncode == 0 and m and m.group(2) == head_tag, "the latest version tag is fetched and its commit is printed", detail(r))
    check("was not pinned" in r.stderr and m and m.group(3) in r.stderr, "an unpinned fetch warns and shows the hash", detail(r))
    check(read(os.path.join(parent, "latest", "README.md")).startswith("# Second Edition\n"), "the latest tag supplies the files")
    r = dpm(parent, "new", "older", "--template", url + "@1.0.0", "--set", "owner=me")
    m1 = FETCH_LINE.search(r.stdout)
    check(r.returncode == 0 and m1 and m1.group(2) == git(repo, "rev-parse", "v1.0.0^{commit}") and read(os.path.join(parent, "older", "README.md")).startswith("# First Edition\n"),
          "@version selects that tag", detail(r))
    r = dpm(parent, "new", "bybranch", "--template", url, "--ref", "dev", "--set", "owner=me")
    mb = FETCH_LINE.search(r.stdout)
    check(r.returncode == 0 and mb and mb.group(2) == git(repo, "rev-parse", "dev") and read(os.path.join(parent, "bybranch", "README.md")).startswith("# Dev Edition\n"),
          "--ref fetches a branch", detail(r))
    r = dpm(parent, "new", "bycommit", "--template", url, "--ref", head_tag, "--set", "owner=me")
    check(r.returncode == 0 and FETCH_LINE.search(r.stdout) and FETCH_LINE.search(r.stdout).group(2) == head_tag, "--ref fetches a commit id", detail(r))
    r = dpm(parent, "new", "bytag", "--template", url, "--ref", "v1.0.0", "--set", "owner=me")
    check(r.returncode == 0 and read(os.path.join(parent, "bytag", "README.md")).startswith("# First Edition\n"), "--ref fetches a tag", detail(r))

    git(repo, "checkout", "-q", "dev")
    write(os.path.join(repo, "moved.txt"), "moved\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "moved")
    git(repo, "checkout", "-q", "main")
    r = dpm(parent, "new", "moved", "--template", url, "--ref", "dev", "--set", "owner=me")
    mm = FETCH_LINE.search(r.stdout)
    check(r.returncode == 0 and mm and mm.group(2) == git(repo, "rev-parse", "dev") and mm.group(2) != mb.group(2), "a moved branch gives its new commit", detail(r))

    r = dpm(parent, "new", "pinned", "--template", url + "@1.0.0", "--set", "owner=me", "--pin", m1.group(3))
    check(r.returncode == 0 and "was not pinned" not in r.stderr, "a matching --pin is accepted silently", detail(r))
    r = dpm(parent, "new", "wrongpin", "--template", url + "@1.0.0", "--set", "owner=me", "--pin", "sha256:" + "0" * 64)
    check(r.returncode == 4 and not os.path.exists(os.path.join(parent, "wrongpin")), "a wrong --pin is an integrity error and nothing is created", detail(r))
    r = dpm(parent, "new", "badpin", "--template", url, "--pin", "abc")
    check(r.returncode == 2 and "--pin needs a hash" in r.stderr, "a malformed --pin is a usage error", detail(r))
    r = dpm(parent, "new", "both", "--template", url + "@1.0.0", "--ref", "dev")
    check(r.returncode == 2 and "cannot be used together" in r.stderr, "--ref and @version cannot be combined", detail(r))
    r = dpm(parent, "new", "local", "--template", "builtin:executable", "--ref", "dev")
    check(r.returncode == 2 and "apply to templates that are fetched" in r.stderr, "--ref does not apply to built-in templates", detail(r))
    r = dpm(parent, "new", "gone", "--template", url, "--ref", "nowhere", "--set", "owner=me")
    check(r.returncode in (4, 5) and "'nowhere'" in r.stderr and not os.path.exists(os.path.join(parent, "gone")), "an unknown ref fails and names it", detail(r))
    r = dpm(parent, "template", "info", url, "--ref", "dev")
    check(r.returncode == 0 and FETCH_LINE.search(r.stdout) and "template sample" in r.stdout and "owner (string, required)" in r.stdout, "template info fetches too", detail(r))

    tagless = make_template_repo("tagless", [("", "Tagless")])
    r = dpm(parent, "new", "t1", "--template", "git+file://" + tagless, "--set", "owner=me")
    check(r.returncode != 0 and "--ref" in r.stderr and not os.path.exists(os.path.join(parent, "t1")), "a repository without version tags asks for --ref", detail(r))
    r = dpm(parent, "new", "t2", "--template", "git+file://" + tagless, "--ref", "main", "--set", "owner=me")
    check(r.returncode == 0 and read(os.path.join(parent, "t2", "README.md")).startswith("# Tagless\n"), "--ref main works on a repository without tags", detail(r))

    sub = os.path.join(ROOT, "repos", "monotpl")
    os.makedirs(os.path.join(sub, "templates"))
    git(sub, "init", "-q", "--initial-branch=main")
    shutil.copytree(os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates", "sample"), os.path.join(sub, "templates", "inner"))
    write(os.path.join(sub, "other.txt"), "not part of the template\n")
    git(sub, "add", "-A")
    git(sub, "commit", "-q", "-m", "mono")
    git(sub, "tag", "templates/inner/v2.0.0")
    r = dpm(parent, "new", "inner", "--template", "git+file://" + sub + "#templates/inner", "--set", "owner=me")
    check(r.returncode == 0 and os.path.isfile(os.path.join(parent, "inner", "project.dcon")) and not os.path.exists(os.path.join(parent, "inner", "other.txt")),
          "a template in a subdirectory uses the subdirectory tags", detail(r))

    import tarfile
    archive = os.path.join(ROOT, "repos", "sample-1.0.tar.gz")
    with tarfile.open(archive, "w:gz") as tar:
        tar.add(os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates", "sample"), arcname="sample-1.0")
    r = dpm(parent, "new", "tarred", "--template", "file://" + archive, "--set", "owner=me")
    mt = re.search(r"template (\S+) hash (sha256:[0-9a-f]{64})", r.stdout)
    check(r.returncode == 0 and mt and os.path.isfile(os.path.join(parent, "tarred", "README.md")), "an https tarball template (file:// in tests)", detail(r))
    r = dpm(parent, "new", "tarpin", "--template", "file://" + archive, "--set", "owner=me", "--pin", mt.group(2))
    check(r.returncode == 0, "a tarball with a matching --pin", detail(r))
    r = dpm(parent, "new", "tarbad", "--template", "file://" + archive, "--set", "owner=me", "--pin", "sha256:" + "1" * 64)
    check(r.returncode == 4 and not os.path.exists(os.path.join(parent, "tarbad")), "a tarball with a wrong --pin", detail(r))
    r = dpm(parent, "new", "tarref", "--template", "file://" + archive, "--ref", "main")
    check(r.returncode == 2 and "not to tarballs" in r.stderr, "--ref does not apply to tarballs", detail(r))

    fresh = os.path.join(ROOT, "fresh-home")
    r = dpm(parent, "new", "cold", "--template", url, "--ref", "main", "--offline", "--set", "owner=me", env={"DPM_HOME": fresh})
    check(r.returncode == 5 and not os.path.exists(os.path.join(parent, "cold")), "offline with an empty cache is a network error", detail(r))
    r = dpm(parent, "new", "warm1", "--template", url, "--ref", "main", "--set", "owner=me", env={"DPM_HOME": fresh})
    r2 = dpm(parent, "new", "warm2", "--template", url, "--ref", "main", "--offline", "--set", "owner=me", env={"DPM_HOME": fresh})
    check(r.returncode == 0 and r2.returncode == 0 and FETCH_LINE.search(r.stdout).group(2) == FETCH_LINE.search(r2.stdout).group(2), "offline uses the cached ref", detail(r2))
    r = dpm(parent, "new", "novers", "--template", url, "--offline", "--set", "owner=me", env={"DPM_HOME": fresh})
    check(r.returncode == 5 and "offline" in r.stderr, "offline without --ref or @version cannot list tags", detail(r))
    r = dpm(parent, "new", "cached_tag", "--template", url + "@1.0.0", "--offline", "--set", "owner=me", env={"DPM_HOME": fresh})
    check(r.returncode == 5, "offline: a tag that was never fetched is not in the cache", detail(r))

    bad = make_template_repo("linked", [("v1.0.0", "Linked")])
    os.symlink("README.md", os.path.join(bad, "files", "link"))
    git(bad, "add", "-A")
    git(bad, "commit", "-q", "-m", "link")
    git(bad, "tag", "v1.0.1")
    r = dpm(parent, "new", "linked", "--template", "git+file://" + bad, "--set", "owner=me")
    check(r.returncode != 0 and not os.path.exists(os.path.join(parent, "linked")), "a fetched template with a symbolic link is refused", detail(r))
    empty = os.path.join(ROOT, "repos", "notemplate")
    os.makedirs(empty)
    git(empty, "init", "-q", "--initial-branch=main")
    write(os.path.join(empty, "readme.txt"), "x\n")
    git(empty, "add", "-A")
    git(empty, "commit", "-q", "-m", "x")
    r = dpm(parent, "new", "notpl", "--template", "git+file://" + empty, "--ref", "main")
    check(r.returncode == 3 and "has no template.dcon" in r.stderr and not os.path.exists(os.path.join(parent, "notpl")), "a repository without template.dcon is a template error", detail(r))
    leftovers = [n for n in os.listdir(parent) if n.startswith(".dpm-new")]
    check(leftovers == [], "no staging directory is left behind by fetched templates")


def scenario_new_registry():
    parent = os.path.join(ROOT, "newreg")
    os.makedirs(parent)
    repo = make_template_repo("regtpl", [("v1.0.0", "First Edition"), ("v1.1.0", "Second Edition"), ("v2.0.0", "Third Edition")])
    facts = {}
    for version in ("1.0.0", "1.1.0", "2.0.0"):
        r = dpm(parent, "template", "info", "git+file://%s@%s" % (repo, version))
        m = FETCH_LINE.search(r.stdout)
        check(r.returncode == 0 and m is not None, "probe the template at " + version, detail(r))
        facts[version] = (m.group(2), m.group(3))

    def record(name, location, versions, extra=None):
        entries = []
        for version in versions:
            commit, digest = facts[version]
            entry = '{ version = "%s", source = "git+file://%s", commit = "%s", hash = "%s"%s }' % (version, repo, commit, digest, (extra or {}).get(version, ""))
            entries.append(entry)
        return 'dpm::v1::RegistryTemplate { name = "%s", location = "git+file://%s", versions = { %s } }\n' % (name, location, ", ".join(entries))

    reg = Registry("tpls")
    write(os.path.join(reg.dir, "templates", "sa", "sample.dcon"), record("sample", repo, ["1.0.0", "1.1.0", "2.0.0"], {"1.1.0": ", yanked = true"}))
    env = {"DPM_REGISTRY": reg.url}

    r = dpm(parent, "new", "latest", "--template", "sample", "--set", "owner=me", env=env)
    check(r.returncode == 0 and "template sample@2.0.0 from %s commit %s hash %s" % (reg.url, facts["2.0.0"][0], facts["2.0.0"][1]) in r.stdout and "was not pinned" not in r.stderr,
          "a registry template is fetched at the latest version that is not yanked", detail(r))
    check(read(os.path.join(parent, "latest", "README.md")).startswith("# Third Edition\n"), "the registry's version supplies the files")
    r = dpm(parent, "new", "older", "--template", "sample@1.0.0", "--set", "owner=me", env=env)
    check(r.returncode == 0 and read(os.path.join(parent, "older", "README.md")).startswith("# First Edition\n"), "name@version selects a version", detail(r))
    r = dpm(parent, "new", "yanked", "--template", "sample@1.1.0", "--set", "owner=me", env=env)
    check(r.returncode == 0 and "sample@1.1.0 is yanked" in r.stderr, "a yanked version can still be asked for, with a warning", detail(r))
    r = dpm(parent, "new", "pinned", "--template", "sample", "--set", "owner=me", "--pin", facts["2.0.0"][1], env=env)
    check(r.returncode == 0, "a --pin equal to the record's hash is accepted", detail(r))
    r = dpm(parent, "new", "wrongpin", "--template", "sample", "--set", "owner=me", "--pin", "sha256:" + "2" * 64, env=env)
    check(r.returncode == 4 and "the registry lists" in r.stderr and not os.path.exists(os.path.join(parent, "wrongpin")), "a --pin that differs from the record is an integrity error", detail(r))
    r = dpm(parent, "new", "withref", "--template", "sample", "--ref", "main", env=env)
    check(r.returncode == 2 and "--ref applies to git locations" in r.stderr, "--ref does not apply to registry templates", detail(r))
    r = dpm(parent, "new", "novers", "--template", "sample@9.9.9", "--set", "owner=me", env=env)
    check(r.returncode == 4 and "has no version 9.9.9 (published: 1.0.0, 1.1.0, 2.0.0)" in r.stderr, "an unpublished version is refused with the list", detail(r))
    r = dpm(parent, "new", "absent", "--template", "nosuchtpl", "--set", "owner=me", env=env)
    check(r.returncode == 4 and "template 'nosuchtpl' is not in any registry" in r.stderr, "an unknown template name", detail(r))
    r = dpm(parent, "new", "noreg", "--template", "sample", "--set", "owner=me")
    check(r.returncode == 4 and "no registry is configured" in r.stderr, "a bare name needs a registry", detail(r))
    r = dpm(parent, "template", "info", "sample", env=env)
    check(r.returncode == 0 and "template sample" in r.stdout and "owner (string, required)" in r.stdout, "template info resolves registry names", detail(r))

    bad_hash = record("sample", repo, ["2.0.0"]).replace(facts["2.0.0"][1], "sha256:" + "3" * 64)
    other = Registry("badtpls")
    write(os.path.join(other.dir, "templates", "sa", "sample.dcon"), bad_hash)
    r = dpm(parent, "new", "lying", "--template", "sample", "--set", "owner=me", env={"DPM_REGISTRY": other.url, "DPM_HOME": os.path.join(ROOT, "lying-home")})
    check(r.returncode == 4 and "hash mismatch" in r.stderr and not os.path.exists(os.path.join(parent, "lying")), "a record whose hash does not match the fetched tree is refused", detail(r))

    claim = Registry("claimtpls")
    write(os.path.join(claim.dir, "templates", "ot", "other.dcon"), record("other", repo, ["2.0.0"]))
    r = dpm(parent, "new", "claimed", "--template", "other", "--set", "owner=me", env={"DPM_REGISTRY": claim.url})
    check(r.returncode == 4 and "is named 'sample', but the registry lists it as 'other'" in r.stderr and not os.path.exists(os.path.join(parent, "claimed")),
          "a template that does not claim its registry name is refused", detail(r))

    misnamed = Registry("misnamed")
    write(os.path.join(misnamed.dir, "templates", "sa", "sample.dcon"), record("sample", repo, ["2.0.0"]).replace('name = "sample"', 'name = "different"'))
    r = dpm(parent, "new", "misnamed", "--template", "sample", "--set", "owner=me", env={"DPM_REGISTRY": misnamed.url})
    check(r.returncode == 4 and "describes template 'different'" in r.stderr, "a record for another name is refused", detail(r))

    with_alias = Registry("both")
    write(os.path.join(with_alias.dir, "templates", "sa", "sample.dcon"), record("sample", repo, ["2.0.0"]))
    libs = make_lib_repo("samplelib", [("1.0.0", 5)])
    with_alias.add_alias("sample", libs, [{"version": "1.0.0"}])
    r = dpm(parent, "new", "tplside", "--template", "sample", "--set", "owner=me", env={"DPM_REGISTRY": with_alias.url})
    check(r.returncode == 0 and os.path.isfile(os.path.join(parent, "tplside", "README.md")), "a template and a package may share a name", detail(r))
    app = new_app("aliasside")
    write(os.path.join(app, "src", "main.dc"), import_main("samplelib"))
    r = dpm(app, "add", "sample", env={"DPM_REGISTRY": with_alias.url})
    check(r.returncode == 0 and 'location = "sample"' in read(os.path.join(app, "project.dcon")), "and dpm add still finds the package", detail(r))

    home = os.path.join(ROOT, "offline-home")
    r = dpm(parent, "new", "warm", "--template", "sample", "--set", "owner=me", env={"DPM_REGISTRY": reg.url, "DPM_HOME": home})
    r2 = dpm(parent, "new", "cold", "--template", "sample", "--set", "owner=me", "--offline", env={"DPM_REGISTRY": reg.url, "DPM_HOME": home})
    check(r.returncode == 0 and r2.returncode == 0, "offline uses the cached record and tree", detail(r2))
    r = dpm(parent, "new", "never", "--template", "sample", "--set", "owner=me", "--offline", env={"DPM_REGISTRY": reg.url, "DPM_HOME": os.path.join(ROOT, "empty-home")})
    check(r.returncode == 5 and not os.path.exists(os.path.join(parent, "never")), "offline with an empty cache is a network error", detail(r))


def retemplate(repo, title, tag=None):
    text = read(os.path.join(repo, "template.dcon"))
    write(os.path.join(repo, "template.dcon"), re.sub(r'default_value = "[A-Za-z ]*", max_length = 40', 'default_value = "%s", max_length = 40' % title, text))
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", title)
    if tag:
        git(repo, "tag", "-f", tag)


def scenario_publish_template():
    repo = make_template_repo("pubtpl", [("v1.0.0", "First Edition")])
    reg = Registry("pubtpls")
    args = ["--registry-dir", reg.dir]
    env = {"DPM_REGISTRY": reg.url}
    location = "git+file://" + repo
    record = os.path.join(reg.dir, "templates", "sa", "sample.dcon")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "--location" in r.stderr and "dpm registry template" in r.stderr, "publishing an unknown template needs a location", detail(r))
    check(not os.path.exists(os.path.join(reg.dir, "templates")), "a refused publish writes nothing")
    r = dpm(repo, "publish", *args, "--location", location, "--dry-run")
    check(r.returncode == 0 and "would write" in r.stdout and not os.path.exists(os.path.join(reg.dir, "templates")), "--dry-run writes nothing", detail(r))
    r = dpm(repo, "publish", *args, "--location", location)
    check(r.returncode == 0 and os.path.isfile(record), "publish writes the template record", detail(r))
    check("git checkout -b 'publish-template-sample-1.0.0'" in r.stdout and "gh pr create" in r.stdout, "publish prints the git commands", r.stdout)
    check(not os.path.exists(os.path.join(reg.dir, ".git")) and not os.path.exists(os.path.join(reg.dir, "aliases")) and not os.path.exists(os.path.join(reg.dir, "locations")),
          "publish never runs git and writes no alias or pointer")
    text = read(record)
    probe = dpm(ROOT, "template", "info", location + "@1.0.0")
    m = FETCH_LINE.search(probe.stdout)
    check(m and git(repo, "rev-parse", "HEAD") in text and m.group(3) in text, "the record holds the tag's commit and tree hash", text)
    check(text.startswith("dpm::v1::RegistryTemplate {\n    name = \"sample\",\n"), "the record is printed canonically", text)
    r = dpm(ROOT, "registry", "check", reg.dir)
    check(r.returncode == 0 and "1 templates, 1 template versions, 0 problems" in r.stdout, "registry check accepts the published template", detail(r))
    r = dpm(ROOT, "registry", "check", reg.dir, "--verify-sources")
    check(r.returncode == 0, "registry check re-fetches and verifies the template", detail(r))

    parent = os.path.join(ROOT, "pubtplconsumer")
    os.makedirs(parent)
    r = dpm(parent, "new", "consumer", "--template", "sample", "--set", "owner=me", env=env)
    check(r.returncode == 0 and read(os.path.join(parent, "consumer", "README.md")).startswith("# First Edition\n"), "the published template is consumed by name", detail(r))

    r = dpm(repo, "publish", *args)
    check(r.returncode == 0 and "already in the registry" in r.stdout and read(record) == text, "publishing the same version twice changes nothing", detail(r))
    r = dpm(repo, "publish", *args, "--location", "git+file://" + repo + "-elsewhere")
    check(r.returncode == 1 and "belongs to" in r.stderr, "a different location for a known template is refused", detail(r))

    retemplate(repo, "Second Edition")
    write(os.path.join(repo, "extra.txt"), "x")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "not clean" in r.stderr, "an untracked file is refused", detail(r))
    os.remove(os.path.join(repo, "extra.txt"))
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "no version tag points at HEAD" in r.stderr, "HEAD without a version tag is refused", detail(r))
    git(repo, "tag", "v1.1.0")
    git(repo, "tag", "v1.2.0")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "more than one version tag" in r.stderr and "v1.1.0" in r.stderr and "v1.2.0" in r.stderr, "two version tags at HEAD are refused", detail(r))
    git(repo, "tag", "-d", "v1.2.0")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 0, "publish a second version", detail(r))
    check(re.findall(r'version = "([0-9.]+)"', read(record)) == ["1.0.0", "1.1.0"], "versions stay ascending", read(record))
    retemplate(repo, "Third Edition", tag="1.2.0")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 0 and "1.2.0" in r.stdout, "a tag without the v prefix is a version tag too", detail(r))
    check(re.findall(r'version = "([0-9.]+)"', read(record)) == ["1.0.0", "1.1.0", "1.2.0"], "three versions", read(record))
    check(dpm(ROOT, "registry", "check", reg.dir).returncode == 0, "registry check after three versions")
    retemplate(repo, "Mutated", tag="1.2.0")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "immutable" in r.stderr, "a published version cannot change", detail(r))
    check(len(re.findall("version =", read(record))) == 3, "the immutable record is untouched")

    r = dpm(ROOT, "registry", "yank-template", "sample", "1.2.0", *args)
    check(r.returncode == 0 and "yanked = true" in read(record), "yank-template", detail(r))
    r = dpm(parent, "new", "afteryank", "--template", "sample", "--set", "owner=me", env=env)
    check(r.returncode == 0 and "sample@1.1.0" in r.stdout and read(os.path.join(parent, "afteryank", "README.md")).startswith("# Second Edition\n"),
          "a yanked version is skipped by default", detail(r))
    r = dpm(ROOT, "registry", "yank-template", "sample", "1.2.0", *args)
    check(r.returncode == 0 and "nothing to change" in r.stdout, "yanking twice is a no-op", detail(r))
    r = dpm(ROOT, "registry", "yank-template", "sample", "1.2.0", "--undo", *args)
    check(r.returncode == 0 and "yanked = false" in read(record), "unyank-template", detail(r))
    r = dpm(ROOT, "registry", "yank-template", "sample", "9.9.9", *args)
    check(r.returncode == 1 and "not in the record" in r.stderr, "yanking an unknown version fails", detail(r))
    r = dpm(ROOT, "registry", "yank-template", "nosuch", "1.0.0", *args)
    check(r.returncode == 1 and "is not in" in r.stderr, "yanking an unknown template fails", detail(r))

    r = dpm(ROOT, "registry", "template", "git+file://" + repo + "-two", "second", *args)
    check(r.returncode == 0 and os.path.isfile(os.path.join(reg.dir, "templates", "se", "second.dcon")), "registry template reserves a name", detail(r))
    r = dpm(ROOT, "registry", "template", "git+file://" + repo + "-three", "second", *args)
    check(r.returncode == 1 and "already taken" in r.stderr, "a taken template name is refused", detail(r))
    r = dpm(ROOT, "registry", "template", "git+file://" + repo + "-two", "third", *args)
    check(r.returncode == 1 and "already claimed" in r.stderr, "a claimed location is refused", detail(r))
    r = dpm(ROOT, "registry", "template", "git+file://" + repo + "-four", "Bad_Name", *args)
    check(r.returncode == 1 and "not a valid template name" in r.stderr, "a bad template name is refused", detail(r))
    check(dpm(ROOT, "registry", "check", reg.dir).returncode == 0, "registry check after the commands")

    write(os.path.join(reg.dir, "templates", "zz", "junk.dcon"), "garbage")
    r = dpm(ROOT, "registry", "check", reg.dir)
    check(r.returncode != 0 and "templates/zz/junk.dcon" in r.stderr, "registry check reports a broken template record", detail(r))
    os.remove(os.path.join(reg.dir, "templates", "zz", "junk.dcon"))
    os.rmdir(os.path.join(reg.dir, "templates", "zz"))

    broken = make_template_repo("brokentpl", [("v1.0.0", "Broken")])
    write(os.path.join(broken, "files", "README.md"), "# @@nope@@\n")
    git(broken, "add", "-A")
    git(broken, "commit", "-q", "-m", "broken")
    git(broken, "tag", "v1.0.1")
    breg = Registry("brokenreg")
    r = dpm(broken, "publish", "--registry-dir", breg.dir, "--location", "git+file://" + broken)
    check(r.returncode == 1 and "does not pass 'dpm template check'" in r.stderr and "unknown variable 'nope'" in r.stderr, "a template that fails the template check is refused", detail(r))
    check(not os.path.exists(os.path.join(breg.dir, "templates")), "and nothing is written")

    both = make_template_repo("bothtpl", [("v1.0.0", "Both")])
    write(os.path.join(both, "project.dcon"), 'dpm::v1::Project { name = "both", version = "1.0.0" }\n')
    git(both, "add", "-A")
    git(both, "commit", "-q", "-m", "both")
    r = dpm(both, "publish", "--registry-dir", breg.dir, "--location", "git+file://" + both)
    check(r.returncode == 2 and "both project.dcon and template.dcon" in r.stderr, "a directory with both files is refused", detail(r))

    sub = os.path.join(ROOT, "repos", "pubmono")
    os.makedirs(sub)
    git(sub, "init", "-q", "--initial-branch=main")
    shutil.copytree(os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates", "sample"), os.path.join(sub, "templates", "inner"))
    git(sub, "add", "-A")
    git(sub, "commit", "-q", "-m", "mono")
    git(sub, "tag", "templates/inner/v1.0.0")
    mreg = Registry("monoreg")
    inner = os.path.join(sub, "templates", "inner")
    r = dpm(inner, "publish", "--registry-dir", mreg.dir, "--location", "git+file://" + sub)
    check(r.returncode == 1 and "names the subdirectory ''" in r.stderr, "the location must name the subdirectory", detail(r))
    r = dpm(inner, "publish", "--registry-dir", mreg.dir, "--location", "git+file://" + sub + "#templates/inner")
    check(r.returncode == 0 and "tag templates/inner/v1.0.0" in r.stdout, "a template in a subdirectory publishes with its subdirectory tag", detail(r))
    r = dpm(ROOT, "registry", "check", mreg.dir, "--verify-sources")
    check(r.returncode == 0, "and verifies", detail(r))
    r = dpm(parent, "new", "frommono", "--template", "sample", "--set", "owner=me", env={"DPM_REGISTRY": mreg.url})
    check(r.returncode == 0 and os.path.isfile(os.path.join(parent, "frommono", "project.dcon")), "and is consumed", detail(r))


def elf_entry(path):
    import struct
    with open(path, "rb") as f:
        header = f.read(64)
    return struct.unpack_from("<Q", header, 24)[0]


def scenario_kernel_build():
    app = new_app("kern")
    write(os.path.join(app, "project.dcon"), """dpm::v1::Project {
    name = "kern",
    version = "0.1.0",
    products = {
        {
            name = "kern",
            entry = "src/main.dc",
            linker_script = "link/kernel.ld",
            settings = {
                target = Target::X86_64Elf,
                model = Model::Kernel,
                libdcext = Libdcext::Freestanding,
                red_zone = Tri::Off,
                simd = Tri::Off,
                x87 = Tri::Off,
                stack_protector = Tri::Off,
                partial_eval = Tri::On,
            },
        },
    },
}
""")
    write(os.path.join(app, "src", "main.dc"), "module main;\n\nimport ticks;\n\n@nomangle\npublic void kmain()\n{\n    while ticks::count(3) != 0 {}\n}\n")
    write(os.path.join(app, "src", "ticks.dc"), "module ticks;\n\nvolatile i32 sink;\n\npublic i32 count(i32 n)\n{\n    i32 i = 0;\n    while i < n {\n        sink = i;\n        i = i + 1;\n    }\n    return sink - 2;\n}\n")
    script = os.path.join(app, "link", "kernel.ld")
    def lds(base):
        return "ENTRY(kmain)\nSECTIONS\n{\n    . = %s;\n    .text : { *(.text .text.*) }\n    .rodata : { *(.rodata .rodata.*) }\n    .data : { *(.data .data.*) }\n    .bss : { *(.bss .bss.*) *(COMMON) }\n}\n" % base
    write(script, lds("0xffffffff80000000"))
    r = dpm(app, "build", "-v")
    out = os.path.join(app, ".dpm", "out", "debug", "x86_64-elf", "kern")
    check(r.returncode == 0 and os.path.isfile(out), "a kernel product with a linker script and kernel settings builds", detail(r))
    check("-T link/kernel.ld" in r.stderr, "dcc gets -T with the script", r.stderr)
    for flag in ("-mcmodel=kernel", "-fno-red-zone", "-fno-simd", "-fno-x87", "-fno-stack-protector", "-fpartial-eval", "-flibdcext=freestanding"):
        check(flag in r.stderr, "dcc gets " + flag, r.stderr)
    first = elf_entry(out)
    check(first >= 0xffffffff80000000 and first < 0xffffffff80100000, "the program is linked at the script's address", hex(first))
    r = dpm(app, "build")
    check(r.returncode == 0 and "Fresh kern" in r.stdout, "an unchanged kernel is fresh", detail(r))
    write(script, lds("0xffffffff90000000"))
    r = dpm(app, "build")
    check(r.returncode == 0 and "Building kern" in r.stdout, "editing the linker script rebuilds", detail(r))
    second = elf_entry(out)
    check(second >= 0xffffffff90000000 and second < 0xffffffff90100000, "and the new script is used", hex(second))
    r = dpm(app, "build")
    check("Fresh kern" in r.stdout, "the rebuilt kernel is fresh again", detail(r))
    os.remove(script)
    r = dpm(app, "build")
    check(r.returncode == 3 and "linker script 'link/kernel.ld' does not exist" in r.stderr, "a missing linker script is a manifest error", detail(r))
    write(script, lds("0xffffffff80000000"))
    text = read(os.path.join(app, "project.dcon"))
    write(os.path.join(app, "project.dcon"), text.replace("partial_eval = Tri::On,", "partial_eval = Tri::On,\n                extra_flags = { \"-T\", \"other.ld\" },"))
    r = dpm(app, "build")
    check(r.returncode == 3 and "set by dpm from linker_script" in r.stderr, "-T in extra_flags is refused", detail(r))


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


def make_asset_tarball(name, files):
    import tarfile
    import io
    archive = os.path.join(ROOT, "repos", name + ".tar.gz")
    os.makedirs(os.path.dirname(archive), exist_ok=True)
    with tarfile.open(archive, "w:gz") as tar:
        top = tarfile.TarInfo(name)
        top.type = tarfile.DIRTYPE
        top.mode = 0o755
        tar.addfile(top)
        for rel, content in files.items():
            info = tarfile.TarInfo(name + "/" + rel)
            data = content.encode()
            info.size = len(data)
            info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    return archive


def set_assets(app, assets):
    path = os.path.join(app, "project.dcon")
    text = read(path)
    text = re.sub(r",?\s*assets = \{.*?\n\s*\},?\n", "\n", text, flags=re.S)
    body = "".join("        { name = \"%s\", location = \"%s\"%s },\n" % (n, loc, ', version = "%s"' % v if v else "") for n, loc, v in assets)
    text = text.rstrip()
    assert text.endswith("}")
    text = text[:-1].rstrip().rstrip(",") + ",\n    assets = {\n" + body + "    },\n}\n"
    write(path, text)


def scenario_assets():
    limine = make_asset_tarball("limine-binary", {"limine.c": "int main(void) { return 0; }\n", "limine-bios.sys": "bios"})
    fw = os.path.join(ROOT, "repos", "fw")
    os.makedirs(fw)
    git(fw, "init", "-q", "--initial-branch=main")
    write(os.path.join(fw, "code.fd"), "one")
    git(fw, "add", "-A")
    git(fw, "commit", "-q", "-m", "one")
    git(fw, "tag", "v1.0.0")
    first_commit = git(fw, "rev-parse", "HEAD")
    app = new_app("assetapp")
    write(os.path.join(app, "vendor", "data", "x.txt"), "local")
    before = dpm(app, "lock")
    check(before.returncode == 0 and "assets = {" not in read(os.path.join(app, "project.lock.dcon")), "a lock without assets has no assets field", detail(before))
    set_assets(app, [("limine", "file://" + limine, "v12.9.6"), ("fw", "git+file://" + fw, ""), ("local", "path:vendor/data", "")])
    r = dpm(app, "build", "--locked")
    check(r.returncode == 4 and "out of date" in r.stderr, "declaring an asset stales the lock for --locked", detail(r))
    r = dpm(app, "lock")
    check(r.returncode == 0 and "(0 packages, 3 assets)" in r.stdout, "lock resolves assets", detail(r))
    check(r.stderr.count("new asset") == 2 and "new asset fw: commit " + first_commit in r.stderr, "first use of an asset is reported with its commit and hash", detail(r))
    lock = read(os.path.join(app, "project.lock.dcon"))
    check(lock.index('name = "fw"') < lock.index('name = "limine"') < lock.index('name = "local"'), "lock assets are sorted by name", lock)
    check('commit = "%s"' % first_commit in lock and 'source = "path:vendor/data"' in lock and 'version = "v12.9.6"' in lock, "lock records commit, source and version", lock)
    check(dpm(app, "lock", "--check").returncode == 0, "lock --check passes with assets")
    r = dpm(app, "build", "--locked")
    check(r.returncode == 0, "build --locked with locked assets", detail(r))
    r = dpm(app, "tree")
    check(r.returncode == 0 and "assets:" in r.stdout and "fw git+file://" in r.stdout and first_commit in r.stdout, "tree lists assets with their pins", detail(r))

    write(os.path.join(fw, "code.fd"), "two")
    git(fw, "add", "-A")
    git(fw, "commit", "-q", "-m", "two")
    git(fw, "tag", "v1.1.0")
    r = dpm(app, "lock")
    check(r.returncode == 0 and "up to date" in r.stdout and "new asset" not in r.stderr, "a new upstream release does not move a locked asset", detail(r))

    home = os.path.join(ROOT, "asset-home")
    r = dpm(app, "fetch", env={"DPM_HOME": home})
    check(r.returncode == 0 and "2 fetched" in r.stdout, "fetch downloads locked assets into an empty cache", detail(r))
    r = dpm(app, "fetch", "--offline", env={"DPM_HOME": home})
    check(r.returncode == 0 and "2 already in the cache" in r.stdout, "offline fetch uses cached assets", detail(r))
    r = dpm(app, "build", "--offline", env={"DPM_HOME": home})
    check(r.returncode == 0, "offline build after fetching assets", detail(r))
    r = dpm(app, "fetch", "--offline", env={"DPM_HOME": os.path.join(ROOT, "asset-cold")})
    check(r.returncode == 5 and "asset" in r.stderr, "offline fetch without cached assets exits 5", detail(r))

    good = read(os.path.join(app, "project.lock.dcon"))
    limine_hash = re.search(r'name = "limine",\n.*?hash = "(sha256:[0-9a-f]+)"', good, re.S).group(1)
    write(os.path.join(app, "project.lock.dcon"), good.replace(limine_hash, "sha256:" + "1" * 64))
    r = dpm(app, "fetch", env={"DPM_HOME": os.path.join(ROOT, "asset-tamper")})
    check(r.returncode == 4 and "asset limine" in r.stderr and "mismatch" in r.stderr, "a lock hash that does not match the asset is fatal", detail(r))
    r = dpm(app, "build", env={"DPM_HOME": os.path.join(ROOT, "asset-tamper2")})
    check(r.returncode == 4 and "mismatch" in r.stderr, "build refuses a mismatching asset", detail(r))
    write(os.path.join(app, "project.lock.dcon"), good)
    make_asset_tarball("limine-binary", {"limine.c": "int main(void) { return 1; }\n", "limine-bios.sys": "bios"})
    r = dpm(app, "fetch", env={"DPM_HOME": os.path.join(ROOT, "asset-replaced")})
    check(r.returncode == 4 and "asset limine" in r.stderr and "mismatch" in r.stderr, "an asset replaced upstream is detected against the lock", detail(r))
    check(read(os.path.join(app, "project.lock.dcon")) == good, "a failed fetch leaves the lock alone")

    set_assets(app, [("limine", "file://" + limine, "v12.9.6"), ("fw", "git+file://" + fw, "1.1.0"), ("local", "path:vendor/data", "")])
    r = dpm(app, "build", "--locked")
    check(r.returncode == 4, "changing an asset version stales the lock", detail(r))
    r = dpm(app, "lock")
    check(r.returncode == 0 and "new asset fw" in r.stderr and "new asset limine" not in r.stderr and git(fw, "rev-parse", "v1.1.0") in read(os.path.join(app, "project.lock.dcon")),
          "a changed version resolves that asset again and keeps the other pins", detail(r))
    check(limine_hash in read(os.path.join(app, "project.lock.dcon")), "the pinned tarball stays pinned to its cached hash")
    set_assets(app, [("fw", "git+file://" + fw, "1.1.0"), ("local", "path:vendor/data", "")])
    r = dpm(app, "lock")
    check(r.returncode == 0 and 'name = "limine"' not in read(os.path.join(app, "project.lock.dcon")), "removing an asset removes it from the lock", detail(r))
    set_assets(app, [("gone", "path:no/such", "")])
    r = dpm(app, "lock")
    check(r.returncode == 4 and "asset gone" in r.stderr and "does not exist" in r.stderr, "a missing path asset is an error", detail(r))
    set_assets(app, [("x", "json5", "")])
    r = dpm(app, "lock")
    check(r.returncode == 3 and "registry alias" in r.stderr, "an alias is not an asset location", detail(r))

    parent = os.path.join(ROOT, "assetdeps")
    os.makedirs(parent)
    check(dpm(parent, "init", "withassets", "--lib").returncode == 0, "init a library with assets")
    lib = os.path.join(parent, "withassets")
    set_assets(lib, [("missing", "path:does/not/exist", "")])
    consumer = os.path.join(parent, "consumer")
    check(dpm(parent, "init", "consumer").returncode == 0, "init an asset consumer")
    r = dpm(consumer, "add", "path:../withassets")
    check(r.returncode == 0 and "assets = {" not in read(os.path.join(consumer, "project.lock.dcon")), "assets of dependencies are ignored", detail(r))


def set_tasks(app, tasks_text, extra=""):
    path = os.path.join(app, "project.dcon")
    text = read(path)
    text = re.sub(r"\n    tasks = \{.*\n    \},\n", "\n", text, flags=re.S)
    text = text.rstrip()
    assert text.endswith("}")
    text = text[:-1].rstrip().rstrip(",") + ",\n" + extra + "    tasks = {\n" + tasks_text + "\n    },\n}\n"
    write(path, text)


def scenario_tasks():
    app = new_app("taskapp", "return 9;")
    log = os.path.join(app, "log.txt")
    r = dpm(app, "task")
    check(r.returncode == 0 and "declares no tasks" in r.stdout, "dpm task lists nothing without tasks", detail(r))
    write(os.path.join(app, "data", "in.txt"), "one\n")
    write(os.path.join(app, "vendor", "fw", "code.fd"), "firmware\n")
    set_tasks(app, r"""
        { name = "first", description = "write the first line", command = { "sh", "-c", "echo first >> log.txt" } },
        { name = "second", command = { "sh", "-c", "echo second $$1 >> log.txt", "sh" }, needs = { "first" } },
        { name = "both", command = { "sh", "-c", "echo both \"$$@\" >> log.txt", "sh" }, needs = { "second", "first" } },
        { name = "copy", description = "copy the input", command = { "cp", "data/in.txt", "${task_dir}/out.txt" }, inputs = { "data/*.txt" }, outputs = { "${task_dir}/out.txt" }, env = { { name = "MODE", value = "a" } } },
        { name = "env", command = { "sh", "-c", "echo $$DPM_ROOT $$DPM_PROFILE $$DPM_TASK $$DPM_OUT_TASKAPP $$GREETING > env.txt" }, env = { { name = "GREETING", value = "hi ${profile}" } } },
        { name = "fail", command = { "sh", "-c", "exit 3" } },
        { name = "killed", command = { "sh", "-c", "kill -9 $$$$" } },
        { name = "slow", command = { "sleep", "30" }, timeout = 1 },
        { name = "tool", command = { "true" }, tools = { "no-such-tool-for-dpm" } },
        { name = "literal", command = { "printf", "%s|%s\n", "a; touch pwned", "$$(touch pwned2)" } },
        { name = "built", command = { "sh", "-c", "\"$$1\"; echo $$? > built.txt", "sh", "${out:taskapp}" }, build = { "taskapp" } },
        { name = "asset", command = { "cp", "${asset:fw}/code.fd", "fw.txt" } },
        { name = "missing-input", command = { "true" }, inputs = { "nothing.txt" }, outputs = { "x" } },
        { name = "cd", command = { "sh", "-c", "pwd > where.txt" }, cwd = "data" },""", extra='    assets = { { name = "fw", location = "path:vendor/fw" } },\n')
    r = dpm(app, "task")
    check(r.returncode == 0 and "first" in r.stdout and "write the first line" in r.stdout and "no-such-tool-for-dpm" in r.stdout, "dpm task lists the tasks", detail(r))
    r = dpm(app, "task", "both", "--", "x", "y z")
    check(r.returncode == 0, "a task with needs runs", detail(r))
    check(read(log) == "first\nsecond\nboth x y z\n", "needs run first, each once, and arguments go to the named task only", read(log) if os.path.exists(log) else "")
    check(not os.path.exists(os.path.join(app, "project.lock.dcon")), "a task without products or assets does not write the lock")
    check(not os.path.exists(os.path.join(app, ".dpm", "out")), "and builds nothing")

    r = dpm(app, "task", "copy")
    check(r.returncode == 0 and "Running task copy" in r.stdout and read(os.path.join(app, ".dpm", "tasks", "copy", "out.txt")) == "one\n", "a task writes into its task directory", detail(r))
    r = dpm(app, "task", "copy")
    check(r.returncode == 0 and "Fresh task copy" in r.stdout, "a task with unchanged inputs and outputs is skipped", detail(r))
    write(os.path.join(app, "data", "in.txt"), "two\n")
    r = dpm(app, "task", "copy")
    check("Running task copy" in r.stdout and read(os.path.join(app, ".dpm", "tasks", "copy", "out.txt")) == "two\n", "a changed input reruns the task", detail(r))
    write(os.path.join(app, "data", "new.txt"), "new\n")
    r = dpm(app, "task", "copy")
    check("Running task copy" in r.stdout, "a new file matching an input glob reruns the task", detail(r))
    check("Fresh task copy" in dpm(app, "task", "copy").stdout, "and then it is fresh again")
    write(os.path.join(app, ".dpm", "tasks", "copy", "out.txt"), "tampered\n")
    r = dpm(app, "task", "copy")
    check("Running task copy" in r.stdout and read(os.path.join(app, ".dpm", "tasks", "copy", "out.txt")) == "two\n", "a modified output reruns the task", detail(r))
    os.remove(os.path.join(app, ".dpm", "tasks", "copy", "out.txt"))
    check("Running task copy" in dpm(app, "task", "copy").stdout, "a missing output reruns the task")
    check("Running task copy" in dpm(app, "task", "copy", "--", "-p").stdout, "different arguments rerun the task")
    check("Running task copy" in dpm(app, "task", "copy").stdout, "and going back reruns it again")
    text = read(os.path.join(app, "project.dcon"))
    write(os.path.join(app, "project.dcon"), text.replace('value = "a" }', 'value = "b" }'))
    check("Running task copy" in dpm(app, "task", "copy").stdout, "a changed environment variable reruns the task")
    text = read(os.path.join(app, "project.dcon"))
    write(os.path.join(app, "project.dcon"), text.replace('command = { "cp", "data/in.txt"', 'command = { "cp", "-f", "data/in.txt"'))
    check("Running task copy" in dpm(app, "task", "copy").stdout, "a changed command reruns the task")
    check("Fresh task copy" in dpm(app, "task", "copy").stdout, "and then it is fresh")
    check("Fresh task copy" in dpm(app, "task", "copy", "--profile", "debug").stdout, "an explicit default profile keeps it fresh")

    r = dpm(app, "task", "env")
    root = os.path.realpath(app)
    out = read(os.path.join(app, "env.txt")).split() if os.path.exists(os.path.join(app, "env.txt")) else []
    check(r.returncode == 0 and len(out) == 6 and os.path.realpath(out[0]) == root and out[1:3] == ["debug", "env"] and out[3].endswith("/.dpm/out/debug/host/taskapp") and out[4:] == ["hi", "debug"],
          "tasks see DPM_ROOT, DPM_PROFILE, DPM_TASK, DPM_OUT_<PRODUCT> and their own variables", detail(r) + str(out))
    r = dpm(app, "task", "fail")
    check(r.returncode == 1 and "task fail failed (exit 3)" in r.stderr, "a failing task exits 1 and names its exit code", detail(r))
    r = dpm(app, "task", "killed")
    check(r.returncode == 1 and "task killed failed (signal 9)" in r.stderr, "a task killed by a signal is reported", detail(r))
    r = dpm(app, "task", "slow")
    check(r.returncode == 1 and "timed out after 1 s" in r.stderr, "a task that runs too long is stopped", detail(r))
    r = dpm(app, "task", "tool")
    check(r.returncode == 1 and "needs 'no-such-tool-for-dpm', which is not on PATH" in r.stderr, "a missing tool is reported before the task starts", detail(r))
    r = dpm(app, "task", "literal")
    check(r.returncode == 0 and "a; touch pwned|$(touch pwned2)" in r.stdout and not os.path.exists(os.path.join(app, "pwned")) and not os.path.exists(os.path.join(app, "pwned2")),
          "arguments are passed without a shell", detail(r))
    r = dpm(app, "task", "missing-input")
    check(r.returncode == 1 and "does not exist" in r.stderr, "a missing input is an error", detail(r))
    r = dpm(app, "task", "cd")
    check(r.returncode == 0 and os.path.realpath(read(os.path.join(app, "data", "where.txt")).strip()) == os.path.realpath(os.path.join(app, "data")), "cwd is relative to the project root", detail(r))
    r = dpm(app, "task", "nope")
    check(r.returncode == 2 and "no task 'nope'" in r.stderr, "an unknown task is a usage error", detail(r))

    r = dpm(app, "task", "built", "--dry-run")
    check(r.returncode == 0 and "task built" in r.stdout and "build taskapp" in r.stdout and not os.path.exists(os.path.join(app, ".dpm", "out")) and not os.path.exists(os.path.join(app, "built.txt")),
          "--dry-run prints the commands and builds and runs nothing", detail(r))
    r = dpm(app, "task", "built")
    check(r.returncode == 0 and "Building taskapp" in r.stdout and read(os.path.join(app, "built.txt")).strip() == "9", "build products are built before the task and ${out:...} names them", detail(r))
    r = dpm(app, "build")
    check(r.returncode == 0 and "Fresh taskapp" in r.stdout, "dpm build after a task rebuilds nothing", detail(r))
    r = dpm(app, "task", "asset")
    check(r.returncode == 0 and read(os.path.join(app, "fw.txt")) == "firmware\n" and 'name = "fw"' in read(os.path.join(app, "project.lock.dcon")), "a task reads a locked asset", detail(r))
    r = dpm(app, "task", "asset", "--dry-run")
    check(r.returncode == 0 and "vendor/fw/code.fd" in r.stdout, "--dry-run shows locked asset paths", detail(r))

    write(log, "")
    for args in (["build"], ["lock"], ["fetch"], ["tree"], ["check"], ["test"], ["run"]):
        dpm(app, *args)
    check(read(log) == "", "no other command runs a task", read(log))
    check(os.path.isdir(os.path.join(app, ".dpm", "tasks")), "task directories exist before clean")
    r = dpm(app, "clean")
    check(r.returncode == 0 and not os.path.exists(os.path.join(app, ".dpm", "tasks")) and not os.path.exists(os.path.join(app, ".dpm", "out")), "clean removes task directories and stamps", detail(r))

    manifest = read(os.path.join(app, "project.dcon"))
    write(os.path.join(app, "project.dcon"), manifest.replace('needs = { "first" } }', 'needs = { "both" } }'))
    r = dpm(app, "task", "both")
    check(r.returncode == 3 and "task cycle" in r.stderr, "a task cycle is a manifest error", detail(r))
    write(os.path.join(app, "project.dcon"), manifest)
    stale = manifest.replace('{ name = "fw", location = "path:vendor/fw" }', '{ name = "fw", location = "path:vendor/fw" }, { name = "fw2", location = "path:vendor/fw" }')
    write(os.path.join(app, "project.dcon"), stale)
    r = dpm(app, "task", "built", "--locked")
    check(r.returncode == 4 and "out of date" in r.stderr, "--locked applies to tasks that need the lock", detail(r))
    r = dpm(app, "task", "first", "--locked")
    check(r.returncode == 0, "a task that needs neither products nor assets ignores the lock", detail(r))
    write(os.path.join(app, "project.dcon"), manifest)

    kern = new_app("runtask", "return 0;")
    write(os.path.join(kern, "src", "main.dc"), "module main;\n\n@nomangle\npublic void kmain()\n{\n    while true {}\n}\n")
    write(os.path.join(kern, "link", "kernel.ld"), "ENTRY(kmain)\nSECTIONS\n{\n    . = 0xffffffff80000000;\n    .text : { *(.text .text.*) }\n    .rodata : { *(.rodata .rodata.*) }\n    .data : { *(.data .data.*) }\n    .bss : { *(.bss .bss.*) *(COMMON) }\n}\n")
    write(os.path.join(kern, "project.dcon"), """dpm::v1::Project {
    name = "runtask",
    version = "0.1.0",
    products = {
        { name = "runtask", run = "boot", linker_script = "link/kernel.ld", settings = { target = Target::X86_64Elf, model = Model::Kernel, libdcext = Libdcext::Freestanding } },
        { name = "other", entry = "src/main.dc", run = "crash", linker_script = "link/kernel.ld", settings = { target = Target::X86_64Elf, model = Model::Kernel, libdcext = Libdcext::Freestanding } },
    },
    tasks = {
        { name = "boot", command = { "sh", "-c", "echo booting \\"$$0\\" \\"$$@\\" > booted.txt", "${out:runtask}" } },
        { name = "crash", command = { "sh", "-c", "exit 5" } },
    },
}
""")
    r = dpm(kern, "run", "runtask", "--", "-m", "4G")
    booted = os.path.join(kern, "booted.txt")
    words = read(booted).split() if os.path.exists(booted) else []
    check(r.returncode == 0 and "Building runtask" in r.stdout and len(words) == 4 and words[0] == "booting" and os.path.realpath(words[1]) == os.path.realpath(os.path.join(kern, ".dpm/out/debug/x86_64-elf/runtask")) and words[2:] == ["-m", "4G"],
          "dpm run of a product with run builds it and runs the task with the arguments", detail(r) + str(words))
    check(os.path.isfile(os.path.join(kern, ".dpm/out/debug/x86_64-elf/runtask")), "the run product was built")
    r = dpm(kern, "run", "runtask", "--runner", "qemu")
    check(r.returncode == 2 and "--runner does not apply" in r.stderr, "--runner is refused for a product that runs a task", detail(r))
    r = dpm(kern, "run", "other")
    check(r.returncode == 1 and "task crash failed (exit 5)" in r.stderr, "a failing run task fails dpm run", detail(r))
    text = read(os.path.join(kern, "project.dcon")).replace('run = "boot", ', "")
    write(os.path.join(kern, "project.dcon"), text)
    r = dpm(kern, "run", "runtask")
    check(r.returncode == 2 and "--runner" in r.stderr and "run field" in r.stderr, "a non-host product without run mentions the run field", detail(r))

    parent = os.path.join(ROOT, "taskdeps")
    os.makedirs(parent)
    check(dpm(parent, "init", "evil", "--lib").returncode == 0, "init a library with tasks")
    lib = os.path.join(parent, "evil")
    marker = os.path.join(ROOT, "evil-ran")
    set_tasks(lib, '        { name = "evil", command = { "touch", "%s" } },\n        { name = "build", command = { "touch", "%s" } },' % (marker, marker))
    consumer = os.path.join(parent, "user")
    check(dpm(parent, "init", "user").returncode == 0, "init a consumer of a library with tasks")
    write(os.path.join(consumer, "src", "main.dc"), "module main;\n\npublic i32 main()\n{\n    return 0;\n}\n")
    r = dpm(consumer, "add", "path:../evil")
    check(r.returncode == 0, "add the library with tasks", detail(r))
    for args in (["build"], ["run"], ["task"], ["task", "evil"], ["task", "build"], ["lock"], ["fetch"]):
        r = dpm(consumer, *args)
    check(not os.path.exists(marker), "tasks of dependencies never run")
    r = dpm(consumer, "task", "evil")
    check(r.returncode == 2, "a dependency's task is not a task of the project", detail(r))


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


def make_publishable(dirname, name, version, alias=None, answer=1, extra="", products=True):
    path = os.path.join(ROOT, "repos", dirname)
    os.makedirs(path)
    git(path, "init", "-q", "--initial-branch=main")
    alias_text = ', alias = "%s"' % (alias if alias is not None else name)
    prod = ', products = { { name = "tool", entry = "src/main.dc" } }' if products else ""
    write(os.path.join(path, "project.dcon"), 'dpm::v1::Project { name = "%s", version = "%s"%s%s%s }\n' % (name, version, alias_text, prod, extra))
    write(os.path.join(path, "src", name, "lib.dc"), "module %s::lib;\n\npublic i32 answer()\n{\n    return %d;\n}\n" % (name, answer))
    write(os.path.join(path, "src", "main.dc"), "module main;\n\npublic i32 main()\n{\n    return 0;\n}\n")
    write(os.path.join(path, ".gitignore"), ".dpm/\nproject.lock.dcon\ncompile_commands.json\n")
    git(path, "add", "-A")
    git(path, "commit", "-q", "-m", version)
    git(path, "tag", "v" + version)
    return path


def release(repo, name, version, answer):
    text = read(os.path.join(repo, "project.dcon"))
    write(os.path.join(repo, "project.dcon"), re.sub(r'version = "[^"]*"', 'version = "%s"' % version, text, count=1))
    write(os.path.join(repo, "src", name, "lib.dc"), "module %s::lib;\n\npublic i32 answer()\n{\n    return %d;\n}\n" % (name, answer))
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", version)


def scenario_publish():
    repo = make_publishable("pubpkg", "pubpkg", "1.0.0", answer=31)
    reg = Registry("pub")
    args = ["--registry-dir", reg.dir]
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "--location" in r.stderr, "publishing an unknown alias needs a location", detail(r))
    check(not os.path.exists(os.path.join(reg.dir, "aliases")), "a refused publish writes nothing")
    location = "git+file://" + repo
    r = dpm(repo, "publish", *args, "--location", location, "--dry-run")
    check(r.returncode == 0 and "would write" in r.stdout and not os.path.exists(os.path.join(reg.dir, "aliases")), "--dry-run writes nothing", detail(r))
    r = dpm(repo, "publish", *args, "--location", location)
    check(r.returncode == 0, "publish", detail(r))
    record = os.path.join(reg.dir, "aliases", "pu", "pubpkg.dcon")
    check(os.path.isfile(record), "publish writes the alias record", r.stdout)
    check("git checkout -b 'publish-pubpkg-1.0.0'" in r.stdout and "gh pr create" in r.stdout, "publish prints the git commands", r.stdout)
    check(not os.path.exists(os.path.join(reg.dir, ".git")), "publish never runs git in the registry")
    text = read(record)
    commit = git(repo, "rev-parse", "HEAD")
    check(commit in text and tree_hash(repo, "1.0.0", reg.url) in text, "the record holds the tag's commit and tree hash", text)
    r = dpm(ROOT, "registry", "check", reg.dir)
    check(r.returncode == 0, "registry check accepts the published registry", detail(r))
    r = dpm(ROOT, "registry", "check", reg.dir, "--verify-sources")
    check(r.returncode == 0, "registry check re-fetches and verifies the source", detail(r))

    app = new_app("pubconsumer")
    write(os.path.join(app, "src", "main.dc"), import_main("pubpkg"))
    r = dpm(app, "add", "pubpkg", env={"DPM_REGISTRY": reg.url})
    check(r.returncode == 0, "consume the published package", detail(r))
    r = dpm(app, "run", env={"DPM_REGISTRY": reg.url})
    check(r.returncode == 31, "the consumer runs the published code", detail(r))

    r = dpm(repo, "publish", *args)
    check(r.returncode == 0 and "already in the registry" in r.stdout and read(record) == text, "publishing the same version twice changes nothing", detail(r))

    write(os.path.join(repo, "src", "pubpkg", "lib.dc"), "module pubpkg::lib;\n\npublic i32 answer()\n{\n    return 99;\n}\n")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "not clean" in r.stderr, "a dirty tree is refused", detail(r))
    git(repo, "checkout", "-q", "--", ".")
    write(os.path.join(repo, "extra.txt"), "x")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "not clean" in r.stderr, "an untracked file is refused", detail(r))
    os.remove(os.path.join(repo, "extra.txt"))

    release(repo, "pubpkg", "1.1.0", 32)
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "tag v1.1.0 does not exist" in r.stderr, "a missing tag is refused", detail(r))
    git(repo, "tag", "v1.1.0", "HEAD~1")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "not to HEAD" in r.stderr, "a tag that is not at HEAD is refused", detail(r))
    git(repo, "tag", "-f", "v1.1.0", "HEAD")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 0, "publish a second version", detail(r))
    versions = re.findall(r'version = "([0-9.]+)"', read(record))
    check(versions == ["1.0.0", "1.1.0"], "versions stay ascending", read(record))
    check(dpm(ROOT, "registry", "check", reg.dir).returncode == 0, "registry check after the second version")

    release(repo, "pubpkg", "1.1.0", 77)
    git(repo, "tag", "-f", "v1.1.0", "HEAD")
    r = dpm(repo, "publish", *args)
    check(r.returncode == 1 and "immutable" in r.stderr, "a published version cannot change", detail(r))
    check(len(re.findall("version =", read(record))) == 2, "the immutable record is untouched")

    r = dpm(ROOT, "registry", "yank", "pubpkg", "1.0.0", *args)
    check(r.returncode == 0 and "yanked = true" in read(record), "yank", detail(r))
    after = read(record)
    check(after.replace("            yanked = true,\n", "") == text.replace("", "") or "yanked = true" in after, "yank adds one line")
    check(dpm(ROOT, "registry", "check", reg.dir).returncode == 0, "registry check after yank")
    r = dpm(ROOT, "registry", "yank", "pubpkg", "1.0.0", *args)
    check(r.returncode == 0 and "nothing to change" in r.stdout, "yanking twice is a no-op", detail(r))
    app2 = new_app("pubyank")
    write(os.path.join(app2, "src", "main.dc"), import_main("pubpkg"))
    r = dpm(app2, "add", "pubpkg@1.0.0", env={"DPM_REGISTRY": reg.url})
    check(r.returncode == 0 and "yanked" in r.stderr and 'version = "1.1.0"' in read(os.path.join(app2, "project.lock.dcon")),
          "a yanked version is replaced by the next release of its series", detail(r))
    r = dpm(ROOT, "registry", "yank", "pubpkg", "1.0.0", "--undo", *args)
    check(r.returncode == 0 and "yanked = false" in read(record), "unyank", detail(r))
    r = dpm(ROOT, "registry", "yank", "pubpkg", "9.9.9", *args)
    check(r.returncode == 1 and "not in the record" in r.stderr, "yanking an unknown version fails", detail(r))


def scenario_publish_refusals():
    reg = Registry("pubrefuse")
    args = ["--registry-dir", reg.dir]
    noalias = make_publishable("noalias", "noalias", "1.0.0", alias="", products=False)
    write(os.path.join(noalias, "project.dcon"), 'dpm::v1::Project { name = "noalias", version = "1.0.0" }\n')
    git(noalias, "add", "-A")
    git(noalias, "commit", "-q", "-m", "no alias")
    git(noalias, "tag", "-f", "v1.0.0")
    r = dpm(noalias, "publish", *args, "--location", "git+file://" + noalias)
    check(r.returncode == 1 and "needs an alias" in r.stderr, "a package without an alias is refused", detail(r))

    pathdep = make_publishable("pathdep", "pathdep", "1.0.0", extra=', dependencies = { { location = "path:../x" } }', products=False)
    r = dpm(pathdep, "publish", *args, "--location", "git+file://" + pathdep)
    check(r.returncode == 1 and "path:../x" in r.stderr, "a path dependency is refused", detail(r))

    future = make_publishable("future", "future", "1.0.0", extra=', requires_dcc = "99.0.0"', products=False)
    r = dpm(future, "publish", *args, "--location", "git+file://" + future)
    check(r.returncode == 1 and "requires_dcc" in r.stderr, "a requires_dcc newer than dcc is refused", detail(r))

    taken = make_publishable("taken", "taken", "1.0.0", products=False)
    other = make_publishable("taken2", "taken2", "1.0.0", alias="taken", products=False)
    check(dpm(taken, "publish", *args, "--location", "git+file://" + taken).returncode == 0, "first owner of an alias")
    r = dpm(other, "publish", *args, "--location", "git+file://" + other)
    check(r.returncode == 1 and "belongs to" in r.stderr or "already" in r.stderr, "a taken alias is refused", detail(r))
    r = dpm(taken, "publish", *args, "--location", "git+file://" + other)
    check(r.returncode == 1 and "belongs to" in r.stderr, "an alias cannot move to another location", detail(r))

    broken = make_publishable("brokenbuild", "brokenbuild", "1.0.0")
    write(os.path.join(broken, "src", "main.dc"), "module main;\n\npublic i32 main()\n{\n    return nope;\n}\n")
    git(broken, "add", "-A")
    git(broken, "commit", "-q", "-m", "broken")
    git(broken, "tag", "-f", "v1.0.0")
    r = dpm(broken, "publish", *args, "--location", "git+file://" + broken)
    check(r.returncode == 1, "a package that does not build is refused", detail(r))
    check(not os.path.exists(os.path.join(reg.dir, "aliases", "br")), "a failed build writes nothing")
    r = dpm(broken, "publish", *args, "--location", "git+file://" + broken, "--no-build")
    check(r.returncode == 0, "--no-build skips the build", detail(r))

    plain = os.path.join(ROOT, "notgit")
    os.makedirs(plain)
    write(os.path.join(plain, "project.dcon"), 'dpm::v1::Project { name = "ng", version = "1.0.0", alias = "ngpkg" }\n')
    r = dpm(plain, "publish", *args, "--location", "git+file://" + plain)
    check(r.returncode == 1 and "not in a git repository" in r.stderr, "a package outside git is refused", detail(r))
    r = dpm(plain, "publish", "--registry-dir", os.path.join(ROOT, "nowhere"))
    check(r.returncode == 1 and "does not exist" in r.stderr, "a missing registry directory is refused", detail(r))
    r = dpm(plain, "publish")
    check(r.returncode == 2, "publish without a registry directory is a usage error", detail(r))


def scenario_registry_commands():
    reg = Registry("regcmd")
    args = ["--registry-dir", reg.dir]
    r = dpm(ROOT, "registry", "alias", "github.com/u/json5", "json5", *args)
    check(r.returncode == 0 and os.path.isfile(os.path.join(reg.dir, "aliases", "js", "json5.dcon")), "registry alias", detail(r))
    check(os.path.isfile(os.path.join(reg.dir, "locations", "github.com", "u", "json5.dcon")), "registry alias writes the pointer")
    check("git add" in r.stdout and "dpm did not run git" in r.stdout, "registry alias prints commands", r.stdout)
    r = dpm(ROOT, "registry", "alias", "github.com/u/other", "json5", *args)
    check(r.returncode == 1 and "already taken" in r.stderr, "an alias cannot be taken twice", detail(r))
    r = dpm(ROOT, "registry", "alias", "github.com/u/json5", "second", *args)
    check(r.returncode == 1 and "already claimed" in r.stderr, "a location cannot have two aliases", detail(r))
    r = dpm(ROOT, "registry", "alias", "github.com/u/x", "Bad", *args)
    check(r.returncode == 1, "an invalid alias is refused", detail(r))
    r = dpm(ROOT, "registry", "check", reg.dir)
    check(r.returncode == 0 and "1 aliases" in r.stdout, "check after alias", detail(r))

    os.remove(os.path.join(reg.dir, "locations", "github.com", "u", "json5.dcon"))
    write(os.path.join(reg.dir, "aliases", "zz", "stray.dcon"), read(os.path.join(reg.dir, "aliases", "js", "json5.dcon")))
    os.symlink("/etc/passwd", os.path.join(reg.dir, "aliases", "js", "link.dcon"))
    r = dpm(ROOT, "registry", "check", reg.dir)
    check(r.returncode == 4, "check fails on a broken registry", detail(r))
    for fragment in ("missing location pointer", "does not belong at this path", "not a regular file"):
        check(fragment in r.stderr, "check reports: " + fragment, r.stderr)
    r = dpm(ROOT, "registry", "bogus")
    check(r.returncode == 2, "unknown registry command", detail(r))


def scenario_registry_verify_sources():
    good = make_publishable("vgood", "vgood", "1.0.0", products=False)
    wrong_claim = make_publishable("vclaim", "vclaim", "1.0.0", alias="elsewhere", products=False)
    reg = Registry("verify")
    reg.add_alias("vgood", good, [{"version": "1.0.0"}])
    reg.add_alias("vclaim", wrong_claim, [{"version": "1.0.0"}])
    r = dpm(ROOT, "registry", "check", reg.dir, "--verify-sources")
    check(r.returncode == 4 and "claims the alias 'elsewhere'" in r.stderr, "the alias claim is verified", detail(r))
    check("vgood" not in r.stderr, "a good entry produces no problem", r.stderr)
    entry = os.path.join(reg.dir, "aliases", "vg", "vgood.dcon")
    original = read(entry)
    write(entry, original.replace(tree_hash(good, "1.0.0", reg.url), "sha256:" + "e" * 64))
    r = dpm(ROOT, "registry", "check", reg.dir, "--verify-sources")
    check(r.returncode == 4 and "vgood.dcon 1.0.0" in r.stderr and "mismatch" in r.stderr, "a wrong hash is found by re-fetching", detail(r))
    write(entry, original.replace(git(good, "rev-parse", "HEAD"), "1" * 40))
    r = dpm(ROOT, "registry", "check", reg.dir, "--verify-sources")
    check(r.returncode == 4 and "vgood.dcon 1.0.0" in r.stderr, "a wrong commit is found by re-fetching", detail(r))
    write(entry, original.replace('version = "1.0.0"', 'version = "1.0.0", dependencies = { { location = "github.com/u/x", version = "1.0.0" } }'))
    r = dpm(ROOT, "registry", "check", reg.dir, "--verify-sources")
    check(r.returncode == 4 and "dependency summary" in r.stderr, "a wrong dependency summary is found", detail(r))

    os.remove(os.path.join(reg.dir, "aliases", "vc", "vclaim.dcon"))
    write(entry, original.replace(tree_hash(good, "1.0.0", reg.url), "sha256:" + "e" * 64))
    git(reg.dir, "init", "-q", "--initial-branch=main")
    git(reg.dir, "add", "-A")
    git(reg.dir, "commit", "-q", "-m", "base")
    release(good, "vgood", "1.1.0", 5)
    git(good, "tag", "v1.1.0")
    r = dpm(good, "publish", "--registry-dir", reg.dir, "--no-build")
    check(r.returncode == 0, "publish a new version into a git registry", detail(r))
    r = dpm(ROOT, "registry", "check", reg.dir, "--verify-sources")
    check(r.returncode == 4, "without --base every version is verified", detail(r))
    r = dpm(ROOT, "registry", "check", reg.dir, "--verify-sources", "--base", "HEAD")
    check(r.returncode == 0, "--base verifies only the new versions", detail(r))
    check(subprocess.run(["git", "status", "--porcelain"], cwd=reg.dir, env=ENV, capture_output=True, text=True).stdout.strip() != "", "publish left the git registry for the user to commit")


def scenario_publish_subdirectory():
    repo = os.path.join(ROOT, "repos", "mono")
    os.makedirs(repo)
    git(repo, "init", "-q", "--initial-branch=main")
    pkg = os.path.join(repo, "pkgs", "sublib")
    write(os.path.join(pkg, "project.dcon"), 'dpm::v1::Project { name = "sublib", version = "1.0.0", alias = "sublib" }\n')
    write(os.path.join(pkg, "src", "sublib", "lib.dc"), "module sublib::lib;\n\npublic i32 answer()\n{\n    return 41;\n}\n")
    write(os.path.join(repo, "README"), "monorepo\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "mono")
    reg = Registry("mono")
    args = ["--registry-dir", reg.dir]
    git(repo, "tag", "v1.0.0")
    r = dpm(pkg, "publish", *args, "--location", "git+file://" + repo)
    check(r.returncode == 1 and "subdirectory" in r.stderr, "a location without the subdirectory is refused", detail(r))
    r = dpm(pkg, "publish", *args, "--location", "git+file://%s#other/dir" % repo)
    check(r.returncode == 1 and "subdirectory" in r.stderr, "a location with another subdirectory is refused", detail(r))
    git(repo, "tag", "-d", "v1.0.0")
    r = dpm(pkg, "publish", *args, "--location", "git+file://%s#pkgs/sublib" % repo)
    check(r.returncode == 1 and "tag pkgs/sublib/v1.0.0 does not exist" in r.stderr, "a monorepo package is tagged with its directory", detail(r))
    git(repo, "tag", "pkgs/sublib/v1.0.0")
    r = dpm(pkg, "publish", *args, "--location", "git+file://%s#pkgs/sublib" % repo)
    check(r.returncode == 0, "publish a subdirectory package", detail(r))
    record = read(os.path.join(reg.dir, "aliases", "su", "sublib.dcon"))
    check('source = "git+file://%s#pkgs/sublib"' % repo in record, "the record names the subdirectory", record)
    check(dpm(ROOT, "registry", "check", reg.dir, "--verify-sources").returncode == 0, "the subdirectory record verifies")
    app = new_app("monoapp")
    write(os.path.join(app, "src", "main.dc"), import_main("sublib"))
    r = dpm(app, "add", "sublib", env={"DPM_REGISTRY": reg.url})
    check(r.returncode == 0, "consume a subdirectory package", detail(r))
    r = dpm(app, "run", env={"DPM_REGISTRY": reg.url})
    check(r.returncode == 41, "the subdirectory package runs", detail(r))


def scenario_registry_init():
    target = os.path.join(ROOT, "fresh-registry")
    r = dpm(ROOT, "registry", "init", target)
    check(r.returncode == 0 and os.path.isfile(os.path.join(target, "registry.dcon")), "registry init", detail(r))
    check('name = "fresh-registry"' in read(os.path.join(target, "registry.dcon")), "the name defaults to the directory name")
    r = dpm(ROOT, "registry", "check", target)
    check(r.returncode == 0 and "0 aliases" in r.stdout, "registry check accepts a fresh registry", detail(r))
    r = dpm(ROOT, "registry", "init", target)
    check(r.returncode == 1 and "not empty" in r.stderr, "init refuses an existing registry", detail(r))
    github = os.path.join(ROOT, "github-registry")
    os.makedirs(os.path.join(github, ".git"))
    write(os.path.join(github, "README.md"), "# dc-registry\n")
    write(os.path.join(github, "LICENSE"), "licence\n")
    r = dpm(ROOT, "registry", "init", github, "--name", "dc-registry")
    check(r.returncode == 0 and 'name = "dc-registry"' in read(os.path.join(github, "registry.dcon")), "init works in a fresh GitHub clone", detail(r))
    check(read(os.path.join(github, "README.md")) == "# dc-registry\n", "init keeps the README")
    shutil.rmtree(os.path.join(github, "aliases"))
    shutil.rmtree(os.path.join(github, "locations"))
    check(dpm(ROOT, "registry", "check", github).returncode == 0, "check passes without the empty directories")
    r = dpm(ROOT, "registry", "alias", "git+file://" + github, "freshalias", "--registry-dir", github)
    check(r.returncode == 0 and os.path.isfile(os.path.join(github, "aliases", "fr", "freshalias.dcon")), "the directories appear with the first alias", detail(r))
    r = dpm(ROOT, "registry", "init")
    check(r.returncode == 2, "init needs a directory", detail(r))
    busy = os.path.join(ROOT, "busy-registry")
    write(os.path.join(busy, "something.txt"), "x")
    r = dpm(ROOT, "registry", "init", busy)
    check(r.returncode == 1 and "something.txt" in r.stderr and not os.path.exists(os.path.join(busy, "registry.dcon")), "init refuses a busy directory", detail(r))


def scenario_help():
    empty = os.path.join(ROOT, "no-project")
    os.makedirs(empty)
    r = dpm(empty, "--help")
    check(r.returncode == 0 and r.stdout.startswith("usage: dpm <command> [options]") and "\n  build [<product>]" in r.stdout, "dpm --help", detail(r))
    for command in ("build", "registry check", "publish", "cache clean", "init"):
        words = command.split()
        a = dpm(empty, "help", *words)
        b = dpm(empty, *words, "--help")
        check(a.returncode == 0 and a.stdout == b.stdout and a.stdout.startswith("usage: dpm " + command), "help for " + command, detail(a))
    r = dpm(empty, "build", "--help", "--bogus")
    check(r.returncode == 0 and "usage: dpm build" in r.stdout, "--help wins over later errors", detail(r))
    r = dpm(empty)
    check(r.returncode == 2 and r.stdout == "" and r.stderr.startswith("usage: dpm"), "bare dpm prints the overview to stderr", detail(r))
    r = dpm(empty, "frobnicate")
    check(r.returncode == 2 and "unknown command 'frobnicate' (see 'dpm --help')" in r.stderr, "unknown command", detail(r))
    r = dpm(empty, "init", "--dev")
    check(r.returncode == 2 and "does not apply" in r.stderr and "dpm init --help" in r.stderr, "an option of another command", detail(r))
    r = dpm(empty, "cache")
    check(r.returncode == 2 and "usage: dpm cache <command>" in r.stderr, "a group without a command", detail(r))
    r = dpm(empty, "help", "nothing")
    check(r.returncode == 2 and "dpm --help" in r.stderr, "help for an unknown topic", detail(r))
    r = dpm(empty, "add")
    check(r.returncode == 2 and "usage: dpm add <location>[@version]" in r.stderr, "a missing argument", detail(r))


def scenario_stamps():
    app = new_app("stamped", "return 5;")
    write(os.path.join(app, "src", "stamped", "util.dc"), "module stamped::util;\n\npublic i32 base()\n{\n    return 10;\n}\n")
    main = os.path.join(app, "src", "main.dc")
    write(main, "module main;\nimport stamped::util;\n\npublic i32 main()\n{\n    return stamped::util::base() + 5;\n}\n")
    util = os.path.join(app, "src", "stamped", "util.dc")
    binary = os.path.join(app, ".dpm/out/debug/host/stamped")
    stamp = binary + ".stamp"

    def build(*args, env=None):
        return dpm(app, "build", *args, env=env)

    def fresh(r):
        return r.returncode == 0 and "Fresh stamped" in r.stdout and "Building" not in r.stdout

    def rebuilt(r):
        return r.returncode == 0 and "Building stamped" in r.stdout and "Fresh" not in r.stdout

    r = build()
    check(rebuilt(r) and os.path.isfile(stamp), "the first build compiles and writes a stamp", detail(r))
    first = os.stat(binary).st_mtime_ns
    r = build()
    check(fresh(r) and os.stat(binary).st_mtime_ns == first, "an unchanged build is skipped", detail(r))
    r = dpm(app, "run")
    check(r.returncode == 15 and "Fresh stamped" in r.stdout, "run uses the fresh build", detail(r))
    os.remove(os.path.join(app, "compile_commands.json"))
    r = build()
    check(fresh(r) and os.path.isfile(os.path.join(app, "compile_commands.json")), "a skipped product is still merged into compile_commands.json", detail(r))

    os.utime(util, None)
    check(fresh(build()), "touching an input without changing it does not rebuild")
    write(util, read(util).replace("return 10;", "return 11;"))
    r = build("-v")
    check(rebuilt(r) and "rebuilding stamped" in r.stderr and "util.dc" in r.stderr, "changing an input rebuilds and says why", detail(r))
    check(dpm(app, "run").returncode == 16, "the rebuilt program is the new one")
    check(fresh(build()), "and it is fresh afterwards")

    future = (os.stat(util).st_atime + 3600, os.stat(util).st_mtime + 3600)
    os.utime(util, future)
    r = build()
    check(fresh(r), "a moved mtime with unchanged content is not a change", detail(r))
    mtime = os.stat(util).st_mtime_ns
    write(util, read(util).replace("return 11;", "return 12;"))
    os.utime(util, ns=(mtime, mtime))
    r = build()
    check(rebuilt(r), "an input written around the build is compared by content even when size and mtime match", detail(r))
    check(dpm(app, "run").returncode == 17, "and the program reflects it")

    r = build("-O", "2")
    check(rebuilt(r), "other options rebuild", detail(r))
    check(fresh(build("-O", "2")), "the same options are fresh")
    check(rebuilt(build()), "returning to the earlier options rebuilds once")
    check(fresh(build()), "and is fresh again")
    r = build("--profile", "release")
    check(rebuilt(r), "another profile has its own output", detail(r))
    check(fresh(build("--profile", "release")) and fresh(build()), "and its own stamp")

    os.remove(binary)
    check(rebuilt(build()), "a deleted output is rebuilt")
    os.remove(stamp)
    check(rebuilt(build()) and os.path.isfile(stamp), "a missing stamp is rebuilt and rewritten")
    write(stamp, "garbage")
    check(rebuilt(build()), "a damaged stamp is ignored")
    text = read(stamp)
    write(stamp, text[: len(text) // 2])
    check(rebuilt(build()), "a truncated stamp is ignored")
    with open(binary, "ab") as f:
        f.write(b"x")
    check(rebuilt(build()), "a modified output is rebuilt")

    write(util, "module stamped::util;\n\npublic i32 base()\n{\n    return missing;\n}\n")
    r = build()
    check(r.returncode == 1 and not os.path.exists(stamp), "a failed build leaves no stamp", detail(r))
    write(util, "module stamped::util;\n\npublic i32 base()\n{\n    return 10;\n}\n")
    check(rebuilt(build()) and dpm(app, "run").returncode == 15, "the next build compiles again")

    check(dpm(app, "clean").returncode == 0 and not os.path.exists(stamp), "clean removes the stamps")
    check(rebuilt(build()), "and the next build compiles")

    compiler = os.path.join(ROOT, "dcc-copy")
    write(compiler, '#!/bin/sh\nexec "%s" "$@"\n' % DCC)
    os.chmod(compiler, 0o755)
    env = {"DCC": compiler}
    check(rebuilt(build(env=env)), "a different compiler path rebuilds")
    check(fresh(build(env=env)), "the same compiler is fresh")
    os.utime(compiler, (1_000_000_000, 1_000_000_000))
    r = build(env=env)
    check(rebuilt(r), "a replaced compiler binary rebuilds", detail(r))

    parent = os.path.join(ROOT, "stampdeps")
    os.makedirs(parent)
    lib = os.path.join(parent, "dep")
    check(dpm(parent, "init", "dep", "--lib").returncode == 0, "init a path dependency")
    consumer = os.path.join(parent, "user")
    check(dpm(parent, "init", "user").returncode == 0, "init the consumer")
    write(os.path.join(consumer, "src", "main.dc"), "module main;\nimport dep::lib;\n\npublic i32 main()\n{\n    return dep::lib::answer();\n}\n")
    check(dpm(consumer, "add", "path:../dep").returncode == 0, "add the path dependency")
    r = dpm(consumer, "build")
    check("Building user" in r.stdout, "build with a path dependency", detail(r))
    check("Fresh user" in dpm(consumer, "build").stdout, "and it is fresh")
    write(os.path.join(lib, "src", "dep", "lib.dc"), "module dep::lib;\n\npublic i32 answer()\n{\n    return 7;\n}\n")
    r = dpm(consumer, "build")
    check("Building user" in r.stdout and dpm(consumer, "run").returncode == 7, "a change in a path dependency rebuilds", detail(r))


try:
    scenario_basic()
    scenario_run_args()
    scenario_modules()
    scenario_kernel_build()
    scenario_new()
    scenario_new_fetch()
    scenario_new_registry()
    scenario_publish_template()
    scenario_path_dependency()
    scenario_git_dependency()
    scenario_offline_and_cache()
    scenario_assets()
    scenario_tasks()
    scenario_registry_file()
    scenario_registry_dependencies()
    scenario_registry_configuration()
    scenario_registry_http()
    scenario_publish()
    scenario_publish_refusals()
    scenario_registry_commands()
    scenario_registry_verify_sources()
    scenario_publish_subdirectory()
    scenario_registry_init()
    scenario_help()
    scenario_stamps()
    scenario_tests()
    scenario_formatting_and_determinism()
    scenario_misc()
finally:
    shutil.rmtree(ROOT, ignore_errors=True)

print("%d checks, %d failed" % (CHECKS, len(FAILURES)))
sys.exit(1 if FAILURES else 0)
