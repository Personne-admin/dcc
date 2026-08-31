const std = @import("std");

const Build = std.Build;
const Step = Build.Step;
const LazyPath = Build.LazyPath;
const GeneratedFile = Build.GeneratedFile;

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

/// What should the compiler produce?
pub const OutputKind = enum {
    executable,
    object,
    assembly,
    shared_library,
};

/// Terminal dumps terminate the pipeline and write their result to stdout.
/// They do not produce filesystem artifacts and therefore cannot use --depfile.
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
            if (arch == .x86_64 and (os == .linux or os == .freestanding))
                break :match .@"x86_64-elf";

            if (arch == .x86 and (os == .linux or os == .freestanding))
                break :match .@"x86-elf";

            if (arch == .x86_64 and os == .windows)
                break :match .@"x86_64-coff";

            if (arch == .x86 and os == .windows)
                break :match .@"x86-coff";

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
    source_file: LazyPath,
    target: Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,

    /// Defaults to `OptLevel.fromZigOptimize(optimize)`.
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

    include_dirs: []const LazyPath = &.{},
    injected_decls: []const []const u8 = &.{},

    track_dependencies: bool = true,

    /// Raw arguments appended after SDK-managed options and before the source
    /// file. These must not duplicate source/output arguments.
    extra_args: []const []const u8 = &.{},
};

/// A symbolic command-line argument.
///
/// Keeping paths as LazyPath values is important: generated paths do not exist
/// during build-graph construction and must only be resolved during the make
/// phase.
pub const CommandArg = union(enum) {
    literal: []const u8,
    file: LazyPath,
    directory: LazyPath,

    fn addStepDependencies(arg: CommandArg, step: *Step) void {
        switch (arg) {
            .literal => {},
            .file => |path| path.addStepDependencies(step),
            .directory => |path| path.addStepDependencies(step),
        }
    }

    fn addToRun(arg: CommandArg, run: *Step.Run) void {
        switch (arg) {
            .literal => |value| run.addArg(value),
            .file => |path| run.addFileArg(path),
            .directory => |path| run.addDirectoryArg(path),
        }
    }
};

/// The semantic DCC command represented without eagerly resolving LazyPaths.
///
/// `arguments` excludes:
/// - argv[0] (`dcc_exe`)
/// - the source file
/// - Zig-only `--depfile` bookkeeping
/// - the output path
///
/// The compilation database writer reconstructs a normal compile command as:
///
///   dcc_exe + arguments + file + -o + output
pub const CompileCommand = struct {
    dcc_exe: []const u8,
    directory: LazyPath,
    file: LazyPath,
    arguments: []const CommandArg,
    output: []const u8,
};

pub const DccArtifact = struct {
    step: *Step.Run,
    output_file: LazyPath,
    depfile: ?LazyPath,

    /// Null for terminal stdout-only dump invocations.
    compile_command: ?CompileCommand,
};

