import std;
import dccd.format;
import dccd.protocol;
import dcc.sm;
import dcc.si;

#include "harness.hh"

namespace
{
    [[nodiscard]] std::optional<std::string> format_source_with(std::string_view src, std::uint32_t tab_size, bool insert_spaces)
    {
        dcc::sm::SourceManager sm;
        auto const fid = sm.add_synthetic("test_format.dc", std::string{src});
        auto const* sf = sm.get(fid);
        if (!sf)
            return std::nullopt;

        dcc::si::string_interner interner;
        dccd::protocol::FormattingOptions opts;
        opts.tabSize = tab_size;
        opts.insertSpaces = insert_spaces;

        auto edit = dccd::format::format_document(*sf, interner, opts);
        if (!edit)
            return std::nullopt;

        return edit->newText;
    }

    [[nodiscard]] std::optional<std::string> format_source(std::string_view src)
    {
        return format_source_with(src, 4, true);
    }

    struct FormatResult
    {
        std::optional<std::string> text;
        dccd::format::FormatAnalysisStats stats;
    };

    [[nodiscard]] FormatResult format_source_with_stats(std::string_view src)
    {
        dcc::sm::SourceManager sm;
        auto const fid = sm.add_synthetic("test_format.dc", std::string{src});
        auto const* sf = sm.get(fid);
        if (!sf)
            return {};

        dcc::si::string_interner interner;
        dccd::protocol::FormattingOptions opts;
        opts.tabSize = 4;
        opts.insertSpaces = true;

        dccd::format::FormatAnalysisStats stats;
        auto edit = dccd::format::format_document(*sf, interner, opts, stats);
        if (!edit)
            return {std::nullopt, stats};

        return {edit->newText, stats};
    }

    [[nodiscard]] std::size_t analysis_budget(dccd::format::FormatAnalysisStats const& s)
    {
        return s.range_scan_steps + s.delimiter_match_lookups + s.enum_region_lookups;
    }

    void check_format(std::string_view input, std::string_view expected)
    {
        auto out = format_source(input);
        REQUIRE(out.has_value());
        CHECK_EQ(*out, expected);

        auto again = format_source(*out);
        REQUIRE(again.has_value());
        CHECK_EQ(*again, *out);
    }

    void check_format_with(std::string_view input, std::string_view expected, std::uint32_t tab_size, bool insert_spaces)
    {
        auto out = format_source_with(input, tab_size, insert_spaces);
        REQUIRE(out.has_value());
        CHECK_EQ(*out, expected);

        auto again = format_source_with(*out, tab_size, insert_spaces);
        REQUIRE(again.has_value());
        CHECK_EQ(*again, *out);
    }

    struct RangeFormatResult
    {
        std::vector<dccd::protocol::TextEdit> edits;
        dccd::format::FormatAnalysisStats stats;
    };

    [[nodiscard]] RangeFormatResult format_range_with(std::string_view src, dccd::protocol::LspRange range, dcc::sm::PositionEncoding enc,
                                                      dccd::protocol::FormattingOptions const& opts = {})
    {
        dcc::sm::SourceManager sm;
        auto const fid = sm.add_synthetic("test_format.dc", std::string{src});
        auto const* sf = sm.get(fid);
        if (!sf)
            return {};

        dcc::si::string_interner interner;
        dccd::format::FormatAnalysisStats stats;
        auto edits = dccd::format::format_range(*sf, interner, opts, range, stats, enc);
        return {std::move(edits), stats};
    }

    [[nodiscard]] dccd::protocol::LspRange make_range(std::uint32_t sl, std::uint32_t sc, std::uint32_t el, std::uint32_t ec)
    {
        dccd::protocol::LspRange r;
        r.start.line = sl;
        r.start.character = sc;
        r.end.line = el;
        r.end.character = ec;
        return r;
    }

    [[nodiscard]] std::vector<dccd::protocol::TextEdit> format_on_type_with(std::string_view src, std::string_view trigger, dccd::protocol::LspPosition pos,
                                                                            dcc::sm::PositionEncoding enc, dccd::protocol::FormattingOptions const& opts = {})
    {
        dcc::sm::SourceManager sm;
        auto const fid = sm.add_synthetic("test_format.dc", std::string{src});
        auto const* sf = sm.get(fid);
        if (!sf)
            return {};

        dcc::si::string_interner interner;
        dccd::format::FormatAnalysisStats stats;
        return dccd::format::format_on_type(*sf, interner, opts, trigger, pos, stats, enc);
    }

    [[nodiscard]] dccd::protocol::LspPosition make_pos(std::uint32_t line, std::uint32_t character)
    {
        dccd::protocol::LspPosition p;
        p.line = line;
        p.character = character;
        return p;
    }

} // namespace

SECTION("format: regressions");

TEST_CASE("multiline call keeps grouped first arg stable")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    foo((a + b) * 2,\n"
                 "        bar);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        (a + b) * 2,\n"
                 "        bar\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("lambda keeps compact leading pipe and spaces binary or")
{
    check_format("module m;\n"
                 "i32 a = |x -> x | 1;\n"
                 "i32 b = |x, y -> x | y;\n",
                 "module m;\n"
                 "i32 a = |x -> x | 1;\n"
                 "i32 b = |x, y -> x | y;\n");
}

TEST_CASE("lambda formats spaced pipes into compact form")
{
    check_format("module m;\n"
                 "i32 a = | x -> x;\n"
                 "i32 b = | i32 n -> n * 2;\n",
                 "module m;\n"
                 "i32 a = |x -> x;\n"
                 "i32 b = |i32 n -> n * 2;\n");
}

TEST_CASE("lambda no-param and nested forms stay stable")
{
    check_format("module m;\n"
                 "i32 a = | -> 42;\n"
                 "i32 b = |x -> |y -> x + y;\n",
                 "module m;\n"
                 "i32 a = | -> 42;\n"
                 "i32 b = |x -> |y -> x + y;\n");
}

TEST_CASE("lambda in call arguments keeps compact pipe")
{
    check_format("module m;\n"
                 "void apply(F)(F f) {\n"
                 "    f(1);\n"
                 "}\n"
                 "void g() {\n"
                 "    apply(|v -> v * 2);\n"
                 "    apply(|a, b -> a | b);\n"
                 "}\n",
                 "module m;\n"
                 "void apply(F)(F f) {\n"
                 "    f(1);\n"
                 "}\n"
                 "void g() {\n"
                 "    apply(|v -> v * 2);\n"
                 "    apply(|a, b -> a | b);\n"
                 "}\n");
}

TEST_CASE("unary bang keeps separating whitespace in conditions")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    if!ready {\n"
                 "        run();\n"
                 "    }\n"
                 "    if!(a && !b) {\n"
                 "        run();\n"
                 "    }\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    if !ready {\n"
                 "        run();\n"
                 "    }\n"
                 "    if !(a && !b) {\n"
                 "        run();\n"
                 "    }\n"
                 "}\n");
}

TEST_CASE("wrapped outer call keeps fitting nested call on one line")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = outer_process(inner_compute(alpha_value, beta_value, gamma_value),\n"
                 "        delta_value);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = outer_process(\n"
                 "        inner_compute(alpha_value, beta_value, gamma_value),\n"
                 "        delta_value\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("fitting one line nested call stays one line")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = outer_process(inner_compute(a, b), c);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = outer_process(inner_compute(a, b), c);\n"
                 "}\n");
}

