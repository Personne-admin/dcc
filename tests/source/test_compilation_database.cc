import std;
import dcc.session;
import dcc.target;
import dccd.compilation_database;

#include "harness.hh"

namespace
{
    struct TempDir
    {
        std::filesystem::path path;

        TempDir()
        {
            auto base = std::filesystem::temp_directory_path();
            auto tag = std::format("dcc-compdb-test-{}", std::chrono::steady_clock::now().time_since_epoch().count());
            path = base / tag;
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    void write_file(std::filesystem::path const& path, std::string_view content)
    {
        std::ofstream out{path};
        out << content;
    }

    [[nodiscard]] std::optional<std::string> dump_db(std::string_view json)
    {
        TempDir td;
        auto db_path = td.path / "compile_commands.json";
        write_file(db_path, json);

        std::ostringstream log;
        dccd::CompilationDatabase db;
        if (!db.load(db_path, log))
            return std::nullopt;
        if (db.empty())
            return std::nullopt;

        auto* cmd = db.command_for(std::filesystem::path{"/project/src/main.dc"});
        if (!cmd)
            return std::nullopt;

        auto analysis = dccd::project_analysis_command(*cmd, log);
        if (!analysis)
            return std::nullopt;

        std::string out;
        out += "file=" + cmd->file.string() + "\n";
        for (auto const& r : analysis->import_roots)
            out += "root=" + r.string() + "\n";
        for (auto const& d : analysis->injected_decls)
            out += "decl=" + d + "\n";
        out += "triple=" + (analysis->target ? analysis->target->triple : std::string{"<none>"}) + "\n";
        out += "libdcext=" + std::to_string(analysis->inject_libdcext_prelude ? 1 : 0) + "\n";
        return out;
    }

} // namespace

SECTION("compilation database: parser");

TEST_CASE("basic arguments form")
{
    auto out = dump_db(R"json([
        {
            "directory": "/project",
            "file": "src/main.dc",
            "arguments": [
                "dcc",
                "-target", "x86-elf",
                "-I", "source",
                "-Jusing bool A = true;",
                "src/main.dc"
            ]
        }
    ])json");

    REQUIRE(out.has_value());
    CHECK(out->find("file=/project/src/main.dc") != std::string::npos);
    CHECK(out->find("root=/project/source") != std::string::npos);
    CHECK(out->find("decl=using bool A = true;") != std::string::npos);
    CHECK(out->find("triple=x86-elf") != std::string::npos);
}

TEST_CASE("joined forms")
{
    auto out = dump_db(R"json([
        {
            "directory": "/project",
            "file": "src/main.dc",
            "arguments": [
                "dcc",
                "--target=x86-elf",
                "-Isource",
                "-Jusing bool A = true;",
                "--inject=using u8 B = 2;",
                "-flibdcext",
                "src/main.dc"
            ]
        }
    ])json");

    REQUIRE(out.has_value());
    CHECK(out->find("root=/project/source") != std::string::npos);
    CHECK(out->find("decl=using bool A = true;") != std::string::npos);
    CHECK(out->find("decl=using u8 B = 2;") != std::string::npos);
    CHECK(out->find("triple=x86-elf") != std::string::npos);
    CHECK(out->find("libdcext=1") != std::string::npos);
}

TEST_CASE("command string form with quoted declaration")
{
    auto out = dump_db(R"json([
        {
            "directory": "/project",
            "file": "src/main.dc",
            "command": "dcc -target x86-elf -I source -J\"using bool FEATURE = true;\" src/main.dc"
        }
    ])json");

    REQUIRE(out.has_value());
    CHECK(out->find("file=/project/src/main.dc") != std::string::npos);
    CHECK(out->find("root=/project/source") != std::string::npos);
    CHECK(out->find("decl=using bool FEATURE = true;") != std::string::npos);
    CHECK(out->find("triple=x86-elf") != std::string::npos);
}

