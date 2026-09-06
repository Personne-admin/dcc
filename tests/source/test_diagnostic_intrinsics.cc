import std;

#include "harness.hh"

namespace
{
    struct Compilation
    {
        int status;
        std::string diagnostics;
        std::string ir;
    };

    Compilation compile(std::string_view source, bool stdlib = false)
    {
        auto const dcc = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() / "dcc";
        auto const dir = std::filesystem::temp_directory_path() /
                         std::format("dcc-diagnostic-intrinsics-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(dir);
        auto const src = dir / "main.dc";
        auto const log = dir / "diagnostics";
        auto const ir = dir / "ir";
        { std::ofstream out{src}; out << source; }
        int status = std::system(std::format("'{}' {} -fdump-ir '{}' > '{}' 2> '{}'", dcc.string(),
                                            stdlib ? "-flibdcext" : "", src.string(), ir.string(), log.string()).c_str());
        auto read = [](std::filesystem::path const& path) {
            std::ifstream in{path};
            return std::string{std::istreambuf_iterator<char>{in}, {}};
        };
        auto raw = read(log);
        std::string diagnostics;
        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i] == '\x1b')
            {
                while (i < raw.size() && raw[i] != 'm') ++i;
            }
            else diagnostics += raw[i];
        }
        Compilation result{status, std::move(diagnostics), read(ir)};
        std::filesystem::remove_all(dir);
        return result;
    }

    std::size_t count(std::string const& text, std::string_view needle)
    {
        std::size_t n = 0;
        for (std::size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size()) ++n;
        return n;
    }
}

SECTION("compiler diagnostic intrinsics");

TEST_CASE("warning and note are sema actions with no runtime IR, including CTFE")
{
    auto result = compile(R"(module test;
import core;
[]const char message() { return "constant message {verbatim}"; }
i32 evaluated() { core::compile_note("CTFE note"); return 3; }
public i32 main() {
    core::compile_warning(message());
    core::compile_note("direct note");
    static if evaluated() == 3 {} else { core::compile_error("bad CTFE"); }
    static if false { core::compile_warning("dead"); core::compile_note("dead"); }
    return 0;
}
)");
    CHECK_EQ(result.status, 0);
    CHECK_EQ(count(result.diagnostics, "warning: constant message {verbatim}"), 1u);
    CHECK_EQ(count(result.diagnostics, "note: CTFE note"), 1u);
    CHECK_EQ(count(result.diagnostics, "note: direct note"), 1u);
    CHECK_EQ(count(result.diagnostics, "error:"), 0u);
    CHECK_EQ(count(result.diagnostics, "warning: dead"), 0u);
    CHECK_EQ(count(result.diagnostics, "note: dead"), 0u);
    CHECK_EQ(count(result.ir, "compile_warning"), 0u);
    CHECK_EQ(count(result.ir, "compile_note"), 0u);
}

TEST_CASE("fmt rejects unsupported types with one intentional diagnostic")
{
    auto result = compile(R"(module test;
public import std::fmt;
struct Unsupported { i32 value; }
public void test(const std::fmt::Writer* writer, const std::fmt::Options* options) {
    Unsupported value = { 1 };
    std::fmt::format_value(writer, value, options);
}
)", true);
    CHECK_NE(result.status, 0);
    CHECK_EQ(count(result.diagnostics, "error:"), 1u);
    CHECK_EQ(count(result.diagnostics, "error: type is not formattable: add format(const T*, const Writer*, const Options*)"), 1u);
    CHECK_EQ(count(result.diagnostics, "unknown name"), 0u);
    CHECK_EQ(count(result.diagnostics, "no matching call"), 0u);
}

TEST_CASE("supported fmt specialization still lowers")
{
    auto result = compile(R"(module test;
public import std::fmt;
public void test(const std::fmt::Writer* writer, const std::fmt::Options* options) {
    std::fmt::format_value(writer, 42 as i32, options);
    std::fmt::format_value(writer, true, options);
    std::fmt::format_value(writer, 1.5 as f64, options);
    std::fmt::format_value(writer, "hello" as []const u8, options);
}
)", true);
    CHECK_EQ(result.status, 0);
    CHECK_EQ(count(result.diagnostics, "error:"), 0u);
}
