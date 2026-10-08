import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from test_compdb_output import check_server


def run(dcc, server, zig):
    sdk = Path(__file__).resolve().parents[1] / "zig/dcc_sdk.zig"
    with tempfile.TemporaryDirectory(prefix="dcc-zig-compdb-") as temp:
        root = Path(temp)
        shutil.copyfile(sdk, root / "sdk.zig")
        wrapper = root / "dcc-bounded"
        wrapper.write_text(f'#!/bin/sh\nexec timeout 60 "{dcc}" "$@"\n')
        wrapper.chmod(0o755)
        (root / "include spaces").mkdir()
        (root / "include spaces/helper.dc").write_text("module helper; public i32 value() { return 42; }\n")
        (root / "main spaces.dc").write_text("module main;\nimport helper;\ni32 f() { return helper::value(); }\n")
        (root / "build.zig").write_text('''const std = @import("std");
const sdk = @import("sdk.zig");
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    if (b.option(bool, "terminal-only", "Test a terminal dump") orelse false) {
        const dump = sdk.compile(b, .{
            .dcc_exe = b.pathFromRoot("dcc-bounded"), .name = "dump", .source_file = b.path("main spaces.dc"),
            .target = target, .optimize = .Debug, .dump = .ir,
            .include_dirs = &.{b.path("include spaces")},
        });
        if (dump.compdb_entry != null) @panic("terminal dump has an entry");
        b.getInstallStep().dependOn(&b.addInstallFile(dump.output_file, "main.ir").step);
        return;
    }
    const generated = b.addWriteFiles();
    const source = generated.add("generated.dc", "module generated; public i32 f() { return 9; }\\n");
    const first = sdk.compile(b, .{
        .dcc_exe = b.pathFromRoot("dcc-bounded"), .name = "first", .source_file = b.path("main spaces.dc"),
        .target = target, .optimize = .Debug, .output = .object,
        .include_dirs = &.{b.path("include spaces")},
    });
    const second = sdk.compile(b, .{
        .dcc_exe = b.pathFromRoot("dcc-bounded"), .name = "second", .source_file = source,
        .target = target, .optimize = .ReleaseFast, .output = .object, .track_dependencies = false,
        .extra_args = &.{"-fbackend", "custom"},
    });
    b.getInstallStep().dependOn(&b.addInstallFile(first.output_file, "lib/first.o").step);
    b.getInstallStep().dependOn(&b.addInstallFile(second.output_file, "lib/second.o").step);
    if (first.compdb_entry == null or second.compdb_entry == null) @panic("missing entry outputs");
}
''')

        def build(*args):
            result = subprocess.run([zig, "build", *args], cwd=root, capture_output=True, timeout=120)
            assert result.returncode == 0, (result.stdout.decode(), result.stderr.decode())

        database = root / "compile_commands.json"
        build("-j4")
        commands = json.loads(database.read_text())
        assert len(commands) == 2, commands
        for command in commands:
            assert command["directory"] == str(root)
            assert Path(command["file"]).is_file(), command
            assert Path(command["output"]).suffix == ".o", command
            assert "--compdb-entry" in command["arguments"]
            entry = Path(command["arguments"][command["arguments"].index("--compdb-entry") + 1])
            candidates = list((root / ".zig-cache").rglob(entry.name))
            assert any(json.loads(candidate.read_text()) == command for candidate in candidates), command
        assert any(str(root / "include spaces") in c["arguments"] for c in commands)
        assert (root / "zig-out/lib/first.o").is_file()
        assert (root / "zig-out/lib/second.o").is_file()
        check_server(server, root, root / "main spaces.dc", False, 2)
        before = database.read_bytes()
        build("-j4")
        assert database.read_bytes() == before
        database.unlink()
        build("-j4")
        assert database.read_bytes() == before
        database.unlink()
        build("dcc-compdb", "-j4")
        assert database.read_bytes() == before
        (root / "include spaces/helper.dc").write_text("module helper; public i32 value() { return 43; }\n")
        build("-j4")
        assert len(json.loads(database.read_text())) == 2
        database.unlink()
        build("-Dterminal-only=true", "-j4")
        assert not database.exists()
    print("PASS automatic Zig SDK database, generated paths, both backends, cached builds, and dependency rebuilds")


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3] if len(sys.argv) > 3 else "zig")
