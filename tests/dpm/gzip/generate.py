import gzip
import os
import random
import struct
import subprocess
import sys
import zlib

out = os.path.dirname(os.path.abspath(__file__))
rng = random.Random(20261030)


def text(n):
    words = ["alpha", "beta", "gamma", "delta", "module", "import", "public", "struct", "return", "value"]
    parts = []
    size = 0
    while size < n:
        w = rng.choice(words) + (" " if rng.random() < 0.8 else "\n")
        parts.append(w)
        size += len(w)
    return "".join(parts).encode()[:n]


def noise(n):
    return bytes(rng.getrandbits(8) for _ in range(n))


def wrap(raw, deflated, flags=0, extra=b"", name=b"", comment=b"", hcrc=False):
    flg = flags
    head = b"\x1f\x8b\x08" + bytes([flg]) + struct.pack("<I", 0) + b"\x00\x03"
    if flg & 4:
        head += struct.pack("<H", len(extra)) + extra
    if flg & 8:
        head += name + b"\x00"
    if flg & 16:
        head += comment + b"\x00"
    if flg & 2:
        head += struct.pack("<H", zlib.crc32(head) & 0xFFFF)
    return head + deflated + struct.pack("<II", zlib.crc32(raw) & 0xFFFFFFFF, len(raw) & 0xFFFFFFFF)


def raw_deflate(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY, flush=None, chunk=None):
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
    if chunk is None:
        return c.compress(data) + c.flush()
    out_bytes = b""
    for i in range(0, len(data), chunk):
        out_bytes += c.compress(data[i:i + chunk])
        out_bytes += c.flush(flush)
    return out_bytes + c.flush()


def save(name, raw, gz):
    with open(os.path.join(out, name + ".raw"), "wb") as f:
        f.write(raw)
    with open(os.path.join(out, name + ".gz"), "wb") as f:
        f.write(gz)


def gzip_tool(raw, level):
    return subprocess.run(["gzip", "-n", "-c", "-" + str(level)], input=raw, stdout=subprocess.PIPE, check=True).stdout


save("empty", b"", wrap(b"", raw_deflate(b"")))
save("empty-tool", b"", gzip_tool(b"", 6))
small = text(3000)
save("stored", small, wrap(small, raw_deflate(small, 0)))
big_stored = noise(70000)
save("stored-multi", big_stored, wrap(big_stored, raw_deflate(big_stored, 0)))
save("fixed", small, wrap(small, raw_deflate(small, 6, zlib.Z_FIXED)))
body = text(20000)
save("dynamic", body, wrap(body, raw_deflate(body, 6)))
for level in (1, 6, 9):
    mixed = text(30000) + noise(2000) + text(30000)
    save("level%d" % level, mixed, gzip_tool(mixed, level))
    save("zlib-level%d" % level, mixed, wrap(mixed, raw_deflate(mixed, level)))
window = noise(32768)
far = window + window[:258] + text(100)
save("max-distance", far, wrap(far, raw_deflate(far, 9)))
runs = b"a" * 1000 + b"ab" * 700 + b"\x00" * 5000
save("long-runs", runs, wrap(runs, raw_deflate(runs, 9)))
multi = text(40000)
save("multi-block", multi, wrap(multi, raw_deflate(multi, 6, flush=zlib.Z_FULL_FLUSH, chunk=5000)))
hdr = text(1000)
save("header-all", hdr, wrap(hdr, raw_deflate(hdr), flags=2 | 4 | 8 | 16, extra=b"XY\x04\x00abcd", name=b"file.tar", comment=b"a comment", hcrc=True))
save("header-name", hdr, wrap(hdr, raw_deflate(hdr), flags=8, name=b"file.tar"))