TEST_CASE("relative paths resolve against directory")
{
    TempDir td;
    auto db_path = td.path / "compile_commands.json";
    {
        std::ofstream out{db_path};
        out << "[{"
            << "\"directory\":\"" << td.path.string() << "\","
            << "\"file\":\"src/main.dc\","
            << "\"arguments\":[\"dcc\",\"-I\",\"include\",\"src/main.dc\"],"
            << "\"output\":\"main.o\""
            << "}]";
    }

    std::ostringstream log;
    dccd::CompilationDatabase db;
    REQUIRE(db.load(db_path, log));

    auto* cmd = db.command_for(td.path / "src" / "main.dc");
    REQUIRE(cmd != nullptr);

    CHECK(cmd->file.is_absolute());
    CHECK(cmd->output.has_value());
    CHECK_EQ(cmd->output->string(), (td.path / "main.o").string());

    auto analysis = dccd::project_analysis_command(*cmd, log);
    REQUIRE(analysis.has_value());
    REQUIRE(analysis->import_roots.size() == 1);
    CHECK_EQ(analysis->import_roots[0].string(), (td.path / "include").string());
}

TEST_CASE("duplicate commands select the first deterministically")
{
    TempDir td;
    auto db_path = td.path / "compile_commands.json";
    write_file(db_path, R"json([
        {
            "directory": "/p",
            "file": "a.dc",
            "arguments": ["dcc", "-Jusing bool FIRST = true;", "a.dc"]
        },
        {
            "directory": "/p",
            "file": "a.dc",
            "arguments": ["dcc", "-Jusing bool SECOND = true;", "a.dc"]
        }
    ])json");

    std::ostringstream log;
    dccd::CompilationDatabase db;
    REQUIRE(db.load(db_path, log));
    CHECK_EQ(db.size(), 2u);

    auto* cmd = db.command_for("/p/a.dc");
    REQUIRE(cmd != nullptr);

    auto analysis = dccd::project_analysis_command(*cmd, log);
    REQUIRE(analysis.has_value());
    REQUIRE(analysis->injected_decls.size() == 1);
    CHECK_EQ(analysis->injected_decls[0], "using bool FIRST = true;");
}

TEST_CASE("malformed entries are skipped, invalid JSON is rejected")
{
    {
        TempDir td;
        auto db_path = td.path / "compile_commands.json";
        write_file(db_path, R"json(not-an-array-or-object)json");

        std::ostringstream log;
        dccd::CompilationDatabase db;
        CHECK(!db.load(db_path, log));
        CHECK(db.empty());
    }

    {
        TempDir td;
        auto db_path = td.path / "compile_commands.json";
        write_file(db_path, R"json([
            { "directory": "/p", "file": "ok.dc", "arguments": ["dcc", "ok.dc"] },
            { "directory": "/p", "arguments": ["dcc", "no-file.dc"] },
            { "file": "no-dir.dc", "arguments": ["dcc", "no-dir.dc"] },
            { "directory": "/p", "file": "no-args.dc" },
            { "directory": "/p", "file": "bad-arg.dc", "arguments": ["dcc", 42] },
            { "directory": "/p", "file": "bad-cmd.dc", "command": "dcc \"unterminated" }
        ])json");

        std::ostringstream log;
        dccd::CompilationDatabase db;
        REQUIRE(db.load(db_path, log));
        CHECK_EQ(db.size(), 1u);

        auto* cmd = db.command_for("/p/ok.dc");
        REQUIRE(cmd != nullptr);
    }
}

TEST_CASE("backend and output operands are ignored in the projection")
{
    TempDir td;
    auto db_path = td.path / "compile_commands.json";
    write_file(db_path, R"json([
        {
            "directory": "/p",
            "file": "main.dc",
            "arguments": [
                "dcc",
                "-target", "x86-elf",
                "-O0",
                "-fbounds-check",
                "-mcmodel", "small",
                "-fbackend", "llvm",
                "-farch", "pentium",
                "-c",
                "main.dc",
                "-o",
                "main.o"
            ]
        }
    ])json");

    std::ostringstream log;
    dccd::CompilationDatabase db;
    REQUIRE(db.load(db_path, log));

    auto* cmd = db.command_for("/p/main.dc");
    REQUIRE(cmd != nullptr);

    auto analysis = dccd::project_analysis_command(*cmd, log);
    REQUIRE(analysis.has_value());
    CHECK(analysis->target.has_value());
    CHECK_EQ(analysis->target->triple, "x86-elf");
    CHECK(analysis->import_roots.empty());
    CHECK(analysis->injected_decls.empty());
}

