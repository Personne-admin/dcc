import std;
import learndc.text;
import learndc.textedit;
import learndc.highlight;

#include "harness.hh"

namespace te = learndc::textedit;
namespace hl = learndc::highlight;
using te::Buffer;
using te::Pos;

namespace
{
    Buffer make(std::string_view text)
    {
        Buffer b;
        b.load(text);
        return b;
    }

    Buffer make_at(std::string_view text, std::size_t line, std::size_t col)
    {
        auto b = make(text);
        b.set_cursor({line, col});
        return b;
    }

    void type(Buffer& b, std::string_view s)
    {
        for (std::size_t i = 0; i < s.size();)
        {
            if (s[i] == '\n')
            {
                b.newline();
                ++i;
                continue;
            }
            auto const d = learndc::text::decode_utf8(s, i);
            b.insert(s.substr(i, d.length));
            i += d.length;
        }
    }

    using Lines = std::vector<std::string>;

    bool valid(Buffer const& b)
    {
        if (b.lines.empty())
            return false;
        if (b.cursor.line >= b.lines.size() || b.cursor.col > b.lines[b.cursor.line].size())
            return false;
        if (b.anchor && (b.anchor->line >= b.lines.size() || b.anchor->col > b.lines[b.anchor->line].size()))
            return false;
        for (auto const& l : b.lines)
        {
            for (std::size_t i = 0; i < l.size();)
            {
                auto const d = learndc::text::decode_utf8(l, i);
                if (!d.valid)
                    return false;
                i += d.length;
            }
            if (l.find('\n') != std::string::npos)
                return false;
        }
        auto on_boundary = [&](Pos p) { return p.col == b.lines[p.line].size() || (static_cast<unsigned char>(b.lines[p.line][p.col]) & 0xC0) != 0x80; };
        return on_boundary(b.cursor) && (!b.anchor || on_boundary(*b.anchor));
    }

    bool tiles(std::vector<hl::Span> const& spans, std::string_view line)
    {
        std::size_t pos = 0;
        for (auto const& s : spans)
        {
            if (s.begin != pos || s.end <= s.begin)
                return false;
            pos = s.end;
        }
        return pos == line.size();
    }

    std::optional<hl::Kind> kind_of(hl::Language lang, std::string_view line, std::string_view part, hl::State state = {})
    {
        auto const spans = hl::highlight_line(lang, line, state);
        auto const at = line.find(part);
        if (at == std::string_view::npos)
            return std::nullopt;
        std::optional<hl::Kind> result;
        for (std::size_t i = at; i < at + part.size(); ++i)
        {
            std::optional<hl::Kind> k;
            for (auto const& s : spans)
                if (i >= s.begin && i < s.end)
                    k = s.kind;
            if (!k)
                return std::nullopt;
            if (result && *result != *k)
                return std::nullopt;
            result = k;
        }
        return result;
    }

    std::optional<hl::Kind> dc(std::string_view line, std::string_view part)
    {
        return kind_of(hl::Language::Dc, line, part);
    }

    std::optional<hl::Kind> toml(std::string_view line, std::string_view part)
    {
        return kind_of(hl::Language::Toml, line, part);
    }

    std::optional<hl::Kind> md(std::string_view line, std::string_view part)
    {
        return kind_of(hl::Language::Markdown, line, part);
    }

    std::optional<hl::Kind> dcon(std::string_view line, std::string_view part)
    {
        return kind_of(hl::Language::Dcon, line, part);
    }

    std::vector<hl::Span> run(hl::Language lang, std::string_view line, hl::State& state)
    {
        return hl::highlight_line(lang, line, state);
    }

    struct Rng
    {
        std::uint64_t s;
        std::uint32_t next()
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<std::uint32_t>(s >> 33);
        }
        std::uint32_t below(std::uint32_t n) { return next() % n; }
    };
} // namespace

TEST_CASE("textedit round trips")
{
    std::vector<std::string> samples = {
        "",
        "a",
        "a\n",
        "a\nb",
        "a\nb\n",
        "\n",
        "\n\n",
        "\n\n\n",
        "a\r\nb\r\n",
        "a\r\nb",
        "\r\n",
        "\r\n\r\n",
        "\xEF\xBB\xBF"
        "hi\n",
        "\xEF\xBB\xBF",
        "\xEF\xBB\xBF"
        "a\r\nb\r\n",
        "h\xC3\xA9llo\n",
        "e\xCC\x81x\n",
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\n",
        "a\rb\n",
        "a\r",
        "tab\there\n",
        "\xF0\x9F\x98\x80 emoji",
        "line one\nline two\n\nline four",
    };
    for (auto const& s : samples)
    {
        Buffer b;
        auto r = b.load(s);
        CHECK(r.editable);
        CHECK(r.reason.empty());
        CHECK(b.text() == s);
        CHECK(!b.lines.empty());
    }
}

TEST_CASE("textedit load detection")
{
    SECTION("lf");
    auto b = make("a\nb\n");
    CHECK(b.eol == te::Eol::Lf);
    CHECK(b.final_newline);
    CHECK(!b.bom);
    CHECK_EQ(b.lines.size(), 2u);

    SECTION("crlf");
    b = make("a\r\nb\r\nc");
    CHECK(b.eol == te::Eol::Crlf);
    CHECK(!b.final_newline);
    CHECK_EQ(b.lines.size(), 3u);
    CHECK_EQ(b.lines[0], "a");
    CHECK_EQ(b.lines[1], "b");

    SECTION("majority");
    b = make("a\r\nb\r\nc\nd\r\n");
    CHECK(b.eol == te::Eol::Crlf);
    b = make("a\nb\nc\r\nd\n");
    CHECK(b.eol == te::Eol::Lf);
    CHECK_EQ(b.lines[2], "c\r");

    SECTION("lone cr");
    b = make("a\rb\nc\n");
    CHECK(b.eol == te::Eol::Lf);
    CHECK_EQ(b.lines[0], "a\rb");

    SECTION("bom");
    b = make("\xEF\xBB\xBFx\n");
    CHECK(b.bom);
    CHECK_EQ(b.lines[0], "x");

    SECTION("empty");
    b = make("");
    CHECK_EQ(b.lines.size(), 1u);
    CHECK(b.lines[0].empty());
    CHECK(!b.final_newline);
    CHECK(!b.modified());

    SECTION("mixed endings normalise to the majority");
    b = make("a\r\nb\r\nc\n");
    CHECK_EQ(b.text(), "a\r\nb\r\nc\r\n");

    SECTION("reload resets state");
    b = make("abc");
    type(b, "x");
    CHECK(b.modified());
    b.load("fresh");
    CHECK(!b.modified());
    CHECK(!b.undo());
    CHECK_EQ(b.cursor, (Pos{0, 0}));
}

TEST_CASE("textedit limits")
{
    SECTION("nul");
    Buffer b;
    auto r = b.load(std::string_view("ab\0cd", 5));
    CHECK(!r.editable);
    CHECK(r.reason.find("NUL") != std::string::npos);
    CHECK(b.read_only);
    CHECK(!b.lines.empty());

    SECTION("invalid utf8");
    for (std::string_view bad : {"\xFF", "a\x80z", "\xC0\x80", "\xE3\x81", "\xED\xA0\x80", "\xF5\x80\x80\x80", "ok\xC3"})
    {
        Buffer c;
        auto rr = c.load(bad);
        CHECK(!rr.editable);
        CHECK(rr.reason.find("UTF-8") != std::string::npos);
        CHECK(c.read_only);
        CHECK(valid(c));
    }

    SECTION("too large");
    Buffer d;
    te::Limits lim;
    lim.max_bytes = 10;
    auto rd = d.load("0123456789a", lim);
    CHECK(!rd.editable);
    CHECK(rd.reason.find("larger") != std::string::npos);
    CHECK(d.read_only);
    rd = d.load("0123456789", lim);
    CHECK(rd.editable);
    CHECK(!d.read_only);

    SECTION("too many lines");
    Buffer e;
    te::Limits ll;
    ll.max_lines = 3;
    auto re = e.load("a\nb\nc\nd", ll);
    CHECK(!re.editable);
    CHECK(re.reason.find("lines") != std::string::npos);
    CHECK(e.read_only);
    CHECK_EQ(e.lines.size(), 3u);
    re = e.load("a\nb\nc\n", ll);
    CHECK(re.editable);
    CHECK_EQ(e.lines.size(), 3u);

    SECTION("editable again after a good load");
    Buffer f;
    f.load("\xFF");
    CHECK(f.read_only);
    f.load("fine");
    CHECK(!f.read_only);
    CHECK(f.insert("x"));
    CHECK_EQ(f.text(), "xfine");

    SECTION("large preview is truncated");
    Buffer g;
    te::Limits gl;
    gl.max_bytes = 8;
    g.load("abc\ndef\nghi\njkl\n", gl);
    CHECK(g.read_only);
    CHECK(g.lines.size() <= 3u);
    CHECK(valid(g));
}

TEST_CASE("textedit read only buffers refuse edits")
{
    Buffer b;
    b.load("\xFF text\nsecond");
    REQUIRE(b.read_only);
    auto const before = b.lines;
    CHECK(!b.insert("x"));
    CHECK(!b.paste("x"));
    CHECK(!b.newline());
    CHECK(!b.backspace());
    CHECK(!b.del());
    CHECK(!b.tab());
    CHECK(!b.untab());
    CHECK(!b.delete_line());
    CHECK(!b.duplicate_line());
    CHECK(!b.undo());
    CHECK(!b.redo());
    b.select_all();
    CHECK(b.cut().empty());
    CHECK(b.lines == before);
    CHECK(!b.modified());
    b.buffer_start();
    b.right();
    CHECK_EQ(b.cursor, (Pos{0, 3}));
    CHECK(b.find_next("second", true));
    CHECK(valid(b));
}

