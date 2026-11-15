#!/usr/bin/env python3
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
checks = 0
failures = []


def check(condition, label, detail=""):
    global checks
    checks += 1
    if not condition:
        failures.append(label)
        print(f"    FAIL  {label}", file=sys.stderr)
        if detail:
            print("          " + str(detail).replace("\n", "\n          "), file=sys.stderr)


def uri(path):
    return "file://" + quote(str(Path(path).resolve()))


def path_of(value):
    return Path(unquote(urlparse(value).path))


class Lsp:
    def __init__(self, folders, log):
        self.process = subprocess.Popen([SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(log, "w"))
        self.inbox = queue.Queue()
        self.diagnostics = {}
        self.messages = []
        self.next_id = 1
        threading.Thread(target=self.read, daemon=True).start()
        self.request("initialize", {"processId": None, "rootUri": uri(folders[0]), "capabilities": {},
                                    "workspaceFolders": [{"uri": uri(f), "name": f.name} for f in folders]})
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

    def open(self, path, text=None):
        self.notify("textDocument/didOpen", {"textDocument": {"uri": uri(path), "languageId": "dc", "version": 1,
                                                              "text": Path(path).read_text() if text is None else text}})
        self.sync()

    def change(self, path, version, text):
        self.notify("textDocument/didChange", {"textDocument": {"uri": uri(path), "version": version}, "contentChanges": [{"text": text}]})
        self.sync()

    def close_document(self, path):
        self.notify("textDocument/didClose", {"textDocument": {"uri": uri(path)}})
        self.sync()

    def at(self, path, needle, offset=0, occurrence=0):
        lines = Path(path).read_text().splitlines()
        seen = 0
        for number, line in enumerate(lines):
            start = 0
            while (column := line.find(needle, start)) >= 0:
                if seen == occurrence:
                    return {"textDocument": {"uri": uri(path)}, "position": {"line": number, "character": column + offset}}
                seen += 1
                start = column + 1
        raise AssertionError(f"{needle!r} not found in {path}")

    def diags(self, path):
        return [d["message"] for d in self.diagnostics.get(uri(path), [])]

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


ENV = None


def run(cwd, *args):
    return subprocess.run([DPM, *args], cwd=cwd, env=ENV, capture_output=True, text=True, timeout=600)


def git(cwd, *args):
    subprocess.run(["git", *args], cwd=cwd, env=ENV, check=True, capture_output=True, timeout=60)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def commit(repo, version, files, manifest):
    write(repo / "project.dcon", manifest % version)
    for rel, text in files.items():
        write(repo / rel, text)
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", version)
    git(repo, "tag", "v" + version)


RENDERER_V1 = """module gfx::renderer;
public import gfx::types;
import gfx::video;
import mathx::vec;

public i32 draw(types::Color c) {
    mathx::vec::Vec2 v = mathx::vec::Vec2 { x = 1, y = 2 };
    return video::width() + mathx::vec::dot(v, v) + c.r;
}
"""

RENDERER_V2 = RENDERER_V1.replace("+ c.r;", "+ c.r + video::height();")

VIDEO_V1 = "module gfx::video;\npublic import gfx::types;\n\npublic i32 width() {\n    return 640;\n}\n"
VIDEO_V2 = VIDEO_V1 + "\npublic i32 height() {\n    return 480;\n}\n"


def app_main(name):
    return f"""module main;
import gfx::renderer;
import gfx::types;

public i32 main() {{
    gfx::types::Color c = gfx::types::Color {{ r = 1, g = 2, b = 3 }};
    return gfx::renderer::draw(c) - 646;
}}
"""


def make_app(root, name, gfx, version):
    app = root / "apps" / name
    write(app / "project.dcon", f"""dpm::v1::Project {{
    name = "{name}",
    version = "0.1.0",
    products = {{
        {{ name = "{name}", entry = "src/main.dc" }},
    }},
    dependencies = {{
        {{ location = "git+file://{gfx}", version = "{version}" }},
    }},
}}
""")
    write(app / "src" / "main.dc", app_main(name))
    r = run(app, "build")
    check(r.returncode == 0 and (app / "compile_commands.json").is_file(), f"dpm builds {name}", r.stderr[-2000:])
    return app


def main():
    global ENV
    with tempfile.TemporaryDirectory(prefix="dccd dpm deps ") as directory:
        root = Path(directory)
        home = root / "dpm cache 工具"
        ENV = dict(os.environ, DPM_HOME=str(home), DCC=DCC, DPM_ALLOW_FILE_URLS="1", DPM_REGISTRY="", HOME=str(root),
                   GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null", GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
        try:
            mathx = root / "repos" / "mathx"
            mathx.mkdir(parents=True)
            git(mathx, "init", "-q", "--initial-branch=main")
            commit(mathx, "0.1.0", {"src/mathx/vec.dc": "module mathx::vec;\n\npublic struct Vec2 {\n    i32 x;\n    i32 y;\n}\n\npublic i32 dot(Vec2 a, Vec2 b) {\n    return a.x * b.x + a.y * b.y;\n}\n"},
                   'dpm::v1::Project { name = "mathx", version = "%s" }\n')
            gfx = root / "repos" / "gfx"
            gfx.mkdir(parents=True)
            git(gfx, "init", "-q", "--initial-branch=main")
            manifest = 'dpm::v1::Project { name = "gfx", version = "%s", dependencies = { { location = "git+file://' + str(mathx) + '", version = "0.1.0" } } }\n'
            common = {"src/gfx/types.dc": "module gfx::types;\n\npublic struct Color {\n    i32 r;\n    i32 g;\n    i32 b;\n}\n",
                      "src/gfx/extra.dc": "module gfx::extra;\nimport gfx::types;\n\npublic i32 green(types::Color c) {\n    return c.g;\n}\n"}
            commit(gfx, "0.1.0", {**common, "src/gfx/video.dc": VIDEO_V1, "src/gfx/renderer.dc": RENDERER_V1}, manifest)
            commit(gfx, "0.2.0", {**common, "src/gfx/video.dc": VIDEO_V2, "src/gfx/renderer.dc": RENDERER_V2}, manifest)

            app1 = make_app(root, "little_witch", gfx, "0.1.0")
            app2 = make_app(root, "other_witch", gfx, "0.2.0")
            if failures:
                return 1
            main1 = app1 / "src" / "main.dc"
            main2 = app2 / "src" / "main.dc"
            lsp = Lsp([app1, app2], root / "dccd.log")
            try:
                scenario(lsp, home, app1, app2, main1, main2)
            finally:
                lsp.shutdown()
            if failures:
                print("dccd log: " + str(root / "dccd.log"), file=sys.stderr)
                print((root / "dccd.log").read_text()[-6000:], file=sys.stderr)
        finally:
            for dirpath, dirnames, filenames in os.walk(root):
                for name in dirnames + filenames:
                    p = os.path.join(dirpath, name)
                    if not os.path.islink(p):
                        os.chmod(p, os.stat(p).st_mode | stat.S_IWUSR)
    if failures:
        print(f"  FAIL     lsp_dpm_dependencies: {len(failures)} of {checks} checks failed", file=sys.stderr)
        return 1
    print(f"  PASS     lsp_dpm_dependencies ({checks} checks)")
    return 0


def unresolved(messages):
    return [m for m in messages if "could not resolve module" in m or "unknown type" in m or "unknown path" in m]


def scenario(lsp, home, app1, app2, main1, main2):
    lsp.open(main1)
    check(lsp.diags(main1) == [], "the application has no diagnostics", lsp.diags(main1))

    target = locations(lsp.request("textDocument/definition", lsp.at(main1, "draw(", 1)))
    check(len(target) == 1 and target[0].name == "renderer.dc" and home in target[0].parents and target[0].is_file(),
          "Go to Definition returns the cached package source", target)
    if not target:
        return
    renderer1 = target[0]
    package1 = renderer1.parent.parent.parent

    lsp.open(renderer1)
    check(unresolved(lsp.diags(renderer1)) == [] and lsp.diags(renderer1) == [], "opening the cached file reports no false import or type errors",
          lsp.diags(renderer1))
    check(lsp.diags(main1) == [], "the application stays clean after opening the dependency", lsp.diags(main1))

    hover = lsp.request("textDocument/hover", lsp.at(renderer1, "width()", 1))
    check(hover is not None and "width" in json.dumps(hover), "hover works inside the dependency", hover)
    video = locations(lsp.request("textDocument/definition", lsp.at(renderer1, "width()", 1)))
    check(video == [package1 / "src" / "gfx" / "video.dc"], "definition inside the dependency stays in the same cached package", video)
    vec = locations(lsp.request("textDocument/definition", lsp.at(renderer1, "dot(v", 1)))
    check(len(vec) == 1 and vec[0].name == "vec.dc" and home in vec[0].parents and vec[0].parent.parent.parent != package1,
          "definition reaches the transitive dependency in its own cached package", vec)
    refs = lsp.request("textDocument/references", {**lsp.at(renderer1, "draw(", 1), "context": {"includeDeclaration": True}})
    ref_files = {path_of(r["uri"]) for r in (refs or [])}
    check(renderer1 in ref_files and main1.resolve() in ref_files, "references include the application call site", ref_files)
    symbols = lsp.request("workspace/symbol", {"query": "draw"}) or []
    check(any(path_of(sym.get("location", {}).get("uri", "")) == renderer1 for sym in symbols), "workspace symbols include the cached package symbol",
          json.dumps(symbols)[:600])
    completion = lsp.request("textDocument/completion", lsp.at(renderer1, "width()", 0))
    check(completion is not None and "width" in json.dumps(completion), "completion works inside the dependency")

    if vec:
        lsp.open(vec[0])
        check(lsp.diags(vec[0]) == [], "the transitive dependency opens without diagnostics", lsp.diags(vec[0]))
        lsp.close_document(vec[0])

    broken = renderer1.read_text().replace("video::width()", "video::widthx()")
    lsp.change(renderer1, 2, broken)
    messages = lsp.diags(renderer1)
    check(len(messages) >= 1 and all("widthx" in m for m in unresolved(messages)) and not any("could not resolve module" in m for m in messages),
          "an unsaved edit is analysed in the project context", messages)
    lsp.change(renderer1, 3, renderer1.read_text())
    check(lsp.diags(renderer1) == [], "reverting the unsaved edit clears the diagnostic", lsp.diags(renderer1))

    extra = package1 / "src" / "gfx" / "extra.dc"
    lsp.open(extra)
    check(lsp.diags(extra) == [], "a package module the application does not import is analysed with the project options", lsp.diags(extra))
    lsp.close_document(extra)

    lsp.close_document(renderer1)
    check(lsp.diags(renderer1) == [], "closing the dependency clears its diagnostics")
    lsp.change(main1, 2, main1.read_text())
    check(lsp.diags(main1) == [], "application diagnostics stay correct after closing the dependency", lsp.diags(main1))

    lsp.open(main2)
    target2 = locations(lsp.request("textDocument/definition", lsp.at(main2, "draw(", 1)))
    check(len(target2) == 1 and target2[0].name == "renderer.dc" and target2[0] != renderer1, "the second project resolves its own package version", target2)
    if target2:
        renderer2 = target2[0]
        lsp.open(renderer2)
        check(lsp.diags(renderer2) == [], "the second project's cached file has no false diagnostics", lsp.diags(renderer2))
        height = locations(lsp.request("textDocument/definition", lsp.at(renderer2, "height()", 1)))
        check(height == [renderer2.parent / "video.dc"], "the 0.2.0 module identity is used for the second project", height)
        lsp.open(renderer1)
        check(lsp.diags(renderer1) == [], "the first project's copy still uses the first project's context", lsp.diags(renderer1))
        check(locations(lsp.request("textDocument/definition", lsp.at(renderer1, "width()", 1))) == [package1 / "src" / "gfx" / "video.dc"],
              "same-named modules do not leak between project contexts")
        lsp.close_document(renderer1)
        lsp.close_document(renderer2)

    project = app1 / "project.dcon"
    project.write_text(project.read_text().replace('version = "0.1.0" }', 'version = "0.2.0" }'))
    r = run(app1, "build")
    check(r.returncode == 0, "the first project moves to gfx 0.2.0", r.stderr[-1500:])
    lsp.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri(app1 / "compile_commands.json"), "type": 2},
                                                               {"uri": uri(app1 / "project.lock.dcon"), "type": 2}]})
    lsp.sync()
    moved = locations(lsp.request("textDocument/definition", lsp.at(main1, "draw(", 1)))
    check(len(moved) == 1 and moved[0] != renderer1 and moved[0].is_file(), "after the lock changes, definition follows the new package hash", moved)
    if moved:
        lsp.open(moved[0])
        check(lsp.diags(moved[0]) == [], "the new package version opens cleanly", lsp.diags(moved[0]))
        lsp.close_document(moved[0])

    before = len(lsp.messages)
    lsp.open(renderer1)
    messages = lsp.diags(renderer1)
    check(not any("gfx::" in m for m in unresolved(messages)), "a stale cached file still resolves its own package from its package root", messages)
    check(any("mathx::vec" in m for m in messages), "its missing dependency is reported instead of being guessed", messages)
    check(any("renderer.dc" in m for m in lsp.messages[before:]), "dccd explains that the file has no owning project", lsp.messages[before:])
    lsp.close_document(renderer1)


if __name__ == "__main__":
    sys.exit(main())
