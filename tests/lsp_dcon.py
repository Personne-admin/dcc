import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

SERVER = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "build/bin/dccd"
SCHEMA = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1] / "dpm/schema"
CHECKS = 0
FAILURES = []


def check(ok, label, detail=""):
    global CHECKS
    CHECKS += 1
    if not ok:
        FAILURES.append(label)
        print("FAIL  " + label)
        if detail:
            print(detail)


class Session:
    def __init__(self, root, schema=SCHEMA, env=None):
        self.root = root
        self.messages = []
        self.next_id = 100
        self.schema = schema
        self.env = env or {}
        self.send("initialize", {"rootUri": root.as_uri()}, 1)
        self.send("initialized", {})

    def send(self, method, params, request_id=None):
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if request_id is not None:
            message["id"] = request_id
        self.messages.append(message)
        return request_id

    def request(self, method, params):
        self.next_id += 1
        return self.send(method, params, self.next_id)

    def open(self, path, text, version=1, language="dcon"):
        self.send("textDocument/didOpen", {"textDocument": {"uri": path.as_uri(), "languageId": language, "version": version, "text": text}})

    def change(self, path, text, version):
        self.send("textDocument/didChange", {"textDocument": {"uri": path.as_uri(), "version": version}, "contentChanges": [{"text": text}]})

    def close(self, path):
        self.send("textDocument/didClose", {"textDocument": {"uri": path.as_uri()}})

    def at(self, method, path, line, character):
        return self.request(method, {"textDocument": {"uri": path.as_uri()}, "position": {"line": line, "character": character}})

    def run(self):
        self.send("shutdown", {}, 2)
        self.send("exit", {})
        payload = bytearray()
        for message in self.messages:
            body = json.dumps(message).encode()
            payload.extend(f"Content-Length: {len(body)}\r\n\r\n".encode())
            payload.extend(body)
        env = dict(os.environ)
        env["DPM_SCHEMA_ROOT"] = str(self.schema)
        env.update(self.env)
        env["HOME"] = str(self.root)
        env["DPM_HOME"] = str(self.root / "dpm-home")
        process = subprocess.run([str(SERVER)], input=bytes(payload), capture_output=True, timeout=60, env=env)
        check(process.returncode == 0, "dccd exits cleanly", process.stderr.decode()[-2000:])
        output = process.stdout
        self.published = []
        self.responses = {}
        while output:
            header, output = output.split(b"\r\n\r\n", 1)
            length = int(header[len(b"Content-Length: "):])
            message = json.loads(output[:length])
            output = output[length:]
            if message.get("method") == "textDocument/publishDiagnostics":
                self.published.append(message["params"])
            elif "id" in message:
                self.responses[message["id"]] = message
        return self

    def last(self, path):
        found = [p for p in self.published if p["uri"] == path.as_uri()]
        return found[-1] if found else None

    def result(self, request_id):
        message = self.responses.get(request_id)
        assert message is not None, request_id
        assert "error" not in message, message
        return message["result"]


def lsp_range(d):
    r = d["range"]
    return (r["start"]["line"], r["start"]["character"], r["end"]["line"], r["end"]["character"])


def scenario_diagnostics():
    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        manifest = root / "project.dcon"
        text = 'dpm::v1::Project {\n    name = "app",\n    version = "1.0.0",\n    bogus = 1,\n}\n'
        s = Session(root)
        s.open(manifest, text)
        s.change(manifest, text.replace("    bogus = 1,\n", ""), 2)
        s.close(manifest)
        s.run()
        first = [p for p in s.published if p["uri"] == manifest.as_uri()]
        check(len(first) == 3, "open, change and close each publish", str(first))
        d = first[0]["diagnostics"]
        check(len(d) >= 1 and d[0]["severity"] == 1 and d[0]["source"] == "dpm", "an unknown field is a dpm error", str(first[0]))
        check(d and lsp_range(d[0])[0] == 3 and "bogus" in d[0]["message"] + text.splitlines()[3][lsp_range(d[0])[1]:lsp_range(d[0])[3]],
              "the error range is on the offending field", str(d))
        check(first[0]["version"] == 1 and first[1]["version"] == 2, "versions are reported", str(first))
        check(first[1]["diagnostics"] == [], "fixing the file clears the diagnostics", str(first[1]))
        check(first[2]["diagnostics"] == [], "closing the file clears the diagnostics", str(first[2]))