TEST_CASE("textedit typing and insertion")
{
    auto b = make("");
    type(b, "abc");
    CHECK_EQ(b.text(), "abc");
    CHECK_EQ(b.cursor, (Pos{0, 3}));
    CHECK(b.modified());

    SECTION("multi-line insert");
    Buffer c = make("ad");
    c.set_cursor({0, 1});
    CHECK(c.insert("b\nc"));
    CHECK(c.lines == (Lines{"ab", "cd"}));
    CHECK_EQ(c.cursor, (Pos{1, 1}));

    SECTION("crlf and cr become breaks");
    Buffer d = make("");
    d.insert("a\r\nb\rc\nd");
    CHECK(d.lines == (Lines{"a", "b", "c", "d"}));
    CHECK_EQ(d.cursor, (Pos{3, 1}));

    SECTION("control characters are dropped");
    Buffer e = make("");
    e.insert(std::string_view("a\x01"
                              "b\x7F"
                              "c\0d",
                              7));
    CHECK_EQ(e.lines[0], "abcd");
    CHECK(!e.insert(""));
    CHECK(!e.insert("\x01"));

    SECTION("invalid bytes are dropped");
    Buffer f = make("");
    f.insert("a\xFF"
             "b");
    CHECK_EQ(f.lines[0], "ab");

    SECTION("tabs stay");
    Buffer g = make("");
    g.insert("a\tb");
    CHECK_EQ(g.lines[0], "a\tb");

    SECTION("unicode");
    Buffer h = make("");
    type(h, "h\xC3\xA9llo \xE6\x97\xA5");
    CHECK_EQ(h.lines[0], "h\xC3\xA9llo \xE6\x97\xA5");
    CHECK_EQ(h.cursor.col, h.lines[0].size());

    SECTION("replaces selection");
    Buffer i = make("hello world");
    i.select_range({0, 0}, {0, 5});
    i.insert("X");
    CHECK_EQ(i.text(), "X world");
    CHECK_EQ(i.cursor, (Pos{0, 1}));
    CHECK(!i.has_selection());
    CHECK(!i.anchor.has_value());

    SECTION("replaces multi-line selection");
    Buffer j = make("one\ntwo\nthree");
    j.select_range({0, 1}, {2, 2});
    j.insert("-");
    CHECK_EQ(j.text(), "o-ree");

    SECTION("empty buffer end insert");
    Buffer k = make("a\n");
    k.buffer_end();
    CHECK_EQ(k.cursor, (Pos{0, 1}));
    k.insert("b");
    CHECK_EQ(k.text(), "ab\n");
}

TEST_CASE("textedit newline and auto indent")
{
    SECTION("copies indent");
    auto b = make_at("    foo", 0, 7);
    CHECK(b.newline());
    CHECK(b.lines == (Lines{"    foo", "    "}));
    CHECK_EQ(b.cursor, (Pos{1, 4}));

    SECTION("no indent");
    b = make_at("foo", 0, 3);
    b.newline();
    CHECK(b.lines == (Lines{"foo", ""}));
    CHECK_EQ(b.cursor, (Pos{1, 0}));

    SECTION("extra level after brace");
    b = make_at("if x {", 0, 6);
    b.newline();
    CHECK(b.lines == (Lines{"if x {", "    "}));
    CHECK_EQ(b.cursor, (Pos{1, 4}));

    SECTION("extra level after paren and bracket");
    b = make_at("  call(", 0, 7);
    b.newline();
    CHECK_EQ(b.lines[1], "      ");
    b = make_at("[", 0, 1);
    b.newline();
    CHECK_EQ(b.lines[1], "    ");

    SECTION("trailing spaces after opener are ignored");
    b = make_at("x {   ", 0, 6);
    b.newline();
    CHECK_EQ(b.lines[1], "    ");

    SECTION("block form");
    b = make_at("fn() {}", 0, 6);
    b.newline();
    CHECK(b.lines == (Lines{"fn() {", "    ", "}"}));
    CHECK_EQ(b.cursor, (Pos{1, 4}));

    SECTION("block form keeps indent");
    b = make_at("  {}", 0, 3);
    b.newline();
    CHECK(b.lines == (Lines{"  {", "      ", "  }"}));
    CHECK_EQ(b.cursor, (Pos{1, 6}));

    SECTION("block form with tabs");
    b = make("{}");
    b.insert_spaces = false;
    b.set_cursor({0, 1});
    b.newline();
    CHECK(b.lines == (Lines{"{", "\t", "}"}));
    CHECK_EQ(b.cursor, (Pos{1, 1}));

    SECTION("block form with inner space");
    b = make_at("{ }", 0, 1);
    b.newline();
    CHECK(b.lines == (Lines{"{", "    ", "}"}));

    SECTION("text after cursor moves down");
    b = make_at("    ab", 0, 5);
    b.newline();
    CHECK(b.lines == (Lines{"    a", "    b"}));
    CHECK_EQ(b.cursor, (Pos{1, 4}));

    SECTION("cursor inside indentation");
    b = make_at("    foo", 0, 2);
    b.newline();
    CHECK(b.lines == (Lines{"  ", "  foo"}));
    CHECK_EQ(b.cursor, (Pos{1, 2}));

    SECTION("replaces selection");
    b = make("ab cd");
    b.select_range({0, 2}, {0, 3});
    b.newline();
    CHECK(b.lines == (Lines{"ab", "cd"}));

    SECTION("tab indent unit");
    b = make_at("\t{", 0, 2);
    b.insert_spaces = false;
    b.newline();
    CHECK_EQ(b.lines[1], "\t\t");

    SECTION("custom width");
    b = make_at("{", 0, 1);
    b.indent_width = 2;
    b.newline();
    CHECK_EQ(b.lines[1], "  ");

    SECTION("blank line keeps indent on enter");
    b = make_at("    ", 0, 4);
    b.newline();
    CHECK(b.lines == (Lines{"    ", "    "}));

    SECTION("empty buffer");
    b = make("");
    b.newline();
    CHECK(b.lines == (Lines{"", ""}));
}

TEST_CASE("textedit closing bracket dedent")
{
    SECTION("brace");
    auto b = make_at("    ", 0, 4);
    b.insert("}");
    CHECK_EQ(b.lines[0], "}");
    CHECK_EQ(b.cursor, (Pos{0, 1}));

    SECTION("deeper");
    b = make_at("        ", 0, 8);
    b.insert("}");
    CHECK_EQ(b.lines[0], "    }");
    CHECK_EQ(b.cursor, (Pos{0, 5}));

    SECTION("paren and bracket");
    b = make_at("    ", 0, 4);
    b.insert(")");
    CHECK_EQ(b.lines[0], ")");
    b = make_at("      ", 0, 6);
    b.insert("]");
    CHECK_EQ(b.lines[0], "  ]");

    SECTION("not after text");
    b = make_at("a ", 0, 2);
    b.insert("}");
    CHECK_EQ(b.lines[0], "a }");

    SECTION("not for other characters");
    b = make_at("    ", 0, 4);
    b.insert("x");
    CHECK_EQ(b.lines[0], "    x");

    SECTION("tab indent");
    b = make_at("\t\t", 0, 2);
    b.insert("}");
    CHECK_EQ(b.lines[0], "\t}");

    SECTION("no whitespace");
    b = make("");
    b.insert("}");
    CHECK_EQ(b.lines[0], "}");

    SECTION("text after cursor stays");
    b = make_at("    x", 0, 4);
    b.insert("}");
    CHECK_EQ(b.lines[0], "}x");

    SECTION("typing a block");
    b = make("");
    type(b, "fn() {\nreturn 1;\n}");
    CHECK(b.lines == (Lines{"fn() {", "    return 1;", "}"}));

    SECTION("dedent is one undo step");
    b = make_at("    ", 0, 4);
    b.insert("}");
    b.undo();
    CHECK_EQ(b.lines[0], "    ");
    CHECK_EQ(b.cursor, (Pos{0, 4}));

    SECTION("narrower than a level");
    b = make_at("  ", 0, 2);
    b.insert("}");
    CHECK_EQ(b.lines[0], "}");
}

TEST_CASE("textedit backspace")
{
    SECTION("simple");
    auto b = make_at("abc", 0, 3);
    CHECK(b.backspace());
    CHECK_EQ(b.lines[0], "ab");
    CHECK_EQ(b.cursor, (Pos{0, 2}));

    SECTION("code point");
    b = make_at("a\xC3\xA9", 0, 3);
    b.backspace();
    CHECK_EQ(b.lines[0], "a");
    b = make_at("\xE6\x97\xA5", 0, 3);
    b.backspace();
    CHECK(b.lines[0].empty());

    SECTION("combining mark deleted alone");
    b = make_at("e\xCC\x81", 0, 3);
    b.backspace();
    CHECK_EQ(b.lines[0], "e");
    b.backspace();
    CHECK(b.lines[0].empty());

    SECTION("join lines");
    b = make_at("ab\ncd", 1, 0);
    CHECK(b.backspace());
    CHECK(b.lines == (Lines{"abcd"}));
    CHECK_EQ(b.cursor, (Pos{0, 2}));

    SECTION("start of buffer");
    b = make_at("ab", 0, 0);
    CHECK(!b.backspace());
    CHECK(!b.modified());

    SECTION("indent stops");
    b = make_at("      ", 0, 6);
    b.backspace();
    CHECK_EQ(b.lines[0], "    ");
    b.backspace();
    CHECK(b.lines[0].empty());

    SECTION("three spaces");
    b = make_at("   ", 0, 3);
    b.backspace();
    CHECK(b.lines[0].empty());

    SECTION("five spaces");
    b = make_at("     ", 0, 5);
    b.backspace();
    CHECK_EQ(b.lines[0], "    ");

    SECTION("before text");
    b = make_at("    x", 0, 4);
    b.backspace();
    CHECK_EQ(b.lines[0], "x");

    SECTION("middle of indent");
    b = make_at("      x", 0, 6);
    b.backspace();
    CHECK_EQ(b.lines[0], "    x");
    b = make_at("      x", 0, 5);
    b.backspace();
    CHECK_EQ(b.lines[0], "    "
                         " x");

    SECTION("spaces after text delete one");
    b = make_at("a    ", 0, 5);
    b.backspace();
    CHECK_EQ(b.lines[0], "a   ");

    SECTION("tab deleted whole");
    b = make_at("\tx", 0, 1);
    b.backspace();
    CHECK_EQ(b.lines[0], "x");

    SECTION("selection");
    b = make("hello");
    b.select_range({0, 1}, {0, 4});
    b.backspace();
    CHECK_EQ(b.lines[0], "ho");
    CHECK_EQ(b.cursor, (Pos{0, 1}));

    SECTION("custom indent width");
    b = make_at("      ", 0, 6);
    b.indent_width = 2;
    b.backspace();
    CHECK_EQ(b.lines[0], "    ");
    b = make_at("   ", 0, 3);
    b.indent_width = 3;
    b.backspace();
    CHECK(b.lines[0].empty());
}

TEST_CASE("textedit delete forward")
{
    auto b = make_at("abc", 0, 0);
    CHECK(b.del());
    CHECK_EQ(b.lines[0], "bc");
    CHECK_EQ(b.cursor, (Pos{0, 0}));

    SECTION("joins at line end");
    b = make_at("ab\ncd", 0, 2);
    CHECK(b.del());
    CHECK(b.lines == (Lines{"abcd"}));
    CHECK_EQ(b.cursor, (Pos{0, 2}));

    SECTION("end of buffer");
    b = make_at("ab", 0, 2);
    CHECK(!b.del());

    SECTION("unicode");
    b = make_at("\xC3\xA9"
                "a",
                0, 0);
    b.del();
    CHECK_EQ(b.lines[0], "a");

    SECTION("selection");
    b = make("hello");
    b.select_range({0, 4}, {0, 1});
    b.del();
    CHECK_EQ(b.lines[0], "ho");
    CHECK_EQ(b.cursor, (Pos{0, 1}));
}