/// Collector for standard JSON Compilation Database output.
///
/// Usage:
///
///     const compdb = dcc.sdk.CompilationDatabase.init(b);
///
///     const artifact = dcc.sdk.compile(b, options);
///     compdb.add(artifact);
///
///     const compile_commands = compdb.write();
///
/// The writer is a real build step. It resolves LazyPaths during the make
/// phase, so generated source/include paths are supported correctly.
pub const CompilationDatabase = struct {
    step: Step,
    commands: std.ArrayList(CompileCommand),
    generated_file: GeneratedFile,

    pub fn init(b: *Build) *CompilationDatabase {
        const self = b.allocator.create(CompilationDatabase) catch @panic("OOM");

        self.* = .{
            .step = Step.init(.{
                .id = .custom,
                .name = "dcc compilation database",
                .owner = b,
                .makeFn = make,
            }),
            .commands = .empty,
            .generated_file = undefined,
        };

        self.generated_file = .{
            .step = &self.step,
        };

        return self;
    }

    pub fn add(self: *CompilationDatabase, artifact: DccArtifact) void {
        const command = artifact.compile_command orelse return;
        const b = self.step.owner;

        self.commands.append(
            b.allocator,
            dupeCompileCommand(b, command),
        ) catch @panic("OOM");

        command.directory.addStepDependencies(&self.step);
        command.file.addStepDependencies(&self.step);

        for (command.arguments) |arg| {
            arg.addStepDependencies(&self.step);
        }
    }

    /// Returns the generated `compile_commands.json`.
    ///
    /// Calling this does not make it part of a top-level build step by itself;
    /// the caller should install/copy it or depend on the returned LazyPath.
    pub fn write(self: *CompilationDatabase) LazyPath {
        return .{
            .generated = .{
                .file = &self.generated_file,
            },
        };
    }

    const JsonCompileCommand = struct {
        directory: []const u8,
        file: []const u8,
        arguments: []const []const u8,
        output: []const u8,
    };

    fn make(step: *Step, options: Step.MakeOptions) !void {
        _ = options;

        const self: *CompilationDatabase = @fieldParentPtr("step", step);
        const b = step.owner;
        const io = b.graph.io;
        const arena = b.allocator;

        step.clearWatchInputs();

        var json_commands: std.ArrayList(JsonCompileCommand) = .empty;

        for (self.commands.items) |command| {
            const directory = resolveLazyPath(b, step, command.directory);
            const file = resolveLazyPath(b, step, command.file);

            var argv: std.ArrayList([]const u8) = .empty;

            argv.append(arena, command.dcc_exe) catch @panic("OOM");

            for (command.arguments) |arg| {
                const value = switch (arg) {
                    .literal => |literal| literal,
                    .file => |path| resolveLazyPath(b, step, path),
                    .directory => |path| resolveLazyPath(b, step, path),
                };

                argv.append(arena, value) catch @panic("OOM");
            }

            argv.append(arena, file) catch @panic("OOM");
            argv.append(arena, "-o") catch @panic("OOM");
            argv.append(arena, command.output) catch @panic("OOM");

            json_commands.append(arena, .{
                .directory = directory,
                .file = file,
                .arguments = argv.toOwnedSlice(arena) catch @panic("OOM"),
                .output = command.output,
            }) catch @panic("OOM");
        }

        const json = std.json.Stringify.valueAlloc(
            arena,
            json_commands.items,
            .{
                .whitespace = .indent_2,
            },
        ) catch |err| {
            return step.fail(
                "failed to serialize DCC compilation database: {s}",
                .{@errorName(err)},
            );
        };

        // Use Zig's local build cache as the canonical location for the
        // generated file. The JSON bytes completely determine the artifact.
        var man = b.graph.cache.obtain();
        defer man.deinit();

        man.hash.addBytes("dcc-compilation-database-v1");
        man.hash.addBytes(json);

        if (try step.cacheHit(&man)) {
            const digest = man.final();

            self.generated_file.path = try b.cache_root.join(
                arena,
                &.{ "o", &digest, "compile_commands.json" },
            );

            step.result_cached = true;
            return;
        }

        const digest = man.final();
        const cache_path = "o" ++ std.fs.path.sep_str ++ digest;

        self.generated_file.path = try b.cache_root.join(
            arena,
            &.{ "o", &digest, "compile_commands.json" },
        );

        var cache_dir = b.cache_root.handle.createDirPathOpen(
            io,
            cache_path,
            .{},
        ) catch |err| {
            return step.fail(
                "unable to create compilation database cache directory '{f}{s}': {s}",
                .{
                    b.cache_root,
                    cache_path,
                    @errorName(err),
                },
            );
        };
        defer cache_dir.close(io);

        cache_dir.writeFile(io, .{
            .sub_path = "compile_commands.json",
            .data = json,
        }) catch |err| {
            return step.fail(
                "unable to write compile_commands.json: {s}",
                .{@errorName(err)},
            );
        };

        try step.writeManifest(&man);
    }
};