TEST_CASE("long nested calls wrap structurally and indent by nesting")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = outer_process(inner_compute(alpha_value, beta_value, gamma_value, epsilon_value, zeta_value, eta_value), delta_value);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = outer_process(\n"
                 "        inner_compute(\n"
                 "            alpha_value,\n"
                 "            beta_value,\n"
                 "            gamma_value,\n"
                 "            epsilon_value,\n"
                 "            zeta_value,\n"
                 "            eta_value\n"
                 "        ),\n"
                 "        delta_value\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("mixed grouped binary expressions stay compact when they fit")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = ((a + b) * (c - d)) + e;\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = ((a + b) * (c - d)) + e;\n"
                 "}\n");
}

TEST_CASE("template bang member access strings and pointers stay stable")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 a = max!i32(1, 2);\n"
                 "    obj.field.method(1, 2);\n"
                 "    const char* s = \"hello world\";\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 a = max!i32(1, 2);\n"
                 "    obj.field.method(1, 2);\n"
                 "    const char* s = \"hello world\";\n"
                 "}\n");
}

TEST_CASE("leading comments are preserved")
{
    check_format("module m;\n"
                 "// keep comment positioning untouched\n"
                 "void f() {}\n",
                 "module m;\n"
                 "// keep comment positioning untouched\n"
                 "void f() {}\n");
}

TEST_CASE("doc comments are preserved")
{
    check_format("module m;\n"
                 "/// doc comment\n"
                 "void documented() {}\n",
                 "module m;\n"
                 "/// doc comment\n"
                 "void documented() {}\n");
}

TEST_CASE("trailing comments after arguments stay in place")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    foo(alpha, // first arg\n"
                 "        beta, /* second */\n"
                 "        gamma);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        alpha, // first arg\n"
                 "        beta, /* second */\n"
                 "        gamma\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("comment between arguments keeps a flat call flat")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    foo(a /* between args */, b);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    foo(a /* between args */, b);\n"
                 "}\n");
}

TEST_CASE("comment between operator operands is preserved")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = a + /* plus */ b;\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = a + /* plus */ b;\n"
                 "}\n");
}

TEST_CASE("block comment before a value stays inline")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = /* note */ compute();\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = /* note */ compute();\n"
                 "}\n");
}

TEST_CASE("comment inside a parenthesized expression is preserved")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = (a + /* c */ b) * 2;\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = (a + /* c */ b) * 2;\n"
                 "}\n");
}

TEST_CASE("comments around braces are preserved")
{
    check_format("module m;\n"
                 "void f() { // open\n"
                 "    run(); // body\n"
                 "} // close\n",
                 "module m;\n"
                 "void f() { // open\n"
                 "    run(); // body\n"
                 "} // close\n");
}

TEST_CASE("inline comment in an empty body keeps spacing")
{
    check_format("module m;\n"
                 "void f() { /* inline body */ }\n",
                 "module m;\n"
                 "void f() { /* inline body */ }\n");
}

TEST_CASE("comment inside a compact struct literal keeps spacing")
{
    check_format("module m;\n"
                 "struct S { i32 x; }\n"
                 "void f() {\n"
                 "    S s = { /* empty-ish */ x = 1 };\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct S {\n"
                 "    i32 x;\n"
                 "}\n"
                 "void f() {\n"
                 "    S s = { /* empty-ish */ x = 1 };\n"
                 "}\n");
}

TEST_CASE("blank lines adjacent to comments are preserved")
{
    check_format("module m;\n"
                 "struct S {\n"
                 "    i32 x;\n"
                 "\n"
                 "    // doc for y\n"
                 "    i32 y;\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct S {\n"
                 "    i32 x;\n"
                 "\n"
                 "    // doc for y\n"
                 "    i32 y;\n"
                 "}\n");
}

TEST_CASE("comment between top-level declarations is preserved")
{
    check_format("module m;\n"
                 "struct A { i32 x; }\n"
                 "// separator\n"
                 "struct B { i32 y; }\n",
                 "module m;\n"
                 "\n"
                 "struct A {\n"
                 "    i32 x;\n"
                 "}\n"
                 "// separator\n"
                 "struct B {\n"
                 "    i32 y;\n"
                 "}\n");
}

TEST_CASE("trailing comment after a struct field is preserved")
{
    check_format("module m;\n"
                 "struct S {\n"
                 "    i32 x; // trailing field comment\n"
                 "    i32 y;\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct S {\n"
                 "    i32 x; // trailing field comment\n"
                 "    i32 y;\n"
                 "}\n");
}

TEST_CASE("leading comment before a wrapped argument is preserved")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        // explain a\n"
                 "        a,\n"
                 "        b\n"
                 "    );\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        // explain a\n"
                 "        a,\n"
                 "        b\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("comment before the closing paren of a wrapped call is preserved")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        a,\n"
                 "        // explain last\n"
                 "        b\n"
                 "    );\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        a,\n"
                 "        // explain last\n"
                 "        b\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("multiline block comments are preserved verbatim")
{
    check_format("module m;\n"
                 "/* line one\n"
                 "   line two */\n"
                 "void f() {}\n",
                 "module m;\n"
                 "/* line one\n"
                 "   line two */\n"
                 "void f() {}\n");
}

TEST_CASE("nested block comments are preserved verbatim")
{
    check_format("module m;\n"
                 "/* outer /* nested */ still outer */\n"
                 "void f() {}\n",
                 "module m;\n"
                 "/* outer /* nested */ still outer */\n"
                 "void f() {}\n");
}

TEST_CASE("accidental blank lines in a parameter list are canonicalized")
{
    check_format("module m;\n"
                 "public void* allocate_pages_aligned(usize count,\n"
                 "\n"
                 "\n"
                 "    usize alignment) {\n"
                 "    return nullptr;\n"
                 "}\n",
                 "module m;\n"
                 "public void* allocate_pages_aligned(\n"
                 "    usize count,\n"
                 "    usize alignment\n"
                 ") {\n"
                 "    return nullptr;\n"
                 "}\n");
}

TEST_CASE("fitting declaration stays flat")
{
    check_format("module m;\n"
                 "void process(i32 input, i32 output) {\n"
                 "    run(input, output);\n"
                 "}\n",
                 "module m;\n"
                 "void process(i32 input, i32 output) {\n"
                 "    run(input, output);\n"
                 "}\n");
}

TEST_CASE("wrapped function declaration aligns closing paren with context")
{
    check_format("module m;\n"
                 "public void process_very_long_name(i32 alpha_value, i32 beta_value, i32 gamma_value, i32 delta_value,\n"
                 "    i32 epsilon_value);\n",
                 "module m;\n"
                 "public void process_very_long_name(\n"
                 "    i32 alpha_value,\n"
                 "    i32 beta_value,\n"
                 "    i32 gamma_value,\n"
                 "    i32 delta_value,\n"
                 "    i32 epsilon_value\n"
                 ");\n");
}

TEST_CASE("nested generic parameter syntax stays flat")
{
    check_format("module m;\n"
                 "void f(Map(String, Vec(Value)) m, i32 n) {\n"
                 "    use(m);\n"
                 "}\n",
                 "module m;\n"
                 "void f(Map(String, Vec(Value)) m, i32 n) {\n"
                 "    use(m);\n"
                 "}\n");
}