TEST_CASE("textedit tab and untab")
{
    SECTION("insert to stop");
    auto b = make("");
    b.tab();
    CHECK_EQ(b.lines[0], "    ");
    CHECK_EQ(b.cursor, (Pos{0, 4}));
    b = make_at("ab", 0, 2);
    b.tab();
    CHECK_EQ(b.lines[0], "ab  ");
    b = make_at("abcd", 0, 4);
    b.tab();
    CHECK_EQ(b.lines[0], "abcd    ");

    SECTION("tab character");
    b = make_at("ab", 0, 2);
    b.insert_spaces = false;
    b.tab();
    CHECK_EQ(b.lines[0], "ab\t");

    SECTION("width");
    b = make_at("a", 0, 1);
    b.indent_width = 8;
    b.tab();
    CHECK_EQ(b.lines[0].size(), 8u);

    SECTION("wide characters count");
    b = make_at("\xE6\x97\xA5", 0, 3);
    b.tab();
    CHECK_EQ(b.lines[0], "\xE6\x97\xA5  ");

    SECTION("multi-line selection indents lines");
    b = make("a\nb\nc");
    b.select_range({0, 0}, {2, 1});
    CHECK(b.tab());
    CHECK(b.lines == (Lines{"    a", "    b", "    c"}));
    CHECK_EQ(*b.anchor, (Pos{0, 0}));
    CHECK_EQ(b.cursor, (Pos{2, 5}));

    SECTION("selection ending at column zero skips last line");
    b = make("a\nb\nc");
    b.select_range({0, 0}, {2, 0});
    b.tab();
    CHECK(b.lines == (Lines{"    a", "    b", "c"}));

    SECTION("empty lines are skipped");
    b = make("a\n\nb");
    b.select_range({0, 0}, {2, 1});
    b.tab();
    CHECK(b.lines == (Lines{"    a", "", "    b"}));

    SECTION("tab characters on selection");
    b = make("a\nb");
    b.insert_spaces = false;
    b.select_range({0, 0}, {1, 1});
    b.tab();
    CHECK(b.lines == (Lines{"\ta", "\tb"}));

    SECTION("single line selection is replaced");
    b = make("abc");
    b.select_range({0, 0}, {0, 2});
    b.tab();
    CHECK_EQ(b.lines[0], "    c");
    CHECK_EQ(b.cursor, (Pos{0, 4}));
    CHECK(!b.has_selection());

    SECTION("untab");
    b = make_at("    a", 0, 5);
    CHECK(b.untab());
    CHECK_EQ(b.lines[0], "a");
    CHECK_EQ(b.cursor, (Pos{0, 1}));

    SECTION("untab partial");
    b = make_at("  a", 0, 3);
    b.untab();
    CHECK_EQ(b.lines[0], "a");
    b = make_at("      a", 0, 7);
    b.untab();
    CHECK_EQ(b.lines[0], "  a");

    SECTION("untab tab");
    b = make_at("\ta", 0, 2);
    b.untab();
    CHECK_EQ(b.lines[0], "a");

    SECTION("untab nothing");
    b = make_at("a", 0, 1);
    CHECK(!b.untab());
    CHECK(!b.modified());

    SECTION("untab cursor in indentation");
    b = make_at("    a", 0, 2);
    b.untab();
    CHECK_EQ(b.cursor, (Pos{0, 0}));

    SECTION("untab selection");
    b = make("    a\n  b\nc\n        d");
    b.select_range({0, 1}, {3, 9});
    CHECK(b.untab());
    CHECK(b.lines == (Lines{"a", "b", "c", "    d"}));
    CHECK_EQ(*b.anchor, (Pos{0, 0}));
    CHECK_EQ(b.cursor, (Pos{3, 5}));

    SECTION("tab then untab round trip");
    b = make("a\nb");
    b.select_range({0, 0}, {1, 1});
    b.tab();
    b.untab();
    CHECK_EQ(b.text(), "a\nb");

    SECTION("tab is undoable as one step");
    b = make("a\nb");
    b.select_range({0, 0}, {1, 1});
    b.tab();
    CHECK(b.undo());
    CHECK_EQ(b.text(), "a\nb");
    CHECK_EQ(*b.anchor, (Pos{0, 0}));
    CHECK_EQ(b.cursor, (Pos{1, 1}));
    CHECK(!b.modified());
}

TEST_CASE("textedit line operations")
{
    SECTION("delete middle");
    auto b = make_at("a\nb\nc", 1, 0);
    CHECK(b.delete_line());
    CHECK(b.lines == (Lines{"a", "c"}));
    CHECK_EQ(b.cursor, (Pos{1, 0}));

    SECTION("delete last");
    b = make_at("a\nb\nc", 2, 0);
    b.delete_line();
    CHECK(b.lines == (Lines{"a", "b"}));
    CHECK_EQ(b.cursor.line, 1u);

    SECTION("delete first");
    b = make_at("a\nb\nc", 0, 1);
    b.delete_line();
    CHECK(b.lines == (Lines{"b", "c"}));
    CHECK_EQ(b.cursor.line, 0u);

    SECTION("delete only line");
    b = make_at("abc", 0, 2);
    b.delete_line();
    CHECK(b.lines == (Lines{""}));
    CHECK_EQ(b.cursor, (Pos{0, 0}));

    SECTION("delete keeps column");
    b = make_at("abcd\nefgh\nijkl", 1, 2);
    b.delete_line();
    CHECK_EQ(b.cursor, (Pos{1, 2}));

    SECTION("delete selected lines");
    b = make("a\nb\nc\nd");
    b.select_range({1, 0}, {2, 1});
    b.delete_line();
    CHECK(b.lines == (Lines{"a", "d"}));
    CHECK(!b.has_selection());

    SECTION("delete undo");
    b = make_at("a\nb\nc", 2, 1);
    b.delete_line();
    CHECK(b.undo());
    CHECK_EQ(b.text(), "a\nb\nc");
    CHECK_EQ(b.cursor, (Pos{2, 1}));

    SECTION("delete keeps final newline");
    b = make_at("a\nb\n", 1, 0);
    b.delete_line();
    CHECK_EQ(b.text(), "a\n");

    SECTION("duplicate");
    b = make_at("a\nb", 0, 1);
    CHECK(b.duplicate_line());
    CHECK(b.lines == (Lines{"a", "a", "b"}));
    CHECK_EQ(b.cursor, (Pos{1, 1}));

    SECTION("duplicate last");
    b = make_at("a\nb", 1, 1);
    b.duplicate_line();
    CHECK(b.lines == (Lines{"a", "b", "b"}));
    CHECK_EQ(b.cursor, (Pos{2, 1}));

    SECTION("duplicate selection");
    b = make("a\nb\nc");
    b.select_range({0, 0}, {1, 1});
    b.duplicate_line();
    CHECK(b.lines == (Lines{"a", "b", "a", "b", "c"}));
    CHECK_EQ(b.cursor, (Pos{3, 1}));
    CHECK_EQ(*b.anchor, (Pos{2, 0}));

    SECTION("duplicate undo");
    b = make_at("a\nb", 0, 1);
    b.duplicate_line();
    b.undo();
    CHECK_EQ(b.text(), "a\nb");
    CHECK_EQ(b.cursor, (Pos{0, 1}));
}

TEST_CASE("textedit horizontal movement")
{
    SECTION("left right across lines");
    auto b = make_at("ab\ncd", 0, 2);
    b.right();
    CHECK_EQ(b.cursor, (Pos{1, 0}));
    b.left();
    CHECK_EQ(b.cursor, (Pos{0, 2}));
    b.left();
    CHECK_EQ(b.cursor, (Pos{0, 1}));

    SECTION("edges");
    b = make_at("ab", 0, 0);
    b.left();
    CHECK_EQ(b.cursor, (Pos{0, 0}));
    b.buffer_end();
    b.right();
    CHECK_EQ(b.cursor, (Pos{0, 2}));

    SECTION("by code point");
    b = make_at("\xC3\xA9\xE6\x97\xA5x", 0, 0);
    b.right();
    CHECK_EQ(b.cursor.col, 2u);
    b.right();
    CHECK_EQ(b.cursor.col, 5u);
    b.right();
    CHECK_EQ(b.cursor.col, 6u);
    b.left();
    b.left();
    CHECK_EQ(b.cursor.col, 2u);
    b.left();
    CHECK_EQ(b.cursor.col, 0u);

    SECTION("select extends");
    b = make_at("hello", 0, 1);
    b.right(true);
    b.right(true);
    CHECK_EQ(*b.anchor, (Pos{0, 1}));
    CHECK_EQ(b.cursor, (Pos{0, 3}));
    CHECK_EQ(b.selected_text(), "el");
    b.left(true);
    CHECK_EQ(b.selected_text(), "e");
    b.left(true);
    CHECK(!b.has_selection());

    SECTION("collapse to sides");
    b = make("hello");
    b.select_range({0, 1}, {0, 4});
    b.left();
    CHECK_EQ(b.cursor, (Pos{0, 1}));
    CHECK(!b.anchor.has_value());
    b.select_range({0, 4}, {0, 1});
    b.right();
    CHECK_EQ(b.cursor, (Pos{0, 4}));
    CHECK(!b.has_selection());

    SECTION("left with selection backwards");
    b = make("hello");
    b.select_range({0, 4}, {0, 1});
    b.left();
    CHECK_EQ(b.cursor, (Pos{0, 1}));

    SECTION("select across lines");
    b = make_at("ab\ncd", 0, 1);
    b.right(true);
    b.right(true);
    CHECK_EQ(b.selected_text(), "b\n");

    SECTION("home end");
    b = make_at("  foo bar", 0, 6);
    b.home();
    CHECK_EQ(b.cursor.col, 2u);
    b.home();
    CHECK_EQ(b.cursor.col, 0u);
    b.home();
    CHECK_EQ(b.cursor.col, 2u);
    b.end();
    CHECK_EQ(b.cursor.col, 9u);
    b.end(true);
    CHECK(!b.has_selection());
    b.home(true);
    CHECK_EQ(b.selected_text(), "foo bar");

    SECTION("home without indent");
    b = make_at("foo", 0, 2);
    b.home();
    CHECK_EQ(b.cursor.col, 0u);
    b.home();
    CHECK_EQ(b.cursor.col, 0u);

    SECTION("end selects");
    b = make_at("abc", 0, 1);
    b.end(true);
    CHECK_EQ(b.selected_text(), "bc");

    SECTION("buffer start end");
    b = make_at("ab\ncd\nef", 1, 1);
    b.buffer_start();
    CHECK_EQ(b.cursor, (Pos{0, 0}));
    b.buffer_end();
    CHECK_EQ(b.cursor, (Pos{2, 2}));
    b.buffer_start(true);
    CHECK_EQ(b.selected_text(), "ab\ncd\nef");
    b.buffer_end(true);
    CHECK(!b.has_selection());

    SECTION("select all");
    b = make("ab\ncd");
    b.select_all();
    CHECK_EQ(b.selected_text(), "ab\ncd");
    CHECK_EQ(b.cursor, (Pos{1, 2}));
    CHECK(b.has_selection());
}