TEST_CASE("x86-elf resolves to 32-bit x86 and accepts control registers")
{
    std::vector<std::vector<std::string>> spellings = {
        {"dcc", "-target", "x86-elf", "-c", "main.dc", "-o", "main.o"},
        {"dcc", "--target", "x86-elf", "-c", "main.dc", "-o", "main.o"},
        {"dcc", "-target=x86-elf", "-c", "main.dc", "-o", "main.o"},
        {"dcc", "--target=x86-elf", "-c", "main.dc", "-o", "main.o"},
    };

    std::optional<dcc::target::TargetConfig> resolved;
    for (auto const& argv : spellings)
    {
        TempDir td;
        std::string args;
        for (auto const& arg : argv)
        {
            if (!args.empty())
                args += ",";
            args += "\"" + arg + "\"";
        }
        write_file(td.path / "compile_commands.json", "[{\"directory\":\"" + td.path.string() + "\",\"file\":\"main.dc\",\"arguments\":[" + args + "]}]");

        std::ostringstream log;
        dccd::CompilationDatabase db;
        REQUIRE(db.load(td.path / "compile_commands.json", log));
        auto* cmd = db.command_for(td.path / "main.dc");
        REQUIRE(cmd != nullptr);
        auto analysis = dccd::project_analysis_command(*cmd, log);
        REQUIRE(analysis.has_value());
        REQUIRE(analysis->target.has_value());
        CHECK(analysis->target->arch == dcc::target::Arch::X86);
        CHECK(analysis->target->arch != dcc::target::Arch::X86_64);
        CHECK_EQ(analysis->target->triple, "x86-elf");
        CHECK(analysis->target->pointer_bits == 32);
        resolved = *analysis->target;
    }

    REQUIRE(resolved.has_value());
    TempDir td;
    write_file(td.path / "main.dc", "module main;\n"
                                    "usize read_cr3() {\n"
                                    "    usize v = asm @[output(usize in eax)] { \"mov %%cr3, %0\" };\n"
                                    "    return v;\n"
                                    "}\n"
                                    "void write_cr3(usize v) {\n"
                                    "    asm @[inputs(v in eax = v)] { \"mov %0, %%cr3\" };\n"
                                    "}\n");

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    dcc::session::CompileOptions copts;
    copts.target = *resolved;
    copts.import_roots.push_back(td.path);
    auto result = session.analyze_entry(td.path / "main.dc", copts);
    REQUIRE(result.module != nullptr);
    CHECK(!result.has_errors);
    CHECK(!session.diagnostics().has_errors());
}

TEST_CASE("libdcext with separate os argument")
{
    auto out = dump_db(R"json([
        {
            "directory": "/project",
            "file": "src/main.dc",
            "arguments": [
                "dcc",
                "-flibdcext", "windows",
                "src/main.dc"
            ]
        }
    ])json");

    REQUIRE(out.has_value());
    CHECK(out->find("libdcext=1") != std::string::npos);
    CHECK(out->find("triple=x86_64-coff") != std::string::npos);
}

TEST_CASE("libdcext flag matching is exact")
{
    auto out = dump_db(R"json([
        {
            "directory": "/project",
            "file": "src/main.dc",
            "arguments": [
                "dcc",
                "-flibdcextra",
                "src/main.dc"
            ]
        }
    ])json");

    REQUIRE(out.has_value());
    CHECK(out->find("libdcext=0") != std::string::npos);

    auto negated = dump_db(R"json([
        {
            "directory": "/project",
            "file": "src/main.dc",
            "arguments": [
                "dcc",
                "-fno-libdcext",
                "src/main.dc"
            ]
        }
    ])json");

    REQUIRE(negated.has_value());
    CHECK(negated->find("libdcext=0") != std::string::npos);
}