def scenario_utf16_columns():
    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        manifest = root / "project.dcon"
        line = '    name = "café \U0001F600", bogus = 1,'
        text = 'dpm::v1::Project {\n' + line + '\n    version = "1.0.0",\n}\n'
        s = Session(root)
        s.open(manifest, text)
        s.run()
        d = s.last(manifest)["diagnostics"]
        check(len(d) >= 1, "diagnostic after multi-byte text", str(s.published))
        if d:
            units = line.encode("utf-16-le")
            begin, end = lsp_range(d[0])[1], lsp_range(d[0])[3]
            word = units[begin * 2:end * 2].decode("utf-16-le")
            check("bogus" in word, "columns count UTF-16 code units", repr(word) + str(d[0]))


def scenario_lock():
    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        manifest = root / "project.dcon"
        text = ('dpm::v1::Project {\n    name = "app",\n    version = "1.0.0",\n    dependencies = {\n'
                '        { location = "github.com/u/a", version = "1.2.0" },\n    },\n}\n')
        (root / "project.dcon").write_text(text)
        s0 = Session(root)
        s0.open(manifest, text)
        s0.run()
        check(s0.last(manifest)["diagnostics"] == [], "no lock means no lock diagnostics", str(s0.last(manifest)))
        lock = ('dpm::v1::Lock {\n    manifest = "sha256:' + "0" * 64 + '",\n    packages = {\n        {\n'
                '            location = "github.com/u/a",\n            version = "1.1.0",\n'
                '            source = "git+https://github.com/u/a.git",\n            commit = "' + "c" * 40 + '",\n'
                '            hash = "sha256:' + "a" * 64 + '",\n        },\n    },\n}\n')
        (root / "project.lock.dcon").write_text(lock)
        s = Session(root)
        s.open(manifest, text)
        s.open(root / "project.lock.dcon", lock)
        s.run()
        d = s.last(manifest)["diagnostics"]
        messages = [x["message"] for x in d]
        check(any("fingerprint differs" in m for m in messages), "a stale fingerprint is reported", str(messages))
        check(any("locked version 1.1.0 is lower than the required minimum 1.2.0" in m for m in messages), "a lower locked version is reported", str(messages))
        element = [x for x in d if "lower than" in x["message"]]
        check(element and element[0]["severity"] == 2 and lsp_range(element[0])[0] == 4, "the element is the range", str(element))
        check(any("not in the dpm cache" in m and x["severity"] == 3 for x, m in zip(d, messages)), "uncached packages are an information hint", str(d))
        lock_diag = s.last(root / "project.lock.dcon")["diagnostics"]
    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        manifest = root / "project.dcon"
        text = ('dpm::v1::Project {\n    name = "app",\n    version = "1.0.0",\n    dependencies = {\n'
                '        { location = "github.com/u/a", version = "1.2.0" },\n    },\n}\n')
        manifest.write_text(text)
        lock_path = root / "project.lock.dcon"
        s = Session(root)
        s.open(manifest, text)
        s.send("workspace/didChangeWatchedFiles", {"changes": [{"uri": lock_path.as_uri(), "type": 1}]})
        s.run()
        published = [p for p in s.published if p["uri"] == manifest.as_uri()]
        check(len(published) == 2, "a lock change re-analyses the open manifest", str(published))
        check(any("different dependencies" in x["message"] for x in lock_diag), "the lock file reports its stale manifest", str(lock_diag))


