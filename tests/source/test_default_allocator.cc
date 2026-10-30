import std;
import dcc.cli;
import dcc.session;
import dcc.sm;
import dcc.diag;
import dcc.ast;
import dcc.lex;
import dcc.parser;
import dcc.si;
import dcc.sema;
import dcc.ir;
import dcc.ir.lower;
import dcc.target;

#include "harness.hh"

#include <unistd.h>

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        static inline std::atomic<int> s_counter{0};

        TempDir()
        {
            auto tmp = std::filesystem::temp_directory_path();
            auto dir = tmp / ("dcc_test_allocator_" + std::to_string(::getpid()) + "_" + std::to_string(++s_counter));
            std::filesystem::create_directories(dir);
            path = std::move(dir);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write_file(std::filesystem::path const& relative, std::string const& text) const
        {
            auto full = path / relative;
            auto parent = full.parent_path();
            if (!parent.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
            }

            std::ofstream out(full);
            out << text;
        }

        [[nodiscard]] std::filesystem::path file(std::filesystem::path const& relative) const { return path / relative; }
    };

    void write_module(TempDir const& td, std::filesystem::path const& relative, std::string_view module_name, std::string_view body)
    {
        td.write_file(relative, std::format("module {};\n\n{}\n", module_name, body));
    }

    struct Outcome
    {
        bool ok{};
        std::string diagnostics;
    };

    [[nodiscard]] Outcome compile_entry(std::filesystem::path const& entry, std::vector<std::filesystem::path> import_roots, std::string default_allocator = {})
    {
        std::stringstream diagnostics;
        dcc::session::SessionOptions session_options;
        session_options.diagnostic_stream = &diagnostics;

        dcc::session::CompilerSession session{session_options};
        session.diagnostics().set_color(false);
        dcc::session::CompileOptions options;
        options.import_roots = std::move(import_roots);
        options.default_allocator = std::move(default_allocator);

        auto result = session.analyze_entry(entry, options);
        return {!result.has_errors && result.module != nullptr, diagnostics.str()};
    }

    [[nodiscard]] bool has_text(std::string const& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

    [[nodiscard]] std::size_t count_errors(std::string const& text)
    {
        std::size_t total = 0;
        for (std::size_t pos = 0; (pos = text.find("error:", pos)) != std::string::npos; pos += 6)
            ++total;
        return total;
    }

    constexpr std::string_view container_module = "import core::allocator_config;\n\npublic struct Box(T, A = DefaultAllocator) {\n    A allocator;\n    T value;\n}\n";
    constexpr std::string_view memory_module =
        "public struct MyAlloc {\n}\n\npublic struct BigAlloc {\n    u64 state;\n}\n\npublic struct Pair(T) {\n    T first;\n}\n\nstruct HiddenAlloc {\n}\n";

    void write_selection_project(TempDir const& td)
    {
        write_module(td, "app/memory.dc", "app::memory", memory_module);
        write_module(td, "app/container.dc", "app::container", container_module);
        td.write_file("main.dc", R"(module main;

import core;
import app::container;
import app::memory;
import core::allocator_config;

void takes_my_alloc(app::memory::MyAlloc* alloc) {
}

A make_alloc(A = DefaultAllocator)() {
    A value = {};
    takes_my_alloc(&value);
    return value;
}

i32 main() {
    static if sizeof(app::container::Box(u8)) != 1 {
        core::compile_error("selected allocator did not fold into a zero-size field");
    }
    app::container::Box(u8) box = {};
    app::memory::MyAlloc* pointer = &box.allocator;
    takes_my_alloc(&box.allocator);
    make_alloc();
    return 0;
}
)");
    }
} // namespace

SECTION("Default allocator: option");

TEST_CASE("option accepts a module-qualified type")
{
    std::vector<std::string> args{"dcc", "-fdefault-allocator=app::memory::MyAlloc", "main.dc"};
    auto opts = dcc::cli::parse_args(std::span<std::string const>{args});
    CHECK_EQ(opts.default_allocator, std::string{"app::memory::MyAlloc"});
}