TEST_CASE("textedit vertical movement")
{
    SECTION("desired column survives short lines");
    auto b = make_at("abcdef\nab\nabcdef", 0, 5);
    b.down();
    CHECK_EQ(b.cursor, (Pos{1, 2}));
    b.down();
    CHECK_EQ(b.cursor, (Pos{2, 5}));
    b.up();
    b.up();
    CHECK_EQ(b.cursor, (Pos{0, 5}));

    SECTION("horizontal move resets desired");
    b = make_at("abcdef\nab\nabcdef", 0, 5);
    b.down();
    b.left();
    b.down();
    CHECK_EQ(b.cursor, (Pos{2, 1}));

    SECTION("wide characters");
    b = make_at("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\nabcdef", 0, 6);
    CHECK_EQ(b.display_col(b.cursor), 4u);
    b.down();
    CHECK_EQ(b.cursor, (Pos{1, 4}));
    b.up();
    CHECK_EQ(b.cursor, (Pos{0, 6}));

    SECTION("into the middle of a wide character");
    b = make_at("abc\n\xE6\x97\xA5\xE6\x9C\xAC", 0, 1);
    b.down();
    CHECK_EQ(b.cursor, (Pos{1, 3}));
    b = make_at("abcd\n\xE6\x97\xA5\xE6\x9C\xAC", 0, 3);
    b.down();
    CHECK_EQ(b.cursor, (Pos{1, 6}));

    SECTION("tabs");
    b = make_at("\tx\nabcdefgh", 0, 1);
    CHECK_EQ(b.display_col(b.cursor), 4u);
    b.down();
    CHECK_EQ(b.cursor, (Pos{1, 4}));
    b = make_at("abcdefgh\n\tx", 0, 4);
    b.down();
    CHECK_EQ(b.cursor, (Pos{1, 1}));

    SECTION("first and last line");
    b = make_at("abc\ndef", 0, 2);
    b.up();
    CHECK_EQ(b.cursor, (Pos{0, 0}));
    b = make_at("abc\ndef", 1, 1);
    b.down();
    CHECK_EQ(b.cursor, (Pos{1, 3}));

    SECTION("select");
    b = make_at("abc\ndef", 0, 1);
    b.down(true);
    CHECK_EQ(b.selected_text(), "bc\nd");
    b.up(true);
    CHECK(!b.has_selection());

    SECTION("unselected move clears selection");
    b = make_at("abc\ndef", 0, 1);
    b.right(true);
    b.down();
    CHECK(!b.anchor.has_value());
    CHECK_EQ(b.cursor, (Pos{1, 2}));

    SECTION("page");
    b = make_at("0\n1\n2\n3\n4\n5\n6", 0, 0);
    b.page_down(3);
    CHECK_EQ(b.cursor.line, 3u);
    b.page_down(10);
    CHECK_EQ(b.cursor.line, 6u);
    b.page_up(4);
    CHECK_EQ(b.cursor.line, 2u);
    b.page_up(10, true);
    CHECK_EQ(b.cursor.line, 0u);
    CHECK(b.has_selection());
}

TEST_CASE("textedit word movement")
{
    SECTION("ascii");
    auto b = make("foo bar_baz  qux");
    b.word_right();
    CHECK_EQ(b.cursor.col, 3u);
    b.word_right();
    CHECK_EQ(b.cursor.col, 11u);
    b.word_right();
    CHECK_EQ(b.cursor.col, 16u);
    b.word_left();
    CHECK_EQ(b.cursor.col, 13u);
    b.word_left();
    CHECK_EQ(b.cursor.col, 4u);
    b.word_left();
    CHECK_EQ(b.cursor.col, 0u);

    SECTION("punctuation runs");
    b = make("a.b");
    b.word_right();
    CHECK_EQ(b.cursor.col, 1u);
    b.word_right();
    CHECK_EQ(b.cursor.col, 2u);
    b.word_right();
    CHECK_EQ(b.cursor.col, 3u);
    b.word_left();
    CHECK_EQ(b.cursor.col, 2u);
    b.word_left();
    CHECK_EQ(b.cursor.col, 1u);
    b.word_left();
    CHECK_EQ(b.cursor.col, 0u);

    SECTION("operator runs");
    b = make("x == y");
    b.word_right();
    b.word_right();
    CHECK_EQ(b.cursor.col, 4u);

    SECTION("unicode words");
    b = make("h\xC3\xA9llo w\xC3\xB6rld");
    b.word_right();
    CHECK_EQ(b.cursor.col, 6u);
    b.word_right();
    CHECK_EQ(b.cursor.col, 13u);
    b.word_left();
    CHECK_EQ(b.cursor.col, 7u);
    b.word_left();
    CHECK_EQ(b.cursor.col, 0u);

    SECTION("cjk");
    b = make("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E abc");
    b.word_right();
    CHECK_EQ(b.cursor.col, 9u);
    b.word_right();
    CHECK_EQ(b.cursor.col, 13u);

    SECTION("across lines");
    b = make("ab\n  cd");
    b.set_cursor({0, 2});
    b.word_right();
    CHECK_EQ(b.cursor, (Pos{1, 0}));
    b.word_right();
    CHECK_EQ(b.cursor, (Pos{1, 4}));
    b.word_left();
    CHECK_EQ(b.cursor, (Pos{1, 2}));
    b.word_left();
    CHECK_EQ(b.cursor, (Pos{1, 0}));
    b.word_left();
    CHECK_EQ(b.cursor, (Pos{0, 2}));

    SECTION("trailing spaces stop at line end");
    b = make("ab   \ncd");
    b.set_cursor({0, 2});
    b.word_right();
    CHECK_EQ(b.cursor, (Pos{0, 5}));

    SECTION("select");
    b = make("foo bar");
    b.word_right(true);
    b.word_right(true);
    CHECK_EQ(b.selected_text(), "foo bar");
    b.word_left(true);
    CHECK_EQ(b.selected_text(), "foo ");
    b.buffer_end();
    b.word_left(true);
    CHECK_EQ(b.selected_text(), "bar");

    SECTION("digits and underscores");
    b = make("a1_b2 c");
    b.word_right();
    CHECK_EQ(b.cursor.col, 5u);

    SECTION("buffer edges");
    b = make("x");
    b.word_left();
    CHECK_EQ(b.cursor, (Pos{0, 0}));
    b.buffer_end();
    b.word_right();
    CHECK_EQ(b.cursor, (Pos{0, 1}));
}

TEST_CASE("textedit positions and display columns")
{
    SECTION("set cursor clamps");
    auto b = make("ab\ncd");
    b.set_cursor({9, 9});
    CHECK_EQ(b.cursor, (Pos{1, 2}));
    b.set_cursor({0, 9});
    CHECK_EQ(b.cursor, (Pos{0, 2}));
    b.set_cursor({0, 1}, true);
    CHECK(b.has_selection());
    b.set_cursor({0, 0});
    CHECK(!b.anchor.has_value());

    SECTION("snaps to boundary");
    b = make("\xC3\xA9x");
    b.set_cursor({0, 1});
    CHECK_EQ(b.cursor.col, 0u);

    SECTION("clamp");
    b = make("\xE6\x97\xA5");
    CHECK_EQ(b.clamp({0, 2}), (Pos{0, 0}));
    CHECK_EQ(b.clamp({0, 3}), (Pos{0, 3}));
    CHECK_EQ(b.clamp({5, 0}), (Pos{0, 3}));

    SECTION("goto line");
    b = make("abc\ndef\nghi");
    b.goto_line(2, 3);
    CHECK_EQ(b.cursor, (Pos{1, 2}));
    b.goto_line(3);
    CHECK_EQ(b.cursor, (Pos{2, 0}));
    b.goto_line(100, 100);
    CHECK_EQ(b.cursor, (Pos{2, 3}));
    b.goto_line(0, 0);
    CHECK_EQ(b.cursor, (Pos{0, 0}));

    SECTION("select range");
    b = make("abc\ndef");
    b.select_range({1, 2}, {0, 1});
    CHECK(b.has_selection());
    CHECK_EQ(b.selection_start(), (Pos{0, 1}));
    CHECK_EQ(b.selection_end(), (Pos{1, 2}));
    CHECK_EQ(b.cursor, (Pos{0, 1}));
    CHECK_EQ(*b.anchor, (Pos{1, 2}));
    b.select_range({9, 9}, {9, 9});
    CHECK(!b.has_selection());
    CHECK_EQ(b.cursor, (Pos{1, 3}));

    SECTION("selection start and end without selection");
    b = make_at("abc", 0, 2);
    CHECK(!b.has_selection());
    CHECK_EQ(b.selection_start(), (Pos{0, 2}));
    CHECK_EQ(b.selection_end(), (Pos{0, 2}));
    b.right(true);
    CHECK_EQ(b.selection_start(), (Pos{0, 2}));
    CHECK_EQ(b.selection_end(), (Pos{0, 3}));

    SECTION("select range resets the desired column");
    b = make("abcdef\nab\nabcdef");
    b.set_cursor({0, 5});
    b.down();
    b.select_range({2, 1}, {2, 1});
    b.up();
    CHECK_EQ(b.cursor, (Pos{1, 1}));

    SECTION("display col");
    b = make("a\tb\xE6\x97\xA5"
             "e\xCC\x81"
             "x");
    CHECK_EQ(b.display_col({0, 0}), 0u);
    CHECK_EQ(b.display_col({0, 1}), 1u);
    CHECK_EQ(b.display_col({0, 2}), 4u);
    CHECK_EQ(b.display_col({0, 3}), 5u);
    CHECK_EQ(b.display_col({0, 6}), 7u);
    CHECK_EQ(b.display_col({0, 7}), 8u);
    CHECK_EQ(b.display_col({0, 9}), 8u);
    CHECK_EQ(b.display_col({0, 10}), 9u);
    b.indent_width = 8;
    CHECK_EQ(b.display_col({0, 2}), 8u);

    SECTION("pos at display");
    b = make("abc\tx\xE6\x97\xA5y");
    CHECK_EQ(b.pos_at_display(0, 0), (Pos{0, 0}));
    CHECK_EQ(b.pos_at_display(0, 2), (Pos{0, 2}));
    CHECK_EQ(b.pos_at_display(0, 3), (Pos{0, 3}));
    CHECK_EQ(b.pos_at_display(0, 4), (Pos{0, 4}));
    CHECK_EQ(b.pos_at_display(0, 5), (Pos{0, 5}));
    CHECK_EQ(b.pos_at_display(0, 6), (Pos{0, 8}));
    CHECK_EQ(b.pos_at_display(0, 7), (Pos{0, 8}));
    CHECK_EQ(b.pos_at_display(0, 8), (Pos{0, 9}));
    CHECK_EQ(b.pos_at_display(0, 99), (Pos{0, 9}));
    CHECK_EQ(b.pos_at_display(7, 0), (Pos{0, 0}));

    SECTION("pos at display second line");
    b = make("a\nbcd");
    CHECK_EQ(b.pos_at_display(1, 2), (Pos{1, 2}));
    CHECK_EQ(b.pos_at_display(1, 50), (Pos{1, 3}));
}