def scenario_hover_completion_definition():
    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        manifest = root / "project.dcon"
        text = ('dpm::v1::Project {\n    name = "app",\n    version = "1.0.0",\n    settings = {\n        optimize = Optimize::O2,\n    },\n    \n'
                '    dependencies = {\n        { location = "github.com/u/a", version = "1.0.0" },\n    },\n}\n')
        lock = ('dpm::v1::Lock {\n    manifest = "sha256:' + "0" * 64 + '",\n    packages = {\n        {\n'
                '            location = "github.com/u/a",\n            version = "1.0.4",\n'
                '            source = "git+https://github.com/u/a.git",\n            commit = "' + "c" * 40 + '",\n'
                '            hash = "sha256:' + "a" * 64 + '",\n        },\n    },\n}\n')
        (root / "project.lock.dcon").write_text(lock)
        s = Session(root)
        s.open(manifest, text)
        field = s.at("textDocument/hover", manifest, 1, 6)
        variant = s.at("textDocument/hover", manifest, 4, 30)
        enum_type = s.at("textDocument/hover", manifest, 4, 20)
        dependency = s.at("textDocument/hover", manifest, 8, 25)
        nothing = s.at("textDocument/hover", manifest, 2, 18)
        fields = s.at("textDocument/completion", manifest, 6, 4)
        enumerators = s.request("textDocument/completion", {"textDocument": {"uri": manifest.as_uri()}, "position": {"line": 4, "character": 19}})
        field_def = s.at("textDocument/definition", manifest, 1, 6)
        variant_def = s.at("textDocument/definition", manifest, 4, 30)
        string_def = s.at("textDocument/definition", manifest, 1, 14)
        s.run()
        h = s.result(field)
        check(h and "Field of `Project`" in h["contents"]["value"] and h["range"]["start"]["line"] == 1, "hover on a field name", str(h))
        h = s.result(variant)
        check(h and "Optimize::O2" in h["contents"]["value"] and "-O2" in h["contents"]["value"], "hover on an enumerator", str(h))
        h = s.result(enum_type)
        check(h and "enum Optimize" in h["contents"]["value"], "hover on an enum type", str(h))
        h = s.result(dependency)
        check(h and "Locked: version 1.0.4" in h["contents"]["value"], "hover on a dependency shows the lock", str(h))
        check(s.result(nothing) is None, "hover on a plain value is empty")
        c = s.result(fields)
        labels = [i["label"] for i in c["items"]]
        check("test_dir" in labels and "name" not in labels and "settings" not in labels, "completion offers the missing fields", str(labels))
        item = next(i for i in c["items"] if i["label"] == "test_dir")
        check(item["insertTextFormat"] == 2 and item["textEdit"]["newText"] == "test_dir = $0,", "field completion is a snippet", str(item))
        e = s.result(enumerators)
        check(any(i["label"] == "O1" for i in e["items"]) or any(i["label"] == "Optimize::O1" for i in e["items"]), "enumerator completion", str(e)[:300])
        d = s.result(field_def)
        check(d and d["uri"].endswith("/v1.dc") and d["range"]["start"]["line"] > 0, "definition of a field is in the schema", str(d))
        schema_lines = (SCHEMA / "dpm" / "v1.dc").read_text().splitlines()
        check(d and "name" in schema_lines[d["range"]["start"]["line"]], "definition line names the field", str(d))
        d = s.result(variant_def)
        check(d and "O2" in schema_lines[d["range"]["start"]["line"]], "definition of an enumerator", str(d))
        check(s.result(string_def) is None, "definition of a string is empty")


