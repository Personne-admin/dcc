#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import random
import subprocess
import tempfile
import time

from lsp_fuzzer import RpcTransport, _send_message


class Client:
    def __init__(self, executable):
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([executable], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.log)
        self.transport = RpcTransport(self.process)
        self.identifier = 0
        result = self.request("initialize", {})
        capabilities = result["capabilities"]
        assert capabilities["positionEncoding"] == "utf-16", capabilities
        assert capabilities["textDocumentSync"] == 2, capabilities
        assert capabilities["semanticTokensProvider"]["full"] is True, capabilities
        self.notify("initialized", {})

    def notify(self, method, params):
        _send_message(self.process, {"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method, params):
        self.identifier += 1
        _send_message(self.process, {"jsonrpc": "2.0", "id": self.identifier,
                                     "method": method, "params": params})
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            message, error = self.transport.read_message(max(0, deadline - time.monotonic()))
            if error:
                raise RuntimeError(error)
            if message and message.get("id") == self.identifier:
                assert "error" not in message, message
                return message["result"]
        raise TimeoutError(f"{method} exceeded five seconds")

    def open(self, uri, text):
        self.notify("textDocument/didOpen", {"textDocument": {
            "uri": uri, "languageId": "dc", "version": 1, "text": text}})

    def tokens(self, uri):
        return self.request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})["data"]

    def close(self):
        self.process.kill()
        self.process.communicate(timeout=5)
        self.log.seek(0)
        log = self.log.read().decode(errors="replace")
        self.log.close()
        assert "uncaught" not in log, log


def position(text, offset):
    prefix = text[:offset].replace("\r\n", "\n").replace("\r", "\n")
    return {"line": prefix.count("\n"),
            "character": len(prefix.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}


def run(executable, iterations, seeds):
    root = Path(__file__).resolve().parent.parent
    sources = [(root / path).read_text() for path in (
        "vscode/src/test/fixtures/grammar-sample.dc",
        "tests/stdlib/conditional-return-loop.dc")]
    sources += ["module edit;\n/// café 😀\ni32 f() {\n  /* block */\n  \"café 😀\";\n  return 1;\n}\n"]
    sources += [text.replace("\n", "\r\n") for text in sources]
    fragments = ["/*", "*/", "//", "//!", "///", '"', "'", "\\", "\n", "\r\n", "é", "😀", "", "x", " "]
    for seed in seeds:
        rng = random.Random(seed)
        client = Client(executable)
        uri = "file:///tmp/dccd_edit_wire.dc"
        text = sources[seed % len(sources)]
        client.open(uri, text)
        try:
            for iteration in range(iterations):
                changes = []
                for _ in range(rng.randrange(1, 4)):
                    boundaries = [i for i in range(len(text) + 1)
                                  if not (0 < i < len(text) and text[i - 1:i + 1] == "\r\n")]
                    start = rng.choice(boundaries)
                    if rng.random() < .75:
                        anchors = [i for i in boundaries if text[i:i + 1] in ('/', '*', '"', "é", "😀")]
                        if anchors:
                            start = rng.choice(anchors)
                    end = rng.choice([i for i in boundaries if start <= i <= start + 8])
                    replacement = rng.choice(fragments)
                    if iteration % 31 == 0:
                        start, end = 0, len(text)
                        replacement = sources[(iteration // 31) % len(sources)]
                    elif iteration % 17 == 0:
                        start = end = len(text)
                    changes.append({"range": {"start": position(text, start), "end": position(text, end)},
                                    "text": replacement})
                    text = text[:start] + replacement + text[end:]
                client.notify("textDocument/didChange", {
                    "textDocument": {"uri": uri, "version": iteration + 2}, "contentChanges": changes})
                actual = client.tokens(uri)
                fresh = Client(executable)
                try:
                    fresh.open(uri, text)
                    expected = fresh.tokens(uri)
                    assert actual == expected, (actual, expected)
                finally:
                    fresh.close()
                for method in ("hover", "definition", "completion"):
                    client.request(f"textDocument/{method}", {
                        "textDocument": {"uri": uri}, "position": position(text, len(text))})
        except BaseException:
            Path(tempfile.gettempdir(), "dccd-edit-failure.json").write_text(json.dumps({
                "seed": seed, "iteration": iteration, "text": text, "changes": changes}, ensure_ascii=False, indent=2))
            print(f"FAIL seed={seed} iteration={iteration}; /tmp/dccd-edit-failure.json", flush=True)
            raise
        finally:
            client.close()
        print(f"PASS edit wire seed={seed} edits={iterations}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server")
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--seeds", type=int, nargs="+", default=[42, 8086, 20261018])
    args = parser.parse_args()
    run(args.server, args.iterations, args.seeds)