pub fn compile(b: *Build, options: CompileOptions) DccArtifact {
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

    const arguments = buildArguments(
        b,
        options,
        dcc_triple,
    );

    const terminal_dump = if (options.dump) |mode|
        mode.isTerminal()
    else
        false;

    const run = b.addSystemCommand(&.{options.dcc_exe});
    run.step.name = b.fmt("dcc {s}", .{options.name});
    run.has_side_effects = false;
    run.setCwd(b.path("."));

    addArgumentsToRun(run, arguments);

    var depfile: ?LazyPath = null;

    if (options.track_dependencies and !terminal_dump) {
        run.addArg("--depfile");
        depfile = run.addDepFileOutputArg(
            b.fmt("{s}.d", .{options.name}),
        );
    }

    run.addFileArg(options.source_file);

    if (terminal_dump) {
        return .{
            .step = run,
            .output_file = run.captureStdOut(.{}),
            .depfile = null,
            .compile_command = null,
        };
    }

    run.addArg("-o");

    const out_name = b.fmt(
        "{s}{s}",
        .{
            options.name,
            extension(options.output, dcc_triple),
        },
    );

    const out_file = run.addOutputFileArg(out_name);

    return .{
        .step = run,
        .output_file = out_file,
        .depfile = depfile,
        .compile_command = makeCompileCommand(
            b,
            options,
            arguments,
            out_name,
        ),
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
        std.debug.print(
            "dcc: warning: -shared without -fPIC/-fPIE for \"{s}\"\n",
            .{options.name},
        );
    }
}

/// Builds the SDK-managed portion of the DCC command line exactly once.
///
/// Both the real Step.Run and compile_commands.json consume this same symbolic
/// representation, preventing flag serialization from drifting.
fn buildArguments(
    b: *Build,
    options: CompileOptions,
    dcc_triple: TargetTriple,
) []const CommandArg {
    var args: std.ArrayList(CommandArg) = .empty;

    addLiteral(b, &args, "-target");
    addLiteral(b, &args, @tagName(dcc_triple));

    if (options.backend) |backend| {
        addLiteral(b, &args, "-fbackend");
        addLiteral(b, &args, @tagName(backend));
    }

    if (options.arch) |cpu| {
        addLiteral(b, &args, "-farch");
        addLiteral(b, &args, cpu);
    }

    const opt = options.opt_level orelse
        OptLevel.fromZigOptimize(options.optimize);

    addLiteral(
        b,
        &args,
        b.fmt("-{s}", .{@tagName(opt)}),
    );

    const is_debug = options.optimize == .Debug;

    const do_bounds_check = options.bounds_check orelse
        (is_debug or options.optimize == .ReleaseSafe);

    const do_emit_debug = options.emit_debug_info orelse
        is_debug;

    const do_omit_fp = options.omit_frame_pointer orelse
        !is_debug;

    if (do_bounds_check) {
        addLiteral(b, &args, "-fbounds-check");
    }

    addLiteral(
        b,
        &args,
        if (do_omit_fp)
            "-fomit-frame-pointer"
        else
            "-fno-omit-frame-pointer",
    );

    if (do_emit_debug) {
        if (options.debug_format) |fmt| {
            addLiteral(
                b,
                &args,
                switch (fmt) {
                    .none => "-gnone",
                    .auto => "-g3",
                    .dwarf => "-gdwarf",
                    .pdb => "-gpdb",
                },
            );
        } else {
            addLiteral(b, &args, "-g3");
        }
    } else {
        addLiteral(b, &args, "-g0");
    }

    if (options.libdcext) {
        addLiteral(b, &args, "-flibdcext");
    }

    if (options.pic) |mode| {
        addLiteral(
            b,
            &args,
            switch (mode) {
                .pic => "-fPIC",
                .pie => "-fPIE",
            },
        );
    }

    if (options.no_red_zone) {
        addLiteral(b, &args, "-fno-red-zone");
    }

    if (options.no_simd) {
        addLiteral(b, &args, "-fno-simd");
    }

    if (options.no_x87) {
        addLiteral(b, &args, "-fno-x87");
    }

    if (options.no_stack_protector) {
        addLiteral(b, &args, "-fno-stack-protector");
    }

    if (options.no_stack_probe) {
        addLiteral(b, &args, "-fno-stack-probe");
    }

    if (options.code_model) |model| {
        addLiteral(b, &args, "-mcmodel");
        addLiteral(b, &args, @tagName(model));
    }

    for (options.include_dirs) |dir| {
        addLiteral(b, &args, "-I");
        addDirectory(b, &args, dir);
    }

    for (options.injected_decls) |decl| {
        addLiteral(
            b,
            &args,
            b.fmt("-J{s}", .{decl}),
        );
    }

    if (options.dump) |mode| {
        addLiteral(b, &args, mode.flag());
    }

    switch (options.output) {
        .executable => {},
        .object => addLiteral(b, &args, "-c"),
        .assembly => addLiteral(b, &args, "-S"),
        .shared_library => addLiteral(b, &args, "-shared"),
    }

    for (options.extra_args) |arg| {
        addLiteral(b, &args, arg);
    }

    return args.toOwnedSlice(b.allocator) catch @panic("OOM");
}