def scenario_other_requests_and_isolation():
    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        manifest = root / "project.dcon"
        source = root / "a.dc"
        text = 'dpm::v1::Project { name = "app", version = "1.0.0", bogus = 2 }\n'
        s = Session(root)
        s.open(manifest, text)
        s.open(source, "module main; i32 x = missing;\n", language="dc")
        ids = []
        for method in ("textDocument/semanticTokens/full", "textDocument/formatting", "textDocument/inlayHint", "textDocument/codeAction",
                       "textDocument/references", "textDocument/documentHighlight", "textDocument/prepareRename", "textDocument/signatureHelp"):
            params = {"textDocument": {"uri": manifest.as_uri()}, "position": {"line": 0, "character": 5},
                      "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 5}},
                      "options": {"tabSize": 4, "insertSpaces": True}, "context": {"includeDeclaration": True, "diagnostics": []}}
            ids.append((method, s.request(method, params)))
        s.change(source, "module main; i32 x = 0;\n", 2)
        s.change(source, "module main; i32 x = missing;\n", 3)
        s.change(manifest, text.replace("bogus", "bogus2"), 2)
        s.run()
        for method, request_id in ids:
            check(s.result(request_id) is None, method + " answers null for a manifest")
        manifest_pubs = [p for p in s.published if p["uri"] == manifest.as_uri()]
        check(all(len(p["diagnostics"]) >= 1 for p in manifest_pubs), "analysing a .dc file never clears manifest diagnostics", str(manifest_pubs))
        check(len(s.last(source)["diagnostics"]) == 1, "the .dc file is still analysed as before", str(s.last(source)))
        check(not [p for p in s.published if p["uri"] == source.as_uri() and "dpm" in json.dumps(p)], ".dc diagnostics carry no dpm source")


def scenario_environment():
    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        manifest = root / "project.dcon"
        s = Session(root, schema=root / "nowhere")
        s.open(manifest, 'dpm::v1::Project { name = "app", version = "1.0.0" }\n')
        hover = s.at("textDocument/hover", manifest, 0, 20)
        completion = s.at("textDocument/completion", manifest, 0, 20)
        s.run()
        d = s.last(manifest)["diagnostics"]
        check(len(d) == 1 and "schema was not found" in d[0]["message"] and d[0]["severity"] == 2, "a missing schema is reported once", str(d))
        check(s.result(hover) is None, "hover without a schema is empty")
        check(s.result(completion)["items"] == [], "completion without a schema is empty")

    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        manifest = root / "project.dcon"
        s = Session(root)
        s.open(manifest, " " * (1 << 20) + "x")
        s.run()
        d = s.last(manifest)["diagnostics"]
        check(len(d) == 1 and d[0]["severity"] == 1, "an oversized manifest is one error", str(d)[:300])

    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        manifest = root / "project.dcon"
        s = Session(root)
        s.open(manifest, 'dpm::v1::Project { name = "app", version = "1.0.0", requires_dcc = "99.0.0" }\n')
        s.run()
        d = s.last(manifest)["diagnostics"]
        check(any("requires dcc 99.0.0" in x["message"] and x["severity"] == 2 for x in d), "requires_dcc newer than dccd is a warning", str(d))

    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        untitled = "untitled:Untitled-1.dcon"
        s = Session(root)
        s.send("textDocument/didOpen", {"textDocument": {"uri": untitled, "languageId": "dcon", "version": 1, "text": 'dpm::v1::Project { name = "x", version = "1.0.0" }\n'}})
        s.run()
        check(True, "a non-file dcon document does not crash")


def scenario_no_side_effects():
    with tempfile.TemporaryDirectory(prefix="dccd-dcon-") as directory:
        root = Path(directory)
        manifest = root / "project.dcon"
        text = ('dpm::v1::Project {\n    name = "app",\n    version = "1.0.0",\n    dependencies = {\n'
                '        { location = "git+file:///nonexistent/never.git", version = "1.0.0" },\n        { location = "json5", version = "1.0.0" },\n    },\n}\n')
        before = sorted(p.name for p in root.iterdir())
        s = Session(root, env={"PATH": "/nonexistent"})
        s.open(manifest, text)
        s.at("textDocument/hover", manifest, 4, 25)
        s.run()
        after = sorted(p.name for p in root.iterdir())
        check(after == before or after == sorted(before + ["dpm-home"]) and not list((root / "dpm-home").glob("*")), "analysis creates no files and fetches nothing", str(after))
        check(s.last(manifest) is not None, "analysis works without git or curl on PATH")


scenario_diagnostics()
scenario_utf16_columns()
scenario_lock()
scenario_hover_completion_definition()
scenario_other_requests_and_isolation()
scenario_environment()
scenario_no_side_effects()
print("%d checks, %d failed" % (CHECKS, len(FAILURES)))
sys.exit(1 if FAILURES else 0)