TEST_CASE("textedit clipboard")
{
    SECTION("cut and paste multi-line");
    auto b = make("one\ntwo\nthree");
    b.select_range({0, 2}, {2, 2});
    CHECK_EQ(b.selected_text(), "e\ntwo\nth");
    auto cut = b.cut();
    CHECK_EQ(cut, "e\ntwo\nth");
    CHECK_EQ(b.text(), "onree");
    CHECK_EQ(b.cursor, (Pos{0, 2}));
    CHECK(!b.has_selection());
    CHECK(b.paste(cut));
    CHECK_EQ(b.text(), "one\ntwo\nthree");
    CHECK_EQ(b.cursor, (Pos{2, 2}));

    SECTION("cut without selection");
    b = make("abc");
    CHECK(b.cut().empty());
    CHECK(!b.modified());

    SECTION("paste crlf");
    b = make("");
    b.paste("a\r\nb\r\nc");
    CHECK(b.lines == (Lines{"a", "b", "c"}));

    SECTION("paste replaces selection");
    b = make("hello world");
    b.select_range({0, 6}, {0, 11});
    b.paste("there");
    CHECK_EQ(b.text(), "hello there");

    SECTION("paste does not auto indent");
    b = make_at("    x", 0, 5);
    b.paste("\ny");
    CHECK(b.lines == (Lines{"    x", "y"}));

    SECTION("empty paste");
    b = make("a");
    CHECK(!b.paste(""));

    SECTION("selected text without selection");
    b = make("abc");
    CHECK(b.selected_text().empty());

    SECTION("cut undo");
    b = make("hello world");
    b.select_range({0, 0}, {0, 5});
    b.cut();
    b.undo();
    CHECK_EQ(b.text(), "hello world");
    CHECK_EQ(*b.anchor, (Pos{0, 0}));
    CHECK_EQ(b.cursor, (Pos{0, 5}));

    SECTION("unicode clipboard");
    b = make("\xE6\x97\xA5\xE6\x9C\xAC x");
    b.select_range({0, 0}, {0, 6});
    CHECK_EQ(b.cut(), "\xE6\x97\xA5\xE6\x9C\xAC");
    CHECK_EQ(b.text(), " x");
}

TEST_CASE("textedit undo and redo")
{
    SECTION("typing groups");
    auto b = make("");
    type(b, "abc");
    CHECK_EQ(b.undo_depth(), 1u);
    CHECK(b.undo());
    CHECK_EQ(b.text(), "");
    CHECK_EQ(b.cursor, (Pos{0, 0}));
    CHECK(b.redo());
    CHECK_EQ(b.text(), "abc");
    CHECK_EQ(b.cursor, (Pos{0, 3}));
    CHECK(!b.redo());

    SECTION("newline breaks group");
    b = make("");
    type(b, "ab\ncd");
    CHECK_EQ(b.undo_depth(), 3u);
    b.undo();
    CHECK_EQ(b.text(), "ab\n");
    b.undo();
    CHECK_EQ(b.text(), "ab");
    b.undo();
    CHECK_EQ(b.text(), "");
    CHECK(!b.undo());

    SECTION("cursor jump breaks group");
    b = make("");
    type(b, "ab");
    b.right();
    type(b, "c");
    CHECK_EQ(b.undo_depth(), 2u);
    b = make("");
    type(b, "ab");
    b.set_cursor({0, 0});
    b.set_cursor({0, 2});
    type(b, "c");
    CHECK_EQ(b.undo_depth(), 2u);

    SECTION("paste breaks group");
    b = make("");
    type(b, "a");
    b.paste("b");
    type(b, "c");
    CHECK_EQ(b.undo_depth(), 3u);

    SECTION("backspace groups");
    b = make_at("abcd", 0, 4);
    b.backspace();
    b.backspace();
    b.backspace();
    CHECK_EQ(b.undo_depth(), 1u);
    b.undo();
    CHECK_EQ(b.text(), "abcd");
    CHECK_EQ(b.cursor, (Pos{0, 4}));
    b.redo();
    CHECK_EQ(b.text(), "a");
    CHECK_EQ(b.cursor, (Pos{0, 1}));

    SECTION("delete groups");
    b = make_at("abcd", 0, 0);
    b.del();
    b.del();
    CHECK_EQ(b.undo_depth(), 1u);
    b.undo();
    CHECK_EQ(b.text(), "abcd");

    SECTION("typing and backspace are separate groups");
    b = make("");
    type(b, "ab");
    b.backspace();
    type(b, "c");
    CHECK_EQ(b.undo_depth(), 3u);

    SECTION("restores selection");
    b = make("hello");
    b.select_range({0, 0}, {0, 3});
    b.insert("X");
    b.undo();
    CHECK_EQ(b.text(), "hello");
    CHECK_EQ(*b.anchor, (Pos{0, 0}));
    CHECK_EQ(b.cursor, (Pos{0, 3}));
    b.redo();
    CHECK_EQ(b.text(), "Xlo");
    CHECK(!b.anchor.has_value());
    CHECK_EQ(b.cursor, (Pos{0, 1}));

    SECTION("new edit clears redo");
    b = make("");
    type(b, "ab");
    b.undo();
    type(b, "x");
    CHECK(!b.redo());
    CHECK_EQ(b.redo_depth(), 0u);

    SECTION("empty history");
    b = make("abc");
    CHECK(!b.undo());
    CHECK(!b.redo());

    SECTION("no-op edits leave history alone");
    b = make("abc");
    type(b, "x");
    b.undo();
    CHECK(b.redo_depth() == 1u);
    b.buffer_start();
    CHECK(!b.backspace());
    CHECK(!b.untab());
    CHECK(b.redo_depth() == 1u);
    CHECK(b.redo());

    SECTION("newline block form");
    b = make_at("{}", 0, 1);
    b.newline();
    b.undo();
    CHECK_EQ(b.text(), "{}");
    CHECK_EQ(b.cursor, (Pos{0, 1}));
    b.redo();
    CHECK(b.lines == (Lines{"{", "    ", "}"}));
    CHECK_EQ(b.cursor, (Pos{1, 4}));

    SECTION("join lines");
    b = make_at("ab\ncd", 1, 0);
    b.backspace();
    b.undo();
    CHECK_EQ(b.text(), "ab\ncd");
    CHECK_EQ(b.cursor, (Pos{1, 0}));

    SECTION("multi-line paste");
    b = make("x");
    b.paste("a\nb\nc");
    b.undo();
    CHECK_EQ(b.text(), "x");
    b.redo();
    CHECK_EQ(b.text(), "a\nb\ncx");

    SECTION("undo then edit then undo");
    b = make("");
    type(b, "one");
    b.newline();
    type(b, "two");
    b.undo();
    b.undo();
    CHECK_EQ(b.text(), "one");
    type(b, "!");
    CHECK_EQ(b.text(), "one!");
    b.undo();
    CHECK_EQ(b.text(), "one");
    b.undo();
    CHECK_EQ(b.text(), "");
}

TEST_CASE("textedit modified tracking")
{
    auto b = make("abc");
    CHECK(!b.modified());
    auto const r0 = b.revision();
    type(b, "x");
    CHECK(b.modified());
    CHECK(b.revision() != r0);
    b.undo();
    CHECK(!b.modified());
    CHECK_EQ(b.revision(), r0);
    b.redo();
    CHECK(b.modified());
    b.mark_saved();
    CHECK(!b.modified());
    b.set_cursor({0, 0});
    type(b, "y");
    CHECK(b.modified());
    b.undo();
    CHECK(!b.modified());
    b.undo();
    CHECK(b.modified());
    b.redo();
    CHECK(!b.modified());

    SECTION("saving ends a typing group");
    b = make("");
    type(b, "ab");
    b.mark_saved();
    type(b, "c");
    CHECK(b.modified());
    b.undo();
    CHECK_EQ(b.text(), "ab");
    CHECK(!b.modified());

    SECTION("edit after undo past save never matches");
    b = make("");
    type(b, "ab");
    b.mark_saved();
    b.undo();
    type(b, "ab");
    CHECK(b.modified());

    SECTION("moves do not modify");
    b = make("abc");
    b.right();
    b.select_all();
    b.home();
    CHECK(!b.modified());
    CHECK_EQ(b.revision(), 0u);

    SECTION("revision distinct for every edit");
    b = make("");
    std::set<std::uint64_t> seen{b.revision()};
    for (int i = 0; i < 5; ++i)
    {
        b.paste("x");
        CHECK(seen.insert(b.revision()).second);
    }
}

TEST_CASE("textedit undo memory bound")
{
    SECTION("step count");
    Buffer b;
    te::Limits lim;
    lim.max_undo = 5;
    b.load("", lim);
    for (int i = 0; i < 20; ++i)
        b.paste("x");
    CHECK(b.undo_depth() <= 5u);
    int n = 0;
    while (b.undo())
        ++n;
    CHECK_EQ(n, 5);
    CHECK_EQ(b.text().size(), 15u);

    SECTION("byte bound");
    Buffer c;
    te::Limits lb;
    lb.max_undo_bytes = 100;
    c.load("", lb);
    for (int i = 0; i < 6; ++i)
        c.paste(std::string(60, 'a'));
    CHECK(c.undo_depth() <= 1u);
    CHECK_EQ(c.text().size(), 360u);

    SECTION("small edits fit");
    Buffer d;
    te::Limits ld;
    ld.max_undo_bytes = 100;
    d.load("", ld);
    for (int i = 0; i < 80; ++i)
        d.paste("ab");
    CHECK(d.undo_depth() <= 50u);
    CHECK(d.undo_depth() >= 40u);

    SECTION("oldest dropped first");
    Buffer e;
    te::Limits le;
    le.max_undo = 2;
    e.load("", le);
    e.paste("a");
    e.paste("b");
    e.paste("c");
    e.undo();
    e.undo();
    CHECK(!e.undo());
    CHECK_EQ(e.text(), "a");

    SECTION("dropped save point stays modified");
    Buffer f;
    te::Limits lf;
    lf.max_undo = 2;
    f.load("", lf);
    f.paste("a");
    f.mark_saved();
    f.paste("b");
    f.paste("c");
    f.undo();
    f.undo();
    CHECK(f.modified() || f.text() == "a");
    CHECK(valid(f));
}

