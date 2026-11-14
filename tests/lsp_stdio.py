import argparse
import json
from pathlib import Path
import os
import subprocess


def check(server, launcher):
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "processId": None, "rootUri": None, "capabilities": {}, "clientInfo": {"name": "DCC \u5de5\u5177"}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    payload = bytearray()
    for message in messages:
        body = json.dumps(message, ensure_ascii=False, indent=2).replace('\n', '\r\n').encode('utf-8')
        payload.extend(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    result = subprocess.run([*launcher, str(Path(server).resolve())], input=payload, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr.decode(errors='replace')
    output = result.stdout
    responses = {}
    while output:
        header, output = output.split(b'\r\n\r\n', 1)
        assert b'\r\r\n' not in header, 'stdout underwent Windows text-mode newline translation'
        fields = dict(line.split(b':', 1) for line in header.split(b'\r\n'))
        length = int(fields[b'Content-Length'])
        body, output = output[:length], output[length:]
        assert len(body) == length
        response = json.loads(body.decode('utf-8'))
        if 'id' in response:
            responses[response['id']] = response
    assert 'capabilities' in responses[1]['result'], responses
    assert responses[2]['result'] is None, responses
    print('PASS dccd binary stdio, UTF-8/CRLF payloads, initialize and shutdown')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('server')
    parser.add_argument('--wine')
    args = parser.parse_args()
    if os.name != 'nt':
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    check(args.server, [args.wine] if args.wine else [])
