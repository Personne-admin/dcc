import std;

#include "harness.hh"

#include <stdio.h>
#include <sys/wait.h>

namespace
{
    [[nodiscard]] std::string shell_quote(std::filesystem::path const& p)
    {
        std::string s = p.string();
        if (s.find('\'') != std::string::npos)
            return '"' + s + '"';

        return "'" + s + "'";
    }

    [[nodiscard]] std::filesystem::path dcc_path()
    {
        std::error_code ec;
        auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec)
            return {};

        return std::filesystem::weakly_canonical(exe, ec).parent_path().parent_path() / "dcc";
    }

    struct Build
    {
        int compile_status{-1};
        std::optional<int> run_status;
    };

    [[nodiscard]] Build build_and_run(std::string_view source, bool run)
    {
        auto dcc = dcc_path();
        if (dcc.empty())
            return {};

        auto dir = std::filesystem::temp_directory_path() / "dcc-stdlib-format";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        auto src = dir / "main.dc";
        auto exe = dir / "prog";
        {
            std::ofstream f{src};
            f << source;
        }

        Build out;
        auto compile = std::format("{} -flibdcext -target x86_64-elf -o {} {} 2>&1", shell_quote(dcc), shell_quote(exe), shell_quote(src));
        out.compile_status = std::system(compile.c_str());
        if (run && out.compile_status == 0 && std::filesystem::exists(exe))
        {
            int rc = std::system((shell_quote(exe) + " 2>/dev/null").c_str());
            out.run_status = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        }

        std::filesystem::remove_all(dir, ec);
        return out;
    }

    constexpr std::string_view kSink = R"(
struct Sink { []u8 buffer; usize pos; }

void write(Sink* s, []const u8 bytes) {
    for (usize i = 0; i < bytes.len; i += 1) {
        if s.pos < s.buffer.len {
            s.buffer[s.pos] = bytes[i];
            s.pos += 1;
        }
    }
}

bool same([]const u8 a, []const u8 b) {
    if a.len != b.len { return false; }
    for (usize i = 0; i < a.len; i += 1) {
        if a[i] != b[i] { return false; }
    }
    return true;
}
)";

} // namespace

SECTION("std::print: compile-time formatting");

#if DCC_ENABLE_LLVM
TEST_CASE("print_fmt renders during constant evaluation")
{
    auto source = std::format(R"(module main;
import std::print;
using Arg = std::print::FormatArg;
{}
bool rendered() {{
    u8[64] storage;
    Sink sink = {{ buffer = storage[0..64], pos = 0 }};
    Sink* out = &sink;
    Arg[2] args;
    args[0] = Arg::Str("disk");
    args[1] = Arg::Uint(4096);
    out.print_fmt("{{}} uses {{}} bytes", args[0..2]);
    return same(storage[0..sink.pos], "disk uses 4096 bytes");
}}

const bool FOLDED = rendered();

public i32 main() {{
    static if !FOLDED {{
        constant_evaluation_disagreed();
    }}
    return 0;
}}
)",
                              kSink);

    auto build = build_and_run(source, false);
    CHECK_EQ(build.compile_status, 0);
}

TEST_CASE("the same formatting runs at runtime")
{
    auto source = std::format(R"(module main;
import std::print;
using Arg = std::print::FormatArg;
{}
i32 render() {{
    u8[64] storage;
    Sink sink = {{ buffer = storage[0..64], pos = 0 }};
    Sink* out = &sink;
    Arg[3] args;
    args[0] = Arg::Uint(48879);
    args[1] = Arg::Int(-42);
    args[2] = Arg::Str("end");
    out.println_fmt("{{x}} {{}} {{}}", args[0..3]);
    if !same(storage[0..sink.pos - 1], "beef -42 end") {{ return 1; }}
    if storage[sink.pos - 1] != 10 {{ return 2; }}
    return 0;
}}

public i32 main() {{
    return render();
}}
)",
                              kSink);

    auto build = build_and_run(source, true);
    CHECK_EQ(build.compile_status, 0);
    REQUIRE(build.run_status.has_value());
    CHECK_EQ(*build.run_status, 0);
}

TEST_CASE("variadic pack formatting with static for at compile-time and runtime")
{
    auto source = std::format(R"(module main;
{}
void format_val(W)(W out, []const u8 s) if compiles(W x, []const u8 b) {{ x.write(b); }} {{
    out.write(s);
}}
void format_val(W)(W out, u64 val) if compiles(W x, []const u8 b) {{ x.write(b); }} {{
    u8[20] buf;
    usize pos = 20;
    u64 v = val;
    if v == 0 {{
        pos -= 1;
        buf[pos] = '0' as u8;
    }} else {{
        while v > 0 {{
            pos -= 1;
            buf[pos] = ('0' as u8) + (v % 10) as u8;
            v /= 10;
        }}
    }}
    out.write(buf[pos..20]);
}}
void format_all(W, T...)(W out, []const u8 fmt, T args) if compiles(W x, []const u8 b) {{ x.write(b); }} {{
    usize cursor = 0;
    usize i = 0;
    static for arg in args {{
        i = cursor;
        while i < fmt.len && fmt[i] != ('{{' as u8) {{
            i += 1;
        }}
        if i > cursor {{
            out.write(fmt[cursor..i]);
        }}
        if i < fmt.len && fmt[i] == ('{{' as u8) {{
            while i < fmt.len && fmt[i] != ('}}' as u8) {{
                i += 1;
            }}
            if i < fmt.len {{
                i += 1;
            }}
            cursor = i;
            out.format_val(arg);
        }} else {{
            cursor = i;
        }}
    }}
    if cursor < fmt.len {{
        out.write(fmt[cursor..fmt.len]);
    }}
}}
bool test_ctfe() {{
    u8[64] storage;
    Sink sink = {{ buffer = storage, pos = 0 }};
    Sink* out = &sink;
    []const u8 name = "pack";
    out.format_all("{{}}: count = {{}} items", name, 128 as u64);
    return same(storage[0..sink.pos], "pack: count = 128 items");
}}
const bool OK = test_ctfe();
public i32 main() {{
    static if !OK {{
        return 1;
    }}
    u8[64] storage;
    Sink sink = {{ buffer = storage, pos = 0 }};
    Sink* out = &sink;
    []const u8 name = "runtime";
    out.format_all("{{}}: count = {{}} items", name, 256 as u64);
    if !same(storage[0..sink.pos], "runtime: count = 256 items") {{
        return 2;
    }}
    return 0;
}}
)",
                              kSink);

    auto build = build_and_run(source, true);
    CHECK_EQ(build.compile_status, 0);
    REQUIRE(build.run_status.has_value());
    CHECK_EQ(*build.run_status, 0);
}
#endif