TEST_CASE("option accepts a separate value")
{
    std::vector<std::string> args{"dcc", "-fdefault-allocator", "app::memory::MyAlloc", "main.dc"};
    auto opts = dcc::cli::parse_args(std::span<std::string const>{args});
    CHECK_EQ(opts.default_allocator, std::string{"app::memory::MyAlloc"});
}

TEST_CASE("option rejects an unqualified type name")
{
    std::vector<std::string> args{"dcc", "-fdefault-allocator=MyAlloc", "main.dc"};
    bool rejected = false;
    try
    {
        std::ignore = dcc::cli::parse_args(std::span<std::string const>{args});
    }
    catch (dcc::cli::CliError const& error)
    {
        rejected = true;
        CHECK(has_text(error.message, "MODULE::TYPE"));
    }
    CHECK(rejected);
}

TEST_CASE("option rejects an empty type name")
{
    std::vector<std::string> args{"dcc", "-fdefault-allocator=app::memory::", "main.dc"};
    bool rejected = false;
    try
    {
        std::ignore = dcc::cli::parse_args(std::span<std::string const>{args});
    }
    catch (dcc::cli::CliError const&)
    {
        rejected = true;
    }
    CHECK(rejected);
}

TEST_CASE("defaults to no selection")
{
    std::vector<std::string> args{"dcc", "main.dc"};
    auto opts = dcc::cli::parse_args(std::span<std::string const>{args});
    CHECK(opts.default_allocator.empty());
}

SECTION("Default allocator: selection");

TEST_CASE("selected type reaches the omitted generic argument")
{
    TempDir td;
    write_selection_project(td);
    auto outcome = compile_entry(td.file("main.dc"), {}, "app::memory::MyAlloc");
    CHECK(outcome.ok);
}

TEST_CASE("explicit argument overrides the selection")
{
    TempDir td;
    write_module(td, "app/memory.dc", "app::memory", memory_module);
    write_module(td, "app/container.dc", "app::container", container_module);
    td.write_file("main.dc", R"(module main;

import app::container;
import app::memory;
import core::allocator_config;

struct FixedAlloc {
}

i32 main() {
    app::container::Box(u8, FixedAlloc) box = {};
    FixedAlloc* pointer = &box.allocator;
    return 0;
}
)");

    auto outcome = compile_entry(td.file("main.dc"), {}, "app::memory::MyAlloc");
    CHECK(outcome.ok);
}

TEST_CASE("explicit argument works without a selection")
{
    TempDir td;
    write_module(td, "app/container.dc", "app::container", container_module);
    td.write_file("main.dc", R"(module main;

import app::container;
import core::allocator_config;

struct FixedAlloc {
}

i32 main() {
    app::container::Box(u8, FixedAlloc) box = {};
    FixedAlloc* pointer = &box.allocator;
    return 0;
}
)");

    auto outcome = compile_entry(td.file("main.dc"), {}, {});
    CHECK(outcome.ok);
}

TEST_CASE("selection resolves through an import root")
{
    TempDir td;
    write_module(td, "lib/vendor/memory.dc", "vendor::memory", memory_module);
    write_module(td, "app/container.dc", "app::container", container_module);
    td.write_file("main.dc", R"(module main;

import app::container;
import vendor::memory;
import core::allocator_config;

i32 main() {
    app::container::Box(u8) box = {};
    vendor::memory::MyAlloc* pointer = &box.allocator;
    return 0;
}
)");

    auto outcome = compile_entry(td.file("main.dc"), {td.file("lib")}, "vendor::memory::MyAlloc");
    CHECK(outcome.ok);
}