TEST_CASE("textedit find")
{
    auto b = make("foo Bar foo\nbar\nFOO");
    using R = std::optional<std::pair<Pos, Pos>>;
    auto const m = [](Pos a, Pos c) { return R(std::pair{a, c}); };

    SECTION("forward");
    CHECK(b.find("foo", {0, 0}, true, true) == m({0, 0}, {0, 3}));
    CHECK(b.find("foo", {0, 1}, true, true) == m({0, 8}, {0, 11}));
    CHECK(b.find("foo", {0, 8}, true, true) == m({0, 8}, {0, 11}));

    SECTION("wrap forward");
    CHECK(b.find("foo", {0, 9}, true, true) == m({0, 0}, {0, 3}));
    CHECK(b.find("foo", {0, 9}, true, false) == m({2, 0}, {2, 3}));
    CHECK(b.find("foo", {2, 3}, true, false) == m({0, 0}, {0, 3}));

    SECTION("backward");
    CHECK(b.find("foo", {0, 8}, false, true) == m({0, 0}, {0, 3}));
    CHECK(b.find("foo", {0, 11}, false, true) == m({0, 8}, {0, 11}));
    CHECK(b.find("foo", {0, 0}, false, true) == m({0, 8}, {0, 11}));
    CHECK(b.find("foo", {0, 0}, false, false) == m({2, 0}, {2, 3}));
    CHECK(b.find("bar", {2, 0}, false, true) == m({1, 0}, {1, 3}));

    SECTION("case");
    CHECK(b.find("bar", {0, 0}, true, true) == m({1, 0}, {1, 3}));
    CHECK(b.find("bar", {0, 0}, true, false) == m({0, 4}, {0, 7}));
    CHECK(b.find("bAR", {0, 0}, true, false) == m({0, 4}, {0, 7}));

    SECTION("not found");
    CHECK(!b.find("zzz", {0, 0}, true, true).has_value());
    CHECK(!b.find("zzz", {0, 0}, false, true).has_value());
    CHECK(!b.find("", {0, 0}, true, true).has_value());

    SECTION("ascii case only");
    auto u = make("\xC3\x89"
                  "a");
    CHECK(!u.find("\xC3\xA9"
                  "a",
                  {0, 0}, true, false)
               .has_value());
    CHECK(u.find("\xC3\x89"
                 "A",
                 {0, 0}, true, false)
              .has_value());

    SECTION("find_next selects and cycles");
    b.set_cursor({0, 0});
    CHECK(b.find_next("foo", true));
    CHECK_EQ(*b.anchor, (Pos{0, 0}));
    CHECK_EQ(b.cursor, (Pos{0, 3}));
    CHECK(b.find_next("foo", true));
    CHECK_EQ(*b.anchor, (Pos{0, 8}));
    CHECK_EQ(b.cursor, (Pos{0, 11}));
    CHECK(b.find_next("foo", true));
    CHECK_EQ(*b.anchor, (Pos{0, 0}));
    CHECK_EQ(b.selected_text(), "foo");

    SECTION("find_prev");
    b.set_cursor({0, 0});
    CHECK(b.find_prev("foo", true));
    CHECK_EQ(*b.anchor, (Pos{0, 8}));
    CHECK(b.find_prev("foo", true));
    CHECK_EQ(*b.anchor, (Pos{0, 0}));

    SECTION("find_next from cursor");
    b.set_cursor({0, 5});
    CHECK(b.find_next("foo", false));
    CHECK_EQ(*b.anchor, (Pos{0, 8}));
    CHECK(b.find_next("foo", false));
    CHECK_EQ(*b.anchor, (Pos{2, 0}));

    SECTION("failed find keeps selection");
    b.select_range({0, 0}, {0, 2});
    CHECK(!b.find_next("zzz", true));
    CHECK_EQ(b.selected_text(), "fo");

    SECTION("single match");
    auto s = make("only one");
    CHECK(s.find_next("one", true));
    CHECK(s.find_next("one", true));
    CHECK_EQ(s.selected_text(), "one");

    SECTION("count");
    CHECK_EQ(b.count_matches("foo", true), 2u);
    CHECK_EQ(b.count_matches("foo", false), 3u);
    CHECK_EQ(b.count_matches("bar", false), 2u);
    CHECK_EQ(b.count_matches("", true), 0u);
    CHECK_EQ(b.count_matches("zzz", true), 0u);
    auto a = make("aaaa");
    CHECK_EQ(a.count_matches("aa", true), 2u);

    SECTION("find unicode");
    auto j = make("\xE6\x97\xA5\xE6\x9C\xAC \xE6\x97\xA5");
    CHECK(j.find("\xE6\x97\xA5", {0, 3}, true, true) == m({0, 7}, {0, 10}));
    CHECK_EQ(j.count_matches("\xE6\x97\xA5", true), 2u);

    SECTION("multi line needle is rejected");
    CHECK(!b.find("o\nb", {0, 0}, true, true).has_value());
}

TEST_CASE("textedit viewport")
{
    std::string text;
    for (int i = 0; i < 100; ++i)
        text += "line " + std::to_string(i) + "\n";
    auto b = make(text);
    te::View v;

    SECTION("stays");
    b.ensure_visible(v, 10, 40);
    CHECK_EQ(v.top, 0u);
    b.set_cursor({5, 0});
    b.ensure_visible(v, 10, 40);
    CHECK_EQ(v.top, 0u);

    SECTION("scrolls down with margin");
    b.set_cursor({9, 0});
    b.ensure_visible(v, 10, 40);
    CHECK_EQ(v.top, 1u);
    b.set_cursor({50, 0});
    b.ensure_visible(v, 10, 40);
    CHECK_EQ(v.top, 42u);

    SECTION("scrolls up with margin");
    v.top = 42;
    b.set_cursor({20, 0});
    b.ensure_visible(v, 10, 40);
    CHECK_EQ(v.top, 19u);
    b.set_cursor({0, 0});
    b.ensure_visible(v, 10, 40);
    CHECK_EQ(v.top, 0u);

    SECTION("last line");
    b.buffer_end();
    b.ensure_visible(v, 10, 40);
    CHECK(v.top <= b.cursor.line);
    CHECK(b.cursor.line < v.top + 10);
    CHECK_EQ(v.top, b.cursor.line - 9);

    SECTION("small heights");
    v.top = 0;
    b.set_cursor({3, 0});
    b.ensure_visible(v, 2, 40);
    CHECK_EQ(v.top, 2u);
    b.ensure_visible(v, 1, 40);
    CHECK_EQ(v.top, 3u);

    SECTION("invariant cursor always inside");
    Rng rng{7};
    for (int i = 0; i < 200; ++i)
    {
        b.set_cursor({rng.below(100), 0});
        auto const h = static_cast<int>(1 + rng.below(30));
        b.ensure_visible(v, h, 20);
        CHECK(b.cursor.line >= v.top);
        CHECK(b.cursor.line < v.top + static_cast<std::size_t>(h));
    }

    SECTION("horizontal");
    auto w = make(std::string(200, 'x'));
    te::View hv;
    w.set_cursor({0, 50});
    w.ensure_visible(hv, 5, 20);
    CHECK_EQ(hv.left, 31u);
    w.set_cursor({0, 31});
    w.ensure_visible(hv, 5, 20);
    CHECK_EQ(hv.left, 31u);
    w.set_cursor({0, 10});
    w.ensure_visible(hv, 5, 20);
    CHECK_EQ(hv.left, 10u);
    w.set_cursor({0, 0});
    w.ensure_visible(hv, 5, 20);
    CHECK_EQ(hv.left, 0u);

    SECTION("horizontal uses display columns");
    auto t = make("\t\t\t\t\t\t");
    te::View tv;
    t.set_cursor({0, 6});
    t.ensure_visible(tv, 5, 10);
    CHECK_EQ(tv.left, 15u);

    SECTION("zero sizes are ignored");
    te::View z{3, 4};
    b.set_cursor({50, 0});
    b.ensure_visible(z, 0, 10);
    b.ensure_visible(z, 10, 0);
    CHECK_EQ(z.top, 3u);
    CHECK_EQ(z.left, 4u);

    SECTION("stale view clamps");
    te::View s{500, 0};
    b.set_cursor({0, 0});
    b.ensure_visible(s, 10, 10);
    CHECK_EQ(s.top, 0u);
}

TEST_CASE("textedit robustness")
{
    SECTION("empty buffer operations");
    Buffer b;
    CHECK(!b.backspace());
    CHECK(!b.del());
    b.left();
    b.right();
    b.up();
    b.down();
    b.home();
    b.end();
    b.word_left();
    b.word_right();
    b.page_up(5);
    b.page_down(5);
    b.select_all();
    CHECK(b.cut().empty());
    CHECK(!b.untab());
    CHECK(!b.find_next("x", true));
    CHECK(b.count_matches("x", true) == 0u);
    CHECK(valid(b));
    CHECK(b.text().empty());
    CHECK(b.delete_line());
    CHECK(valid(b));

    SECTION("cursor beyond the buffer is clamped by edits");
    b = make("ab");
    b.cursor = {10, 10};
    b.insert("x");
    CHECK(valid(b));
    CHECK_EQ(b.text(), "abx");

    SECTION("anchor beyond the buffer");
    b = make("ab");
    b.anchor = Pos{9, 9};
    b.cursor = {0, 0};
    b.backspace();
    CHECK(valid(b));

    SECTION("indent width extremes");
    b = make("");
    b.indent_width = 0;
    b.tab();
    CHECK(valid(b));
    b.indent_width = 100;
    b.tab();
    CHECK(valid(b));
    CHECK(b.lines[0].size() <= 120u);

    SECTION("randomised operations keep invariants and undo to the start");
    for (std::uint64_t seed = 1; seed <= 12; ++seed)
    {
        Rng rng{seed};
        Buffer f = make("alpha beta\n  gamma {\n}\n\xE6\x97\xA5\xE6\x9C\xAC e\xCC\x81\n");
        auto const original = f.text();
        bool ok = true;
        std::vector<std::string> snapshots{original};
        for (int i = 0; i < 300 && ok; ++i)
        {
            bool sel = rng.below(3) == 0;
            switch (rng.below(24))
            {
                case 0:
                    type(f, "x");
                    break;
                case 1:
                    type(f, "\xC3\xA9");
                    break;
                case 2:
                    f.newline();
                    break;
                case 3:
                    f.backspace();
                    break;
                case 4:
                    f.del();
                    break;
                case 5:
                    f.tab();
                    break;
                case 6:
                    f.untab();
                    break;
                case 7:
                    f.left(sel);
                    break;
                case 8:
                    f.right(sel);
                    break;
                case 9:
                    f.up(sel);
                    break;
                case 10:
                    f.down(sel);
                    break;
                case 11:
                    f.home(sel);
                    break;
                case 12:
                    f.end(sel);
                    break;
                case 13:
                    f.word_left(sel);
                    break;
                case 14:
                    f.word_right(sel);
                    break;
                case 15:
                    f.paste("p\nq {\n}");
                    break;
                case 16:
                    f.cut();
                    break;
                case 17:
                    f.delete_line();
                    break;
                case 18:
                    f.duplicate_line();
                    break;
                case 19:
                    type(f, "}");
                    break;
                case 20:
                    f.select_all();
                    break;
                case 21:
                    f.find_next("a", false);
                    break;
                case 22:
                    f.page_down(3, sel);
                    break;
                default:
                    f.set_cursor({rng.below(8), rng.below(12)}, sel);
                    break;
            }
            ok = valid(f);
        }
        CHECK(ok);
        auto const final_text = f.text();
        std::size_t guard = 0;
        while (f.undo() && ++guard < 5000)
            CHECK(valid(f));
        CHECK_EQ(f.text(), original);
        CHECK(!f.modified());
        guard = 0;
        while (f.redo() && ++guard < 5000)
            CHECK(valid(f));
        CHECK_EQ(f.text(), final_text);
    }
}

