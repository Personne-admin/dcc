import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(server):
    with tempfile.TemporaryDirectory(prefix="dccd-diagnostics-") as directory:
        root = Path(directory)
        a, b = [(root / name).as_uri() for name in ("a.dc", "b.dc")]
        invalid = "module main; i32 x = missing;\n"
        valid = "module main; i32 x = 0;\n"
        messages = []
        expectations = {}

        def send(method, params, request_id=None):
            message = {"jsonrpc": "2.0", "method": method, "params": params}
            if request_id is not None:
                message["id"] = request_id
            messages.append(message)

        def checkpoint(expected):
            request_id = len(messages) + 10
            expectations[request_id] = expected
            send("textDocument/hover", {
                "textDocument": {"uri": a},
                "position": {"line": 0, "character": 17},
            }, request_id)

        def change(uri, version, text):
            send("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": text}],
            })

        send("initialize", {"rootUri": root.as_uri()}, 1)
        send("initialized", {})
        for uri in (a, b):
            send("textDocument/didOpen", {"textDocument": {
                "uri": uri, "languageId": "dc", "version": 1, "text": invalid,
            }})
        checkpoint({a: (1, 1), b: (1, 1)})
        for version in range(2, 12):
            change(a, version, invalid + "\n" * version)
            send("textDocument/didSave", {"textDocument": {"uri": a}})
            send("workspace/didChangeWatchedFiles", {"changes": [{"uri": a, "type": 2}]})
            checkpoint({a: (version, 1), b: (1, 1)})
        change(a, 10, valid)
        checkpoint({a: (11, 1), b: (1, 1)})
        send("workspace/didChangeWatchedFiles", {"changes": [{"uri": (root / "dcc.json").as_uri(), "type": 2}]})
        checkpoint({a: (11, 1), b: (1, 1)})
        change(a, 12, valid)
        checkpoint({a: (12, 0), b: (1, 1)})
        change(a, 13, invalid)
        checkpoint({a: (13, 1), b: (1, 1)})
        for method in ("textDocument/definition", "textDocument/completion"):
            send(method, {"textDocument": {"uri": a}, "position": {"line": 0, "character": 17}}, len(messages) + 1000)
        send("textDocument/didClose", {"textDocument": {"uri": b}})
        checkpoint({a: (13, 1), b: (1, 0)})
        send("shutdown", {}, 2)
        send("exit", {})
        payload = bytearray()
        for message in messages:
            body = json.dumps(message).encode()
            payload.extend(f"Content-Length: {len(body)}\r\n\r\n".encode())
            payload.extend(body)
        process = subprocess.run([str(server)], input=payload, capture_output=True, timeout=30)
        assert process.returncode == 0, process.stderr.decode()
        output = process.stdout
        states = {}
        publications = 0
        responses = set()
        while output:
            header, output = output.split(b"\r\n\r\n", 1)
            assert header.startswith(b"Content-Length: "), header
            length = int(header[len(b"Content-Length: "):])
            message = json.loads(output[:length])
            output = output[length:]
            assert "error" not in message, message
            if message.get("method") == "textDocument/publishDiagnostics":
                params = message["params"]
                uri, version, diagnostics = params["uri"], params.get("version"), params["diagnostics"]
                if not diagnostics:
                    assert (uri, version) in ((a, 12), (b, 1)), message
                    if uri == b:
                        assert states.get(a) == (13, 1), message
                states[uri] = (version, len(diagnostics))
                publications += 1
            if "id" in message:
                responses.add(message["id"])
                expected = expectations.get(message["id"])
                if expected is not None:
                    assert states == expected, (states, expected, process.stderr.decode())
        assert responses == {m["id"] for m in messages if "id" in m}, responses
        print(f"PASS: {publications} diagnostic publications, {len(expectations)} lifecycle checkpoints, clean shutdown")


if __name__ == "__main__":
    run(Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "build/bin/dccd")
