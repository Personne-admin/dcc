from pathlib import Path
import json
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
server_binary = Path(sys.argv[2]).resolve()
checks = 0

main_source = """module main;
import std::result;
import util;
using std::result::Result;
Result(i32, i32) make(i32 x) {
    return Result(i32, i32)::Err(x);
}
@nomangle public i32 kmain(i32 x) {
    return make(x).unwrap_err() + util::helper();
}
"""

util_source = """module util;
import std::result;
using std::result::Result;
Result(i32, i32) make_ok() {
    return Result(i32, i32)::Ok(1);
}
public i32 helper() {
    return make_ok().unwrap();
}
"""


def check(condition, message):
    global checks
    if not condition:
        print(f"    FAIL  {message}", file=sys.stderr)
        sys.exit(1)
    checks += 1


def frame(message):
    body = json.dumps(message).encode()
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


def read_frame(stream):
    length = None
    while True:
        line = stream.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    return json.loads(stream.read(length))


def query(root, source, needle):
    process = subprocess.Popen(["timeout", "60", str(server_binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    next_id = 0
    diagnostics = []

    def request(method, params):
        nonlocal next_id
        next_id += 1
        process.stdin.write(frame({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params}))
        process.stdin.flush()
        while True:
            message = read_frame(process.stdout)
            check(message is not None, f"dccd closed the connection during {method}")
            if message.get("method") == "textDocument/publishDiagnostics":
                diagnostics.extend(message["params"]["diagnostics"])
            elif message.get("id") == next_id and "method" not in message:
                return message.get("result")

    def notify(method, params):
        process.stdin.write(frame({"jsonrpc": "2.0", "method": method, "params": params}))
        process.stdin.flush()

    request("initialize", {"processId": None, "rootUri": root.as_uri(), "capabilities": {}, "workspaceFolders": [{"uri": root.as_uri(), "name": "project"}]})
    notify("initialized", {})
    text = source.read_text()
    notify("textDocument/didOpen", {"textDocument": {"uri": source.as_uri(), "languageId": "dc", "version": 1, "text": text}})
    lines = text.splitlines()
    line = next(index for index, value in enumerate(lines) if needle in value)
    position = {"textDocument": {"uri": source.as_uri()}, "position": {"line": line, "character": lines[line].index(needle) + 1}}
    hover = request("textDocument/hover", position)
    definition = request("textDocument/definition", position)
    request("shutdown", None)
    notify("exit", None)
    _, log = process.communicate(timeout=60)
    return hover, definition, diagnostics, log.decode(errors="replace")


with tempfile.TemporaryDirectory(prefix="dcc-dccd-layout-") as directory:
    root = Path(directory)
    kernel = root / "kernel"
    (kernel / "src").mkdir(parents=True)
    (kernel / "obj").mkdir()
    (kernel / "src" / "main.dc").write_text(main_source)
    (kernel / "src" / "util.dc").write_text(util_source)

    for name in ("main", "util"):
        result = subprocess.run(["timeout", "60", str(compiler), "-target", "x86_64-elf", "-flibdcext", "freestanding", "-c", f"src/{name}.dc", "-o", f"obj/{name}.o",
                                 "--compdb-entry", f"obj/{name}.compdb.json"], cwd=kernel, capture_output=True, text=True)
        check(result.returncode == 0, f"dcc failed to compile {name}.dc: {result.stderr}")

    result = subprocess.run(["timeout", "60", str(compiler), "--merge-compdb", "compile_commands.json", "obj/main.compdb.json"], cwd=kernel, capture_output=True, text=True)
    check(result.returncode == 0, f"dcc --merge-compdb failed: {result.stderr}")

    for name, needle in (("main", "unwrap_err"), ("util", "unwrap")):
        hover, definition, diagnostics, log = query(root, kernel / "src" / f"{name}.dc", needle)
        check(not diagnostics, f"{name}.dc has diagnostics: {diagnostics}\n{log}")
        check(hover is not None and f"{needle}(" in hover["contents"]["value"], f"{name}.dc hover on {needle}: {hover}")
        check(definition is not None and definition["uri"].endswith("/std/result.dc"), f"{name}.dc definition of {needle}: {definition}")
        if name == "util":
            check("is not listed in" in log, "util.dc borrowed command is not logged")

print(f"PASS dccd project layout ({checks} checks)")
