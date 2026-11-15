#!/usr/bin/env python3
import hashlib
import json
import os
import queue
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import quote, unquote, urlparse

DPM = str(Path(sys.argv[1]).resolve())
SERVER = str(Path(sys.argv[2]).resolve())
DCC = str(Path(sys.argv[3]).resolve())
LEARNDC = str(Path(sys.argv[4]).resolve()) if len(sys.argv) > 4 else ""
checks = 0
failures = []
ENV = None


def check(condition, label, detail=""):
    global checks
    checks += 1
    if not condition:
        failures.append(label)
        print(f"    FAIL  {label}", file=sys.stderr)
        if detail:
            print("          " + str(detail).replace("\n", "\n          "), file=sys.stderr)


def uri(path, resolve=True):
    return "file://" + quote(str(Path(path).resolve() if resolve else Path(path)))


def path_of(value):
    return Path(unquote(urlparse(value).path))


class Lsp:
    def __init__(self, log):
        self.process = subprocess.Popen([SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(log, "w"), env=ENV, cwd="/")
        self.inbox = queue.Queue()
        self.diagnostics = {}
        self.messages = []
        self.next_id = 1
        self.versions = {}
        threading.Thread(target=self.read, daemon=True).start()
        self.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}, "workspaceFolders": None})
        self.notify("initialized", {})

    def read(self):
        stream = self.process.stdout
        while True:
            length = None
            while True:
                line = stream.readline()
                if not line:
                    return
                line = line.strip()
                if not line:
                    break
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            message = json.loads(stream.read(length))
            if message.get("method") == "textDocument/publishDiagnostics":
                self.diagnostics[message["params"]["uri"]] = message["params"]["diagnostics"]
            elif message.get("method") == "window/showMessage":
                self.messages.append(message["params"]["message"])
            self.inbox.put(message)

    def send(self, message):
        body = json.dumps(message).encode()
        self.process.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.process.stdin.flush()

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method, params, timeout=180):
        request_id = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                message = self.inbox.get(timeout=1)
            except queue.Empty:
                continue
            if message.get("id") == request_id and "method" not in message:
                return message.get("result")
        raise TimeoutError(method)

    def sync(self):
        self.request("workspace/symbol", {"query": "zz_sync"})

    def open(self, path, resolve=True):
        self.versions[str(path)] = 1
        self.notify("textDocument/didOpen", {"textDocument": {"uri": uri(path, resolve), "languageId": "dc", "version": 1, "text": Path(path).read_text()}})
        self.sync()

    def touch(self, path):
        version = self.versions.get(str(path), 1) + 1
        self.versions[str(path)] = version
        self.notify("textDocument/didChange", {"textDocument": {"uri": uri(path), "version": version}, "contentChanges": [{"text": Path(path).read_text()}]})
        self.sync()

    def close_document(self, path):
        self.notify("textDocument/didClose", {"textDocument": {"uri": uri(path)}})
        self.sync()

    def at(self, path, needle, offset=0, resolve=True):
        for number, line in enumerate(Path(path).read_text().splitlines()):
            column = line.find(needle)
            if column >= 0:
                return {"textDocument": {"uri": uri(path, resolve)}, "position": {"line": number, "character": column + offset}}
        raise AssertionError(f"{needle!r} not found in {path}")

    def diags(self, path, resolve=True):
        return [d["message"] for d in self.diagnostics.get(uri(path, resolve), [])]

    def shutdown(self):
        try:
            self.request("shutdown", None, timeout=30)
            self.notify("exit", None)
            self.process.wait(timeout=30)
        except Exception:
            self.process.kill()


def locations(result):
    if result is None:
        return []
    items = result if isinstance(result, list) else [result]
    return [path_of(item.get("uri") or item.get("targetUri")) for item in items]


def dpm(cwd, *args):
    return subprocess.run([DPM, *args], cwd=cwd, env=ENV, capture_output=True, text=True, timeout=600)