TEST_CASE("nested declaration inside a block")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    if (ready) {\n"
                 "        {\n"
                 "            i32 inner = 1;\n"
                 "            use(inner);\n"
                 "        }\n"
                 "    }\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    if (ready) {\n"
                 "        {\n"
                 "            i32 inner = 1;\n"
                 "            use(inner);\n"
                 "        }\n"
                 "    }\n"
                 "}\n");
}

TEST_CASE("template function and template calls stay stable")
{
    check_format("module m;\n"
                 "T max(T)(T a, T b) {\n"
                 "    return if a > b { a } else { b };\n"
                 "}\n"
                 "void f() {\n"
                 "    copy!(u8, 23)(dst, src);\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "T max(T)(T a, T b) {\n"
                 "    return if a > b {\n"
                 "        a\n"
                 "    } else { b };\n"
                 "}\n"
                 "void f() {\n"
                 "    copy!(u8, 23)(dst, src);\n"
                 "}\n");
}

TEST_CASE("multiline parenthesized expression is preserved")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = (a +\n"
                 "        b) * 2;\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = (\n"
                 "        a +\n"
                 "        b\n"
                 "    ) * 2;\n"
                 "}\n");
}

TEST_CASE("empty braces stay compact")
{
    check_format("module m;\n"
                 "void empty() {}\n",
                 "module m;\n"
                 "void empty() {}\n");
}

TEST_CASE("array types and indexing stay flat")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    u8[256] arr;\n"
                 "    i32 v = arr[0];\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    u8[256] arr;\n"
                 "    i32 v = arr[0];\n"
                 "}\n");
}

