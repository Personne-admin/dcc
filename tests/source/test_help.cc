import std;
import dcc.help;

#include "harness.hh"

#include <stdlib.h>

SECTION("help formatting");

TEST_CASE("flag text covers every argument style and negated forms")
{
    using dcc::help::ArgKind;
    using dcc::help::Flag;
    CHECK_EQ(dcc::help::flag_text(Flag{"-c", "", ArgKind::None, ""}), std::string{"-c"});
    CHECK_EQ(dcc::help::flag_text(Flag{"-o", "", ArgKind::Required, "<file>"}), std::string{"-o <file>"});
    CHECK_EQ(dcc::help::flag_text(Flag{"-I", "", ArgKind::Attached, "<dir>"}), std::string{"-I<dir>"});
    CHECK_EQ(dcc::help::flag_text(Flag{"-flibdcext", "", ArgKind::Optional, "<os>"}), std::string{"-flibdcext [<os>]"});
    CHECK_EQ(dcc::help::flag_text(Flag{"-fpic", "-fno-pic", ArgKind::None, ""}), std::string{"-f[no-]pic"});
    CHECK_EQ(dcc::help::flag_text(Flag{"--color", "--no-color", ArgKind::None, ""}), std::string{"--color | --no-color"});
}

TEST_CASE("descriptions append choices, aliases and defaults in a fixed order")
{
    std::array<std::string_view, 2> choices{"a", "b"};
    std::array<std::string_view, 1> one{"--x"};
    std::array<std::string_view, 2> two{"--x", "--y"};
    CHECK_EQ(dcc::help::description_text("plain", {}, {}, ""), std::string{"plain"});
    CHECK_EQ(dcc::help::description_text("text", choices, one, "a"), std::string{"text (one of: a, b) (alias: --x) [default: a]"});
    CHECK_EQ(dcc::help::description_text("text", {}, two, ""), std::string{"text (aliases: --x, --y)"});
    CHECK_EQ(dcc::help::join(choices, "|"), std::string{"a|b"});
}

TEST_CASE("the terminal width follows COLUMNS and falls back to a sane value")
{
    char const* saved = std::getenv("COLUMNS");
    std::string keep = saved ? saved : "";
    ::setenv("COLUMNS", "123", 1);
    CHECK_EQ(dcc::help::terminal_width(), 123);
    ::setenv("COLUMNS", "0", 1);
    CHECK(dcc::help::terminal_width() > 0);
    ::setenv("COLUMNS", "junk", 1);
    CHECK(dcc::help::terminal_width() > 0);
    if (saved)
        ::setenv("COLUMNS", keep.c_str(), 1);
    else
        ::unsetenv("COLUMNS");
}

TEST_CASE("rows are aligned in a flag column and long descriptions wrap under it")
{
    std::vector<dcc::help::Row> rows{
        {"-c", "short"},
        {"--a-flag-that-is-longer-than-the-column", "wide flag"},
        {"-x", "one two three four five six seven eight nine ten eleven twelve thirteen"},
    };
    auto text = dcc::help::format_rows(rows, 60);
    std::string expected =
        "  -c                        short\n"
        "  --a-flag-that-is-longer-than-the-column\n"
        "                            wide flag\n"
        "  -x                        one two three four five six\n"
        "                            seven eight nine ten eleven\n"
        "                            twelve thirteen\n";
    CHECK_EQ(text, expected);
    CHECK_EQ(dcc::help::format_wrapped("aaa bbb ccc", 2, 10), std::string{"aaa bbb ccc\n"});
    CHECK(dcc::help::format_rows({}, 80).empty());
}