fn addArgumentsToRun(
    run: *Step.Run,
    arguments: []const CommandArg,
) void {
    for (arguments) |arg| {
        arg.addToRun(run);
    }
}

fn makeCompileCommand(
    b: *Build,
    options: CompileOptions,
    arguments: []const CommandArg,
    output: []const u8,
) CompileCommand {
    return .{
        .dcc_exe = b.dupe(options.dcc_exe),
        .directory = b.path("."),
        .file = options.source_file.dupe(b),
        .arguments = dupeCommandArgs(b, arguments),
        .output = b.dupe(output),
    };
}

fn dupeCompileCommand(
    b: *Build,
    command: CompileCommand,
) CompileCommand {
    return .{
        .dcc_exe = b.dupe(command.dcc_exe),
        .directory = command.directory.dupe(b),
        .file = command.file.dupe(b),
        .arguments = dupeCommandArgs(b, command.arguments),
        .output = b.dupe(command.output),
    };
}

fn dupeCommandArgs(
    b: *Build,
    arguments: []const CommandArg,
) []const CommandArg {
    const result = b.allocator.alloc(
        CommandArg,
        arguments.len,
    ) catch @panic("OOM");

    for (arguments, result) |arg, *out| {
        out.* = switch (arg) {
            .literal => |value| .{
                .literal = b.dupe(value),
            },
            .file => |path| .{
                .file = path.dupe(b),
            },
            .directory => |path| .{
                .directory = path.dupe(b),
            },
        };
    }

    return result;
}

fn addLiteral(
    b: *Build,
    args: *std.ArrayList(CommandArg),
    value: []const u8,
) void {
    args.append(
        b.allocator,
        .{
            .literal = b.dupe(value),
        },
    ) catch @panic("OOM");
}

fn addDirectory(
    b: *Build,
    args: *std.ArrayList(CommandArg),
    path: LazyPath,
) void {
    args.append(
        b.allocator,
        .{
            .directory = path.dupe(b),
        },
    ) catch @panic("OOM");
}

/// Resolves a LazyPath during the make phase.
///
/// This mirrors LazyPath.getPath2(), but uses the non-deprecated getPath3()
/// API explicitly.
fn resolveLazyPath(
    b: *Build,
    step: *Step,
    path: LazyPath,
) []const u8 {
    const resolved = path.getPath3(b, step);

    return b.pathResolve(
        &.{
            resolved.root_dir.path orelse ".",
            resolved.sub_path,
        },
    );
}
