import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def check_server(server, root, source, expect_error, command_count=1):
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": root.as_uri()}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": source.as_uri(), "languageId": "dc", "version": 1, "text": source.read_text(),
        }}},
        {"jsonrpc": "2.0", "id": 3, "method": "textDocument/definition", "params": {
            "textDocument": {"uri": source.as_uri()}, "position": {"line": 2, "character": 26},
        }},
        {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit", "params": {}},
    ]
    payload = b""
    for message in messages:
        body = json.dumps(message).encode()
        payload += f"Content-Length: {len(body)}\r\n\r\n".encode() + body
    result = subprocess.run([str(server)], input=payload, capture_output=True, timeout=60)
    assert result.returncode == 0, result.stderr.decode()
    output = result.stdout
    diagnostics = None
    definition = None
    while output:
        header, output = output.split(b"\r\n\r\n", 1)
        size = int(header.split(b": ")[1])
        response = json.loads(output[:size])
        output = output[size:]
        if response.get("method") == "textDocument/publishDiagnostics":
            diagnostics = response["params"]["diagnostics"]
        if response.get("id") == 3:
            definition = response.get("result")
    if expect_error:
        assert diagnostics, (diagnostics, result.stderr.decode())
    else:
        assert not diagnostics, (diagnostics, result.stderr.decode())
        assert definition, (definition, result.stderr.decode())
        assert "helper.dc" in json.dumps(definition), definition
        assert f"compilation database: {command_count} commands" in result.stderr.decode(), result.stderr.decode()
        assert "ignoring unknown option in compile command: --compdb-entry" not in result.stderr.decode()


def run(dcc, server):
    with tempfile.TemporaryDirectory(prefix="dcc-compdb-") as temp:
        root = Path(temp)
        includes = root / 'include spaces "quotes" Ω😀'
        includes.mkdir()
        (includes / "helper.dc").write_text("module helper; public i32 value() { return 42; }\n")
        source = root / "main spaces.dc"
        source.write_text("module main;\nimport helper;\ni32 f() { return helper::value(); }\n")
        entry = root / "entry.json"
        obj = root / "main.o"

        def invoke(args, success=True):
            result = subprocess.run([str(dcc), *map(str, args)], cwd=root, capture_output=True, timeout=60)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            return result

        help_text = invoke(["--help"]).stdout.decode()
        assert "--compdb-entry <file>" in help_text and "--merge-compdb <out>" in help_text
        assert "dump custom backend MIR" in help_text
        for backend in ("llvm", "custom"):
            for opt in ("-O0", "-O2"):
                args = ["-c", "-target", "x86_64-elf", "-fbackend", backend, opt,
                        "-I", includes.name, '-Jusing []const u8 text = "spaces and \\\"quotes\\\"";',
                        f"-ffile-prefix-map={root}=/mapped", "--compdb-entry", entry.name,
                        "--depfile", "main.d", "-o", obj.name, source.name]
                invoke(args)
                data = json.loads(entry.read_text())
                assert data == {"directory": str(root), "file": str(source), "arguments": [str(dcc), *args], "output": str(obj)}, data

        database = root / "compile_commands.json"
        invoke(["--merge-compdb", database, entry])
        assert json.loads(database.read_text()) == [data]
        check_server(server, root, source, False)
        database.unlink()
        check_server(server, root, source, True)

        replacement = dict(data, arguments=["dcc", "-I", "last wins", "", 'a"b\\c\n\t', "Ω😀"])
        last = root / "last.json"
        last.write_text(json.dumps(replacement))
        relative = dict(data, directory=".", file=source.name, output=obj.name)
        relative_entry = root / "relative.json"
        relative_entry.write_text(json.dumps(relative))
        other = dict(data, output=str(root / "zzz.o"))
        second = root / "second.json"
        second.write_text(json.dumps(other))
        invoke(["--merge-compdb", database, second, entry, relative_entry, last])
        expected = [replacement, other]
        assert json.loads(database.read_text()) == expected
        deterministic = database.read_bytes()
        invoke(["--merge-compdb", database, entry, relative_entry, last, second])
        assert database.read_bytes() == deterministic

        bad = root / 'bad "entry".json'
        invalids = ["{", "[]", json.dumps(dict(data, output=42)), json.dumps(dict(data, arguments=[1])),
                    json.dumps(dict(data, arguments=[])), json.dumps({k: v for k, v in data.items() if k != "file"}),
                    json.dumps(dict(data, file="")), json.dumps(data) + ",", json.dumps(data) + " garbage",
                    '{"file":"a","file":"b"}', json.dumps(dict(data, arguments=["\\uD800"])).replace("\\\\uD800", "\\uD800")]
        for content in invalids:
            bad.write_text(content)
            result = invoke(["--merge-compdb", database, entry, bad], False)
            assert str(bad).encode() in result.stderr, result.stderr
            assert database.read_bytes() == deterministic
        bad.write_bytes(b'{"directory":"\xff"}')
        result = invoke(["--merge-compdb", database, bad], False)
        assert str(bad).encode() in result.stderr
        bad.unlink()
        result = invoke(["--merge-compdb", database, bad], False)
        assert str(bad).encode() in result.stderr
        assert database.read_bytes() == deterministic
        result = invoke(["--merge-compdb", database, root], False)
        assert str(root).encode() in result.stderr
        invoke(["--merge-compdb", database])
        assert json.loads(database.read_text()) == []
        invoke(["--merge-compdb", root / "absent" / "db.json", entry], False)

        for destination in (source, obj, root / "main.d"):
            before = destination.read_bytes()
            result = invoke(["-c", "-fbackend", "custom", "-I", includes, "--depfile", "main.d",
                             "--compdb-entry", destination, "-o", obj, source], False)
            assert b"conflicts" in result.stderr
            assert destination.read_bytes() == before
        helper = includes / "helper.dc"
        before = helper.read_bytes()
        result = invoke(["-c", "-fbackend", "custom", "-I", includes, "--compdb-entry", helper, "-o", obj, source], False)
        assert b"input source file" in result.stderr and helper.read_bytes() == before
        failed = root / "invalid.dc"
        failed.write_text("module invalid; i32 value = missing;\n")
        before = entry.read_bytes()
        invoke(["-c", "--compdb-entry", entry, failed], False)
        assert entry.read_bytes() == before
        invoke(["-fdump-ast", "--compdb-entry", entry, source], False)
        invoke(["--compdb-entry", entry, obj], False)
        invoke(["-c", "-fbackend", "custom", "-I", includes, "--compdb-entry", root / "absent" / "entry.json", source], False)
        invoke(["-S", "-fbackend", "custom", "-I", includes, "--compdb-entry", entry, source])
        assert json.loads(entry.read_text())["output"] == str(root / "main spaces.s")

        def parallel(index):
            destination = root / f"parallel-{index}.json"
            invoke(["-c", "-fbackend", "custom", "-I", includes, "--compdb-entry", destination, "-o", root / f"parallel-{index}.o", source])
            invoke(["--merge-compdb", database, destination])
            assert isinstance(json.loads(destination.read_text()), dict)
            assert isinstance(json.loads(database.read_text()), list)

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(parallel, range(12)))
        assert not list(root.glob("*.tmp.*"))
    print("PASS compile command database output, merging, atomic publication, and dccd imports")


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