TEST_CASE("canonical input formats to itself")
{
    check_format("module m;\n"
                 "\n"
                 "struct Config {\n"
                 "    i32 x;\n"
                 "    i32 y;\n"
                 "}\n"
                 "\n"
                 "void f() {\n"
                 "    Config c = {x = 1, y = 2};\n"
                 "    i32 v = compute(\n"
                 "        alpha,\n"
                 "        beta\n"
                 "    );\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Config {\n"
                 "    i32 x;\n"
                 "    i32 y;\n"
                 "}\n"
                 "\n"
                 "void f() {\n"
                 "    Config c = {x = 1, y = 2};\n"
                 "    i32 v = compute(\n"
                 "        alpha,\n"
                 "        beta\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("insertSpaces=false emits literal tabs")
{
    check_format_with("module m;\n"
                      "void f() {\n"
                      "    foo(a,\n"
                      "        b);\n"
                      "}\n",
                      "module m;\n"
                      "void f() {\n"
                      "\tfoo(\n"
                      "\t\ta,\n"
                      "\t\tb\n"
                      "\t);\n"
                      "}\n",
                      4, false);
}

TEST_CASE("custom tab size scales continuation indentation")
{
    check_format_with("module m;\n"
                      "void f() {\n"
                      "    foo(a,\n"
                      "        b);\n"
                      "}\n",
                      "module m;\n"
                      "void f() {\n"
                      "  foo(\n"
                      "    a,\n"
                      "    b\n"
                      "  );\n"
                      "}\n",
                      2, true);
}

TEST_CASE("deeply nested calls stay idempotent")
{
    auto out = format_source("module m;\n"
                             "void f() {\n"
                             "    i32 x = f0(f1(f2(f3(f4(f5(f6(f7(f8(f9(alpha))))))))));\n"
                             "}\n");
    REQUIRE(out.has_value());

    auto again = format_source(*out);
    REQUIRE(again.has_value());
    CHECK_EQ(*again, *out);
}

TEST_CASE("enum backing-type colon keeps its separating space")
{
    check_format("module m;\n"
                 "enum Color : u8 {\n"
                 "    Red,\n"
                 "    Green,\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "enum Color : u8 {\n"
                 "    Red,\n"
                 "    Green,\n"
                 "}\n");
}

TEST_CASE("deep template bang chains format in linear time")
{
    std::string src = "module m;\nvoid f() {\n    i32 x = ";
    int const N = 300;
    for (int i = 0; i < N; ++i)
        src += "t" + std::to_string(i) + "!";
    src += "value;\n}\n";

    auto first = format_source_with_stats(src);
    REQUIRE(first.text.has_value());
    CHECK_EQ(first.stats.range_scan_steps, std::size_t{0});
    CHECK(first.stats.delimiter_match_lookups <= 4);

    auto again = format_source_with_stats(*first.text);
    REQUIRE(again.text.has_value());
    CHECK_EQ(*again.text, *first.text);
    CHECK_LT(analysis_budget(again.stats), again.stats.token_count);
}

TEST_CASE("many broken declarations use O(1) paren matches")
{
    std::string src = "module m;\n";
    int const D = 250;
    for (int i = 0; i < D; ++i)
        src += "public void broken_fn_" + std::to_string(i) + "(\n";
    src += "struct Good {\n    i32 x;\n}\n";

    auto first = format_source_with_stats(src);
    REQUIRE(first.text.has_value());
    CHECK_EQ(first.stats.delimiter_match_lookups, std::size_t{D});
    CHECK_EQ(first.stats.range_scan_steps, std::size_t{0});

    auto again = format_source_with_stats(*first.text);
    REQUIRE(again.text.has_value());
    CHECK_EQ(*again.text, *first.text);
    CHECK_LT(analysis_budget(again.stats), again.stats.token_count);
}

TEST_CASE("colon-heavy statements use O(1) enum-region checks")
{
    std::string src = "module m;\nvoid f() {\n    i32 x = ";
    int const K = 300;
    for (int i = 0; i < K; ++i)
        src += "a" + std::to_string(i) + ":";
    src += "z;\n}\n";

    auto first = format_source_with_stats(src);
    REQUIRE(first.text.has_value());
    CHECK_EQ(first.stats.enum_region_lookups, std::size_t{K});
    CHECK_EQ(first.stats.range_scan_steps, std::size_t{0});

    auto again = format_source_with_stats(*first.text);
    REQUIRE(again.text.has_value());
    CHECK_EQ(*again.text, *first.text);
    CHECK_LT(analysis_budget(again.stats), again.stats.token_count);
}

TEST_CASE("deeply nested calls avoid quadratic range scans")
{
    std::string src = "module m;\nvoid f() {\n    i32 x = ";
    int const N = 250;
    for (int i = 0; i < N; ++i)
        src += "f" + std::to_string(i) + "(";
    src += "value";
    for (int i = 0; i < N; ++i)
        src += ")";
    src += ";\n}\n";

    auto first = format_source_with_stats(src);
    REQUIRE(first.text.has_value());
    CHECK_EQ(first.stats.range_scan_steps, std::size_t{0});
    CHECK(first.stats.delimiter_match_lookups <= 4);

    auto again = format_source_with_stats(*first.text);
    REQUIRE(again.text.has_value());
    CHECK_EQ(*again.text, *first.text);
    CHECK_LT(analysis_budget(again.stats), again.stats.token_count);
}

TEST_CASE("comma-less grouping paren containing a width-wrapped call stays flat")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = (some_extremely_long_function_name(alpha_value, beta_value, gamma_value, delta_value));\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = (some_extremely_long_function_name(\n"
                 "        alpha_value,\n"
                 "        beta_value,\n"
                 "        gamma_value,\n"
                 "        delta_value\n"
                 "    ));\n"
                 "}\n");
}

TEST_CASE("bracket context containing a width-wrapped call stays flat")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 v = arr[some_extremely_long_function_name(alpha_value, beta_value, gamma_value, delta_value)];\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 v = arr[some_extremely_long_function_name(\n"
                 "        alpha_value,\n"
                 "        beta_value,\n"
                 "        gamma_value,\n"
                 "        delta_value\n"
                 "    )];\n"
                 "}\n");
}

TEST_CASE("wide single-argument call wraps")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = some_extremely_long_function_name(alpha_value_plus_beta_and_gamma_computation);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = some_extremely_long_function_name(\n"
                 "        alpha_value_plus_beta_and_gamma_computation\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("wide single-parameter declaration wraps")
{
    check_format("module m;\n"
                 "public void process_with_extremely_long_name(i32 alpha_value_with_a_very_long_identifier);\n",
                 "module m;\n"
                 "public void process_with_extremely_long_name(\n"
                 "    i32 alpha_value_with_a_very_long_identifier\n"
                 ");\n");
}

TEST_CASE("wide single-argument template call wraps")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    auto v = make_thing_with_very_long_name!(TypeName)(alpha_value_plus_beta_and_gamma_computation);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    auto v = make_thing_with_very_long_name!(TypeName)(\n"
                 "        alpha_value_plus_beta_and_gamma_computation\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("genuinely nested wrapping indents one level per nesting")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = outer_process(inner_compute(some_very_wide_argument_expression(alpha_value, beta_value), gamma_value),\n"
                 "        delta_value);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = outer_process(\n"
                 "        inner_compute(\n"
                 "            some_very_wide_argument_expression(alpha_value, beta_value),\n"
                 "            gamma_value\n"
                 "        ),\n"
                 "        delta_value\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("wide condition and grouping parentheses do not wrap")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    if (some_very_long_condition_that_exceeds_the_line_width_and_keeps_going_and_going) {}\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    if (some_very_long_condition_that_exceeds_the_line_width_and_keeps_going_and_going) {}\n"
                 "}\n");

    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = (some_very_long_grouped_expression_that_exceeds_the_line_width_and_keeps_going_on);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = (some_very_long_grouped_expression_that_exceeds_the_line_width_and_keeps_going_on);\n"
                 "}\n");
}

TEST_CASE("comment immediately before a wrapped closing delimiter")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        a\n"
                 "        // done\n"
                 "    );\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        a\n"
                 "    // done\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("comments under literal-tab indentation")
{
    check_format_with("module m;\n"
                      "void f() {\n"
                      "    foo(alpha, // first arg\n"
                      "        // explain beta\n"
                      "        beta);\n"
                      "}\n",
                      "module m;\n"
                      "void f() {\n"
                      "\tfoo(\n"
                      "\t\talpha, // first arg\n"
                      "\t\t// explain beta\n"
                      "\t\tbeta\n"
                      "\t);\n"
                      "}\n",
                      4, false);
}

TEST_CASE("blank lines around comments are preserved while list whitespace collapses")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    foo(a,\n"
                 "\n"
                 "        // explain b\n"
                 "        b,\n"
                 "\n"
                 "\n"
                 "\n"
                 "        c);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        a,\n"
                 "\n"
                 "        // explain b\n"
                 "        b,\n"
                 "        c\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("prefix unary operators stay tight while binary operators keep spacing")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = -a + b;\n"
                 "    i32 z = a - -b;\n"
                 "    i32 v = *p + 1;\n"
                 "    i32 w = a * b;\n"
                 "    foo(&v, *q);\n"
                 "    i32 u = **pp;\n"
                 "    i32 t = a * *p;\n"
                 "    i32 m = ~mask;\n"
                 "    const char* s = \"hello\";\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = -a + b;\n"
                 "    i32 z = a - -b;\n"
                 "    i32 v = *p + 1;\n"
                 "    i32 w = a * b;\n"
                 "    foo(&v, *q);\n"
                 "    i32 u = **pp;\n"
                 "    i32 t = a * *p;\n"
                 "    i32 m = ~mask;\n"
                 "    const char* s = \"hello\";\n"
                 "}\n");
}

TEST_CASE("struct literal stays compact and inline when it fits")
{
    check_format("module m;\n"
                 "struct S { i32 x; i32 y; }\n"
                 "void f() {\n"
                 "    S s = {x = compute(a, b), y = compute(c, d)};\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct S {\n"
                 "    i32 x;\n"
                 "    i32 y;\n"
                 "}\n"
                 "void f() {\n"
                 "    S s = {x = compute(a, b), y = compute(c, d)};\n"
                 "}\n");
}

TEST_CASE("long call inside a compact struct literal stays idempotent")
{
    check_format("module m;\n"
                 "struct Config { i32 x; i32 y; }\n"
                 "void f() {\n"
                 "    Config c = {x = some_extremely_long_function_name(alpha_value, beta_value, gamma_value, delta_value), y = 2};\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Config {\n"
                 "    i32 x;\n"
                 "    i32 y;\n"
                 "}\n"
                 "void f() {\n"
                 "    Config c = {x = some_extremely_long_function_name(\n"
                 "            alpha_value,\n"
                 "            beta_value,\n"
                 "            gamma_value,\n"
                 "            delta_value\n"
                 "        ), y = 2};\n"
                 "}\n");
}

TEST_CASE("block argument contents indent one level deeper in a wrapped call")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = some_function({\n"
                 "        i32 tmp = compute();\n"
                 "        tmp + 1\n"
                 "    }, other_arg);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = some_function(\n"
                 "        {\n"
                 "            i32 tmp = compute();\n"
                 "            tmp + 1\n"
                 "        },\n"
                 "        other_arg\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("usize and isize are primitive type keywords")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    usize* p = nullptr;\n"
                 "    isize n = -1;\n"
                 "    usize[4] arr;\n"
                 "    const usize* q;\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    usize* p = nullptr;\n"
                 "    isize n = -1;\n"
                 "    usize[4] arr;\n"
                 "    const usize* q;\n"
                 "}\n");
}

TEST_CASE("user-defined type pointer declarations stay tight and idempotent")
{
    check_format("module m;\n"
                 "struct Expr { i32 kind; i32 value; }\n"
                 "Expr* make_expr();\n"
                 "void f() {\n"
                 "    Expr* a = make_expr();\n"
                 "    Expr* ptr;\n"
                 "    Expr** ptr2;\n"
                 "    Expr* const cptr;\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Expr {\n"
                 "    i32 kind;\n"
                 "    i32 value;\n"
                 "}\n"
                 "\n"
                 "Expr* make_expr();\n"
                 "void f() {\n"
                 "    Expr* a = make_expr();\n"
                 "    Expr* ptr;\n"
                 "    Expr** ptr2;\n"
                 "    Expr* const cptr;\n"
                 "}\n");
}

TEST_CASE("pointer declaration initializer with struct literal call stays tight")
{
    check_format("module m;\n"
                 "struct Expr { i32 kind; i32 value; }\n"
                 "void f() {\n"
                 "    Expr* a = x.create({kind = 1, value = 42});\n"
                 "    Expr* b = x.create({kind = 3, value = 74});\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Expr {\n"
                 "    i32 kind;\n"
                 "    i32 value;\n"
                 "}\n"
                 "void f() {\n"
                 "    Expr* a = x.create({kind = 1, value = 42});\n"
                 "    Expr* b = x.create({kind = 3, value = 74});\n"
                 "}\n");
}

TEST_CASE("pointer return and pointer parameters keep tight stars")
{
    check_format("module m;\n"
                 "struct Expr { i32 kind; }\n"
                 "Expr* foo(Expr* p) {\n"
                 "    return p;\n"
                 "}\n"
                 "Expr** table(i32 n) {\n"
                 "    return nullptr;\n"
                 "}\n"
                 "void use(i32** pp) {\n"
                 "    use_pp(pp);\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Expr {\n"
                 "    i32 kind;\n"
                 "}\n"
                 "\n"
                 "Expr* foo(Expr* p) {\n"
                 "    return p;\n"
                 "}\n"
                 "\n"
                 "Expr** table(i32 n) {\n"
                 "    return nullptr;\n"
                 "}\n"
                 "void use(i32** pp) {\n"
                 "    use_pp(pp);\n"
                 "}\n");
}

TEST_CASE("multiplication and dereference spacing is independent of pointer declarations")
{
    check_format("module m;\n"
                 "struct Expr { i32 kind; }\n"
                 "void f() {\n"
                 "    i32 value = 2;\n"
                 "    i32 count = 3;\n"
                 "    i32 x = value * count;\n"
                 "    Expr* p;\n"
                 "    i32 y = *p;\n"
                 "    i32 z = value * *p;\n"
                 "    i32 w = (*p) + 1;\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Expr {\n"
                 "    i32 kind;\n"
                 "}\n"
                 "void f() {\n"
                 "    i32 value = 2;\n"
                 "    i32 count = 3;\n"
                 "    i32 x = value * count;\n"
                 "    Expr* p;\n"
                 "    i32 y = *p;\n"
                 "    i32 z = value * *p;\n"
                 "    i32 w = (*p) + 1;\n"
                 "}\n");
}

TEST_CASE("pointer stars in array types do not disturb multiplication in sizes")
{
    check_format("module m;\n"
                 "struct Expr { i32 kind; }\n"
                 "void f() {\n"
                 "    i32 x = 2;\n"
                 "    Expr*[x * 4] arr;\n"
                 "    i32[value * count] grid;\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Expr {\n"
                 "    i32 kind;\n"
                 "}\n"
                 "void f() {\n"
                 "    i32 x = 2;\n"
                 "    Expr*[x * 4] arr;\n"
                 "    i32[value * count] grid;\n"
                 "}\n");
}

TEST_CASE("function pointer type star is structural")
{
    check_format("module m;\n"
                 "using Callback = void (*)(i32);\n"
                 "using Sized = void (*)(i32[2 * 3]);\n",
                 "module m;\n"
                 "using Callback = void(*)(i32);\n"
                 "using Sized = void(*)(i32[2 * 3]);\n");
}

TEST_CASE("function pointer documentation-only parameter names are preserved")
{
    check_format("module m;\n"
                 "using AllNamed = void (*) (i32 value, bool enabled);\n"
                 "using Mixed = void(*)(i32, bool enabled);\n"
                 "using Unnamed = void(*)(i32, bool);\n"
                 "using NamedType = void(*)(Callback inner);\n",
                 "module m;\n"
                 "using AllNamed = void(*)(i32 value, bool enabled);\n"
                 "using Mixed = void(*)(i32, bool enabled);\n"
                 "using Unnamed = void(*)(i32, bool);\n"
                 "using NamedType = void(*)(Callback inner);\n");
}

TEST_CASE("full line width participates in the wrap decision")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = some_function(alpha_value, beta_value, gamma_value, delta_value, epsilon_value);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = some_function(\n"
                 "        alpha_value,\n"
                 "        beta_value,\n"
                 "        gamma_value,\n"
                 "        delta_value,\n"
                 "        epsilon_value\n"
                 "    );\n"
                 "}\n");
}