TEST_CASE("highlight language detection")
{
    CHECK(hl::language_for("main.dc") == hl::Language::Dc);
    CHECK(hl::language_for("src/main.dc") == hl::Language::Dc);
    CHECK(hl::language_for("a.b.dc") == hl::Language::Dc);
    CHECK(hl::language_for("project.dcon") == hl::Language::Dcon);
    CHECK(hl::language_for("catalog.toml") == hl::Language::Toml);
    CHECK(hl::language_for("README.md") == hl::Language::Markdown);
    CHECK(hl::language_for("notes.txt") == hl::Language::Plain);
    CHECK(hl::language_for("Makefile") == hl::Language::Plain);
    CHECK(hl::language_for("dir.dc/file") == hl::Language::Plain);
    CHECK(hl::language_for("") == hl::Language::Plain);
    CHECK(hl::language_for("x.dcx") == hl::Language::Plain);
}

TEST_CASE("highlight plain and tiling")
{
    hl::State st;
    auto spans = hl::highlight_line(hl::Language::Plain, "anything // here", st);
    CHECK_EQ(spans.size(), 1u);
    CHECK(spans[0].kind == hl::Kind::Plain);
    CHECK(tiles(spans, "anything // here"));
    for (auto lang : {hl::Language::Plain, hl::Language::Dc, hl::Language::Dcon, hl::Language::Toml, hl::Language::Markdown})
    {
        hl::State s;
        CHECK(hl::highlight_line(lang, "", s).empty());
        CHECK(tiles(hl::highlight_line(lang, "", s), ""));
    }

    std::string const alphabet = "ab_Z09 \t\"'\\/*#`[]=.:-+@{}()\xC3\xA9\xE6\x97\xA5<>,;~!";
    std::vector<std::string> pieces;
    for (std::size_t i = 0; i < alphabet.size();)
    {
        auto const d = learndc::text::decode_utf8(alphabet, i);
        pieces.emplace_back(alphabet.substr(i, d.length));
        i += d.length;
    }
    pieces.push_back("if");
    pieces.push_back("u8");
    pieces.push_back("\"\"\"");
    pieces.push_back("```");
    pieces.push_back("/*");
    pieces.push_back("*/");
    pieces.push_back("//");
    pieces.push_back("0x");
    pieces.push_back("::");
    pieces.push_back("true");
    Rng rng{99};
    for (auto lang : {hl::Language::Dc, hl::Language::Dcon, hl::Language::Toml, hl::Language::Markdown})
    {
        hl::State state;
        bool ok = true;
        for (int n = 0; n < 400; ++n)
        {
            std::string line;
            auto const len = rng.below(14);
            for (std::uint32_t k = 0; k < len; ++k)
                line += pieces[rng.below(static_cast<std::uint32_t>(pieces.size()))];
            auto const out = hl::highlight_line(lang, line, state);
            ok = ok && tiles(out, line);
        }
        CHECK(ok);
    }
}

TEST_CASE("highlight dc")
{
    using K = hl::Kind;
    SECTION("keywords and types");
    CHECK(dc("if x", "if") == K::Keyword);
    CHECK(dc("return 1;", "return") == K::Keyword);
    CHECK(dc("while true {}", "while") == K::Keyword);
    CHECK(dc("while true {}", "true") == K::Keyword);
    CHECK(dc("x = null;", "null") == K::Keyword);
    CHECK(dc("import std::io;", "import") == K::Keyword);
    CHECK(dc("public struct P {}", "public") == K::Keyword);
    CHECK(dc("public struct P {}", "struct") == K::Keyword);
    CHECK(dc("x as u8", "as") == K::Keyword);
    CHECK(dc("sizeof(x)", "sizeof") == K::Keyword);
    CHECK(dc("defer free(p);", "defer") == K::Keyword);
    CHECK(dc("match x {", "match") == K::Keyword);
    CHECK(dc("let i32 count", "i32") == K::Type);
    CHECK(dc("x: usize", "usize") == K::Type);
    CHECK(dc("f64 y", "f64") == K::Type);
    CHECK(dc("void f()", "void") == K::Type);
    CHECK(dc("bool b", "bool") == K::Type);
    CHECK(dc("char c", "char") == K::Type);
    CHECK(dc("p: null_t", "null_t") == K::Type);
    CHECK(dc("a: Array(i32)", "Array") == K::Type);

    SECTION("identifiers stay plain");
    CHECK(dc("iffy = 1", "iffy") == K::Plain);
    CHECK(dc("if_x = 1", "if_x") == K::Plain);
    CHECK(dc("foo(bar)", "foo") == K::Plain);
    CHECK(dc("i128 x", "i128") == K::Plain);
    CHECK(dc("_private", "_private") == K::Plain);
    CHECK(dc("fn main()", "fn") == K::Plain);

    SECTION("comments");
    CHECK(dc("// hello if 1", "// hello if 1") == K::Comment);
    CHECK(dc("x = 1; // trailing", "// trailing") == K::Comment);
    CHECK(dc("x = 1; // trailing", "x") == K::Plain);
    CHECK(dc("/// doc", "/// doc") == K::Comment);
    CHECK(dc("a /* b */ c", "/* b */") == K::Comment);
    CHECK(dc("a /* b */ c", "c") == K::Plain);
    CHECK(dc("\"// not\"", "// not") == K::String);

    SECTION("block comments across lines");
    hl::State st;
    auto s1 = run(hl::Language::Dc, "x /* start", st);
    CHECK(st.in_block_comment);
    CHECK(tiles(s1, "x /* start"));
    auto s2 = run(hl::Language::Dc, "middle if 1 \"s\"", st);
    CHECK(st.in_block_comment);
    CHECK_EQ(s2.size(), 1u);
    CHECK(s2[0].kind == K::Comment);
    CHECK(kind_of(hl::Language::Dc, "end */ if", "end */", st) == K::Comment);
    CHECK(kind_of(hl::Language::Dc, "end */ if", "if", st) == K::Keyword);
    auto s3 = run(hl::Language::Dc, "end */ if", st);
    CHECK(!st.in_block_comment);
    CHECK(tiles(s3, "end */ if"));
    auto s4 = run(hl::Language::Dc, "a /* b */ c /* d", st);
    CHECK(st.in_block_comment);
    auto s5 = run(hl::Language::Dc, "", st);
    CHECK(st.in_block_comment);
    CHECK(s5.empty());
    auto s6 = run(hl::Language::Dc, "*/", st);
    CHECK(!st.in_block_comment);
    CHECK(s4.back().kind == K::Comment);
    CHECK(s6[0].kind == K::Comment);

    SECTION("block comment opener inside line comment");
    hl::State st2;
    run(hl::Language::Dc, "// /* nope", st2);
    CHECK(!st2.in_block_comment);

    SECTION("strings");
    CHECK(dc("s = \"abc\";", "\"abc\"") == K::String);
    CHECK(dc("s = \"a\\\"b\" + 1", "\"a\\\"b\"") == K::String);
    CHECK(dc("s = \"a\\\"b\" + 1", "1") == K::Number);
    CHECK(dc("s = \"unterminated", "\"unterminated") == K::String);
    CHECK(dc("s = u\"wide\";", "u\"wide\"") == K::String);
    CHECK(dc("\"if 12 // x\"", "if 12 // x") == K::String);
    CHECK(dc("s = \"caf\xC3\xA9\"", "caf\xC3\xA9") == K::String);

    SECTION("multiline string lines");
    CHECK(dc("    \\\\ some text if 1", "\\\\ some text if 1") == K::String);
    CHECK(dc("\\\\", "\\\\") == K::String);
    CHECK(dc("x \\\\ y", "\\\\") == K::Punct);

    SECTION("chars");
    CHECK(dc("c = 'x';", "'x'") == K::Char);
    CHECK(dc("c = '\\n';", "'\\n'") == K::Char);
    CHECK(dc("c = '\\'';", "'\\''") == K::Char);
    CHECK(dc("c = u'x';", "u'x'") == K::Char);
    CHECK(dc("c = '\xC3\xA9';", "'\xC3\xA9'") == K::Char);
    CHECK(dc("c = 'x", "'") == K::Punct);

    SECTION("numbers");
    CHECK(dc("x = 42;", "42") == K::Number);
    CHECK(dc("x = 0xDEAD_beef;", "0xDEAD_beef") == K::Number);
    CHECK(dc("x = 0b1010;", "0b1010") == K::Number);
    CHECK(dc("x = 0o17;", "0o17") == K::Number);
    CHECK(dc("x = 1_000_000;", "1_000_000") == K::Number);
    CHECK(dc("x = 3.14;", "3.14") == K::Number);
    CHECK(dc("x = 1e10;", "1e10") == K::Number);
    CHECK(dc("x = 2.5e-3;", "2.5e-3") == K::Number);
    CHECK(dc("x = 2.5E+3;", "2.5E+3") == K::Number);
    CHECK(dc("a[0]", "0") == K::Number);
    CHECK(dc("x = 1..5", "1") == K::Number);
    CHECK(dc("x = 1..5", "5") == K::Number);
    CHECK(dc("v1 = 2", "v1") == K::Plain);

    SECTION("attributes");
    CHECK(dc("@nomangle public i32 f()", "@nomangle") == K::Attribute);
    CHECK(dc("@noreturn", "@noreturn") == K::Attribute);
    CHECK(dc("x @ y", "@") == K::Punct);

    SECTION("punctuation");
    CHECK(dc("std::io::print", "::") == K::Punct);
    CHECK(dc("std::io::print", "std") == K::Plain);
    CHECK(dc("a = b + c;", "=") == K::Punct);
    CHECK(dc("a = b + c;", "+") == K::Punct);
    CHECK(dc("f(x);", "(") == K::Punct);
    CHECK(dc("f(x);", ";") == K::Punct);
    CHECK(dc("a -> b", "->") == K::Punct);
    CHECK(dc("p.x", ".") == K::Punct);

    SECTION("whole line");
    hl::State s;
    auto spans = run(hl::Language::Dc, "@nomangle public i32 dcc_main() { return 0; } // done", s);
    CHECK(tiles(spans, "@nomangle public i32 dcc_main() { return 0; } // done"));
    CHECK(spans.front().kind == K::Attribute);
    CHECK(spans.back().kind == K::Comment);
}

