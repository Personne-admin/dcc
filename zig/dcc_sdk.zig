const std = @import("std");

pub const CodeModel = enum { default, small, kernel, medium, large };
pub const DebugFormat = enum { none, auto, dwarf, pdb };
pub const Backend = enum { llvm, em64t };
pub const PicMode = enum { pic, pie };

pub const OptLevel = enum {
    O0,
    O1,
    O2,
    Os,

    pub fn fromZigOptimize(mode: std.builtin.OptimizeMode) OptLevel {
        return switch (mode) {
            .Debug => .O0,
            .ReleaseSafe, .ReleaseFast => .O2,
            .ReleaseSmall => .Os,
        };
    }
};

/// what should the compiler produce?
pub const OutputKind = enum {
    executable,
    object,
    assembly,
    shared_library,
};

/// terminal dumps terminate the pipeline and write the output to
/// stdout, they do not produce filesystem artifcats and as such
/// can not be used with --depfile.
pub const DumpMode = enum {
    ast,
    ir,
    llvm,
    mir,

    pub fn isTerminal(mode: DumpMode) bool {
        return switch (mode) {
            .ast, .ir => true,
            .llvm, .mir => false,
        };
    }

    pub fn flag(mode: DumpMode) []const u8 {
        return switch (mode) {
            .ast => "-fdump-ast",
            .ir => "-fdump-ir",
            .llvm => "-fdump-llvm",
            .mir => "-fdump-mir",
        };
    }
};

pub const TargetTriple = enum {
    @"x86_64-elf",
    @"x86-elf",
    @"x86_64-coff",
    @"x86-coff",

    pub fn fromZigTarget(target: std.Target) !TargetTriple {
        const arch = target.cpu.arch;
        const os = target.os.tag;

        return match: {
            if (arch == .x86_64 and (os == .linux or os == .freestanding)) break :match .@"x86_64-elf";
            if (arch == .x86 and (os == .linux or os == .freestanding)) break :match .@"x86-elf";
            if (arch == .x86_64 and os == .windows) break :match .@"x86_64-coff";
            if (arch == .x86 and os == .windows) break :match .@"x86-coff";

            return error.UnsupportedDccTarget;
        };
    }

    pub fn isCoff(triple: TargetTriple) bool {
        return switch (triple) {
            .@"x86_64-coff", .@"x86-coff" => true,
            else => false,
        };
    }
};

pub const CompileOptions = struct {
    dcc_exe: []const u8 = "dcc",

    name: []const u8,
    source_file: std.Build.LazyPath,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,

    /// defaults to `OptLevel.fromZigOptimize(optimize)`.
    opt_level: ?OptLevel = null,

    output: OutputKind = .executable,
    dump: ?DumpMode = null,
    backend: ?Backend = null,
    arch: ?[]const u8 = null,

    libdcext: bool = false,
    pic: ?PicMode = null,

    no_red_zone: bool = false,
    no_simd: bool = false,
    no_x87: bool = false,
    no_stack_protector: bool = false,
    no_stack_probe: bool = false,
    code_model: ?CodeModel = null,

    bounds_check: ?bool = null,
    emit_debug_info: ?bool = null,
    debug_format: ?DebugFormat = null,
    omit_frame_pointer: ?bool = null,

    include_dirs: []const std.Build.LazyPath = &.{},
    injected_decls: []const []const u8 = &.{},

    track_dependencies: bool = true,
    extra_args: []const []const u8 = &.{},
};

pub const DccArtifact = struct {
    step: *std.Build.Step.Run,
    output_file: std.Build.LazyPath,
    depfile: ?std.Build.LazyPath,
};

