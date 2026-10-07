#!/usr/bin/env python3
from pathlib import Path
import sys
import tempfile
from test_memory import run

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="dcc-os-gating-") as temporary:
    scratch = Path(temporary)
    source = scratch / "test.dc"
    source.write_text('''module test;
import core;
import std::os::error;
import std::os::file;
import std::os::heap;
import std::os::thread;
import std::os::thread_pool;
import std::os::process;
import std::os::net;
import std::os::time;
@nomangle public usize probe() {
    static if compiles { std::os::heap::allocator(); } {
        core::compile_error("OS allocator leaked");
    }
    static if compiles { std::os::process::exit(0); } {
        core::compile_error("OS exit leaked");
    }
    return sizeof(std::os::file::File)
        + sizeof(std::os::thread::Thread)
        + sizeof(std::os::thread_pool::Task)
        + sizeof(std::os::net::SocketAddrV4)
        + std::os::error::name(std::os::error::Error::NotSupported).len;
}
''')
    for backend in ("llvm", "custom"):
        for opt in ("O0", "O2"):
            run([sys.argv[1], "-flibdcext", "freestanding", "-fbackend", backend, "-" + opt,
                 "-fno-simd", "-fno-x87", "-fno-red-zone", "-fpic", "-I", root / "libdcext",
                 "-c", source, "-o", scratch / "test.o"])
print("  RESULT  freestanding OS data/pure logic and absent services passed on both backends at O0/O2")