TEST_CASE("bang assignment return and parenthesized template args stay tight")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    a = !b;\n"
                 "    return !ok;\n"
                 "    foo!(T);\n"
                 "    auto v = make!(Vec!i32)(1, 2, 3);\n"
                 "    i32 x = max!i32(a, b);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    a = !b;\n"
                 "    return !ok;\n"
                 "    foo!(T);\n"
                 "    auto v = make!(Vec!i32)(1, 2, 3);\n"
                 "    i32 x = max!i32(a, b);\n"
                 "}\n");
}

TEST_CASE("incomplete call before a block close formats canonically")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    call(a,\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    call(\n"
                 "        a,\n"
                 "}\n");
}

TEST_CASE("incomplete argument list without a close stays open")
{
    check_format("foo(\na,\nb,", "foo(\n"
                                 "    a,\n"
                                 "    b,\n");
}

TEST_CASE("unmatched call at end of an incomplete block")
{
    check_format("if condition {\nfoo(", "if condition {\n"
                                         "    foo(\n");
}

TEST_CASE("nested unmatched calls keep continuation levels")
{
    check_format("some_call(\nnested(\nvalue,", "some_call(\n"
                                                "    nested(\n"
                                                "        value,\n");
}

TEST_CASE("content after an unmatched brace stays indented")
{
    check_format("if !condition {\nfoo();\nbar();", "if !condition {\n"
                                                    "    foo();\n"
                                                    "    bar();\n");
}

TEST_CASE("complete statement before an incomplete fragment stays formatted")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = compute(a, b);\n"
                 "    risky(\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = compute(a, b);\n"
                 "    risky(\n"
                 "}\n");
}

TEST_CASE("parser recovery followed by a later statement still formats")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 before = 1;\n"
                 "    risky(\n"
                 "    complete = 1;\n"
                 "    after();\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 before = 1;\n"
                 "    risky(\n"
                 "        complete = 1;\n"
                 "    after();\n"
                 "}\n");
}

TEST_CASE("complete code before and after an invalid fragment formats")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 before = 1;\n"
                 "    broken(\n"
                 "    mid();\n"
                 "    i32 after = 2;\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 before = 1;\n"
                 "    broken(\n"
                 "        mid();\n"
                 "    i32 after = 2;\n"
                 "}\n");
}

TEST_CASE("comment at a re-anchor boundary stays attached")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    risky(\n"
                 "    complete = 1;\n"
                 "    // note about the re-anchored statement\n"
                 "    after();\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    risky(\n"
                 "        complete = 1;\n"
                 "    // note about the re-anchored statement\n"
                 "    after();\n"
                 "}\n");
}

TEST_CASE("for-header semicolons do not re-anchor")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    for (i = 0; i < n; i++\n"
                 "    use(i);\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    for (\n"
                 "        i = 0; i < n; i++\n"
                 "        use(i);\n"
                 "}\n");
}

TEST_CASE("complete top-level declaration after a broken declaration re-anchors")
{
    check_format("module m;\n"
                 "void broken(\n"
                 "struct Good {\n"
                 "    i32 x;\n"
                 "}\n"
                 "void ok() {\n"
                 "    run();\n"
                 "}\n",
                 "module m;\n"
                 "void broken(\n"
                 "struct Good {\n"
                 "    i32 x;\n"
                 "}\n"
                 "void ok() {\n"
                 "    run();\n"
                 "}\n");
}

TEST_CASE("unmatched brace recovering at a mismatched close keeps indent")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 x = {\n"
                 "        a = 1,\n"
                 "        b = 2,\n"
                 "    ]\n"
                 "    i32 y = 3;\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 x = {\n"
                 "        a = 1,\n"
                 "        b = 2,\n"
                 "    ]\n"
                 "    i32 y = 3;\n"
                 "}\n");

    check_format("module m;\n"
                 "void f() {\n"
                 "    if (cond) {\n"
                 "        x = { a = 1 )\n"
                 "        y = 2;\n"
                 "    }\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    if (cond) {\n"
                 "        x = { a = 1)\n"
                 "        y = 2;\n"
                 "    }\n"
                 "}\n");
}