pub fn compile(b: *std.Build, options: CompileOptions) DccArtifact {
    const dcc_triple = TargetTriple.fromZigTarget(options.target.result) catch {
        std.debug.print(
            \\
            \\ Error: Target "{s}-{s}" is not supported by dcc.
            \\ Supported targets are: x86_64-elf, x86-elf, x86_64-coff, x86-coff
            \\
            \\
        , .{
            @tagName(options.target.result.cpu.arch),
            @tagName(options.target.result.os.tag),
        });
        @panic("Unsupported compilation target");
    };

    validate(options);

    const run = b.addSystemCommand(&.{options.dcc_exe});
    run.step.name = b.fmt("dcc {s}", .{options.name});
    run.has_side_effects = false;

    run.addArgs(&.{ "-target", @tagName(dcc_triple) });

    if (options.backend) |backend| run.addArgs(&.{ "-fbackend", @tagName(backend) });
    if (options.arch) |cpu| run.addArgs(&.{ "-farch", cpu });

    const opt = options.opt_level orelse OptLevel.fromZigOptimize(options.optimize);
    run.addArg(b.fmt("-{s}", .{@tagName(opt)}));

    const is_debug = options.optimize == .Debug;
    const do_bounds_check = options.bounds_check orelse (is_debug or options.optimize == .ReleaseSafe);
    const do_emit_debug = options.emit_debug_info orelse is_debug;
    const do_omit_fp = options.omit_frame_pointer orelse !is_debug;

    if (do_bounds_check) run.addArg("-fbounds-check");

    if (do_omit_fp) {
        run.addArg("-fomit-frame-pointer");
    } else run.addArg("-fno-omit-frame-pointer");

    if (do_emit_debug) {
        if (options.debug_format) |fmt| switch (fmt) {
            .none => run.addArg("-gnone"),
            .auto => run.addArg("-g3"),
            .dwarf => run.addArg("-gdwarf"),
            .pdb => run.addArg("-gpdb"),
        } else run.addArg("-g3");
    } else run.addArg("-g0");

    if (options.libdcext) run.addArg("-flibdcext");
    if (options.pic) |mode| run.addArg(switch (mode) {
        .pic => "-fPIC",
        .pie => "-fPIE",
    });
    if (options.no_red_zone) run.addArg("-fno-red-zone");
    if (options.no_simd) run.addArg("-fno-simd");
    if (options.no_x87) run.addArg("-fno-x87");
    if (options.no_stack_protector) run.addArg("-fno-stack-protector");
    if (options.no_stack_probe) run.addArg("-fno-stack-probe");

    if (options.code_model) |model| {
        run.addArgs(&.{ "-mcmodel", @tagName(model) });
    }

    for (options.include_dirs) |dir| {
        run.addArg("-I");
        run.addDirectoryArg(dir);
    }

    for (options.injected_decls) |decl| {
        run.addArg(b.fmt("-J{s}", .{decl}));
    }

    if (options.dump) |mode| run.addArg(mode.flag());

    const terminal_dump = if (options.dump) |mode| mode.isTerminal() else false;

    var depfile: ?std.Build.LazyPath = null;
    if (options.track_dependencies and !terminal_dump) {
        run.addArg("--depfile");
        depfile = run.addDepFileOutputArg(b.fmt("{s}.d", .{options.name}));
    }

    switch (options.output) {
        .executable => {},
        .object => run.addArg("-c"),
        .assembly => run.addArg("-S"),
        .shared_library => run.addArg("-shared"),
    }

    for (options.extra_args) |arg| run.addArg(arg);

    run.addFileArg(options.source_file);

    if (terminal_dump) {
        return .{
            .step = run,
            .output_file = run.captureStdOut(.{}),
            .depfile = null,
        };
    }

    run.addArg("-o");
    const out_name = b.fmt("{s}{s}", .{ options.name, extension(options.output, dcc_triple) });
    const out_file = run.addOutputFileArg(out_name);

    return .{
        .step = run,
        .output_file = out_file,
        .depfile = depfile,
    };
}

fn extension(kind: OutputKind, triple: TargetTriple) []const u8 {
    const coff = triple.isCoff();
    return switch (kind) {
        .executable => if (coff) ".exe" else "",
        .object => if (coff) ".obj" else ".o",
        .assembly => ".s",
        .shared_library => if (coff) ".dll" else ".so",
    };
}

fn validate(options: CompileOptions) void {
    if (options.dump) |mode| {
        if (mode.isTerminal() and options.output != .executable) {
            @panic("dcc: -fdump-ast / -fdump-ir terminate the pipeline");
        }
        if (mode == .mir and options.backend == .llvm) {
            @panic("dcc: -fdump-mir requires the em64t backend");
        }
        if (mode == .llvm and options.backend == .em64t) {
            @panic("dcc: -fdump-llvm requires the llvm backend");
        }
    }

    if (options.output == .shared_library and options.pic == null) {
        std.debug.print("dcc: warning: -shared without -fPIC/-fPIE for \"{s}\"\n", .{options.name});
    }
}