def git(cwd, *args):
    subprocess.run(["git", *args], cwd=cwd, env=ENV, check=True, capture_output=True, timeout=60)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def snapshot(root):
    state = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames:
            p = Path(dirpath) / name
            state[str(p.relative_to(root))] = (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
        for name in dirnames:
            state[str((Path(dirpath) / name).relative_to(root)) + "/"] = None
    return state


def commit(repo, version, files, manifest):
    write(repo / "project.dcon", manifest % version)
    for rel, text in files.items():
        write(repo / rel, text)
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", version)
    git(repo, "tag", "v" + version)


def project_manifest(name, products, dependency=None, extra=""):
    deps = f"    dependencies = {{\n        {{ location = \"git+file://{dependency[0]}\", version = \"{dependency[1]}\" }},\n    }},\n" if dependency else ""
    items = "".join(f"        {{ name = \"{n}\", entry = \"{e}\" }},\n" for n, e in products)
    return f"dpm::v1::Project {{\n    name = \"{name}\",\n    version = \"0.1.0\",\n    products = {{\n{items}    }},\n{deps}{extra}}}\n"


def unresolved(messages):
    return [m for m in messages if "could not resolve module" in m or "unknown type" in m or "unknown path" in m or "unknown name" in m]


def make_repos(root):
    mathx = root / "repos" / "mathx"
    mathx.mkdir(parents=True)
    git(mathx, "init", "-q", "--initial-branch=main")
    commit(mathx, "0.1.0", {"src/mathx/vec.dc": "module mathx::vec;\n\npublic struct Vec2 {\n    i32 x;\n    i32 y;\n}\n\npublic i32 dot(Vec2 a, Vec2 b) {\n    return a.x * b.x + a.y * b.y;\n}\n"},
           'dpm::v1::Project { name = "mathx", version = "%s" }\n')
    gfx = root / "repos" / "gfx"
    gfx.mkdir(parents=True)
    git(gfx, "init", "-q", "--initial-branch=main")
    manifest = 'dpm::v1::Project { name = "gfx", version = "%s", dependencies = { { location = "git+file://' + str(mathx) + '", version = "0.1.0" } } }\n'
    renderer = "module gfx::renderer;\nimport mathx::vec;\n\npublic i32 draw(i32 scale) {\n    mathx::vec::Vec2 v = mathx::vec::Vec2 { x = scale, y = 2 };\n    return mathx::vec::dot(v, v);\n}\n"
    commit(gfx, "0.1.0", {"src/gfx/renderer.dc": renderer}, manifest)
    commit(gfx, "0.2.0", {"src/gfx/renderer.dc": renderer + "\npublic i32 frames() {\n    return 60;\n}\n"}, manifest)
    return gfx


APP_MAIN = """module main;
import util;
import gfx::renderer;

public i32 main() {
    i32 n = util::twice(3);
    return gfx::renderer::draw(n) - 40;
}
"""

UTIL_A = "module util;\n\npublic i32 twice(i32 value) {\n    return value * 2;\n}\n"
UTIL_B = "module util;\n\npublic i32 twice(i32 value, i32 extra) {\n    return value * 2 + extra;\n}\n"
TOOL_MAIN = "module tool;\nimport toolkit;\n\npublic i32 main() {\n    return toolkit::answer();\n}\n"
TOOLKIT = "module toolkit;\n\npublic i32 answer() {\n    return 42;\n}\n"


def main():
    global ENV
    with tempfile.TemporaryDirectory(prefix="dccd auto ü ") as directory:
        root = Path(directory)
        home = root / "dpm cache"
        ENV = dict(os.environ, DPM_HOME=str(home), DCC=DCC, DPM_ALLOW_FILE_URLS="1", DPM_REGISTRY="", HOME=str(root), XDG_CONFIG_HOME=str(root / "config"),
                   GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null", GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
        for key in ("DCC_INCLUDE_PATH", "DCCD_LOG"):
            ENV.pop(key, None)
        try:
            gfx = make_repos(root)
            app = root / "work ü" / "app"
            write(app / "project.dcon", project_manifest("app", [("app", "src/main.dc"), ("tool", "src/tool.dc")], (gfx, "0.1.0")))
            write(app / "src" / "main.dc", APP_MAIN)
            write(app / "src" / "util.dc", UTIL_A)
            write(app / "src" / "tool.dc", TOOL_MAIN)
            write(app / "src" / "toolkit.dc", TOOLKIT)
            nested = app / "examples" / "nested"
            write(nested / "project.dcon", project_manifest("nested", [("nested", "src/main.dc")]))
            write(nested / "src" / "main.dc", "module main;\nimport util;\n\npublic i32 main() {\n    return util::twice(1, 2) - 4;\n}\n")
            write(nested / "src" / "util.dc", UTIL_B)
            other = root / "elsewhere" / "other"
            write(other / "project.dcon", project_manifest("other", [("other", "src/main.dc")]))
            write(other / "src" / "main.dc", "module main;\nimport util;\n\npublic i32 main() {\n    return util::twice(1, 2) - 4;\n}\n")
            write(other / "src" / "util.dc", UTIL_B)

            commands = dpm(app, "compile-commands")
            check(commands.returncode == 0 and "project.lock.dcon does not exist yet" in commands.stderr, "dpm compile-commands explains the missing lock",
                  commands.stderr)
            entries = json.loads(commands.stdout or "[]")
            check([e["product"] for e in entries] == ["app", "tool"] and all("-flibdcext" in e["arguments"] for e in entries),
                  "dpm compile-commands prints one command per product with the build flags", commands.stdout)
            check(not (app / "project.lock.dcon").exists() and not (app / ".dpm").exists() and not (app / "compile_commands.json").exists(),
                  "dpm compile-commands writes nothing")

            lsp = Lsp(root / "dccd.log")
            try:
                scenario(lsp, root, home, gfx, app, nested, other)
            finally:
                lsp.shutdown()
            if failures:
                print("dccd log: " + str(root / "dccd.log"), file=sys.stderr)
                print((root / "dccd.log").read_text()[-8000:], file=sys.stderr)
        finally:
            for dirpath, dirnames, filenames in os.walk(root):
                for name in dirnames + filenames:
                    p = os.path.join(dirpath, name)
                    if not os.path.islink(p):
                        os.chmod(p, os.stat(p).st_mode | stat.S_IWUSR)
    if failures:
        print(f"  FAIL     lsp_dpm_autoconfig: {len(failures)} of {checks} checks failed", file=sys.stderr)
        return 1
    print(f"  PASS     lsp_dpm_autoconfig ({checks} checks)")
    return 0


def scenario(lsp, root, home, gfx, app, nested, other):
    main = app / "src" / "main.dc"
    util = app / "src" / "util.dc"
    log = root / "dccd.log"

    lsp.open(main)
    messages = lsp.diags(main)
    check(unresolved([m for m in messages if "gfx" not in m]) == [], "local modules resolve before any build", messages)
    check(any("gfx::renderer" in m for m in messages), "the unlocked dependency is reported, not guessed", messages)
    check(any("project.lock.dcon does not exist yet" in m for m in lsp.messages), "dccd explains why the dependency is unknown", lsp.messages)
    check("configured from project.dcon" in log.read_text(), "dccd derived the configuration from project.dcon")

    r = dpm(app, "lock")
    check(r.returncode == 0, "dpm lock", r.stderr[-1500:])
    r = dpm(app, "fetch")
    check(r.returncode == 0, "dpm fetch", r.stderr[-1500:])
    check(not (app / "compile_commands.json").exists() and not (app / ".dpm" / "out").exists(), "the project has never been built")
    lsp.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri(app / "project.lock.dcon"), "type": 1}]})
    lsp.sync()
    before = snapshot(app)
    lsp.touch(main)
    check(lsp.diags(main) == [], "after the lock exists, the project analyses cleanly without a build", lsp.diags(main))

    hover = lsp.request("textDocument/hover", lsp.at(main, "twice(", 1))
    check(hover is not None and "twice" in json.dumps(hover), "hover works before the first build", hover)
    completion = lsp.request("textDocument/completion", lsp.at(main, "twice(", 0))
    check(completion is not None and "twice" in json.dumps(completion), "completion works before the first build")
    check(locations(lsp.request("textDocument/definition", lsp.at(main, "twice(", 1))) == [util.resolve()], "Go to Definition reaches the local module")
    target = locations(lsp.request("textDocument/definition", lsp.at(main, "draw(", 1)))
    check(len(target) == 1 and target[0].name == "renderer.dc" and home in target[0].parents, "Go to Definition reaches the cached package", target)
    if target:
        renderer = target[0]
        lsp.open(renderer)
        check(lsp.diags(renderer) == [], "the cached package file opens without false diagnostics", lsp.diags(renderer))
        vec = locations(lsp.request("textDocument/definition", lsp.at(renderer, "dot(v", 1)))
        check(len(vec) == 1 and vec[0].name == "vec.dc" and home in vec[0].parents, "the transitive dependency resolves", vec)
        hover = lsp.request("textDocument/hover", lsp.at(renderer, "Vec2 v", 1))
        check(hover is not None and "Vec2" in json.dumps(hover), "transitive types resolve inside the cached package", hover)
        lsp.close_document(renderer)

    lsp.open(util)
    check(lsp.diags(util) == [], "a non-entry module is analysed in its product's context", lsp.diags(util))
    toolkit = app / "src" / "toolkit.dc"
    lsp.open(toolkit)
    check(lsp.diags(toolkit) == [], "a module imported only by the second product analyses cleanly", lsp.diags(toolkit))
    text = log.read_text()
    check("is analyzed in the context of" in text and "tool.dc" in text, "the module is analysed with the product that imports it", text[-3000:])
    lsp.close_document(toolkit)

    nested_main = nested / "src" / "main.dc"
    lsp.open(nested_main)
    check(lsp.diags(nested_main) == [], "a nested project uses its own configuration and modules", lsp.diags(nested_main))
    twice = locations(lsp.request("textDocument/definition", lsp.at(nested_main, "twice(", 1)))
    check(twice == [(nested / "src" / "util.dc").resolve()], "the nested project resolves its own util module", twice)

    other_main = other / "src" / "main.dc"
    write(other / "compile_commands.json", json.dumps([{"directory": str(other), "file": str(other_main),
                                                        "arguments": ["dcc", str(other_main), "-o", "x", "-I" + str(root / "missing")]}]))
    old = time.time() - 120
    os.utime(other / "compile_commands.json", (old, old))
    lsp.open(other_main)
    check(lsp.diags(other_main) == [], "a stale compilation database yields to the manifest", lsp.diags(other_main))
    check("is older than project.dcon" in log.read_text(), "dccd reports the stale compilation database")
    other_util = locations(lsp.request("textDocument/definition", lsp.at(other_main, "twice(", 1)))
    check(other_util == [(other / "src" / "util.dc").resolve()], "an unrelated project resolves its own util module", other_util)

    lsp.touch(main)
    check(lsp.diags(main) == [], "switching back keeps the first project's context", lsp.diags(main))
    check(locations(lsp.request("textDocument/definition", lsp.at(main, "twice(", 1))) == [util.resolve()], "the first project's util is used again")
    hover = lsp.request("textDocument/hover", lsp.at(main, "twice(", 1))
    check(hover is not None and "extra" not in json.dumps(hover), "hover does not leak the other project's signature", hover)

    link = root / "linked app"
    os.symlink(app, link)
    linked = link / "src" / "main.dc"
    lsp.open(linked, resolve=False)
    check(lsp.diags(linked, resolve=False) == [], "a project opened through a symlinked path is configured", lsp.diags(linked, resolve=False))
    lsp.notify("textDocument/didClose", {"textDocument": {"uri": uri(linked, False)}})
    lsp.sync()

    after = snapshot(app)
    check(before == after, "the language server built, fetched and wrote nothing in the project",
          sorted(set(after) ^ set(before)) or [k for k in after if after[k] != before.get(k)])

    manifest = app / "project.dcon"
    manifest.write_text(manifest.read_text().replace('version = "0.1.0" }', 'version = "0.2.0" }'))
    r = dpm(app, "lock")
    check(r.returncode == 0, "dpm lock after the version change", r.stderr[-1500:])
    r = dpm(app, "fetch")
    check(r.returncode == 0, "dpm fetch after the version change", r.stderr[-1500:])
    main.write_text(APP_MAIN.replace("- 40;", "- 40 + gfx::renderer::frames() - 60;"))
    time.sleep(2.5)
    lsp.touch(main)
    check(lsp.diags(main) == [], "an unwatched manifest and lock change is picked up from the files themselves", lsp.diags(main))
    moved = locations(lsp.request("textDocument/definition", lsp.at(main, "frames()", 1)))
    check(len(moved) == 1 and moved[0].name == "renderer.dc" and (not target or moved[0] != target[0]), "definition follows the new package version", moved)

    scaffold = root / "fresh"
    r = dpm(root, "new", str(scaffold))
    check(r.returncode == 0, "dpm new scaffolds a project", r.stderr[-1500:])
    fresh_main = scaffold / "src" / "main.dc"
    if fresh_main.is_file():
        lsp.open(fresh_main)
        check(lsp.diags(fresh_main) == [], "a newly scaffolded project analyses cleanly without a build", lsp.diags(fresh_main))
        hover = lsp.request("textDocument/hover", lsp.at(fresh_main, "println", 1))
        check(hover is not None, "hover on the scaffolded program's println", hover)

    if LEARNDC:
        learn_home = root / "learndc home"
        r = subprocess.run([LEARNDC, "--home", str(learn_home), "start", "first-steps/hello-world"], env=dict(ENV, LEARNDC_DCC=DCC, LEARNDC_DPM=DPM),
                           capture_output=True, text=True, timeout=120)
        check(r.returncode == 0, "learndc creates an exercise workspace", r.stderr)
        exercise = learn_home / "workspaces" / "first-steps" / "hello-world" / "src" / "main.dc"
        if exercise.is_file():
            lsp.open(exercise)
            check(lsp.diags(exercise) == [], "a fresh learndc workspace analyses cleanly before the first check", lsp.diags(exercise))
            hover = lsp.request("textDocument/hover", lsp.at(exercise, "println", 1))
            check(hover is not None and "println" in json.dumps(hover), "hover works in the learndc workspace", hover)
            check(not (exercise.parent.parent / "compile_commands.json").exists(), "the learndc workspace stays unbuilt")


if __name__ == "__main__":
    sys.exit(main())