TEST_CASE("nested mismatched closers under wrapping stay put")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        { a = 1,\n"
                 "        b = 2 ]\n"
                 "        bar(1, 2]\n"
                 "        baz = 3;\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    foo(\n"
                 "        {\n"
                 "            a = 1,\n"
                 "            b = 2]\n"
                 "        bar(1, 2]\n"
                 "        baz = 3;\n"
                 "}\n");

    check_format("module m;\n"
                 "void broken() {\n"
                 "    x = { a = 1 ]\n"
                 "}\n",
                 "module m;\n"
                 "void broken() {\n"
                 "    x = { a = 1]\n"
                 "}\n");
}

TEST_CASE("parser recovery followed by a later declaration still formats")
{
    check_format("module m;\n"
                 "void broken(\n"
                 "struct Good {\n"
                 "    i32 x;\n"
                 "}\n",
                 "module m;\n"
                 "void broken(\n"
                 "struct Good {\n"
                 "    i32 x;\n"
                 "}\n");
}

TEST_CASE("comments in incomplete calls and blocks are preserved")
{
    check_format("foo(\n    // comment in call\n    a,\n    b,", "foo(\n"
                                                                 "    // comment in call\n"
                                                                 "    a,\n"
                                                                 "    b,\n");

    check_format("if broken {\n    // comment in block\n    foo(\n", "if broken {\n"
                                                                     "    // comment in block\n"
                                                                     "    foo(\n");

    check_format("foo(\n    // trailing comment\n", "foo(\n"
                                                    "    // trailing comment\n");

    check_format("foo(a, // inline\n    b,\n", "foo(\n"
                                               "    a, // inline\n"
                                               "    b,\n");
}

TEST_CASE("stray and mismatched closing delimiters stay put")
{
    check_format("foo(a, b]", "foo(a, b]\n");

    check_format("}\nfoo()", "}\n"
                             "\n"
                             "foo()\n");
}

TEST_CASE("incomplete statement with semicolons stays flat")
{
    check_format("foo(a; b;", "foo(a; b;\n");
}

TEST_CASE("lexically invalid input remains a no-op")
{
    auto out = format_source("module m;\n"
                             "void f() {\n"
                             "    \"unterminated string\n"
                             "}\n");
    CHECK(!out.has_value());

    out = format_source("/* unterminated block comment");
    CHECK(!out.has_value());
}

TEST_CASE("typed struct literal nested in another brace stays compact")
{
    check_format("module m;\n"
                 "struct Point { i32 x; i32 y; }\n"
                 "struct Wrap { Point p; }\n"
                 "void f() {\n"
                 "    Wrap w = { Point {x = 1, y = 2} };\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Point {\n"
                 "    i32 x;\n"
                 "    i32 y;\n"
                 "}\n"
                 "\n"
                 "struct Wrap {\n"
                 "    Point p;\n"
                 "}\n"
                 "void f() {\n"
                 "    Wrap w = { Point {x = 1, y = 2} };\n"
                 "}\n");
}

TEST_CASE("intentionally multiline struct literal is preserved")
{
    check_format("module m;\n"
                 "struct Config { i32 x; i32 y; }\n"
                 "void f() {\n"
                 "    Config c = {\n"
                 "        x = 1,\n"
                 "        y = 2,\n"
                 "    };\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Config {\n"
                 "    i32 x;\n"
                 "    i32 y;\n"
                 "}\n"
                 "void f() {\n"
                 "    Config c = {\n"
                 "        x = 1,\n"
                 "        y = 2,\n"
                 "    };\n"
                 "}\n");
}

TEST_CASE("multiline struct literal with a width-wrapped call stays stable")
{
    check_format("module m;\n"
                 "struct Config { i32 x; i32 y; }\n"
                 "void f() {\n"
                 "    Config c = {\n"
                 "        x = some_extremely_long_function_name(alpha_value, beta_value, gamma_value, delta_value),\n"
                 "        y = 2,\n"
                 "    };\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Config {\n"
                 "    i32 x;\n"
                 "    i32 y;\n"
                 "}\n"
                 "void f() {\n"
                 "    Config c = {\n"
                 "        x = some_extremely_long_function_name(\n"
                 "            alpha_value,\n"
                 "            beta_value,\n"
                 "            gamma_value,\n"
                 "            delta_value\n"
                 "        ),\n"
                 "        y = 2,\n"
                 "    };\n"
                 "}\n");
}

TEST_CASE("compact struct literal braces are tight and idempotent")
{
    check_format("module m;\n"
                 "struct Expr { i32 kind; i32 value; }\n"
                 "void f() {\n"
                 "    Expr e = {kind = 1, value = 42};\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Expr {\n"
                 "    i32 kind;\n"
                 "    i32 value;\n"
                 "}\n"
                 "void f() {\n"
                 "    Expr e = {kind = 1, value = 42};\n"
                 "}\n");
}

TEST_CASE("nested typed struct literals are tight at every level")
{
    check_format("module m;\n"
                 "struct Point { i32 x; i32 y; }\n"
                 "struct Line { Point a; Point b; }\n"
                 "void f() {\n"
                 "    Line l = {Point {x = 1, y = 2}, Point {x = 3, y = 4}};\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Point {\n"
                 "    i32 x;\n"
                 "    i32 y;\n"
                 "}\n"
                 "\n"
                 "struct Line {\n"
                 "    Point a;\n"
                 "    Point b;\n"
                 "}\n"
                 "void f() {\n"
                 "    Line l = {Point {x = 1, y = 2}, Point {x = 3, y = 4}};\n"
                 "}\n");
}

TEST_CASE("wrapped inner struct literal inside compact outer keeps clean close")
{
    check_format("module m;\n"
                 "struct Inner { i32 a; i32 b; i32 c; i32 d; }\n"
                 "struct Outer { Inner inner; i32 tag; }\n"
                 "void f() {\n"
                 "    Outer o = { Inner {\n"
                 "        a = 1,\n"
                 "        b = 2,\n"
                 "        c = 3,\n"
                 "        d = 4,\n"
                 "    }, tag = 5 };\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Inner {\n"
                 "    i32 a;\n"
                 "    i32 b;\n"
                 "    i32 c;\n"
                 "    i32 d;\n"
                 "}\n"
                 "\n"
                 "struct Outer {\n"
                 "    Inner inner;\n"
                 "    i32 tag;\n"
                 "}\n"
                 "void f() {\n"
                 "    Outer o = {Inner {\n"
                 "            a = 1,\n"
                 "            b = 2,\n"
                 "            c = 3,\n"
                 "            d = 4,\n"
                 "        }, tag = 5};\n"
                 "}\n");
}

TEST_CASE("compact block-expression braces keep their spaced style")
{
    check_format("module m;\n"
                 "void f() {\n"
                 "    i32 a = 1;\n"
                 "    i32 b = 2;\n"
                 "    i32 x = if a > 0 { b } else { 0 };\n"
                 "}\n",
                 "module m;\n"
                 "void f() {\n"
                 "    i32 a = 1;\n"
                 "    i32 b = 2;\n"
                 "    i32 x = if a > 0 {\n"
                 "        b\n"
                 "    } else { 0 };\n"
                 "}\n");
}

