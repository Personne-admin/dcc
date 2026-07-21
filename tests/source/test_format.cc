import std;
import dccd.format;
import dccd.protocol;
import dcc.sm;
import dcc.si;

#include "harness.hh"

namespace
{
    [[nodiscard]] std::optional<std::string> format_source(std::string_view src)
    {
        dcc::sm::SourceManager sm;
        auto const fid = sm.add_synthetic("test_format.dc", std::string{src});
        auto const* sf = sm.get(fid);
        if (!sf)
            return std::nullopt;

        dcc::si::string_interner interner;
        dccd::protocol::FormattingOptions opts;
        opts.tabSize = 4;
        opts.insertSpaces = true;

        auto edit = dccd::format::format_document(*sf, interner, opts);
        if (!edit)
            return std::nullopt;

        return edit->newText;
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

TEST_CASE("comment bearing source remains a no-op")
{
    auto out = format_source("module m;\n"
                             "// keep comment positioning untouched\n"
                             "void f() {}\n");
    CHECK(!out.has_value());
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
                 "    S s = { x = compute(a, b), y = compute(c, d) };\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct S {\n"
                 "    i32 x;\n"
                 "    i32 y;\n"
                 "}\n"
                 "void f() {\n"
                 "    S s = { x = compute(a, b), y = compute(c, d) };\n"
                 "}\n");
}

TEST_CASE("long call inside a compact struct literal stays idempotent")
{
    check_format("module m;\n"
                 "struct Config { i32 x; i32 y; }\n"
                 "void f() {\n"
                 "    Config c = { x = some_extremely_long_function_name(alpha_value, beta_value, gamma_value, delta_value), y = 2 };\n"
                 "}\n",
                 "module m;\n"
                 "\n"
                 "struct Config {\n"
                 "    i32 x;\n"
                 "    i32 y;\n"
                 "}\n"
                 "void f() {\n"
                 "    Config c = { x = some_extremely_long_function_name(\n"
                 "            alpha_value,\n"
                 "            beta_value,\n"
                 "            gamma_value,\n"
                 "            delta_value\n"
                 "        ), y = 2 };\n"
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

TEST_CASE("malformed source remains a conservative no-op")
{
    auto out = format_source("module m;\n"
                             "void f() {\n"
                             "    call(a,\n"
                             "}\n");
    CHECK(!out.has_value());
}

TEST_CASE("typed struct literal nested in another brace stays compact")
{
    check_format("module m;\n"
                 "struct Point { i32 x; i32 y; }\n"
                 "struct Wrap { Point p; }\n"
                 "void f() {\n"
                 "    Wrap w = { Point { x = 1, y = 2 } };\n"
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
                 "    Wrap w = { Point { x = 1, y = 2 } };\n"
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