TEST_CASE("highlight dcon")
{
    using K = hl::Kind;
    CHECK(dcon("    name = \"booleans\",", "name") == K::Key);
    CHECK(dcon("    name = \"booleans\",", "=") == K::Punct);
    CHECK(dcon("    name = \"booleans\",", "\"booleans\"") == K::String);
    CHECK(dcon("    version = 3,", "3") == K::Number);
    CHECK(dcon("dpm::v1::Project {", "dpm::v1::Project") == K::Type);
    CHECK(dcon("dpm::v1::Project {", "{") == K::Punct);
    CHECK(dcon("{ name = \"x\", entry = \"y\" },", "name") == K::Key);
    CHECK(dcon("{ name = \"x\", entry = \"y\" },", "entry") == K::Key);
    CHECK(dcon("{ name = \"x\", entry = \"y\" },", "\"y\"") == K::String);
    CHECK(dcon("    enabled = true,", "true") == K::Keyword);
    CHECK(dcon("    enabled = true,", "enabled") == K::Key);
    CHECK(dcon("    // comment", "// comment") == K::Comment);
    CHECK(dcon("\"quoted key\" = 1", "\"quoted key\"") == K::Key);
    CHECK(dcon("x = 1 == 2", "x") == K::Key);
    CHECK(dcon("a == b", "a") == K::Plain);
    CHECK(dcon("Project", "Project") == K::Type);
    CHECK(dcon("size = 0x10,", "0x10") == K::Number);
    hl::State st;
    run(hl::Language::Dcon, "/* open", st);
    CHECK(st.in_block_comment);
    CHECK(dcon("}", "}") == K::Punct);
}

TEST_CASE("highlight toml")
{
    using K = hl::Kind;
    SECTION("headers");
    CHECK(toml("[package]", "[package]") == K::Heading);
    CHECK(toml("[[bin]]", "[[bin]]") == K::Heading);
    CHECK(toml("[a.b]", "[a.b]") == K::Heading);
    CHECK(toml("  [indented]", "[indented]") == K::Heading);
    CHECK(toml("[a] # c", "# c") == K::Comment);
    CHECK(toml("[a] # c", "[a]") == K::Heading);
    CHECK(toml("[broken", "[broken") == K::Heading);

    SECTION("keys and values");
    CHECK(toml("name = \"x\"", "name") == K::Key);
    CHECK(toml("name = \"x\"", "=") == K::Punct);
    CHECK(toml("name = \"x\"", "\"x\"") == K::String);
    CHECK(toml("a.b.c = 1", "a.b.c") == K::Key);
    CHECK(toml("my-key = 1", "my-key") == K::Key);
    CHECK(toml("1234 = 1", "1234") == K::Key);
    CHECK(toml("\"a b\" = 1", "\"a b\"") == K::Key);
    CHECK(toml("a = 'lit'", "'lit'") == K::String);
    CHECK(toml("a = \"esc \\\" q\"", "\"esc \\\" q\"") == K::String);

    SECTION("comments");
    CHECK(toml("# whole", "# whole") == K::Comment);
    CHECK(toml("a = 1 # tail", "# tail") == K::Comment);
    CHECK(toml("a = \"# not\"", "# not") == K::String);

    SECTION("numbers and keywords");
    CHECK(toml("n = 42", "42") == K::Number);
    CHECK(toml("n = -42", "-42") == K::Number);
    CHECK(toml("n = +3", "+3") == K::Number);
    CHECK(toml("f = 1.5e3", "1.5e3") == K::Number);
    CHECK(toml("h = 0xFF", "0xFF") == K::Number);
    CHECK(toml("d = 1979-05-27", "1979-05-27") == K::Number);
    CHECK(toml("d = 1979-05-27T07:32:00Z", "1979-05-27T07:32:00Z") == K::Number);
    CHECK(toml("b = true", "true") == K::Keyword);
    CHECK(toml("b = false", "false") == K::Keyword);
    CHECK(toml("f = inf", "inf") == K::Number);
    CHECK(toml("arr = [1, 2]", "1") == K::Number);
    CHECK(toml("arr = [1, 2]", "[") == K::Punct);
    CHECK(toml("t = { a = 1, b = true }", "a") == K::Key);
    CHECK(toml("t = { a = 1, b = true }", "b") == K::Key);
    CHECK(toml("t = { a = 1, b = true }", "true") == K::Keyword);

    SECTION("multi-line basic strings");
    hl::State st;
    auto l1 = run(hl::Language::Toml, "text = \"\"\"first", st);
    CHECK(st.in_multiline_string);
    CHECK(tiles(l1, "text = \"\"\"first"));
    CHECK(l1.front().kind == K::Key);
    auto l2 = run(hl::Language::Toml, "# not a comment [x] = 1", st);
    CHECK(st.in_multiline_string);
    CHECK_EQ(l2.size(), 1u);
    CHECK(l2[0].kind == K::String);
    auto l3 = run(hl::Language::Toml, "esc \\\"\"\" still", st);
    CHECK(st.in_multiline_string);
    CHECK(l3[0].kind == K::String);
    auto l4 = run(hl::Language::Toml, "end\"\"\" # c", st);
    CHECK(!st.in_multiline_string);
    CHECK(tiles(l4, "end\"\"\" # c"));
    CHECK(l4.front().kind == K::String);
    CHECK(l4.back().kind == K::Comment);

    SECTION("multi-line literal strings");
    hl::State ls;
    run(hl::Language::Toml, "p = '''", ls);
    CHECK(ls.in_multiline_string);
    auto m1 = run(hl::Language::Toml, "a \"\"\" b", ls);
    CHECK(ls.in_multiline_string);
    CHECK_EQ(m1.size(), 1u);
    run(hl::Language::Toml, "'''", ls);
    CHECK(!ls.in_multiline_string);

    SECTION("single line triple quotes");
    hl::State ts;
    CHECK(kind_of(hl::Language::Toml, "a = \"\"\"x\"\"\" # c", "\"\"\"x\"\"\"", ts) == K::String);
    CHECK(kind_of(hl::Language::Toml, "a = \"\"\"x\"\"\" # c", "# c", ts) == K::Comment);
    run(hl::Language::Toml, "a = \"\"\"x\"\"\"", ts);
    CHECK(!ts.in_multiline_string);

    SECTION("header after a closed string line");
    hl::State hs;
    run(hl::Language::Toml, "a = \"\"\"", hs);
    run(hl::Language::Toml, "\"\"\"", hs);
    CHECK(kind_of(hl::Language::Toml, "[next]", "[next]", hs) == K::Heading);
}

TEST_CASE("highlight markdown")
{
    using K = hl::Kind;
    SECTION("headings");
    CHECK(md("# Title", "# Title") == K::Heading);
    CHECK(md("### Sub heading", "### Sub heading") == K::Heading);
    CHECK(md("###### six", "###### six") == K::Heading);
    CHECK(md("####### seven", "####### seven") == K::Plain);
    CHECK(md("#nospace", "#nospace") == K::Plain);
    CHECK(md("   # indented", "# indented") == K::Heading);
    CHECK(md("    # code block", "# code block") == K::Plain);
    CHECK(md("#", "#") == K::Heading);
    CHECK(md("plain text", "plain text") == K::Plain);

    SECTION("inline code");
    CHECK(md("use `foo` and `bar`", "`foo`") == K::String);
    CHECK(md("use `foo` and `bar`", "`bar`") == K::String);
    CHECK(md("use `foo` and `bar`", " and ") == K::Plain);
    CHECK(md("``a`b``", "``a`b``") == K::String);
    CHECK(md("a ` b", "`") == K::Plain);
    CHECK(md("`x`y", "`x`") == K::String);
    CHECK(md("`# not heading`", "`# not heading`") == K::String);

    SECTION("fences");
    hl::State st;
    auto o = run(hl::Language::Markdown, "```dc", st);
    CHECK(st.in_multiline_string);
    CHECK_EQ(o.size(), 1u);
    CHECK(o[0].kind == K::Punct);
    auto c = run(hl::Language::Markdown, "let x = `y`; // # not", st);
    CHECK(st.in_multiline_string);
    CHECK_EQ(c.size(), 1u);
    CHECK(c[0].kind == K::Plain);
    auto h = run(hl::Language::Markdown, "# still code", st);
    CHECK(h[0].kind == K::Plain);
    auto short_fence = run(hl::Language::Markdown, "``", st);
    CHECK(st.in_multiline_string);
    CHECK(short_fence[0].kind == K::Plain);
    auto e = run(hl::Language::Markdown, "```", st);
    CHECK(!st.in_multiline_string);
    CHECK_EQ(e.size(), 1u);
    CHECK(e[0].kind == K::Punct);
    CHECK(md("after", "after") == K::Plain);
    CHECK(kind_of(hl::Language::Markdown, "# after", "# after", st) == K::Heading);

    SECTION("tilde fences");
    hl::State ts;
    run(hl::Language::Markdown, "~~~", ts);
    CHECK(ts.in_multiline_string);
    run(hl::Language::Markdown, "```", ts);
    CHECK(ts.in_multiline_string);
    run(hl::Language::Markdown, "~~~", ts);
    CHECK(!ts.in_multiline_string);

    SECTION("longer fences need longer closers");
    hl::State ls;
    run(hl::Language::Markdown, "````md", ls);
    run(hl::Language::Markdown, "```", ls);
    CHECK(ls.in_multiline_string);
    run(hl::Language::Markdown, "`````", ls);
    CHECK(!ls.in_multiline_string);

    SECTION("closing fence with trailing text does not close");
    hl::State ws;
    run(hl::Language::Markdown, "```", ws);
    run(hl::Language::Markdown, "``` text", ws);
    CHECK(ws.in_multiline_string);

    SECTION("indented fence");
    hl::State is;
    run(hl::Language::Markdown, "  ```rust", is);
    CHECK(is.in_multiline_string);
    CHECK(kind_of(hl::Language::Markdown, "  ```", "```", is) == K::Punct);
    run(hl::Language::Markdown, "  ```", is);
    CHECK(!is.in_multiline_string);

    SECTION("backtick info string cannot contain backticks");
    hl::State bs;
    run(hl::Language::Markdown, "``` a`b", bs);
    CHECK(!bs.in_multiline_string);
}