TEST_CASE("omitted argument without a selection is diagnosed")
{
    TempDir td;
    write_module(td, "app/container.dc", "app::container", container_module);
    td.write_file("main.dc", R"(module main;

import app::container;
import core::allocator_config;

i32 main() {
    app::container::Box(u8) box = {};
    return 0;
}
)");

    auto outcome = compile_entry(td.file("main.dc"), {}, {});
    CHECK(!outcome.ok);
    CHECK(has_text(outcome.diagnostics, "no default allocator selected"));
    CHECK_EQ(count_errors(outcome.diagnostics), 1u);
}

TEST_CASE("unused selection without a use is silent")
{
    TempDir td;
    write_module(td, "app/container.dc", "app::container", container_module);
    td.write_file("main.dc", R"(module main;

import core::allocator_config;

i32 main() {
    return 0;
}
)");

    auto outcome = compile_entry(td.file("main.dc"), {}, {});
    CHECK(outcome.ok);
    CHECK(outcome.diagnostics.empty());
}

SECTION("Default allocator: diagnostics");

TEST_CASE("private selected type is diagnosed")
{
    TempDir td;
    write_selection_project(td);
    auto outcome = compile_entry(td.file("main.dc"), {}, "app::memory::HiddenAlloc");
    CHECK(!outcome.ok);
    CHECK(has_text(outcome.diagnostics, "is not public"));
    CHECK(has_text(outcome.diagnostics, "HiddenAlloc"));
}

TEST_CASE("unknown selected type is diagnosed")
{
    TempDir td;
    write_selection_project(td);
    auto outcome = compile_entry(td.file("main.dc"), {}, "app::memory::Missing");
    CHECK(!outcome.ok);
    CHECK(has_text(outcome.diagnostics, "has no type `Missing`"));
}

TEST_CASE("nonzero-size selected type is diagnosed")
{
    TempDir td;
    write_selection_project(td);
    auto outcome = compile_entry(td.file("main.dc"), {}, "app::memory::BigAlloc");
    CHECK(!outcome.ok);
    CHECK(has_text(outcome.diagnostics, "is not zero-size"));
}

TEST_CASE("generic selected type is diagnosed")
{
    TempDir td;
    write_selection_project(td);
    auto outcome = compile_entry(td.file("main.dc"), {}, "app::memory::Pair");
    CHECK(!outcome.ok);
    CHECK(has_text(outcome.diagnostics, "is generic"));
}

TEST_CASE("missing selected module is diagnosed")
{
    TempDir td;
    write_selection_project(td);
    auto outcome = compile_entry(td.file("main.dc"), {}, "app::absent::MyAlloc");
    CHECK(!outcome.ok);
    CHECK(has_text(outcome.diagnostics, "could not resolve module `app::absent`"));
}

TEST_CASE("selected type from the configuration module is rejected")
{
    TempDir td;
    write_selection_project(td);
    auto outcome = compile_entry(td.file("main.dc"), {}, "core::allocator_config::NoDefaultAllocatorSelected");
    CHECK(!outcome.ok);
    CHECK(has_text(outcome.diagnostics, "must not be declared in `core::allocator_config`"));
}

TEST_CASE("configuration cycle is diagnosed")
{
    TempDir td;
    write_module(td, "app/memory.dc", "app::memory", "import app::holder;\n\npublic struct MyAlloc {\n}\n");
    write_module(td, "app/holder.dc", "app::holder", "import core::allocator_config;\n\npublic struct Holder {\n}\n");
    write_module(td, "app/container.dc", "app::container", container_module);
    td.write_file("main.dc", R"(module main;

import app::container;
import app::memory;
import core::allocator_config;

i32 main() {
    app::container::Box(u8) box = {};
    app::memory::MyAlloc* pointer = &box.allocator;
    return 0;
}
)");

    auto outcome = compile_entry(td.file("main.dc"), {}, "app::memory::MyAlloc");
    CHECK(!outcome.ok);
    CHECK(has_text(outcome.diagnostics, "default allocator configuration cycle"));
    CHECK(has_text(outcome.diagnostics, "app::memory"));
    CHECK(has_text(outcome.diagnostics, "app::holder"));
}