SECTION("format: range formatting");

TEST_CASE("range formatting returns the minimal edit for the unformatted region only")
{
    auto result = format_range_with("module m;\nvoid f() {\n    i32 x=1;\n}\n", make_range(2, 8, 2, 12), dcc::sm::PositionEncoding::Utf16);
    REQUIRE(result.edits.size() == 1);
    auto const& edit = result.edits[0];
    CHECK_EQ(edit.range.start.line, 2u);
    CHECK_EQ(edit.range.start.character, 9u);
    CHECK_EQ(edit.range.end.line, 2u);
    CHECK_EQ(edit.range.end.character, 10u);
    CHECK_EQ(edit.newText, " = ");
}

TEST_CASE("range formatting output is byte-identical to the whole-document formatter outside the edit")
{
    std::string_view src = "module m;\nvoid f() {\n    i32 x=1;\n}\n";
    auto full = format_source(src);
    REQUIRE(full.has_value());

    auto result = format_range_with(src, make_range(2, 8, 2, 12), dcc::sm::PositionEncoding::Utf16);
    REQUIRE(result.edits.size() == 1);

    std::string applied{src};
    auto const& edit = result.edits[0];
    auto start = applied.find("x=1");
    REQUIRE(start != std::string::npos);

    auto const line_begin = applied.find("    i32 ");
    auto const line_end = applied.find('\n', line_begin);
    CHECK_EQ(applied.substr(line_begin, line_end - line_begin), "    i32 x=1;");
    applied.replace(start + 1, 1, edit.newText);
    CHECK_EQ(applied, *full);
}

TEST_CASE("range formatting refuses when another needed change lies outside the selection")
{
    std::string_view src = "module m;\nvoid f() {\n    i32 x=1;\n    i32 y=2;\n}\n";
    auto result = format_range_with(src, make_range(2, 8, 2, 12), dcc::sm::PositionEncoding::Utf16);
    CHECK(result.edits.empty());

    result = format_range_with(src, make_range(2, 8, 3, 12), dcc::sm::PositionEncoding::Utf16);
    REQUIRE(result.edits.size() == 1);
    CHECK_EQ(result.edits[0].newText, " = 1;\n    i32 y = ");
}

TEST_CASE("range formatting refuses a reversed or out-of-bounds range")
{
    std::string_view src = "module m;\nvoid f() {\n    i32 x=1;\n}\n";

    auto result = format_range_with(src, make_range(2, 12, 2, 8), dcc::sm::PositionEncoding::Utf16);
    CHECK(result.edits.empty());

    result = format_range_with(src, make_range(99, 0, 100, 1), dcc::sm::PositionEncoding::Utf16);
    CHECK(result.edits.empty());
}

TEST_CASE("range formatting on already-formatted input is a no-op")
{
    std::string_view src = "module m;\nvoid f() {\n    i32 x = 1;\n}\n";
    auto result = format_range_with(src, make_range(2, 8, 2, 14), dcc::sm::PositionEncoding::Utf16);
    CHECK(result.edits.empty());

    result = format_range_with(src, make_range(0, 0, 3, 1), dcc::sm::PositionEncoding::Utf16);
    CHECK(result.edits.empty());
}

TEST_CASE("range formatting honors utf-8, utf-16 and utf-32 positions with non-ASCII content")
{
    std::string_view src = "module m;\nvoid f() {\n    const char* s=\"caf\u00e9\";\n    i32 y = 2;\n}\n";

    auto r16 = format_range_with(src, make_range(2, 17, 2, 25), dcc::sm::PositionEncoding::Utf16);
    REQUIRE(r16.edits.size() == 1);
    CHECK_EQ(r16.edits[0].range.start.character, 17u);
    CHECK_EQ(r16.edits[0].range.end.character, 18u);
    CHECK_EQ(r16.edits[0].newText, " = ");
    CHECK_EQ(r16.edits[0].range.start.line, 2u);
    CHECK_EQ(r16.edits[0].range.end.line, 2u);

    auto r8 = format_range_with(src, make_range(2, 17, 2, 26), dcc::sm::PositionEncoding::Utf8);
    REQUIRE(r8.edits.size() == 1);
    CHECK_EQ(r8.edits[0].range.start.character, 17u);
    CHECK_EQ(r8.edits[0].range.end.character, 18u);
    CHECK_EQ(r8.edits[0].newText, " = ");

    auto r32 = format_range_with(src, make_range(2, 17, 2, 25), dcc::sm::PositionEncoding::Utf32);
    REQUIRE(r32.edits.size() == 1);
    CHECK_EQ(r32.edits[0].range.start.character, 17u);
    CHECK_EQ(r32.edits[0].range.end.character, 18u);
    CHECK_EQ(r32.edits[0].newText, " = ");

    auto r16_bad = format_range_with(src, make_range(99, 0, 100, 1), dcc::sm::PositionEncoding::Utf16);
    CHECK(r16_bad.edits.empty());

    auto full = format_source(src);
    REQUIRE(full.has_value());
    for (auto const& result : {r8, r16, r32})
    {
        REQUIRE(result.edits.size() == 1);
        std::string applied{src};
        auto at = applied.find("s=\"caf");
        REQUIRE(at != std::string::npos);
        auto eq = applied.find('=', at);
        REQUIRE(eq != std::string::npos);
        applied.replace(eq, 1, result.edits[0].newText);
        CHECK_EQ(applied, *full);
    }
}

TEST_CASE("range formatting never returns a whole-document edit for a subrange")
{
    std::string_view src = "module m;\nvoid f() {\n    i32 x=1;\n    i32 y=2;\n}\n";

    auto result = format_range_with(src, make_range(2, 0, 2, 12), dcc::sm::PositionEncoding::Utf16);
    CHECK(result.edits.empty());

    result = format_range_with(src, make_range(2, 8, 3, 12), dcc::sm::PositionEncoding::Utf16);
    REQUIRE(result.edits.size() == 1);
    CHECK(result.edits[0].range.start.line >= 2u);
    CHECK(result.edits[0].range.end.line <= 3u);
}

SECTION("format: on-type formatting");

TEST_CASE("on-type '}' reindents the closing brace of the current line")
{
    std::string_view src = "module m;\nvoid f() {\n    if (cond) {\n        i32 x = 1;\n   }\n}\n";
    auto edits = format_on_type_with(src, "}", make_pos(4, 4), dcc::sm::PositionEncoding::Utf16);
    REQUIRE(edits.size() == 1);
    auto const& edit = edits[0];
    CHECK_EQ(edit.newText, " ");
    CHECK_EQ(edit.range.start.line, 4u);
    CHECK_EQ(edit.range.start.character, 3u);
    CHECK_EQ(edit.range.end.line, 4u);
    CHECK_EQ(edit.range.end.character, 3u);

    auto full = format_source(src);
    REQUIRE(full.has_value());
    std::string applied{src};
    auto at = applied.find("\n   }\n");
    REQUIRE(at != std::string::npos);
    applied.insert(at + 1 + 3, edit.newText);
    CHECK_EQ(applied, *full);
}

TEST_CASE("on-type ';' cleans up the current line")
{
    std::string_view src = "module m;\nvoid f() {\n    i32 x=1;\n}\n";
    auto edits = format_on_type_with(src, ";", make_pos(2, 12), dcc::sm::PositionEncoding::Utf16);
    REQUIRE(edits.size() == 1);
    auto const& edit = edits[0];
    CHECK_EQ(edit.newText, " = ");
    CHECK_EQ(edit.range.start.line, 2u);
    CHECK_EQ(edit.range.start.character, 9u);
    CHECK_EQ(edit.range.end.line, 2u);
    CHECK_EQ(edit.range.end.character, 10u);

    auto full = format_source(src);
    REQUIRE(full.has_value());
    std::string applied{src};
    auto at = applied.find("x=1");
    REQUIRE(at != std::string::npos);
    applied.replace(at + 1, 1, edit.newText);
    CHECK_EQ(applied, *full);
}

TEST_CASE("on-type formatting refuses mismatched or unsupported triggers")
{
    std::string_view src = "module m;\nvoid f() {\n    i32 x=1;\n}\n";

    auto edits = format_on_type_with(src, "}", make_pos(2, 12), dcc::sm::PositionEncoding::Utf16);
    CHECK(edits.empty());

    edits = format_on_type_with(src, "", make_pos(2, 12), dcc::sm::PositionEncoding::Utf16);
    CHECK(edits.empty());

    edits = format_on_type_with(src, ";;", make_pos(2, 12), dcc::sm::PositionEncoding::Utf16);
    CHECK(edits.empty());

    edits = format_on_type_with(src, ";", make_pos(99, 0), dcc::sm::PositionEncoding::Utf16);
    CHECK(edits.empty());

    edits = format_on_type_with("module m;\nvoid f() {\n    i32 x = 1;\n}\n", ";", make_pos(2, 12), dcc::sm::PositionEncoding::Utf16);
    CHECK(edits.empty());
}

TEST_CASE("on-type formatting refuses when the line's fix would escape the line")
{
    std::string_view src = "module m;\nvoid f() {\n    i32 x=1;\n}  \n   }\n";
    auto edits = format_on_type_with(src, "}", make_pos(3, 1), dcc::sm::PositionEncoding::Utf16);
    CHECK(edits.empty());
}

SECTION("format: formatting options");

TEST_CASE("insertFinalNewline=false drops the final newline and stays idempotent")
{
    dccd::protocol::FormattingOptions opts;
    opts.tabSize = 4;
    opts.insertSpaces = true;
    opts.insertFinalNewline = false;

    dcc::sm::SourceManager sm;
    auto const fid = sm.add_synthetic("test_format.dc", std::string{"module m;\nvoid f() {\n    i32 x=1;\n}\n"});
    auto const* sf = sm.get(fid);
    REQUIRE(sf != nullptr);

    dcc::si::string_interner interner;
    auto edit = dccd::format::format_document(*sf, interner, opts);
    REQUIRE(edit.has_value());
    CHECK(edit->newText.ends_with("\n}"));
    CHECK(!edit->newText.ends_with("\n\n"));
    CHECK_EQ(edit->newText, "module m;\nvoid f() {\n    i32 x = 1;\n}");

    auto const fid2 = sm.add_synthetic("test_format.dc", edit->newText);
    auto const* sf2 = sm.get(fid2);
    REQUIRE(sf2 != nullptr);
    auto edit2 = dccd::format::format_document(*sf2, interner, opts);
    REQUIRE(edit2.has_value());
    CHECK_EQ(edit2->newText, edit->newText);
}

TEST_CASE("trimFinalNewlines trims every trailing newline and re-inserts one per insertFinalNewline")
{
    dccd::protocol::FormattingOptions opts;
    opts.tabSize = 4;
    opts.insertSpaces = true;
    opts.trimFinalNewlines = true;
    opts.insertFinalNewline = false;

    dcc::sm::SourceManager sm;
    auto const fid = sm.add_synthetic("test_format.dc", std::string{"module m;\nvoid f() {\n    i32 x=1;\n}\n\n\n\n"});
    auto const* sf = sm.get(fid);
    REQUIRE(sf != nullptr);

    dcc::si::string_interner interner;
    auto edit = dccd::format::format_document(*sf, interner, opts);
    REQUIRE(edit.has_value());
    CHECK(!edit->newText.ends_with('\n'));
    CHECK_EQ(edit->newText, "module m;\nvoid f() {\n    i32 x = 1;\n}");

    opts.insertFinalNewline = true;
    auto edit_nl = dccd::format::format_document(*sf, interner, opts);
    REQUIRE(edit_nl.has_value());
    CHECK(edit_nl->newText.ends_with("}\n"));
    CHECK_EQ(edit_nl->newText, "module m;\nvoid f() {\n    i32 x = 1;\n}\n");
}

TEST_CASE("trimTrailingWhitespace removes trailing spaces and tabs per line")
{
    dccd::protocol::FormattingOptions opts;
    opts.tabSize = 4;
    opts.insertSpaces = true;
    opts.trimTrailingWhitespace = true;

    dcc::sm::SourceManager sm;
    auto const fid = sm.add_synthetic("test_format.dc", std::string{"module m;\nvoid f() {    \n    i32 x=1;   \t\n}\n"});
    auto const* sf = sm.get(fid);
    REQUIRE(sf != nullptr);

    dcc::si::string_interner interner;
    auto edit = dccd::format::format_document(*sf, interner, opts);
    REQUIRE(edit.has_value());
    CHECK_EQ(edit->newText, "module m;\nvoid f() {\n    i32 x = 1;\n}\n");

    auto const fid2 = sm.add_synthetic("test_format.dc", edit->newText);
    auto const* sf2 = sm.get(fid2);
    REQUIRE(sf2 != nullptr);
    auto edit2 = dccd::format::format_document(*sf2, interner, opts);
    REQUIRE(edit2.has_value());
    CHECK_EQ(edit2->newText, edit->newText);
}

TEST_CASE("formatting options flow through range formatting")
{
    dccd::protocol::FormattingOptions opts;
    opts.tabSize = 4;
    opts.insertSpaces = true;
    opts.insertFinalNewline = false;

    auto result = format_range_with("module m;\nvoid f() {\n    i32 x=1;\n}\n", make_range(2, 8, 4, 0), dcc::sm::PositionEncoding::Utf16, opts);
    REQUIRE(result.edits.size() == 1);
    CHECK_EQ(result.edits[0].newText, " = 1;\n}");

    result = format_range_with("module m;\nvoid f() {\n    i32 x=1;\n}\n", make_range(2, 8, 2, 12), dcc::sm::PositionEncoding::Utf16, opts);
    CHECK(result.edits.empty());
}

TEST_CASE("formatting options on-type ';' with trimTrailingWhitespace")
{
    dccd::protocol::FormattingOptions opts;
    opts.tabSize = 4;
    opts.insertSpaces = true;
    opts.trimTrailingWhitespace = true;

    std::string_view src = "module m;\nvoid f() {\n    i32 x=1;   \n}\n";
    auto edits = format_on_type_with(src, ";", make_pos(2, 12), dcc::sm::PositionEncoding::Utf16, opts);
    REQUIRE(edits.size() == 1);
    CHECK_EQ(edits[0].newText, " = 1;");

    auto full = format_source_with(src, 4, true);
    REQUIRE(full.has_value());
    std::string applied{src};
    auto at = applied.find("x=1");
    REQUIRE(at != std::string::npos);
    auto eq = applied.find('=', at);
    REQUIRE(eq != std::string::npos);
    auto nl = applied.find('\n', eq);
    REQUIRE(nl != std::string::npos);
    applied.replace(eq, nl - eq, edits[0].newText);
    CHECK_EQ(applied, *full);
}
