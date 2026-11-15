import std;
import learndc.text;
import learndc.json;
import learndc.toml;
import learndc.platform;
import learndc.catalog;
import learndc.bundle;
import learndc.diff;
import learndc.settings;
import learndc.progress;
import learndc.workspace;
import learndc.toolchain;
import learndc.runner;
import learndc.watch;
import learndc.editor;
import learndc.doctor;
import learndc.app;
import learndc.cli;
import learndc.tui.model;
import learndc.tui;
import dcc.host.process;

#include <stdlib.h>
#define main harness_main
#include "harness.hh"
#undef main

namespace text = learndc::text;
namespace json = learndc::json;
namespace toml = learndc::toml;
namespace platform = learndc::platform;
namespace catalog = learndc::catalog;
namespace bundle = learndc::bundle;
namespace diff = learndc::diff;
namespace settings = learndc::settings;
namespace progress = learndc::progress;
namespace workspace = learndc::workspace;
namespace toolchain = learndc::toolchain;
namespace runner = learndc::runner;
namespace watch = learndc::watch;
namespace editor = learndc::editor;
namespace doctor = learndc::doctor;

namespace
{
    struct TempDir
    {
        std::filesystem::path path;

        TempDir()
        {
            std::random_device rd;
            path = std::filesystem::temp_directory_path() / std::format("learndc-test-{:08x}{:08x}", rd(), rd());
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        TempDir(TempDir const&) = delete;
        TempDir& operator=(TempDir const&) = delete;
    };

    void write_file(std::filesystem::path const& p, std::string_view data)
    {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    std::string exercise_toml(std::string_view track, std::string_view name, std::string_view prerequisites = "[]")
    {
        return std::format(R"(schema = 1
id = "{0}/{1}"
revision = 1
title = "Title {1}"
summary = "Summary of {1}"
kind = "fix-behavior"
chapter = "intro"
difficulty = "beginner"
estimated_minutes = 5
concepts = ["alpha", "beta gamma"]
tags = ["tagged"]
objectives = ["Do the thing"]
entry_file = "src/main.dc"
product = "prog"
prerequisites = {2}
requires = ["dcc", "dpm"]
min_dcc = "0.6.0"
platforms = ["linux", "windows"]
backends = ["llvm", "custom"]
starter_state = "fails-checks"

[[checks]]
kind = "build"
name = "Compiles"

[[checks]]
kind = "run"
name = "Prints"
expect_stdout = "hi\n"
)",
                           track, name, prerequisites);
    }

    constexpr std::string_view valid_hints = "## Hint 1\n\nfirst\n\n## Hint 2\n\nsecond\n\n## Hint 3\n\nthird\n";

    void add_exercise(catalog::ContentSource& s, std::string_view track, std::string_view name, std::string_view prerequisites = "[]")
    {
        auto const dir = std::format("tracks/{}/{}/", track, name);
        s.add(dir + "exercise.toml", exercise_toml(track, name, prerequisites));
        s.add(dir + "README.md", "# Readme\n");
        s.add(dir + "hints.md", std::string(valid_hints));
        s.add(dir + "explanation.md", "Because.\n");
        s.add(dir + "starter/project.dcon", "dpm::v1::Project { name = \"prog\" }\n");
        s.add(dir + "starter/src/main.dc", "module main;\n");
        s.add(dir + "solution/src/main.dc", "module main;\n// solved\n");
    }

    catalog::ContentSource base_source()
    {
        catalog::ContentSource s;
        s.label = "fixture";
        s.add("catalog.toml", "schema = 1\ntitle = \"Test\"\ntracks = [\"basics\", \"more\"]\n");
        s.add("tracks/basics/track.toml", "schema = 1\nid = \"basics\"\ntitle = \"Basics\"\nsummary = \"Start here\"\nexercises = [\"one\", \"two\"]\n");
        s.add("tracks/more/track.toml", "schema = 1\nid = \"more\"\ntitle = \"More\"\nsummary = \"Go on\"\nexercises = [\"three\"]\n");
        add_exercise(s, "basics", "one");
        add_exercise(s, "basics", "two", "[\"basics/one\"]");
        add_exercise(s, "more", "three", "[\"basics/two\"]");
        return s;
    }

    std::vector<catalog::ContentError> errors_of(catalog::ContentSource const& s)
    {
        auto loaded = catalog::Catalog::load(s);
        if (loaded)
            return {};
        return loaded.error();
    }

    bool has_error(std::vector<catalog::ContentError> const& errors, std::string_view needle, std::string_view file = {}, int line = 0, int column = 0)
    {
        return std::ranges::any_of(errors, [&](catalog::ContentError const& e) {
            return e.message.find(needle) != std::string::npos && (file.empty() || e.file == file) && (line == 0 || e.line == line) &&
                   (column == 0 || e.column == column);
        });
    }

    void dump_errors(std::vector<catalog::ContentError> const& errors)
    {
        for (auto const& e : errors)
            std::println(std::cerr, "      {}", e.format());
    }

    void edit(catalog::ContentSource& s, std::string const& path, std::string_view from, std::string_view to)
    {
        auto& content = s.files.at(path);
        auto const hit = content.find(from);
        if (hit == std::string::npos)
        {
            std::println(std::cerr, "    test setup error: '{}' not found in {}", from, path);
            test::check(false, "edit target present");
            return;
        }
        content.replace(hit, from.size(), to);
    }

    std::string const one_toml = "tracks/basics/one/exercise.toml";
    std::string const one_dir = "tracks/basics/one/";
} // namespace

SECTION("learndc.text");

TEST_CASE("sanitize removes escape sequences")
{
    CHECK_EQ(text::sanitize("\x1b[31mred\x1b[0m"), "red");
    CHECK_EQ(text::sanitize("a\x1b[1;32;40mb\x1b[K"), "ab");
    CHECK_EQ(text::sanitize("x\x1b]0;title\x07y"), "xy");
    CHECK_EQ(text::sanitize("x\x1b]8;;http://e.com\x1b\\link\x1b]8;;\x1b\\y"), "xlinky");
    CHECK_EQ(text::sanitize("x\x1bPdata\x1b\\y"), "xy");
    CHECK_EQ(text::sanitize("x\x1b(By"), "xy");
    CHECK_EQ(text::sanitize("x\x1b"), "x");
    CHECK_EQ(text::sanitize("x\x1b[31"), "x");
    CHECK_EQ(text::sanitize("x\x1b]unterminated"), "x");
    CHECK_EQ(text::sanitize("\x1b[?25l\x1b[2Jdone"), "done");
}

TEST_CASE("sanitize renders controls visibly and keeps newline and tab")
{
    CHECK_EQ(text::sanitize("a\nb\tc"), "a\nb\tc");
    CHECK_EQ(text::sanitize("a\x07z"), "a^Gz");
    CHECK_EQ(text::sanitize(std::string_view("a\0z", 3)), "a^@z");
    CHECK_EQ(text::sanitize("a\x7f"
                            "z"),
             "a^?z");
    CHECK_EQ(text::sanitize("a\x08z"), "a^Hz");
    CHECK_EQ(text::sanitize("a\xC2\x9B"
                            "z"),
             "a\\x9bz");
    CHECK_EQ(text::sanitize("a\xC2\x80"
                            "z"),
             "a\\x80z");
    CHECK_EQ(text::sanitize("\xE2\x80\xAE"
                            "x"),
             "\\u202ex");
}

TEST_CASE("sanitize handles carriage returns")
{
    CHECK_EQ(text::sanitize("a\r\nb\r\n"), "a\nb\n");
    CHECK_EQ(text::sanitize("a\rb"), "a^Mb");
    CHECK_EQ(text::sanitize("a\r"), "a^M");
    CHECK_EQ(text::sanitize("a\r\r\nb"), "a^M\nb");
}

TEST_CASE("sanitize replaces invalid UTF-8")
{
    CHECK_EQ(text::sanitize("a\xFF"
                            "b"),
             "a\xEF\xBF\xBD"
             "b");
    CHECK_EQ(text::sanitize("a\xE2\x82"), "a\xEF\xBF\xBD");
    CHECK_EQ(text::sanitize("a\x80"
                            "b"),
             "a\xEF\xBF\xBD"
             "b");
    CHECK(text::utf8_valid(text::sanitize("\xC0\x80\xED\xA0\x80\xF5\x80\x80\x80\xFF")));
    CHECK_EQ(text::sanitize("\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80"), "\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80");
    CHECK(text::sanitize("\xC0\x80").find("\xEF\xBF\xBD") != std::string::npos);
    CHECK(text::sanitize("\xED\xA0\x80").find("\xEF\xBF\xBD") != std::string::npos);
}

TEST_CASE("strip_ansi only removes sequences")
{
    CHECK_EQ(text::strip_ansi("\x1b[1mbold\x1b[0m\r\n\x01"), "bold\r\n\x01");
    CHECK_EQ(text::strip_ansi("plain"), "plain");
}

TEST_CASE("display width")
{
    CHECK_EQ(text::display_width(""), 0u);
    CHECK_EQ(text::display_width("abc"), 3u);
    CHECK_EQ(text::display_width("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"), 6u);
    CHECK_EQ(text::display_width("e\xCC\x81"), 1u);
    CHECK_EQ(text::display_width("\xC3\xA9"), 1u);
    CHECK_EQ(text::display_width("a\xE2\x80\x8B"
                                 "b"),
             2u);
    CHECK_EQ(text::display_width("\xF0\x9F\x98\x80"), 2u);
    CHECK_EQ(text::display_width("\xEF\xBC\xA1"), 2u);
    CHECK_EQ(text::display_width("\xFF"), 1u);
    CHECK_EQ(text::display_width("\xEA\xB0\x80"), 2u);
}

TEST_CASE("wrap by display width")
{
    CHECK((text::wrap("the quick brown fox", 10) == std::vector<std::string>{"the quick", "brown fox"}));
    CHECK((text::wrap("aaaa bbbb", 4) == std::vector<std::string>{"aaaa", "bbbb"}));
    CHECK((text::wrap("abcdefghij", 4) == std::vector<std::string>{"abcd", "efgh", "ij"}));
    CHECK((text::wrap("one\n\ntwo", 20) == std::vector<std::string>{"one", "", "two"}));
    CHECK((text::wrap("a\n", 5) == std::vector<std::string>{"a"}));
    CHECK((text::wrap("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 4) == std::vector<std::string>{"\xE6\x97\xA5\xE6\x9C\xAC", "\xE8\xAA\x9E"}));
    CHECK((text::wrap("  indented text here", 12) == std::vector<std::string>{"  indented", "  text here"}));
    CHECK((text::wrap("x", 0) == std::vector<std::string>{"x"}));
    for (auto const& line : text::wrap("some longer prose that must wrap at a narrow width without exceeding it", 15))
        CHECK(text::display_width(line) <= 15u);
}

TEST_CASE("truncate_middle")
{
    CHECK_EQ(text::truncate_middle("short", 10), "short");
    CHECK_EQ(text::truncate_middle("abcdefghijklmnop", 9), "abc...nop");
    CHECK_EQ(text::truncate_middle("abcdefghijklmnop", 8), "abc...op");
    CHECK_EQ(text::truncate_middle("abcdefghijklmnop", 3), "...");
    CHECK_EQ(text::truncate_middle("abcdefghijklmnop", 2), "..");
    auto const wide = text::truncate_middle("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 7);
    CHECK(text::display_width(wide) <= 7u);
    CHECK(text::utf8_valid(wide));
}

TEST_CASE("clamp_bytes respects UTF-8 boundaries")
{
    CHECK_EQ(text::clamp_bytes("hello", 10), "hello");
    CHECK_EQ(text::clamp_bytes("hello", 5), "hello");
    CHECK_EQ(text::clamp_bytes("hello world", 5), "hello\n[... 6 more bytes]");
    CHECK_EQ(text::clamp_bytes("a\xC3\xA9", 2), "a\n[... 2 more bytes]");
    CHECK_EQ(text::clamp_bytes("a\xC3\xA9z", 3), "a\xC3\xA9\n[... 1 more bytes]");
    CHECK_EQ(text::clamp_bytes("\xF0\x9F\x98\x80xyz", 3), "\n[... 7 more bytes]");
    CHECK_EQ(text::clamp_bytes("abc", 0), "\n[... 3 more bytes]");
    CHECK(text::utf8_valid(text::clamp_bytes("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 4)));
}

TEST_CASE("visible whitespace")
{
    CHECK_EQ(text::visible_whitespace("a  \nb\n"), "a\xC2\xB7\xC2\xB7\nb\xE2\x8F\x8E\n");
    CHECK_EQ(text::visible_whitespace("a b\tc"), "a b\xE2\x86\x92"
                                                 "c");
    CHECK_EQ(text::visible_whitespace("x\r\n"), "x\xE2\x90\x8D\xE2\x8F\x8E\n");
    CHECK_EQ(text::visible_whitespace("no newline"), "no newline");
    CHECK_EQ(text::visible_whitespace("trail  "), "trail\xC2\xB7\xC2\xB7");
    CHECK_EQ(text::visible_whitespace(""), "");
    CHECK_EQ(text::visible_whitespace_ascii("a  \nb\n"), "a..\nb$\n");
    CHECK_EQ(text::visible_whitespace_ascii("a\tb\r\n"), "a->b\\r$\n");
}

TEST_CASE("string helpers")
{
    CHECK((text::split_lines("a\nb\n") == std::vector<std::string>{"a", "b"}));
    CHECK((text::split_lines("a\n\nb") == std::vector<std::string>{"a", "", "b"}));
    CHECK(text::split_lines("").empty());
    CHECK((text::split_lines("\n") == std::vector<std::string>{""}));
    CHECK((text::split("a,b,,c", ',') == std::vector<std::string>{"a", "b", "", "c"}));
    CHECK_EQ(text::trim("  \t x y \r\n"), "x y");
    CHECK_EQ(text::trim("   "), "");
    CHECK_EQ(text::lower("AbC-9"), "abc-9");
    CHECK(text::starts_with_ci("Hello", "hEL"));
    CHECK(!text::starts_with_ci("He", "hel"));
    CHECK(text::contains_ci("Hello World", "O W"));
    CHECK(!text::contains_ci("Hello", "xyz"));
    CHECK_EQ(text::join(std::vector<std::string>{"a", "b", "c"}, ", "), "a, b, c");
    CHECK_EQ(text::join(std::vector<std::string>{}, ", "), "");
    CHECK_EQ(text::normalize_newlines("a\r\nb\rc\nd"), "a\nb\nc\nd");
    CHECK_EQ(text::replace_all("a.b.c", ".", "::"), "a::b::c");
    CHECK(text::utf8_valid("\xC3\xA9"));
    CHECK(!text::utf8_valid("\xC3"));
    std::string encoded;
    text::append_utf8(encoded, 0x1F600);
    CHECK_EQ(encoded, "\xF0\x9F\x98\x80");
}

SECTION("learndc.json");

TEST_CASE("json parses every value kind")
{
    auto parsed = json::parse(R"({"n":null,"t":true,"f":false,"i":-12,"d":2.5,"e":1e3,"s":"x","a":[1,[2],{}],"o":{"k":"v"}})");
    REQUIRE(parsed);
    auto const& j = *parsed;
    CHECK(j.find("n")->is_null());
    CHECK_EQ(j.find("t")->as_bool().value(), true);
    CHECK_EQ(j.find("f")->as_bool().value(), false);
    CHECK_EQ(j.find("i")->as_int().value(), -12);
    CHECK(j.find("d")->is_double());
    CHECK_EQ(j.find("d")->as_number().value(), 2.5);
    CHECK(j.find("e")->is_double());
    CHECK_EQ(j.find("e")->as_number().value(), 1000.0);
    CHECK_EQ(j.find("s")->as_string().value(), "x");
    CHECK_EQ(j.find("a")->size(), 3u);
    CHECK_EQ(j.find("o")->find("k")->as_string().value(), "v");
    CHECK(j.find("missing") == nullptr);
    CHECK(!j.find("i")->as_string().has_value());
}

TEST_CASE("json integers and large numbers")
{
    CHECK_EQ(json::parse("9223372036854775807")->as_int().value(), std::numeric_limits<std::int64_t>::max());
    CHECK_EQ(json::parse("-9223372036854775808")->as_int().value(), std::numeric_limits<std::int64_t>::min());
    auto big = json::parse("9223372036854775808");
    REQUIRE(big);
    CHECK(big->is_double());
    CHECK_EQ(json::parse("-0")->as_int().value(), 0);
    CHECK(json::parse("1E2")->is_double());
}

TEST_CASE("json string escapes")
{
    auto parsed = json::parse(R"("a\"b\\c\/d\b\f\n\r\t\u0041\u00e9\u65e5\ud83d\ude00")");
    REQUIRE(parsed);
    CHECK_EQ(parsed->as_string().value(), "a\"b\\c/d\b\f\n\r\tA\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80");
    CHECK_EQ(json::parse("\"\xE6\x97\xA5\"")->as_string().value(), "\xE6\x97\xA5");
    CHECK(!json::parse(R"("\ud83d")"));
    CHECK(!json::parse(R"("\ude00")"));
    CHECK(!json::parse(R"("\ud83d\u0041")"));
    CHECK(!json::parse(R"("\x41")"));
    CHECK(!json::parse(R"("\u12")"));
    CHECK(!json::parse(R"("\u12zz")"));
    CHECK(!json::parse(std::string("\"a\nb\"")));
    CHECK(!json::parse(std::string("\"a\x01") + "b\""));
    CHECK(!json::parse("\"\xFF\""));
}

TEST_CASE("json errors carry line and column")
{
    auto e = json::parse("{\n  \"a\": 1,\n  \"b\": ]\n}");
    REQUIRE(!e);
    CHECK_EQ(e.error().line, 3);
    CHECK_EQ(e.error().column, 8);
    CHECK(e.error().message.find("unexpected character") != std::string::npos);

    e = json::parse("");
    REQUIRE(!e);
    CHECK_EQ(e.error().line, 1);
    CHECK_EQ(e.error().column, 1);

    e = json::parse("[1, 2,]");
    REQUIRE(!e);
    CHECK(e.error().message.find("trailing comma") != std::string::npos);
    CHECK_EQ(e.error().column, 7);

    e = json::parse("{\"a\":1,}");
    REQUIRE(!e);
    CHECK(e.error().message.find("trailing comma") != std::string::npos);

    e = json::parse("{\"a\":1,\"a\":2}");
    REQUIRE(!e);
    CHECK(e.error().message.find("duplicate key") != std::string::npos);
    CHECK_EQ(e.error().column, 8);

    e = json::parse("[1] x");
    REQUIRE(!e);
    CHECK(e.error().message.find("after the JSON value") != std::string::npos);
    CHECK_EQ(e.error().column, 5);

    e = json::parse("\n\n  \"abc");
    REQUIRE(!e);
    CHECK_EQ(e.error().line, 3);
    CHECK_EQ(e.error().column, 3);
    CHECK(e.error().message.find("unterminated") != std::string::npos);
    CHECK_EQ(e.error().format(), "3:3: unterminated string");
}

TEST_CASE("json rejects invalid syntax")
{
    for (std::string_view bad :
         {"01",  "1.",  ".5",       "+1",     "-",     "1e",      "tru",      "nul", "[", "{", "{\"a\"}", "{\"a\":}", "{1:2}", "[1 2]", "{\"a\":1 \"b\":2}",
          "'a'", "NaN", "Infinity", "[1,,2]", "truex", "// c\n1", "1 /* c */"})
        CHECK(!json::parse(bad));
    std::string deep(300, '[');
    deep += std::string(300, ']');
    auto e = json::parse(deep);
    REQUIRE(!e);
    CHECK(e.error().message.find("too deep") != std::string::npos);
    CHECK(json::parse("\xEF\xBB\xBF[1]"));
    CHECK(json::parse(" \t\r\n [ 1 , 2 ] \n"));
}

TEST_CASE("json dump and round trip")
{
    auto j = json::Json::make_object();
    j.set("name", "learndc");
    j.set("count", 3);
    j.set("ratio", 0.5);
    j.set("whole", 2.0);
    j.set("ok", true);
    j.set("none", nullptr);
    auto& list = j.set("list", json::Json::make_array());
    list.push(1);
    list.push("two");
    j.set("empty_list", json::Json::make_array());
    j.set("empty_obj", json::Json::make_object());
    j.set("text", "q\"\\\n\t\x01 \xC3\xA9 \x7f");

    auto const pretty = j.dump();
    CHECK(pretty.starts_with("{\n  \"name\": \"learndc\",\n  \"count\": 3,"));
    CHECK(pretty.find("\"whole\": 2.0") != std::string::npos);
    CHECK(pretty.find("\"empty_list\": []") != std::string::npos);
    CHECK(pretty.find("\"empty_obj\": {}") != std::string::npos);
    CHECK(pretty.find("\\u0001") != std::string::npos);
    CHECK(pretty.find("\xC3\xA9") != std::string::npos);
    CHECK(pretty.find("\\n\\t") != std::string::npos);

    auto const compact = j.dump(0);
    CHECK(compact.find('\n') == std::string::npos);
    CHECK(compact.starts_with("{\"name\":\"learndc\",\"count\":3,"));

    auto reparsed = json::parse(pretty);
    REQUIRE(reparsed);
    CHECK(*reparsed == j);
    auto reparsed_compact = json::parse(compact);
    REQUIRE(reparsed_compact);
    CHECK(*reparsed_compact == j);
    CHECK_EQ(reparsed->dump(), pretty);
    CHECK_EQ(json::Json(std::numeric_limits<double>::infinity()).dump(), "null");
}

TEST_CASE("json object order and mutation")
{
    auto j = json::Json::make_object();
    j.set("z", 1);
    j.set("a", 2);
    j.set("m", 3);
    j.set("a", 20);
    auto const& members = *j.as_object();
    CHECK_EQ(members.size(), 3u);
    CHECK_EQ(members[0].key, "z");
    CHECK_EQ(members[1].key, "a");
    CHECK_EQ(members[1].value.as_int().value(), 20);
    CHECK_EQ(members[2].key, "m");
    CHECK_EQ(j.dump(0), R"({"z":1,"a":20,"m":3})");
    json::Json copy = j;
    copy.set("z", 100);
    CHECK_EQ(j.find("z")->as_int().value(), 1);
    CHECK(!(copy == j));
    json::Json scalar = 5;
    scalar.push(1);
    CHECK(scalar.is_array());
    CHECK_EQ(scalar.size(), 1u);
}

SECTION("learndc.toml");

TEST_CASE("toml parses the supported subset")
{
    auto parsed = toml::parse(R"(# comment
schema = 1
title = "Learn \"DC\""   # trailing
literal = 'C:\path\n'
neg = -42
plus = +7
big = 1_000_000
yes = true
no = false
"quoted key" = "v"
'lit key' = "w"
list = ["a", "b",]
multi = [
    1,
    2, # two
    3,
]
nested = [[1, 2], ["x"]]
empty = []
unicode = "\u00e9\U0001F600"
ml = """
first
second \
   joined"""
mll = '''
raw \n text'''
quoted = """say ""hi"" """

[table]
a = 1

[[items]]
name = "one"

[[items]]
name = "two"

[outer.inner]
deep = true
)");
    REQUIRE(parsed);
    auto const& root = *parsed;
    CHECK_EQ(root.find("schema")->integer, 1);
    CHECK_EQ(root.find("title")->text, "Learn \"DC\"");
    CHECK_EQ(root.find("literal")->text, "C:\\path\\n");
    CHECK_EQ(root.find("neg")->integer, -42);
    CHECK_EQ(root.find("plus")->integer, 7);
    CHECK_EQ(root.find("big")->integer, 1000000);
    CHECK(root.find("yes")->boolean);
    CHECK(!root.find("no")->boolean);
    CHECK_EQ(root.find("quoted key")->text, "v");
    CHECK_EQ(root.find("lit key")->text, "w");
    CHECK_EQ(root.find("list")->items.size(), 2u);
    CHECK_EQ(root.find("multi")->items.size(), 3u);
    CHECK_EQ(root.find("nested")->items.size(), 2u);
    CHECK_EQ(root.find("nested")->items[0].items[1].integer, 2);
    CHECK(root.find("empty")->items.empty());
    CHECK_EQ(root.find("unicode")->text, "\xC3\xA9\xF0\x9F\x98\x80");
    CHECK_EQ(root.find("ml")->text, "first\nsecond joined");
    CHECK_EQ(root.find("mll")->text, "raw \\n text");
    CHECK_EQ(root.find("quoted")->text, "say \"\"hi\"\" ");
    CHECK_EQ(root.find("table")->find("a")->integer, 1);
    auto const* items = root.find("items");
    REQUIRE(items != nullptr);
    CHECK(items->is_table_array());
    CHECK_EQ(items->items.size(), 2u);
    CHECK_EQ(items->items[1].find("name")->text, "two");
    CHECK(root.find("outer")->find("inner")->find("deep")->boolean);
    CHECK(root.find("missing") == nullptr);
}

TEST_CASE("toml records positions")
{
    auto parsed = toml::parse("a = 1\n  bb = \"x\"\n\n[t]\nlist = [\n  7,\n  8\n]\n");
    REQUIRE(parsed);
    auto const& root = *parsed;
    CHECK(root.find("a")->pos == toml::Position({1, 5}));
    CHECK(root.find("bb")->pos == toml::Position({2, 8}));
    CHECK(root.find_entry("bb")->pos == toml::Position({2, 3}));
    CHECK(root.find("t")->pos == toml::Position({4, 1}));
    auto const* list = root.find("t")->find("list");
    CHECK(list->pos == toml::Position({5, 8}));
    CHECK(list->items[0].pos == toml::Position({6, 3}));
    CHECK(list->items[1].pos == toml::Position({7, 3}));
}

TEST_CASE("toml handles CRLF line endings and a BOM")
{
    auto parsed = toml::parse("\xEF\xBB\xBF"
                              "a = 1\r\nb = \"\"\"\r\nx\r\ny\"\"\"\r\n[t]\r\nc = [1,\r\n2]\r\n");
    REQUIRE(parsed);
    CHECK_EQ(parsed->find("a")->integer, 1);
    CHECK_EQ(parsed->find("b")->text, "x\ny");
    CHECK_EQ(parsed->find("t")->find("c")->items.size(), 2u);
    CHECK_EQ(parsed->find("t")->find("c")->items[1].pos.line, 7);
}

namespace
{
    void expect_toml_error(std::string_view source, std::string_view needle, int line, int column)
    {
        auto parsed = toml::parse(source);
        if (parsed)
        {
            std::println(std::cerr, "    expected a TOML error containing '{}' for: {}", needle, source);
            test::check(false, "toml error expected");
            return;
        }
        auto const& e = parsed.error();
        if (e.message.find(needle) == std::string::npos || e.pos.line != line || e.pos.column != column)
        {
            std::println(std::cerr, "    TOML error mismatch for: {}\n      got {}:{}: {}\n      want {}:{}: ...{}...", source, e.pos.line, e.pos.column,
                         e.message, line, column, needle);
            test::check(false, "toml error position");
            return;
        }
        test::check(true, "toml error");
    }
} // namespace

TEST_CASE("toml rejects unsupported constructs with positions")
{
    expect_toml_error("a.b = 1\n", "dotted keys", 1, 2);
    expect_toml_error("x = 1\nfoo.bar = 2\n", "dotted keys", 2, 4);
    expect_toml_error("a = { b = 1 }\n", "inline tables", 1, 5);
    expect_toml_error("a = 1.5\n", "floating-point", 1, 5);
    expect_toml_error("a = 1e5\n", "floating-point", 1, 5);
    expect_toml_error("a = inf\n", "floating-point", 1, 5);
    expect_toml_error("a = nan\n", "floating-point", 1, 5);
    expect_toml_error("a = 1979-05-27\n", "dates and times", 1, 5);
    expect_toml_error("a = 07:32:00\n", "dates and times", 1, 5);
    expect_toml_error("a = 0xFF\n", "hexadecimal", 1, 5);
    expect_toml_error("a = 0o7\n", "hexadecimal", 1, 5);
    expect_toml_error("a = 0b1\n", "hexadecimal", 1, 5);
}

TEST_CASE("toml rejects malformed input with positions")
{
    expect_toml_error("a = 1\na = 2\n", "duplicate key 'a'", 2, 1);
    expect_toml_error("[t]\n[t]\n", "defined more than once", 2, 2);
    expect_toml_error("t = 1\n[t]\n", "not a table", 2, 2);
    expect_toml_error("[[t]]\n[t]\n", "not a table", 2, 2);
    expect_toml_error("[t]\n[[t]]\n", "not an array of tables", 2, 3);
    expect_toml_error("a = \"x\nb = 1\n", "unterminated string", 1, 5);
    expect_toml_error("a = \"x", "unterminated string", 1, 5);
    expect_toml_error("a = 'x", "unterminated string", 1, 5);
    expect_toml_error("a = \"\"\"x", "unterminated string", 1, 5);
    expect_toml_error("a = [1, 2\n", "unterminated array", 1, 5);
    expect_toml_error("a = [1 2]\n", "expected ',' or ']'", 1, 8);
    expect_toml_error("a = [1,,2]\n", "unexpected ','", 1, 8);
    expect_toml_error("a = 1 b\n", "unexpected 'b'", 1, 7);
    expect_toml_error("a 1\n", "expected '='", 1, 3);
    expect_toml_error("= 1\n", "expected a key", 1, 1);
    expect_toml_error("a =\n", "expected a value", 1, 4);
    expect_toml_error("a = @\n", "unexpected '@'", 1, 5);
    expect_toml_error("a = \"\\q\"\n", "invalid escape", 1, 6);
    expect_toml_error("a = \"\\u12\"\n", "hexadecimal digit", 1, 10);
    expect_toml_error("a = \"\\uD800\"\n", "valid scalar value", 1, 6);
    expect_toml_error("a = 007\n", "leading zeros", 1, 5);
    expect_toml_error("a = 1__0\n", "underscores", 1, 5);
    expect_toml_error("a = 99999999999999999999\n", "out of range", 1, 5);
    expect_toml_error("a = -9223372036854775809\n", "out of range", 1, 5);
    expect_toml_error("a = true1\n", "invalid value", 1, 5);
    expect_toml_error("[a\n", "expected ']'", 1, 3);
    expect_toml_error("[[a]\n", "expected ']]'", 1, 5);
    expect_toml_error("[]\n", "expected a key", 1, 2);
    expect_toml_error("a = \"\"\"x\"\"\"\"\"\"\n", "too many quotes", 1, 9);
    expect_toml_error("a = 1\r b = 2\n", "bare carriage return", 1, 6);
    expect_toml_error("a = \"x\x01y\"\n", "control character", 1, 7);
    expect_toml_error("a = 1\n\xFF"
                      "b = 2\n",
                      "invalid UTF-8", 2, 1);
    expect_toml_error("\"\"\"k\"\"\" = 1\n", "multi-line strings cannot be keys", 1, 1);
    expect_toml_error("a = \"\"\"x\\ y\"\"\"\n", "invalid escape", 1, 9);
}

TEST_CASE("toml accepts edge cases")
{
    CHECK(toml::parse(""));
    CHECK(toml::parse("# only a comment"));
    CHECK(toml::parse("a = 1 # no trailing newline"));
    CHECK(toml::parse("a=1\nb=2"));
    CHECK(toml::parse("a = [ ]\nb = [\n]\n"));
    CHECK(toml::parse("a = \"\"\"\"\"\"\n"));
    CHECK(toml::parse("[a.b]\nx = 1\n[a]\ny = 2\n"));
    CHECK(toml::parse("[[a]]\n[a.b]\nx = 1\n[[a]]\n"));
    auto zero = toml::parse("a = 0\nb = -0\nc = 9223372036854775807\nd = -9223372036854775808\n");
    REQUIRE(zero);
    CHECK_EQ(zero->find("c")->integer, std::numeric_limits<std::int64_t>::max());
    CHECK_EQ(zero->find("d")->integer, std::numeric_limits<std::int64_t>::min());
    auto tail = toml::parse("a = \"\"\"x\"\"\"\"\n");
    REQUIRE(tail);
    CHECK_EQ(tail->find("a")->text, "x\"");
    toml::ParseError const err{{3, 4}, "boom"};
    CHECK_EQ(err.format("f.toml"), "f.toml:3:4: boom");
}

SECTION("learndc.platform");

TEST_CASE("resolve_dirs honours XDG variables")
{
    platform::Env env{{"HOME", "/home/u"}, {"XDG_CONFIG_HOME", "/x/config"}, {"XDG_DATA_HOME", "/x/data"}, {"XDG_CACHE_HOME", "/x/cache"}};
    auto dirs = platform::resolve_dirs({}, env, platform::Os::Linux);
    REQUIRE(dirs);
    CHECK_EQ(dirs->config_file.string(), "/x/config/learndc/config.json");
    CHECK_EQ(dirs->progress_file.string(), "/x/data/learndc/progress.json");
    CHECK_EQ(dirs->progress_lock.string(), "/x/data/learndc/progress.lock");
    CHECK_EQ(dirs->workspaces_dir.string(), "/x/data/learndc/workspaces");
    CHECK_EQ(dirs->backups_dir.string(), "/x/data/learndc/backups");
    CHECK_EQ(dirs->staging_dir.string(), "/x/cache/learndc/staging");
    CHECK_EQ(dirs->logs_dir.string(), "/x/cache/learndc/logs");
    CHECK_EQ(dirs->cache_dir.string(), "/x/cache/learndc");
    CHECK(!dirs->overridden);
}

TEST_CASE("resolve_dirs falls back to home directories")
{
    platform::Env env{{"HOME", "/home/u"}};
    auto dirs = platform::resolve_dirs({}, env, platform::Os::Linux);
    REQUIRE(dirs);
    CHECK_EQ(dirs->config_file.string(), "/home/u/.config/learndc/config.json");
    CHECK_EQ(dirs->progress_file.string(), "/home/u/.local/share/learndc/progress.json");
    CHECK_EQ(dirs->staging_dir.string(), "/home/u/.cache/learndc/staging");

    env["XDG_CONFIG_HOME"] = "relative/path";
    env["XDG_DATA_HOME"] = "";
    dirs = platform::resolve_dirs({}, env, platform::Os::Linux);
    REQUIRE(dirs);
    CHECK_EQ(dirs->config_file.string(), "/home/u/.config/learndc/config.json");
    CHECK_EQ(dirs->progress_file.string(), "/home/u/.local/share/learndc/progress.json");

    auto missing = platform::resolve_dirs({}, {}, platform::Os::Linux);
    REQUIRE(!missing);
    CHECK(missing.error().message.find("LEARNDC_HOME") != std::string::npos);
    auto relative_home = platform::resolve_dirs({}, {{"HOME", "rel"}}, platform::Os::Linux);
    CHECK(!relative_home);
}

TEST_CASE("resolve_dirs LEARNDC_HOME and explicit overrides")
{
    platform::Env env{{"HOME", "/home/u"}, {"XDG_CONFIG_HOME", "/x/config"}, {"LEARNDC_HOME", "/srv/ld"}};
    auto dirs = platform::resolve_dirs({}, env, platform::Os::Linux);
    REQUIRE(dirs);
    CHECK(dirs->overridden);
    CHECK_EQ(dirs->config_file.string(), "/srv/ld/config.json");
    CHECK_EQ(dirs->progress_file.string(), "/srv/ld/progress.json");
    CHECK_EQ(dirs->progress_lock.string(), "/srv/ld/progress.lock");
    CHECK_EQ(dirs->workspaces_dir.string(), "/srv/ld/workspaces");
    CHECK_EQ(dirs->backups_dir.string(), "/srv/ld/backups");
    CHECK_EQ(dirs->cache_dir.string(), "/srv/ld/cache");
    CHECK_EQ(dirs->staging_dir.string(), "/srv/ld/cache/staging");
    CHECK_EQ(dirs->logs_dir.string(), "/srv/ld/cache/logs");

    platform::Overrides over;
    over.home = std::filesystem::path("/other");
    dirs = platform::resolve_dirs(over, env, platform::Os::Linux);
    REQUIRE(dirs);
    CHECK_EQ(dirs->config_file.string(), "/other/config.json");

    env["LEARNDC_HOME"] = "";
    dirs = platform::resolve_dirs({}, env, platform::Os::Linux);
    REQUIRE(dirs);
    CHECK(!dirs->overridden);

    env["LEARNDC_HOME"] = "relative";
    dirs = platform::resolve_dirs({}, env, platform::Os::Linux);
    REQUIRE(dirs);
    CHECK(dirs->config_file.is_absolute());
    CHECK(dirs->config_file.string().ends_with("/relative/config.json"));
}

TEST_CASE("resolve_dirs Windows layout")
{
    platform::Env env{{"APPDATA", "/r"}, {"LOCALAPPDATA", "/l"}};
    auto dirs = platform::resolve_dirs({}, env, platform::Os::Windows);
    REQUIRE(dirs);
    CHECK_EQ(dirs->config_file.string(), "/r/learndc/config.json");
    CHECK_EQ(dirs->progress_file.string(), "/l/learndc/progress.json");
    CHECK_EQ(dirs->workspaces_dir.string(), "/l/learndc/workspaces");
    CHECK_EQ(dirs->backups_dir.string(), "/l/learndc/backups");
    CHECK_EQ(dirs->cache_dir.string(), "/l/learndc/cache");
    CHECK_EQ(dirs->staging_dir.string(), "/l/learndc/cache/staging");

    platform::Env mixed_case{{"appdata", "/r2"}, {"LocalAppData", "/l2"}};
    dirs = platform::resolve_dirs({}, mixed_case, platform::Os::Windows);
    REQUIRE(dirs);
    CHECK_EQ(dirs->config_file.string(), "/r2/learndc/config.json");
    CHECK_EQ(dirs->progress_file.string(), "/l2/learndc/progress.json");

    dirs = platform::resolve_dirs({}, {{"USERPROFILE", "/u"}}, platform::Os::Windows);
    REQUIRE(dirs);
    CHECK_EQ(dirs->config_file.string(), "/u/AppData/Roaming/learndc/config.json");
    CHECK_EQ(dirs->progress_file.string(), "/u/AppData/Local/learndc/progress.json");

    CHECK(!platform::resolve_dirs({}, {}, platform::Os::Windows));
}

TEST_CASE("atomic_write creates and replaces files")
{
    TempDir tmp;
    auto const target = tmp.path / "sub" / "deeper" / "data.json";
    REQUIRE(platform::atomic_write(target, "first"));
    CHECK_EQ(platform::read_file(target).value(), "first");
    REQUIRE(platform::atomic_write(target, "second, longer content"));
    CHECK_EQ(platform::read_file(target).value(), "second, longer content");
    REQUIRE(platform::atomic_write(target, ""));
    CHECK_EQ(platform::read_file(target).value(), "");
    std::string binary(100000, '\0');
    for (std::size_t i = 0; i < binary.size(); ++i)
        binary[i] = static_cast<char>(i * 31);
    REQUIRE(platform::atomic_write(target, binary));
    CHECK(platform::read_file(target).value() == binary);
    std::size_t entries = 0;
    for (auto const& entry : std::filesystem::directory_iterator(target.parent_path()))
    {
        (void)entry;
        ++entries;
    }
    CHECK_EQ(entries, 1u);
    CHECK(!platform::read_file(tmp.path / "absent").has_value());

    auto const blocker = tmp.path / "blocker";
    write_file(blocker, "x");
    auto failed = platform::atomic_write(blocker / "child.json", "data");
    REQUIRE(!failed);
    CHECK(!failed.error().message.empty());
}

TEST_CASE("file lock excludes a second holder")
{
    TempDir tmp;
    auto const lock_path = tmp.path / "locks" / "progress.lock";
    auto first = platform::FileLock::acquire(lock_path, std::chrono::milliseconds(0));
    REQUIRE(first);
    CHECK(first->locked());
    auto second = platform::FileLock::try_acquire(lock_path);
    REQUIRE(!second);
    CHECK(second.error().busy);

    auto const start = std::chrono::steady_clock::now();
    auto timed = platform::FileLock::acquire(lock_path, std::chrono::milliseconds(120));
    auto const waited = std::chrono::steady_clock::now() - start;
    REQUIRE(!timed);
    CHECK(timed.error().busy);
    CHECK(waited >= std::chrono::milliseconds(100));

    first->release();
    CHECK(!first->locked());
    auto third = platform::FileLock::try_acquire(lock_path);
    REQUIRE(third);

    platform::FileLock moved = std::move(*third);
    CHECK(moved.locked());
    CHECK(!third->locked());
    CHECK(!platform::FileLock::try_acquire(lock_path));
    {
        platform::FileLock scoped = std::move(moved);
    }
    CHECK(platform::FileLock::try_acquire(lock_path));

    std::atomic<bool> got{false};
    auto holder = platform::FileLock::acquire(lock_path, std::chrono::milliseconds(0));
    REQUIRE(holder);
    std::thread waiter([&] {
        auto l = platform::FileLock::acquire(lock_path, std::chrono::milliseconds(3000));
        got = l.has_value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    holder->release();
    waiter.join();
    CHECK(got.load());
}

TEST_CASE("find_program searches PATH")
{
    TempDir tmp;
    auto const bin_a = tmp.path / "a";
    auto const bin_b = tmp.path / "b";
    write_file(bin_a / "tool", "#!/bin/sh\n");
    write_file(bin_b / "tool", "#!/bin/sh\n");
    write_file(bin_b / "plain", "data");
    write_file(bin_b / "runner", "#!/bin/sh\n");
    std::filesystem::permissions(bin_a / "tool", std::filesystem::perms::owner_all);
    std::filesystem::permissions(bin_b / "tool", std::filesystem::perms::owner_all);
    std::filesystem::permissions(bin_b / "runner", std::filesystem::perms::owner_all);
    std::filesystem::create_directories(bin_a / "adir");

    platform::Env env{{"PATH", std::format("{}::{}:/nonexistent", bin_a.string(), bin_b.string())}};
    auto found = platform::find_program("tool", env, platform::Os::Linux);
    REQUIRE(found.has_value());
    CHECK_EQ(found->string(), (bin_a / "tool").string());
    found = platform::find_program("runner", env, platform::Os::Linux);
    REQUIRE(found.has_value());
    CHECK_EQ(found->string(), (bin_b / "runner").string());
    CHECK(!platform::find_program("plain", env, platform::Os::Linux).has_value());
    CHECK(!platform::find_program("adir", env, platform::Os::Linux).has_value());
    CHECK(!platform::find_program("missing", env, platform::Os::Linux).has_value());
    CHECK(!platform::find_program("", env, platform::Os::Linux).has_value());
    CHECK(!platform::find_program("tool", {}, platform::Os::Linux).has_value());
    auto direct = platform::find_program((bin_b / "runner").string(), {}, platform::Os::Linux);
    REQUIRE(direct.has_value());
    CHECK_EQ(direct->string(), (bin_b / "runner").string());

    auto const exe = platform::executable_path();
    REQUIRE(exe.has_value());
    CHECK(std::filesystem::exists(*exe));
    auto const sh = platform::find_program("sh");
    CHECK(sh.has_value());
}

TEST_CASE("environment, locale and path helpers")
{
    auto env = platform::current_env();
    CHECK(env.contains("PATH"));
    CHECK_EQ(platform::getenv("PATH").value(), env.at("PATH"));
    CHECK(!platform::getenv("LEARNDC_SURELY_NOT_SET_12345").has_value());

    CHECK(platform::utf8_locale({{"LANG", "en_US.UTF-8"}}));
    CHECK(platform::utf8_locale({{"LANG", "C.utf8"}}));
    CHECK(!platform::utf8_locale({{"LANG", "C"}}));
    CHECK(!platform::utf8_locale({{"LC_ALL", "C"}, {"LANG", "en_US.UTF-8"}}));
    CHECK(platform::utf8_locale({{"LC_ALL", "de_DE.UTF-8"}, {"LANG", "C"}}));
    CHECK(!platform::utf8_locale({}));

    auto const stamp = platform::now_utc_iso8601();
    CHECK_EQ(stamp.size(), 20u);
    CHECK(stamp.ends_with("Z"));
    CHECK_EQ(stamp[4], '-');
    CHECK_EQ(stamp[10], 'T');
    CHECK_EQ(platform::format_utc_iso8601(std::chrono::sys_seconds{std::chrono::seconds(86400 + 3661)}), "1970-01-02T01:01:01Z");

    auto const unicode = platform::path_from_utf8("dir/\xE6\x97\xA5\xE6\x9C\xAC.txt");
    CHECK_EQ(platform::path_to_utf8(unicode), "dir/\xE6\x97\xA5\xE6\x9C\xAC.txt");

    TempDir tmp;
    write_file(tmp.path / "real", "x");
    std::error_code ec;
    std::filesystem::create_symlink(tmp.path / "real", tmp.path / "link", ec);
    if (!ec)
        CHECK(platform::is_symlink_or_reparse(tmp.path / "link"));
    CHECK(!platform::is_symlink_or_reparse(tmp.path / "real"));
    CHECK(!platform::is_symlink_or_reparse(tmp.path / "absent"));
    (void)platform::stdin_is_tty();
    (void)platform::stdout_is_tty();
    (void)platform::stderr_is_tty();
    auto const size = platform::terminal_size();
    if (size)
    {
        CHECK(size->columns > 0);
        CHECK(size->rows > 0);
    }
}

TEST_CASE("spawn_detached starts programs and reports failures")
{
    TempDir tmp;
    auto const marker = tmp.path / "marker";
    auto started = platform::spawn_detached({"/bin/sh", "-c", std::format("echo ran > '{}'", marker.string())}, tmp.path);
    REQUIRE(started);
    bool appeared = false;
    for (int i = 0; i < 200 && !appeared; ++i)
    {
        appeared = std::filesystem::exists(marker);
        if (!appeared)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(appeared);

    auto missing = platform::spawn_detached({"/nonexistent/learndc-program"}, tmp.path);
    REQUIRE(!missing);
    CHECK(missing.error().message.find("cannot start") != std::string::npos);
    auto bad_cwd = platform::spawn_detached({"/bin/sh", "-c", "true"}, tmp.path / "no-such-dir");
    CHECK(!bad_cwd);
    CHECK(!platform::spawn_detached({}, tmp.path));
}

SECTION("learndc.catalog");

TEST_CASE("valid fixture catalog loads")
{
    auto source = base_source();
    auto loaded = catalog::Catalog::load(source);
    if (!loaded)
        dump_errors(loaded.error());
    REQUIRE(loaded);
    auto const& c = *loaded;
    CHECK_EQ(c.title(), "Test");
    CHECK_EQ(c.tracks().size(), 2u);
    CHECK_EQ(c.exercises().size(), 3u);
    CHECK_EQ(c.tracks()[0].id, "basics");
    CHECK_EQ(c.tracks()[0].exercise_ids.size(), 2u);
    CHECK_EQ(c.tracks()[0].exercise_ids[1], "basics/two");
    auto const* one = c.find("basics/one");
    REQUIRE(one != nullptr);
    CHECK_EQ(one->title, "Title one");
    CHECK_EQ(one->track, "basics");
    CHECK_EQ(one->name, "one");
    CHECK_EQ(one->dir, "tracks/basics/one");
    CHECK_EQ(one->revision, 1);
    CHECK(one->kind == catalog::ExerciseKind::FixBehavior);
    CHECK(one->difficulty == catalog::Difficulty::Beginner);
    CHECK(one->starter_state == catalog::StarterState::FailsChecks);
    CHECK_EQ(one->concepts.size(), 2u);
    CHECK_EQ(one->entry_file, "src/main.dc");
    CHECK_EQ(one->product, "prog");
    CHECK_EQ(one->min_dcc, "0.6.0");
    CHECK(one->supports_platform("windows"));
    CHECK(!one->supports_platform("macos"));
    CHECK(one->supports_backend("custom"));
    CHECK(!one->build_timeout_seconds.has_value());
    CHECK_EQ(one->hints[0], "first");
    CHECK_EQ(one->hints[2], "third");
    CHECK_EQ(one->readme, "# Readme\n");
    CHECK_EQ(one->explanation, "Because.\n");
    CHECK_EQ(one->starter.size(), 2u);
    CHECK(one->starter.contains("src/main.dc"));
    CHECK(one->starter.contains("project.dcon"));
    CHECK_EQ(one->solution.size(), 1u);
    CHECK_EQ(one->checks.size(), 2u);
    CHECK(one->checks[0].kind == catalog::CheckKind::Build);
    CHECK(one->checks[1].kind == catalog::CheckKind::Run);
    CHECK_EQ(one->checks[1].expect_stdout.value(), "hi\n");
    CHECK(one->checks[1].stdout_match == catalog::StdoutMatch::Exact);
    CHECK(one->checks[1].line_endings == catalog::LineEndings::Portable);
    CHECK_EQ(one->checks[1].expect_exit, 0);
    CHECK_EQ(one->checks[1].max_seconds, 10);
    CHECK(c.find("nope/none") == nullptr);
    CHECK(c.find_track("more") != nullptr);
    CHECK(c.find_track("zzz") == nullptr);
    CHECK_EQ(c.in_track("basics").size(), 2u);
    catalog::ContentError const err{"a/b.toml", 3, 9, "bad"};
    CHECK_EQ(err.format(), "a/b.toml:3:9: bad");
}

TEST_CASE("catalog schema version is rejected")
{
    auto source = base_source();
    edit(source, one_toml, "schema = 1", "schema = 2");
    auto errors = errors_of(source);
    CHECK(has_error(errors, "unsupported schema 2 (this learndc supports 1)", one_toml, 1, 10));

    source = base_source();
    edit(source, "catalog.toml", "schema = 1", "schema = 7");
    errors = errors_of(source);
    CHECK(has_error(errors, "unsupported schema 7 (this learndc supports 1)", "catalog.toml", 1, 10));

    source = base_source();
    edit(source, "tracks/basics/track.toml", "schema = 1", "schema = 0");
    CHECK(has_error(errors_of(source), "unsupported schema 0", "tracks/basics/track.toml", 1, 10));

    source = base_source();
    edit(source, one_toml, "schema = 1", "schema = \"1\"");
    CHECK(has_error(errors_of(source), "must be an integer", one_toml, 1, 10));

    source = base_source();
    edit(source, one_toml, "schema = 1\n", "");
    CHECK(has_error(errors_of(source), "missing required key 'schema'", one_toml));
}

TEST_CASE("catalog rejects unknown and missing keys")
{
    auto source = base_source();
    edit(source, one_toml, "revision = 1\n", "revision = 1\nsurprise = \"x\"\n");
    CHECK(has_error(errors_of(source), "unknown key 'surprise'", one_toml, 4, 1));

    source = base_source();
    edit(source, one_toml, "title = \"Title one\"\n", "");
    CHECK(has_error(errors_of(source), "missing required key 'title'", one_toml));

    source = base_source();
    edit(source, "catalog.toml", "title = \"Test\"\n", "title = \"Test\"\nextra = 1\n");
    CHECK(has_error(errors_of(source), "unknown key 'extra'", "catalog.toml", 3, 1));

    source = base_source();
    edit(source, "tracks/basics/track.toml", "summary = \"Start here\"\n", "summary = \"Start here\"\nextra = 1\n");
    CHECK(has_error(errors_of(source), "unknown key 'extra'", "tracks/basics/track.toml"));

    source = base_source();
    edit(source, one_toml, "name = \"Prints\"\n", "name = \"Prints\"\nmystery = 1\n");
    CHECK(has_error(errors_of(source), "unknown key 'mystery'", one_toml));

    source = base_source();
    edit(source, one_toml, "name = \"Compiles\"\n", "name = \"Compiles\"\nargs = []\n");
    CHECK(has_error(errors_of(source), "key 'args' is not valid for a build check", one_toml));
}

TEST_CASE("catalog validates value types and ranges")
{
    auto source = base_source();
    edit(source, one_toml, "revision = 1", "revision = 0");
    CHECK(has_error(errors_of(source), "key 'revision' must be between 1 and", one_toml));

    source = base_source();
    edit(source, one_toml, "title = \"Title one\"", "title = 5");
    CHECK(has_error(errors_of(source), "key 'title' must be a string (found integer)", one_toml, 4, 9));

    source = base_source();
    edit(source, one_toml, "title = \"Title one\"", "title = \"   \"");
    CHECK(has_error(errors_of(source), "must not be empty", one_toml));

    source = base_source();
    edit(source, one_toml, "kind = \"fix-behavior\"", "kind = \"nonsense\"");
    CHECK(has_error(errors_of(source), "invalid kind 'nonsense' (expected one of: repair-compilation", one_toml));

    source = base_source();
    edit(source, one_toml, "difficulty = \"beginner\"", "difficulty = \"hard\"");
    CHECK(has_error(errors_of(source), "invalid difficulty 'hard'", one_toml));

    source = base_source();
    edit(source, one_toml, "starter_state = \"fails-checks\"", "starter_state = \"passes\"");
    CHECK(has_error(errors_of(source), "invalid starter_state 'passes'", one_toml));

    source = base_source();
    edit(source, one_toml, "estimated_minutes = 5", "estimated_minutes = 5000");
    CHECK(has_error(errors_of(source), "estimated_minutes", one_toml));

    source = base_source();
    edit(source, one_toml, "concepts = [\"alpha\", \"beta gamma\"]", "concepts = []");
    CHECK(has_error(errors_of(source), "at least one element", one_toml));

    source = base_source();
    edit(source, one_toml, "objectives = [\"Do the thing\"]", "objectives = [1]");
    CHECK(has_error(errors_of(source), "every element of 'objectives' must be a string", one_toml));

    source = base_source();
    edit(source, one_toml, "tags = [\"tagged\"]", "tags = \"tagged\"");
    CHECK(has_error(errors_of(source), "key 'tags' must be an array of strings", one_toml));

    source = base_source();
    edit(source, one_toml, "platforms = [\"linux\", \"windows\"]", "platforms = [\"linux\", \"amiga\"]");
    CHECK(has_error(errors_of(source), "unknown platform 'amiga'", one_toml));

    source = base_source();
    edit(source, one_toml, "backends = [\"llvm\", \"custom\"]", "backends = [\"gcc\"]");
    CHECK(has_error(errors_of(source), "unknown backend 'gcc'", one_toml));

    source = base_source();
    edit(source, one_toml, "requires = [\"dcc\", \"dpm\"]", "requires = [\"dcc\", \"make\"]");
    CHECK(has_error(errors_of(source), "unknown tool 'make'", one_toml));

    source = base_source();
    edit(source, one_toml, "min_dcc = \"0.6.0\"", "min_dcc = \"0.6\"");
    CHECK(has_error(errors_of(source), "min_dcc '0.6' must look like 1.2.3", one_toml));

    source = base_source();
    edit(source, one_toml, "starter_state = \"fails-checks\"\n", "starter_state = \"fails-build\"\nbuild_timeout_seconds = 5\n");
    CHECK(has_error(errors_of(source), "build_timeout_seconds", one_toml));

    source = base_source();
    edit(source, one_toml, "starter_state = \"fails-checks\"\n", "starter_state = \"fails-build\"\nbuild_timeout_seconds = 300\n");
    auto loaded = catalog::Catalog::load(source);
    REQUIRE(loaded);
    CHECK_EQ(loaded->find("basics/one")->build_timeout_seconds.value(), 300);
    CHECK(loaded->find("basics/one")->starter_state == catalog::StarterState::FailsBuild);
}

TEST_CASE("catalog validates ids product names and paths")
{
    auto source = base_source();
    edit(source, one_toml, "id = \"basics/one\"", "id = \"basics/uno\"");
    CHECK(has_error(errors_of(source), "id 'basics/uno' must equal 'basics/one'", one_toml, 2, 6));

    source = base_source();
    edit(source, one_toml, "id = \"basics/one\"", "id = \"Basics/One\"");
    CHECK(has_error(errors_of(source), "must look like <track>/<name>", one_toml));

    for (std::string_view bad : {"basics", "basics/", "/one", "basics/o_ne", "basics/-one", "basics/one-", "basics/o--ne", "basics/one/two", ""})
    {
        source = base_source();
        edit(source, one_toml, "id = \"basics/one\"", std::format("id = \"{}\"", bad));
        CHECK(!errors_of(source).empty());
    }

    source = base_source();
    edit(source, one_toml, "product = \"prog\"", "product = \"my-prog\"");
    CHECK(has_error(errors_of(source), "product 'my-prog' must be an identifier", one_toml));

    source = base_source();
    edit(source, one_toml, "product = \"prog\"", "product = \"9prog\"");
    CHECK(has_error(errors_of(source), "must be an identifier", one_toml));

    source = base_source();
    edit(source, one_toml, "product = \"prog\"", "product = \"Prog\"");
    CHECK(has_error(errors_of(source), "must be an identifier", one_toml));

    source = base_source();
    edit(source, one_toml, "product = \"prog\"", "product = \"other\"");
    CHECK(has_error(errors_of(source), "does not mention the product \"other\"", one_dir + "starter/project.dcon"));

    for (std::string_view bad : {"/etc/passwd", "../x.dc", "src/../../x.dc", "src\\\\main.dc", "C:main.dc", "src//main.dc", "./src/main.dc", "src/"})
    {
        source = base_source();
        edit(source, one_toml, "entry_file = \"src/main.dc\"", std::format("entry_file = \"{}\"", bad));
        CHECK(has_error(errors_of(source), "entry_file", one_toml));
    }

    source = base_source();
    edit(source, one_toml, "entry_file = \"src/main.dc\"", "entry_file = \"src/other.dc\"");
    CHECK(has_error(errors_of(source), "entry_file 'src/other.dc' does not exist in starter/", one_toml, 13, 14));

    source = base_source();
    edit(source, one_toml, "chapter = \"intro\"", "chapter = \"Intro Chapter\"");
    CHECK(has_error(errors_of(source), "chapter 'Intro Chapter'", one_toml));
}

TEST_CASE("path validation rules")
{
    CHECK(!catalog::path_problem("a/b.dc").has_value());
    CHECK(!catalog::path_problem(".gitignore").has_value());
    CHECK(!catalog::path_problem("src/main.dc").has_value());
    CHECK(catalog::path_problem("").has_value());
    CHECK(catalog::path_problem("/abs").has_value());
    CHECK(catalog::path_problem("a\\b").has_value());
    CHECK(catalog::path_problem("a/../b").has_value());
    CHECK(catalog::path_problem("..").has_value());
    CHECK(catalog::path_problem("a/./b").has_value());
    CHECK(catalog::path_problem("a//b").has_value());
    CHECK(catalog::path_problem("a/").has_value());
    CHECK(catalog::path_problem("C:/x").has_value());
    CHECK(catalog::path_problem("c:x").has_value());
    CHECK(catalog::path_problem("a/b ").has_value());
    CHECK(catalog::path_problem("a/b.").has_value());
    CHECK(catalog::path_problem(std::string("a\x01") + "b").has_value());

    CHECK(catalog::is_slug("hello-world"));
    CHECK(catalog::is_slug("a1"));
    CHECK(!catalog::is_slug(""));
    CHECK(!catalog::is_slug("-a"));
    CHECK(!catalog::is_slug("a--b"));
    CHECK(!catalog::is_slug("A"));
    CHECK(!catalog::is_slug("a_b"));
    CHECK(catalog::is_exercise_id("a/b-c"));
    CHECK(!catalog::is_exercise_id("a"));
    CHECK(catalog::is_identifier("_x1"));
    CHECK(!catalog::is_identifier("1x"));
    CHECK(!catalog::is_identifier(""));
    CHECK(catalog::is_harness_stem("add_one"));
    CHECK(!catalog::is_harness_stem("_add"));
    CHECK(!catalog::is_harness_stem("Add"));
    CHECK(catalog::is_version("0.6.0"));
    CHECK(!catalog::is_version("0.6"));
    CHECK(!catalog::is_version("a.b.c"));
    CHECK(!catalog::is_version("1.2.3.4"));
}

TEST_CASE("catalog rejects bad source paths")
{
    auto source = base_source();
    source.add("tracks/basics/one/../evil.toml", "x");
    CHECK(has_error(errors_of(source), "'..' component", "tracks/basics/one/../evil.toml"));
    source = base_source();
    source.add("/abs.toml", "x");
    CHECK(has_error(errors_of(source), "absolute"));
}

TEST_CASE("catalog checks prerequisites")
{
    auto source = base_source();
    edit(source, "tracks/basics/two/exercise.toml", "prerequisites = [\"basics/one\"]", "prerequisites = [\"basics/ghost\"]");
    CHECK(has_error(errors_of(source), "prerequisite 'basics/ghost' does not exist", "tracks/basics/two/exercise.toml", 15, 18));

    source = base_source();
    edit(source, "tracks/basics/two/exercise.toml", "prerequisites = [\"basics/one\"]", "prerequisites = [\"nonsense\"]");
    CHECK(has_error(errors_of(source), "not a valid exercise id", "tracks/basics/two/exercise.toml"));

    source = base_source();
    edit(source, "tracks/basics/two/exercise.toml", "prerequisites = [\"basics/one\"]", "prerequisites = [\"basics/two\"]");
    CHECK(has_error(errors_of(source), "lists itself as a prerequisite", "tracks/basics/two/exercise.toml"));

    source = base_source();
    edit(source, one_toml, "prerequisites = []", "prerequisites = [\"basics/two\"]");
    auto errors = errors_of(source);
    CHECK(has_error(errors, "prerequisite cycle: basics/one -> basics/two -> basics/one", one_toml, 15, 17));

    source = base_source();
    edit(source, one_toml, "prerequisites = []", "prerequisites = [\"more/three\"]");
    errors = errors_of(source);
    CHECK(has_error(errors, "prerequisite cycle"));
    CHECK_EQ(std::ranges::count_if(errors, [](auto const& e) { return e.message.starts_with("prerequisite cycle"); }), 1);
}

TEST_CASE("catalog checks the kinds of checks")
{
    auto source = base_source();
    edit(source, one_toml, "name = \"Prints\"", "name = \"Compiles\"");
    CHECK(has_error(errors_of(source), "check name 'Compiles' is used more than once", one_toml, 26, 1));

    source = base_source();
    edit(source, one_toml, "kind = \"run\"", "kind = \"launch\"");
    CHECK(has_error(errors_of(source), "invalid kind 'launch'", one_toml));

    source = base_source();
    edit(source, one_toml, "expect_stdout = \"hi\\n\"", "stdout_match = \"fuzzy\"");
    CHECK(has_error(errors_of(source), "invalid stdout_match 'fuzzy'", one_toml));

    source = base_source();
    edit(source, one_toml, "expect_stdout = \"hi\\n\"", "line_endings = \"windows\"");
    CHECK(has_error(errors_of(source), "invalid line_endings 'windows'", one_toml));

    source = base_source();
    edit(source, one_toml, "expect_stdout = \"hi\\n\"", "max_seconds = 61");
    CHECK(has_error(errors_of(source), "key 'max_seconds' must be between 1 and 60", one_toml));

    source = base_source();
    edit(source, one_toml, "expect_stdout = \"hi\\n\"", "expect_exit = 256");
    CHECK(has_error(errors_of(source), "key 'expect_exit'", one_toml));

    source = base_source();
    edit(source, one_toml, "expect_stdout = \"hi\\n\"", "expect_build = \"success\"");
    CHECK(has_error(errors_of(source), "key 'expect_build' is not valid for a run check", one_toml));

    source = base_source();
    edit(source, one_toml, "name = \"Prints\"\n", "name = \"Prints\"\n");
    source.files.at(one_toml) += "\n[[checks]]\nkind = \"diagnostic\"\nname = \"Fails\"\nexpect_build = \"maybe\"\n";
    CHECK(has_error(errors_of(source), "invalid expect_build 'maybe'", one_toml));

    source = base_source();
    source.files.at(one_toml) +=
        "\n[[checks]]\nkind = \"diagnostic\"\nname = \"Warns\"\nexpect_build = \"failure\"\ndiagnostic_contains = \"unused\"\nhint = \"look\"\n";
    auto loaded = catalog::Catalog::load(source);
    REQUIRE(loaded);
    auto const& checks = loaded->find("basics/one")->checks;
    CHECK_EQ(checks.size(), 3u);
    CHECK(checks[2].kind == catalog::CheckKind::Diagnostic);
    CHECK(!checks[2].expect_build_success);
    CHECK_EQ(checks[2].diagnostic_contains, "unused");
    CHECK_EQ(checks[2].hint, "look");

    source = base_source();
    source.files.at(one_toml) +=
        "\n[[checks]]\nkind = \"run\"\nname = \"Args\"\nargs = [\"-x\", \"two words\"]\nstdin = \"in\\n\"\nstdout_match = \"contains\"\nline_endings = "
        "\"exact\"\nexpect_stderr_contains = \"warn\"\nexpect_exit = 3\nmax_seconds = 2\nexpect_stdout = \"ok\"\n";
    loaded = catalog::Catalog::load(source);
    REQUIRE(loaded);
    auto const& run = loaded->find("basics/one")->checks.back();
    CHECK_EQ(run.args.size(), 2u);
    CHECK_EQ(run.args[1], "two words");
    CHECK_EQ(run.stdin_text, "in\n");
    CHECK(run.stdout_match == catalog::StdoutMatch::Contains);
    CHECK(run.line_endings == catalog::LineEndings::Exact);
    CHECK_EQ(run.expect_stderr_contains.value(), "warn");
    CHECK_EQ(run.expect_exit, 3);
    CHECK_EQ(run.max_seconds, 2);
}

TEST_CASE("catalog adds an implicit build check")
{
    auto source = base_source();
    edit(source, one_toml, "[[checks]]\nkind = \"build\"\nname = \"Compiles\"\n\n", "");
    auto loaded = catalog::Catalog::load(source);
    REQUIRE(loaded);
    auto const& checks = loaded->find("basics/one")->checks;
    CHECK_EQ(checks.size(), 2u);
    CHECK(checks[0].kind == catalog::CheckKind::Build);
    CHECK_EQ(checks[0].name, "Compiles");

    source = base_source();
    edit(source, one_toml, "[[checks]]\nkind = \"build\"\nname = \"Compiles\"\n\n", "");
    edit(source, one_toml, "name = \"Prints\"", "name = \"Compiles\"");
    loaded = catalog::Catalog::load(source);
    REQUIRE(loaded);
    CHECK_EQ(loaded->find("basics/one")->checks[0].name, "Compiles (build)");
}

TEST_CASE("catalog validates harness checks")
{
    auto harness_check = [](std::string_view path) {
        return std::format("\n[[checks]]\nkind = \"harness\"\nname = \"Adds\"\nharness = \"{}\"\nexpect_exit = 0\nmax_seconds = 5\n", path);
    };

    auto source = base_source();
    source.files.at(one_toml) += harness_check("checks/add_ok.dc");
    source.add(one_dir + "checks/add_ok.dc", "module add_ok;\nimport shapes;\npublic i32 main() { return 0; }\n");
    auto loaded = catalog::Catalog::load(source);
    if (!loaded)
        dump_errors(loaded.error());
    REQUIRE(loaded);
    auto const& check = loaded->find("basics/one")->checks.back();
    CHECK(check.kind == catalog::CheckKind::Harness);
    CHECK_EQ(check.harness, "checks/add_ok.dc");
    CHECK_EQ(check.harness_stem(), "add_ok");
    CHECK_EQ(loaded->find("basics/one")->check_files.size(), 1u);

    source = base_source();
    source.files.at(one_toml) += harness_check("checks/missing.dc");
    CHECK(has_error(errors_of(source), "harness file 'checks/missing.dc' of check 'Adds' does not exist", one_toml));

    source = base_source();
    source.files.at(one_toml) += harness_check("checks/add_ok.dc");
    source.add(one_dir + "checks/add_ok.dc", "module other;\n");
    CHECK(has_error(errors_of(source), "harness must declare 'module add_ok;'", one_dir + "checks/add_ok.dc"));

    for (std::string_view bad : {"add.dc", "checks/add.txt", "checks/sub/add.dc", "../checks/add.dc", "/checks/add.dc", "checks/.dc", "checks/Add.dc",
                                 "checks/1add.dc", "checks/add-x.dc"})
    {
        source = base_source();
        source.files.at(one_toml) += harness_check(bad);
        CHECK(has_error(errors_of(source), "harness", one_toml));
    }

    source = base_source();
    source.add(one_dir + "checks/Bad-Name.dc", "module x;\n");
    CHECK(has_error(errors_of(source), "harness files must be checks/<stem>.dc", one_dir + "checks/Bad-Name.dc"));
    source = base_source();
    source.add(one_dir + "checks/notes.txt", "x");
    CHECK(has_error(errors_of(source), "harness files must be checks/<stem>.dc"));

    source = base_source();
    source.files.at(one_toml) += "\n[[checks]]\nkind = \"harness\"\nname = \"Adds\"\n";
    CHECK(has_error(errors_of(source), "missing required key 'harness'", one_toml));
}

TEST_CASE("catalog validates hints.md")
{
    auto const hints_path = one_dir + "hints.md";
    auto source = base_source();
    source.files.at(hints_path) = "## Hint 1\n\na\n\n## Hint 2\n\nb\n";
    CHECK(has_error(errors_of(source), "expected exactly three hint sections, found 2", hints_path));

    source = base_source();
    source.files.at(hints_path) = "## Hint 1\n\na\n\n## Hint 2\n\nb\n\n## Hint 3\n\nc\n\n## Hint 4\n\nd\n";
    CHECK(has_error(errors_of(source), "unexpected heading '## Hint 4'", hints_path, 13, 1));

    source = base_source();
    source.files.at(hints_path) = "## Hint 2\n\na\n\n## Hint 1\n\nb\n\n## Hint 3\n\nc\n";
    CHECK(has_error(errors_of(source), "unexpected heading '## Hint 2'", hints_path, 1, 1));

    source = base_source();
    source.files.at(hints_path) = "Intro text\n\n## Hint 1\n\na\n\n## Hint 2\n\nb\n\n## Hint 3\n\nc\n";
    CHECK(has_error(errors_of(source), "text before the '## Hint 1' heading", hints_path, 1, 1));

    source = base_source();
    source.files.at(hints_path) = "## Hint 1\n\na\n\n## Hint 2\n\n\n\n## Hint 3\n\nc\n";
    CHECK(has_error(errors_of(source), "hint 2 is empty", hints_path, 5, 1));

    source = base_source();
    source.files.at(hints_path) = "## Hint 1\n\na\n\n## Hints\n\nb\n\n## Hint 3\n\nc\n";
    CHECK(has_error(errors_of(source), "unexpected heading '## Hints'", hints_path));

    source = base_source();
    source.files.at(hints_path) = "";
    CHECK(has_error(errors_of(source), "expected exactly three hint sections, found 0", hints_path));

    source = base_source();
    source.files.at(hints_path) = "## Hint 1\n\n```\n## not a heading\n```\n\n## Hint 2\n\nb\n\n## Hint 3\n\n### sub\nc\n";
    auto loaded = catalog::Catalog::load(source);
    if (!loaded)
        dump_errors(loaded.error());
    REQUIRE(loaded);
    CHECK(loaded->find("basics/one")->hints[0].find("## not a heading") != std::string::npos);
    CHECK(loaded->find("basics/one")->hints[2].starts_with("### sub"));

    source = base_source();
    source.files.at(hints_path) = "## Hint 1  \r\n\r\na\r\n## Hint 2\r\nb\r\n## Hint 3\r\nc\r\n";
    CHECK(catalog::Catalog::load(source).has_value());
}

TEST_CASE("catalog validates exercise directory contents")
{
    auto source = base_source();
    source.files.erase(one_dir + "README.md");
    CHECK(has_error(errors_of(source), "file is missing", one_dir + "README.md"));

    source = base_source();
    source.files.at(one_dir + "explanation.md") = "  \n";
    CHECK(has_error(errors_of(source), "file is empty", one_dir + "explanation.md"));

    source = base_source();
    source.files.erase(one_dir + "starter/project.dcon");
    CHECK(has_error(errors_of(source), "starter must contain project.dcon"));

    source = base_source();
    source.files.erase(one_dir + "starter/project.dcon");
    source.files.erase(one_dir + "starter/src/main.dc");
    CHECK(has_error(errors_of(source), "starter directory is missing or empty"));

    source = base_source();
    source.files.erase(one_dir + "solution/src/main.dc");
    CHECK(has_error(errors_of(source), "solution directory is missing or empty"));

    source = base_source();
    source.add(one_dir + "notes.txt", "x");
    CHECK(has_error(errors_of(source), "unexpected file 'notes.txt'", one_dir + "notes.txt"));

    source = base_source();
    source.add(one_dir + "extras/more.txt", "x");
    CHECK(has_error(errors_of(source), "unexpected directory 'extras'"));

    source = base_source();
    source.add(one_dir + "starter/src/Main.DC", "x");
    source.add(one_dir + "starter/src/main.DC", "x");
    CHECK(has_error(errors_of(source), "differs only by letter case"));

    source = base_source();
    source.files.at(one_dir + "README.md") = "caf\xC3\xA9\n";
    CHECK(has_error(errors_of(source), "non-ASCII character", one_dir + "README.md", 1, 4));

    source = base_source();
    source.files.at(one_toml) += "# caf\xC3\xA9\n";
    CHECK(has_error(errors_of(source), "non-ASCII", one_toml));

    source = base_source();
    source.add(one_dir + "starter/data.txt", "caf\xC3\xA9 is fine in starter files\n");
    CHECK(catalog::Catalog::load(source).has_value());

    source = base_source();
    source.add(one_dir + "starter/.gitignore", ".dpm/\n");
    auto loaded = catalog::Catalog::load(source);
    REQUIRE(loaded);
    CHECK(loaded->find("basics/one")->starter.contains(".gitignore"));
}

TEST_CASE("catalog validates the catalog and track files")
{
    auto source = base_source();
    source.files.erase("catalog.toml");
    auto errors = errors_of(source);
    CHECK(has_error(errors, "file is missing", "catalog.toml"));

    source = base_source();
    edit(source, "catalog.toml", "tracks = [\"basics\", \"more\"]", "tracks = [\"basics\"]");
    CHECK(has_error(errors_of(source), "track directory 'tracks/more' is not listed in catalog.toml", "catalog.toml"));

    source = base_source();
    edit(source, "catalog.toml", "tracks = [\"basics\", \"more\"]", "tracks = [\"basics\", \"more\", \"ghost\"]");
    CHECK(has_error(errors_of(source), "track 'ghost' has no tracks/ghost/track.toml", "catalog.toml", 3, 29));

    source = base_source();
    edit(source, "catalog.toml", "tracks = [\"basics\", \"more\"]", "tracks = [\"basics\", \"more\", \"basics\"]");
    CHECK(has_error(errors_of(source), "track 'basics' is listed more than once", "catalog.toml"));

    source = base_source();
    edit(source, "catalog.toml", "tracks = [\"basics\", \"more\"]", "tracks = [\"Basics\", \"more\"]");
    CHECK(has_error(errors_of(source), "track name 'Basics'", "catalog.toml"));

    source = base_source();
    edit(source, "tracks/basics/track.toml", "id = \"basics\"", "id = \"other\"");
    CHECK(has_error(errors_of(source), "id 'other' must equal the directory name 'basics'", "tracks/basics/track.toml", 2, 6));

    source = base_source();
    edit(source, "tracks/basics/track.toml", "exercises = [\"one\", \"two\"]", "exercises = [\"one\"]");
    CHECK(has_error(errors_of(source), "exercise directory 'tracks/basics/two' is not listed in track.toml", "tracks/basics/track.toml"));

    source = base_source();
    edit(source, "tracks/basics/track.toml", "exercises = [\"one\", \"two\"]", "exercises = [\"one\", \"two\", \"three\"]");
    CHECK(has_error(errors_of(source), "exercise 'three' has no tracks/basics/three/exercise.toml", "tracks/basics/track.toml", 5, 28));

    source = base_source();
    edit(source, "tracks/basics/track.toml", "exercises = [\"one\", \"two\"]", "exercises = [\"one\", \"two\", \"one\"]");
    CHECK(has_error(errors_of(source), "exercise 'one' is listed more than once", "tracks/basics/track.toml"));

    source = base_source();
    edit(source, "tracks/basics/track.toml", "exercises = [\"one\", \"two\"]", "exercises = [\"One\", \"two\"]");
    CHECK(has_error(errors_of(source), "exercise name 'One'", "tracks/basics/track.toml"));

    source = base_source();
    source.add("tracks/basics/stray.txt", "x");
    CHECK(has_error(errors_of(source), "unexpected file in a track directory", "tracks/basics/stray.txt"));

    source = base_source();
    edit(source, "tracks/basics/track.toml", "exercises = [\"one\", \"two\"]", "exercises = [\"one\", \"two\"\n");
    errors = errors_of(source);
    CHECK(has_error(errors, "unterminated array", "tracks/basics/track.toml"));

    source = base_source();
    source.files.at(one_toml) += "x = {a = 1}\n";
    CHECK(has_error(errors_of(source), "inline tables are not supported", one_toml));
}

TEST_CASE("catalog reports several errors at once")
{
    auto source = base_source();
    edit(source, one_toml, "title = \"Title one\"\n", "");
    edit(source, one_toml, "kind = \"fix-behavior\"", "kind = \"x\"");
    edit(source, "tracks/basics/two/exercise.toml", "[\"basics/one\"]", "[\"basics/zzz\"]");
    auto errors = errors_of(source);
    CHECK(errors.size() >= 3u);
    CHECK(has_error(errors, "missing required key 'title'"));
    CHECK(has_error(errors, "invalid kind 'x'"));
    CHECK(has_error(errors, "does not exist"));
    auto const text_dump = catalog::format_errors(errors);
    CHECK(text_dump.find(one_toml + ":") != std::string::npos);
}

TEST_CASE("catalog search filter order and recommendation")
{
    auto source = base_source();
    edit(source, one_toml, "title = \"Title one\"", "title = \"Printing text\"");
    edit(source, "tracks/basics/two/exercise.toml", "concepts = [\"alpha\", \"beta gamma\"]", "concepts = [\"printing\"]");
    edit(source, "tracks/basics/two/exercise.toml", "difficulty = \"beginner\"", "difficulty = \"advanced\"");
    edit(source, "tracks/more/three/exercise.toml", "summary = \"Summary of three\"", "summary = \"Mentions printing in passing\"");
    edit(source, "tracks/more/three/exercise.toml", "kind = \"fix-behavior\"", "kind = \"challenge\"");
    auto loaded = catalog::Catalog::load(source);
    if (!loaded)
        dump_errors(loaded.error());
    REQUIRE(loaded);
    auto const& c = *loaded;

    auto ids = [](std::vector<catalog::Exercise const*> const& list) {
        std::vector<std::string> out;
        for (auto const* e : list)
            out.push_back(e->id);
        return out;
    };

    CHECK((ids(c.search("")) == std::vector<std::string>{"basics/one", "basics/two", "more/three"}));
    CHECK((ids(c.search("printing")) == std::vector<std::string>{"basics/one", "basics/two", "more/three"}));
    CHECK((ids(c.search("PRINTING mentions")) == std::vector<std::string>{"more/three"}));
    CHECK((ids(c.search("beta gamma")) == std::vector<std::string>{"basics/one", "more/three"}));
    CHECK(c.search("zzzzz").empty());
    CHECK((ids(c.search("basics/tw")) == std::vector<std::string>{"basics/two"}));
    CHECK((ids(c.search("tagged")).size() == 3u));
    CHECK((ids(c.search("challenge")) == std::vector<std::string>{"more/three"}));

    catalog::Filter by_track;
    by_track.track = "basics";
    CHECK((ids(c.filter(by_track)) == std::vector<std::string>{"basics/one", "basics/two"}));
    catalog::Filter by_difficulty;
    by_difficulty.difficulty = catalog::Difficulty::Advanced;
    CHECK((ids(c.filter(by_difficulty)) == std::vector<std::string>{"basics/two"}));
    catalog::Filter by_kind;
    by_kind.kind = catalog::ExerciseKind::Challenge;
    CHECK((ids(c.filter(by_kind)) == std::vector<std::string>{"more/three"}));
    catalog::Filter combined;
    combined.track = "basics";
    combined.query = "printing";
    CHECK((ids(c.filter(combined)) == std::vector<std::string>{"basics/one", "basics/two"}));
    CHECK((ids(c.filter({}, [](catalog::Exercise const& e) { return e.id != "basics/two"; })) == std::vector<std::string>{"basics/one", "more/three"}));

    CHECK((ids(c.topological_order()) == std::vector<std::string>{"basics/one", "basics/two", "more/three"}));

    std::set<std::string> done;
    auto completed = [&](std::string_view id) { return done.contains(std::string(id)); };
    CHECK(c.prerequisites_met(*c.find("basics/one"), completed));
    CHECK(!c.prerequisites_met(*c.find("basics/two"), completed));
    CHECK_EQ(c.next_recommended(completed)->id, "basics/one");
    done.insert("basics/one");
    CHECK(c.prerequisites_met(*c.find("basics/two"), completed));
    CHECK_EQ(c.next_recommended(completed)->id, "basics/two");
    CHECK_EQ(c.next_recommended(completed, "basics/one")->id, "basics/two");
    done.insert("basics/two");
    CHECK_EQ(c.next_recommended(completed)->id, "more/three");
    done.insert("more/three");
    CHECK(c.next_recommended(completed) == nullptr);
    done.clear();
    done.insert("basics/two");
    CHECK_EQ(c.next_recommended(completed)->id, "basics/one");
    CHECK_EQ(c.next_recommended(completed, "basics/one")->id, "more/three");
}

TEST_CASE("topological order puts prerequisites first even across track order")
{
    auto source = base_source();
    edit(source, one_toml, "prerequisites = []", "prerequisites = [\"more/three\"]");
    edit(source, "tracks/more/three/exercise.toml", "prerequisites = [\"basics/two\"]", "prerequisites = []");
    edit(source, "tracks/basics/two/exercise.toml", "prerequisites = [\"basics/one\"]", "prerequisites = [\"more/three\"]");
    auto loaded = catalog::Catalog::load(source);
    if (!loaded)
        dump_errors(loaded.error());
    REQUIRE(loaded);
    auto const order = loaded->topological_order();
    CHECK_EQ(order.size(), 3u);
    CHECK_EQ(order[0]->id, "more/three");
    CHECK_EQ(order[1]->id, "basics/one");
    CHECK_EQ(order[2]->id, "basics/two");
}

TEST_CASE("load_directory reads a content tree")
{
    TempDir tmp;
    auto const root = tmp.path / "content";
    write_file(root / "catalog.toml", "schema = 1\n");
    write_file(root / "tracks/a/track.toml", "x");
    write_file(root / "tracks/a/ex/starter/.gitignore", ".dpm/\n");
    write_file(root / "tracks/a/ex/starter/src/main.dc", "module main;\n");
    write_file(root / ".hidden/skip.toml", "x");
    write_file(root / "tracks/.DS_Store", "x");
    write_file(root / "tracks/a/.swp", "x");
    auto source = catalog::load_directory(root);
    REQUIRE(source);
    CHECK_EQ(source->files.size(), 4u);
    CHECK(source->contains("catalog.toml"));
    CHECK(source->contains("tracks/a/ex/starter/.gitignore"));
    CHECK(source->contains("tracks/a/ex/starter/src/main.dc"));
    CHECK(!source->contains(".hidden/skip.toml"));
    CHECK_EQ(*source->get("tracks/a/track.toml"), "x");
    CHECK(source->get("absent") == nullptr);

    write_file(root / "tracks/a/blob.bin", "x");
    auto bad = catalog::load_directory(root);
    REQUIRE(!bad);
    CHECK(bad.error().message.find("unsupported content file type") != std::string::npos);
    std::filesystem::remove(root / "tracks/a/blob.bin");

    std::error_code ec;
    std::filesystem::create_symlink(root / "catalog.toml", root / "tracks/a/link.toml", ec);
    if (!ec)
    {
        auto linked = catalog::load_directory(root);
        REQUIRE(!linked);
        CHECK(linked.error().message.find("symbolic links") != std::string::npos);
    }
    CHECK(!catalog::load_directory(tmp.path / "nonexistent"));
}

TEST_CASE("load_directory then Catalog::load round trips a written fixture")
{
    TempDir tmp;
    auto const source = base_source();
    for (auto const& [path, content] : source.files)
        write_file(tmp.path / path, content);
    auto loaded_source = catalog::load_directory(tmp.path);
    REQUIRE(loaded_source);
    CHECK(loaded_source->files == source.files);
    auto loaded = catalog::Catalog::load(*loaded_source);
    if (!loaded)
        dump_errors(loaded.error());
    CHECK(loaded.has_value());
}

SECTION("learndc.catalog on the shipped content");

TEST_CASE("real content directory loads")
{
    char const* content = std::getenv("DCC_TEST_LEARNDC_CONTENT");
    if (content == nullptr)
    {
        std::println(std::cerr, "    DCC_TEST_LEARNDC_CONTENT is not set");
        CHECK(content != nullptr);
        return;
    }
    auto source = catalog::load_directory(content);
    REQUIRE(source);
    auto loaded = catalog::Catalog::load(*source);
    if (!loaded)
        dump_errors(loaded.error());
    REQUIRE(loaded);
    auto const& c = *loaded;
    CHECK(c.exercises().size() >= 1u);
    auto const* hello = c.find("fundamentals/hello-world");
    REQUIRE(hello != nullptr);
    CHECK_EQ(hello->product, "hello");
    CHECK_EQ(hello->entry_file, "src/main.dc");
    CHECK(hello->starter.contains("project.dcon"));
    CHECK(hello->starter.contains("src/main.dc"));
    CHECK(hello->solution.contains("src/main.dc"));
    CHECK(!hello->hints[0].empty());
    CHECK(!hello->hints[1].empty());
    CHECK(!hello->hints[2].empty());
    CHECK(hello->checks.size() >= 2u);
    CHECK(hello->checks[0].kind == catalog::CheckKind::Build);
    CHECK(!hello->readme.empty());
    CHECK(!hello->explanation.empty());

    auto const order = c.topological_order();
    CHECK_EQ(order.size(), c.exercises().size());
    std::set<std::string> seen;
    for (auto const* e : order)
    {
        for (auto const& prerequisite : e->prerequisites)
            CHECK(seen.contains(prerequisite));
        seen.insert(e->id);
    }
    CHECK(!c.search("hello").empty());
    std::set<std::string> done;
    auto const* next = c.next_recommended([&](std::string_view id) { return done.contains(std::string(id)); });
    REQUIRE(next != nullptr);
    CHECK(next->prerequisites.empty());
    for (auto const& e : c.exercises())
    {
        CHECK(e.hints[0] != e.hints[1]);
        CHECK(!e.checks.empty());
    }
}

SECTION("learndc.bundle");

namespace
{
    void put_u32(std::string& out, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            out += static_cast<char>((value >> (8 * i)) & 0xFF);
    }

    void put_entry(std::string& out, std::string_view path, std::string_view data)
    {
        put_u32(out, static_cast<std::uint32_t>(path.size()));
        out += path;
        put_u32(out, static_cast<std::uint32_t>(data.size()));
        out += data;
    }
} // namespace

TEST_CASE("bundle decodes archives")
{
    std::string archive(bundle::magic);
    put_entry(archive, "catalog.toml", "schema = 1\n");
    put_entry(archive, "tracks/a/empty.md", "");
    put_entry(archive, "tracks/a/bin.dc", std::string_view("a\0b", 3));
    auto source = bundle::decode(archive);
    REQUIRE(source);
    CHECK_EQ(source->label, "embedded");
    CHECK_EQ(source->files.size(), 3u);
    CHECK_EQ(*source->get("catalog.toml"), "schema = 1\n");
    CHECK_EQ(*source->get("tracks/a/empty.md"), "");
    CHECK_EQ(source->get("tracks/a/bin.dc")->size(), 3u);
    CHECK_EQ(bundle::decode(archive, "label")->label, "label");

    auto empty = bundle::decode(std::string(bundle::magic));
    REQUIRE(empty);
    CHECK(empty->files.empty());
}

TEST_CASE("bundle rejects corrupt archives")
{
    CHECK(!bundle::decode(""));
    CHECK(!bundle::decode("LDCB2\nxxxx"));
    CHECK(!bundle::decode("LDCB1"));
    std::string good(bundle::magic);
    put_entry(good, "a.toml", "data");

    for (std::size_t cut = bundle::magic.size() + 1; cut < good.size(); ++cut)
        CHECK(!bundle::decode(std::string_view(good).substr(0, cut)));

    std::string huge_path(bundle::magic);
    put_u32(huge_path, 0xFFFFFFF0u);
    huge_path += "abc";
    CHECK(!bundle::decode(huge_path));

    std::string huge_data(bundle::magic);
    put_u32(huge_data, 1);
    huge_data += "a";
    put_u32(huge_data, 0xFFFFFFFFu);
    huge_data += "xyz";
    CHECK(!bundle::decode(huge_data));

    std::string zero_path(bundle::magic);
    put_entry(zero_path, "", "x");
    CHECK(!bundle::decode(zero_path));

    std::string duplicate(bundle::magic);
    put_entry(duplicate, "a.toml", "1");
    put_entry(duplicate, "a.toml", "2");
    auto dup = bundle::decode(duplicate);
    REQUIRE(!dup);
    CHECK(dup.error().message.find("twice") != std::string::npos);

    std::string traversal(bundle::magic);
    put_entry(traversal, "../evil.toml", "1");
    auto trav = bundle::decode(traversal);
    REQUIRE(!trav);
    CHECK(trav.error().message.find("'..'") != std::string::npos);

    std::string absolute(bundle::magic);
    put_entry(absolute, "/etc/passwd", "1");
    CHECK(!bundle::decode(absolute));
}

TEST_CASE("embedded bundle decodes and loads")
{
    auto source = bundle::embedded();
    if (!source)
        std::println(std::cerr, "    {}", source.error().message);
    REQUIRE(source);
    CHECK(source->contains("catalog.toml"));
    CHECK(source->contains("tracks/fundamentals/hello-world/exercise.toml"));
    auto loaded = catalog::Catalog::load(*source);
    if (!loaded)
        dump_errors(loaded.error());
    REQUIRE(loaded);
    auto const* hello = loaded->find("fundamentals/hello-world");
    REQUIRE(hello != nullptr);
    CHECK(hello->hints[2].size() > 10u);
    CHECK(bundle::raw().starts_with(bundle::magic));
}

SECTION("learndc.diff");

namespace
{
    std::size_t lcs_length(std::vector<std::string> const& a, std::vector<std::string> const& b)
    {
        std::vector<std::vector<std::size_t>> table(a.size() + 1, std::vector<std::size_t>(b.size() + 1, 0));
        for (std::size_t i = 1; i <= a.size(); ++i)
            for (std::size_t j = 1; j <= b.size(); ++j)
                table[i][j] = a[i - 1] == b[j - 1] ? table[i - 1][j - 1] + 1 : std::max(table[i - 1][j], table[i][j - 1]);
        return table[a.size()][b.size()];
    }

    std::string join_lines(std::vector<std::string> const& lines)
    {
        std::string out;
        for (auto const& l : lines)
            out += l + "\n";
        return out;
    }

    bool edits_reconstruct(std::string const& a, std::string const& b)
    {
        auto const la = diff::to_lines(a);
        auto const lb = diff::to_lines(b);
        auto const edits = diff::diff_lines(la, lb);
        std::size_t ai = 0;
        std::size_t bi = 0;
        for (auto const& e : edits)
        {
            switch (e.op)
            {
                case diff::Op::Equal:
                    if (e.a != ai || e.b != bi || !(la[ai] == lb[bi]))
                        return false;
                    ++ai;
                    ++bi;
                    break;
                case diff::Op::Delete:
                    if (e.a != ai)
                        return false;
                    ++ai;
                    break;
                case diff::Op::Insert:
                    if (e.b != bi)
                        return false;
                    ++bi;
                    break;
            }
        }
        return ai == la.size() && bi == lb.size();
    }
} // namespace

TEST_CASE("unified diff output")
{
    CHECK_EQ(diff::unified("a\nb\n", "a\nb\n"), "");
    CHECK_EQ(diff::unified("", ""), "");
    CHECK_EQ(diff::unified("one\ntwo\nthree\n", "one\n2\nthree\n", "starter/x", "workspace/x"),
             "--- starter/x\n+++ workspace/x\n@@ -1,3 +1,3 @@\n one\n-two\n+2\n three\n");
    CHECK_EQ(diff::unified("", "x\n"), "--- a\n+++ b\n@@ -0,0 +1 @@\n+x\n");
    CHECK_EQ(diff::unified("x\n", ""), "--- a\n+++ b\n@@ -1 +0,0 @@\n-x\n");
    CHECK_EQ(diff::unified("a\n", "a\nb\n"), "--- a\n+++ b\n@@ -1 +1,2 @@\n a\n+b\n");
    CHECK_EQ(diff::unified("a\nb\n", "b\n"), "--- a\n+++ b\n@@ -1,2 +1 @@\n-a\n b\n");
}

TEST_CASE("unified diff splits distant changes into hunks")
{
    std::vector<std::string> a;
    for (int i = 1; i <= 20; ++i)
        a.push_back(std::format("line{}", i));
    auto b = a;
    b[1] = "changed2";
    b[18] = "changed19";
    auto const out = diff::unified(join_lines(a), join_lines(b));
    auto const first = out.find("@@ ");
    auto const second = out.find("@@ ", first + 1);
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    CHECK(out.find("@@ -1,5 +1,5 @@") != std::string::npos);
    CHECK(out.find("@@ -16,5 +16,5 @@") != std::string::npos);
    CHECK(out.find("-line2\n+changed2\n") != std::string::npos);
    CHECK(out.find("-line19\n+changed19\n") != std::string::npos);
    CHECK(out.find(" line10\n") == std::string::npos);

    b[10] = "mid";
    auto const merged = diff::unified(join_lines(a), join_lines(b), "a", "b", 3);
    CHECK_EQ(std::ranges::count(merged, '@') / 4, 3);

    auto const wide = diff::unified(join_lines(a), join_lines(b), "a", "b", 20);
    CHECK_EQ(std::ranges::count(wide, '@') / 4, 1);
    CHECK(wide.find("@@ -1,20 +1,20 @@") != std::string::npos);

    auto const none = diff::unified("a\nb\nc\n", "a\nX\nc\n", "a", "b", 0);
    CHECK_EQ(none, "--- a\n+++ b\n@@ -2 +2 @@\n-b\n+X\n");
}

TEST_CASE("diff handles a missing final newline")
{
    CHECK_EQ(diff::unified("a\nb", "a\nb\n"), "--- a\n+++ b\n@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+b\n");
    CHECK_EQ(diff::unified("a\nb\n", "a\nb"), "--- a\n+++ b\n@@ -1,2 +1,2 @@\n a\n-b\n+b\n\\ No newline at end of file\n");
    CHECK_EQ(diff::unified("a", "a"), "");
    auto const lines = diff::to_lines("a\nb");
    CHECK_EQ(lines.size(), 2u);
    CHECK(lines[0].newline);
    CHECK(!lines[1].newline);
    CHECK(diff::to_lines("").empty());
    CHECK_EQ(diff::to_lines("\n").size(), 1u);
}

TEST_CASE("diff edit scripts are minimal and consistent")
{
    CHECK(edits_reconstruct("", ""));
    CHECK(edits_reconstruct("a\n", ""));
    CHECK(edits_reconstruct("", "a\n"));
    CHECK(edits_reconstruct("a\nb\nc\n", "x\ny\nz\n"));
    CHECK(edits_reconstruct("a\nb\nc\nd\ne\n", "b\nd\ne\nf\n"));

    std::uint32_t state = 12345;
    auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 16;
    };
    for (int round = 0; round < 300; ++round)
    {
        std::vector<std::string> a;
        std::vector<std::string> b;
        auto const alphabet = 2 + next() % 5;
        for (std::uint32_t i = next() % 25; i > 0; --i)
            a.push_back(std::format("l{}", next() % alphabet));
        for (std::uint32_t i = next() % 25; i > 0; --i)
            b.push_back(std::format("l{}", next() % alphabet));
        auto const text_a = join_lines(a);
        auto const text_b = join_lines(b);
        if (!edits_reconstruct(text_a, text_b))
        {
            std::println(std::cerr, "    reconstruction failed in round {}", round);
            CHECK(false);
            continue;
        }
        auto const edits = diff::diff_text(text_a, text_b);
        std::size_t equal = 0;
        for (auto const& e : edits)
            if (e.op == diff::Op::Equal)
                ++equal;
        if (equal != lcs_length(a, b))
        {
            std::println(std::cerr, "    non-minimal diff in round {}: {} equal vs LCS {}", round, equal, lcs_length(a, b));
            CHECK(false);
        }
        else
            CHECK(true);
    }
}

TEST_CASE("diff survives very different large inputs")
{
    std::vector<std::string> a;
    std::vector<std::string> b;
    for (int i = 0; i < 3000; ++i)
    {
        a.push_back(std::format("a{}", i));
        b.push_back(std::format("b{}", i));
    }
    auto const ta = join_lines(a);
    auto const tb = join_lines(b);
    CHECK(edits_reconstruct(ta, tb));
    auto const out = diff::unified(ta, tb);
    CHECK(out.starts_with("--- a\n+++ b\n@@ -1,3000 +1,3000 @@\n"));

    std::vector<std::string> same;
    for (int i = 0; i < 20000; ++i)
        same.push_back(std::format("row{}", i % 97));
    auto changed = same;
    changed[10000] = "different";
    CHECK(edits_reconstruct(join_lines(same), join_lines(changed)));
}

TEST_CASE("side by side rows")
{
    auto rows = diff::side_by_side("one\ntwo\nthree\nfour\n", "one\n2\nthree\nfour\nfive\n");
    CHECK_EQ(rows.size(), 5u);
    CHECK(rows[0].kind == diff::RowKind::Equal);
    CHECK_EQ(rows[0].left, "one");
    CHECK_EQ(rows[0].left_number.value(), 1u);
    CHECK_EQ(rows[0].right_number.value(), 1u);
    CHECK(rows[1].kind == diff::RowKind::Change);
    CHECK_EQ(rows[1].left, "two");
    CHECK_EQ(rows[1].right, "2");
    CHECK_EQ(rows[1].left_number.value(), 2u);
    CHECK_EQ(rows[1].right_number.value(), 2u);
    CHECK(rows[4].kind == diff::RowKind::Insert);
    CHECK(!rows[4].left_number.has_value());
    CHECK_EQ(rows[4].right_number.value(), 5u);
    CHECK_EQ(rows[4].right, "five");

    rows = diff::side_by_side("a\nb\nc\n", "a\n");
    CHECK_EQ(rows.size(), 3u);
    CHECK(rows[1].kind == diff::RowKind::Delete);
    CHECK_EQ(rows[1].left, "b");
    CHECK(!rows[1].right_number.has_value());
    CHECK(rows[2].kind == diff::RowKind::Delete);

    rows = diff::side_by_side("a\nb\n", "x\ny\nz\n");
    CHECK_EQ(rows.size(), 3u);
    CHECK(rows[0].kind == diff::RowKind::Change);
    CHECK(rows[1].kind == diff::RowKind::Change);
    CHECK(rows[2].kind == diff::RowKind::Insert);
    CHECK(diff::side_by_side("", "").empty());
}

namespace
{
    std::string g_self;

    catalog::Exercise make_exercise(std::string id = "basics/one", int revision = 1)
    {
        catalog::Exercise ex;
        ex.id = id;
        auto const slash = id.find('/');
        ex.track = id.substr(0, slash);
        ex.name = id.substr(slash + 1);
        ex.revision = revision;
        ex.starter["project.dcon"] = "dpm::v1::Project { name = \"prog\" }\n";
        ex.starter["src/main.dc"] = "module main;\npublic i32 main() { return 1; }\n";
        ex.starter["src/util/helper.dc"] = "module helper;\n";
        ex.solution["src/main.dc"] = "module main;\npublic i32 main() { return 0; }\n";
        return ex;
    }

    std::string slurp(std::filesystem::path const& p)
    {
        return platform::read_file(p).value_or("<missing>");
    }

    bool symlink_supported(std::filesystem::path const& dir)
    {
        std::error_code ec;
        std::filesystem::create_directory_symlink(dir, dir.parent_path() / "probe-link", ec);
        bool const ok = !ec;
        std::filesystem::remove(dir.parent_path() / "probe-link", ec);
        return ok;
    }

    std::vector<std::filesystem::path> entries_with_prefix(std::filesystem::path const& dir, std::string_view prefix)
    {
        std::vector<std::filesystem::path> out;
        std::error_code ec;
        for (auto const& e : std::filesystem::directory_iterator(dir, ec))
            if (platform::path_to_utf8(e.path().filename()).starts_with(prefix))
                out.push_back(e.path());
        return out;
    }
}

SECTION("learndc.settings");

TEST_CASE("settings defaults list and get")
{
    settings::Settings s;
    CHECK_EQ(settings::get(s, "editor.preset").value(), "auto");
    CHECK_EQ(settings::get(s, "ui.theme").value(), "dark");
    CHECK_EQ(settings::get(s, "checks.timeout_seconds").value(), "10");
    CHECK_EQ(settings::get(s, "checks.build_timeout_seconds").value(), "120");
    CHECK_EQ(settings::get(s, "checks.max_output_kib").value(), "256");
    CHECK_EQ(settings::get(s, "checks.watch_debounce_ms").value(), "400");
    CHECK_EQ(settings::get(s, "ui.open_continue").value(), "true");
    CHECK_EQ(settings::get(s, "onboarding.done").value(), "false");
    CHECK_EQ(settings::get(s, "editor.command").value(), "");
    CHECK_EQ(settings::list(s).size(), settings::keys().size());
    CHECK_EQ(settings::list(s).size(), 16u);
    CHECK(settings::find_key("tools.dcc") != nullptr);
    CHECK(settings::find_key("tools.nope") == nullptr);
}

TEST_CASE("settings set validates every key")
{
    settings::Settings s;
    CHECK(settings::set(s, "editor.preset", "nvim"));
    CHECK_EQ(s.editor_preset, "nvim");
    CHECK(settings::set(s, "editor.preset", " CODE "));
    CHECK_EQ(s.editor_preset, "code");
    CHECK(settings::set(s, "ui.reduced_motion", "yes"));
    CHECK(s.ui_reduced_motion);
    CHECK(settings::set(s, "ui.reduced_motion", "off"));
    CHECK(!s.ui_reduced_motion);
    CHECK(settings::set(s, "checks.timeout_seconds", "30"));
    CHECK_EQ(s.checks_timeout_seconds, 30);
    CHECK(settings::set(s, "tools.dcc", "/opt/dcc/bin/dcc"));
    CHECK_EQ(s.tools_dcc, "/opt/dcc/bin/dcc");
    CHECK(settings::set(s, "learner.level", "experienced"));

    auto before = s;
    auto bad = settings::set(s, "editor.preset", "emacs");
    REQUIRE(!bad);
    CHECK(bad.error().message.find("expected one of") != std::string::npos);
    CHECK(bad.error().message.find("nvim") != std::string::npos);
    bad = settings::set(s, "ui.theme", "");
    CHECK(!bad);
    bad = settings::set(s, "editor.wait", "maybe");
    REQUIRE(!bad);
    CHECK(bad.error().message.find("true or false") != std::string::npos);
    bad = settings::set(s, "checks.timeout_seconds", "0");
    REQUIRE(!bad);
    CHECK(bad.error().message.find("between 1 and 600") != std::string::npos);
    CHECK(!settings::set(s, "checks.timeout_seconds", "601"));
    CHECK(!settings::set(s, "checks.timeout_seconds", "-5"));
    CHECK(!settings::set(s, "checks.timeout_seconds", "12x"));
    CHECK(!settings::set(s, "checks.timeout_seconds", ""));
    CHECK(!settings::set(s, "checks.timeout_seconds", "99999999999999999999"));
    CHECK(!settings::set(s, "checks.build_timeout_seconds", "5"));
    CHECK(!settings::set(s, "checks.watch_debounce_ms", "10"));
    CHECK(!settings::set(s, "tools.dpm", "a\nb"));
    CHECK(!settings::set(s, "tools.dpm", "bad\xff"));
    bad = settings::set(s, "editor.colour", "x");
    REQUIRE(!bad);
    CHECK(bad.error().message.find("unknown setting 'editor.colour'") != std::string::npos);
    CHECK(bad.error().message.find("known settings:") != std::string::npos);
    CHECK(!settings::get(s, "nope"));
    CHECK(s == before);
}

TEST_CASE("settings editor command accepts words and json arrays")
{
    settings::Settings s;
    CHECK(settings::set(s, "editor.command", "code --wait --goto \"{file}:{line}\""));
    CHECK_EQ(s.editor_command.size(), 4u);
    CHECK_EQ(s.editor_command[3], "{file}:{line}");
    CHECK(settings::set(s, "editor.command", "[\"nvim\", \"+{line}\", \"{file}\"]"));
    CHECK_EQ(s.editor_command.size(), 3u);
    CHECK_EQ(s.editor_command[1], "+{line}");
    CHECK_EQ(settings::get(s, "editor.command").value(), "nvim +{line} {file}");
    CHECK(!settings::set(s, "editor.command", "\"unterminated"));
    CHECK(!settings::set(s, "editor.command", "[1, 2]"));
    CHECK(!settings::set(s, "editor.command", "[\"\"]"));
    CHECK(!settings::set(s, "editor.command", "[\"a\\u0001b\"]"));
    CHECK_EQ(s.editor_command.size(), 3u);
    CHECK(settings::set(s, "editor.command", ""));
    CHECK(s.editor_command.empty());
}

TEST_CASE("settings json round trip and strict loading")
{
    settings::Settings s;
    CHECK(settings::set(s, "editor.preset", "custom"));
    CHECK(settings::set(s, "editor.command", "hx {file}"));
    CHECK(settings::set(s, "ui.theme", "high-contrast"));
    CHECK(settings::set(s, "checks.max_output_kib", "512"));
    CHECK(settings::set(s, "onboarding.done", "true"));
    auto text_out = settings::to_json(s).dump(2);
    auto back = settings::parse(text_out);
    REQUIRE(back);
    CHECK(*back == s);

    CHECK(!settings::parse("[]"));
    CHECK(!settings::parse("{"));
    auto unknown = settings::parse("{\"schema\":1,\"editor\":{\"colour\":\"x\"}}");
    REQUIRE(!unknown);
    CHECK(unknown.error().message.find("editor.colour") != std::string::npos);
    CHECK(!settings::parse("{\"schema\":2}"));
    CHECK(!settings::parse("{\"ui\":{\"reduced_motion\":\"yes\"}}"));
    CHECK(!settings::parse("{\"checks\":{\"timeout_seconds\":\"5\"}}"));
    CHECK(!settings::parse("{\"checks\":{\"timeout_seconds\":9999}}"));
    CHECK(!settings::parse("{\"editor\":{\"preset\":\"emacs\"}}"));
    CHECK(!settings::parse("{\"bogus\":1}"));
    auto partial = settings::parse("{\"schema\":1,\"ui\":{\"theme\":\"light\"}}");
    REQUIRE(partial);
    CHECK_EQ(partial->ui_theme, "light");
    CHECK_EQ(partial->checks_timeout_seconds, 10);
}

TEST_CASE("settings files load save and update")
{
    TempDir t;
    auto const file = t.path / "cfg" / "config.json";
    auto missing = settings::load(file);
    REQUIRE(missing);
    CHECK(*missing == settings::Settings{});
    settings::Settings s;
    CHECK(settings::set(s, "ui.ascii", "on"));
    CHECK(settings::save(file, s));
    auto loaded = settings::load(file);
    REQUIRE(loaded);
    CHECK_EQ(loaded->ui_ascii, "on");

    auto updated = settings::update(file, [](settings::Settings& x) { return settings::set(x, "ui.theme", "light"); });
    REQUIRE(updated);
    CHECK_EQ(updated->ui_theme, "light");
    CHECK_EQ(updated->ui_ascii, "on");
    auto failed = settings::update(file, [](settings::Settings& x) { return settings::set(x, "ui.theme", "neon"); });
    CHECK(!failed);
    CHECK_EQ(settings::load(file)->ui_theme, "light");

    write_file(file, "{ not json");
    auto broken = settings::load(file);
    REQUIRE(!broken);
    CHECK(broken.error().message.find("config.json") != std::string::npos);
    CHECK_EQ(slurp(file), "{ not json");
}

SECTION("learndc.progress");

TEST_CASE("progress records checks and never loses completion")
{
    progress::Progress p;
    CHECK(progress::status_of(p, "a/b") == progress::Status::NotStarted);
    progress::record_check(p, "a/b", 1, 2, 1, "2026-01-01T00:00:00Z");
    auto const& e1 = p.exercises.at("a/b");
    CHECK(e1.status == progress::Status::InProgress);
    CHECK_EQ(e1.attempts, 1);
    CHECK_EQ(e1.last_passed, 1);
    CHECK_EQ(e1.last_total, 2);
    CHECK_EQ(e1.first_started_at, "2026-01-01T00:00:00Z");
    CHECK(e1.completed_at.empty());
    CHECK_EQ(p.recent.size(), 1u);

    progress::record_check(p, "a/b", 2, 2, 1, "2026-01-02T00:00:00Z");
    auto const& e2 = p.exercises.at("a/b");
    CHECK(e2.status == progress::Status::Completed);
    CHECK_EQ(e2.completed_at, "2026-01-02T00:00:00Z");
    CHECK_EQ(e2.completed_revision, 1);
    CHECK_EQ(e2.passes, 1);

    progress::record_check(p, "a/b", 0, 2, 1, "2026-01-03T00:00:00Z");
    auto const& e3 = p.exercises.at("a/b");
    CHECK(e3.status == progress::Status::Completed);
    CHECK_EQ(e3.completed_at, "2026-01-02T00:00:00Z");
    CHECK_EQ(e3.attempts, 3);
    CHECK_EQ(e3.last_passed, 0);
    CHECK(progress::needs_revisit(e3, 2));
    CHECK(!progress::needs_revisit(e3, 1));
    CHECK(!progress::needs_revisit(progress::ExerciseProgress{}, 5));

    progress::record_check(p, "a/b", 2, 2, 2, "2026-02-01T00:00:00Z");
    CHECK_EQ(p.exercises.at("a/b").completed_revision, 2);
    CHECK_EQ(p.exercises.at("a/b").completed_at, "2026-01-02T00:00:00Z");
}

TEST_CASE("progress hints bookmarks recent and workspaces")
{
    progress::Progress p;
    progress::reveal_hint(p, "a/b", 2, "t1");
    progress::reveal_hint(p, "a/b", 1, "t2");
    CHECK_EQ(p.exercises.at("a/b").hint_level, 2);
    progress::reveal_hint(p, "a/b", 9, "t3");
    CHECK_EQ(p.exercises.at("a/b").hint_level, 3);
    progress::mark_solution_viewed(p, "a/b", "t4");
    CHECK(p.exercises.at("a/b").solution_viewed);
    CHECK(progress::toggle_bookmark(p, "a/b"));
    CHECK(progress::is_bookmarked(p, "a/b"));
    CHECK(!progress::toggle_bookmark(p, "a/b"));
    CHECK(p.bookmarks.empty());
    for (int i = 0; i < 15; ++i)
        progress::touch_recent(p, std::format("t/e{}", i));
    CHECK_EQ(p.recent.size(), progress::max_recent);
    CHECK_EQ(p.recent.front(), "t/e14");
    progress::touch_recent(p, "t/e10");
    CHECK_EQ(p.recent.front(), "t/e10");
    CHECK_EQ(p.recent.size(), progress::max_recent);
}

TEST_CASE("progress json round trip")
{
    progress::Progress p;
    progress::record_check(p, "a/b", 2, 2, 3, "2026-01-02T00:00:00Z");
    progress::reveal_hint(p, "a/c", 1, "2026-01-03T00:00:00Z");
    progress::toggle_bookmark(p, "a/c");
    progress::set_active(p, "a/c");
    p.workspaces["a/c"] = "/tmp/learn dc/\xC3\xBC" "bung \xE5\xB7\xA5\xE5\x85\xB7";
    auto back = progress::from_json(progress::to_json(p));
    REQUIRE(back);
    CHECK(*back == p);

    CHECK(!progress::from_json(json::Json::make_array()));
    CHECK(!progress::from_json(*json::parse("{}")));
    int unsupported = 0;
    auto future = progress::from_json(*json::parse("{\"schema\":7}"), &unsupported);
    CHECK(!future);
    CHECK_EQ(unsupported, 7);
    CHECK(!progress::from_json(*json::parse("{\"schema\":1,\"exercises\":{\"a/b\":{\"status\":\"done\"}}}")));
    CHECK(!progress::from_json(*json::parse("{\"schema\":1,\"exercises\":{\"a/b\":{\"status\":\"completed\",\"attempts\":-1}}}")));
    CHECK(!progress::from_json(*json::parse("{\"schema\":1,\"exercises\":[]}")));
    CHECK(!progress::from_json(*json::parse("{\"schema\":1,\"bookmarks\":[1]}")));
    auto minimal = progress::from_json(*json::parse("{\"schema\":1,\"exercises\":{\"a/b\":{\"status\":\"completed\"}}}"));
    REQUIRE(minimal);
    CHECK(progress::is_completed(*minimal, "a/b"));
}

TEST_CASE("progress store persists and keeps a backup")
{
    TempDir t;
    progress::ProgressStore store(t.path / "data" / "progress.json", t.path / "data" / "progress.lock");
    auto first = store.load();
    REQUIRE(first);
    CHECK(first->progress == progress::Progress{});
    CHECK(first->warnings.empty());
    CHECK(!std::filesystem::exists(store.file()));

    auto a = store.update([](progress::Progress& p) { progress::record_check(p, "a/b", 1, 3, 1, "2026-01-01T00:00:00Z"); });
    REQUIRE(a);
    CHECK(std::filesystem::exists(store.file()));
    CHECK(!std::filesystem::exists(store.backup_file()));
    auto b = store.update([](progress::Progress& p) { progress::set_active(p, "a/b"); });
    REQUIRE(b);
    CHECK(std::filesystem::exists(store.backup_file()));

    auto loaded = store.load();
    REQUIRE(loaded);
    CHECK_EQ(loaded->progress.active_exercise, "a/b");
    CHECK_EQ(loaded->progress.exercises.at("a/b").attempts, 1);
    auto backup = json::parse(slurp(store.backup_file()));
    REQUIRE(backup);
    auto backup_progress = progress::from_json(*backup);
    REQUIRE(backup_progress);
    CHECK(backup_progress->active_exercise.empty());
    CHECK_EQ(backup_progress->exercises.at("a/b").attempts, 1);
    CHECK(entries_with_prefix(t.path / "data", "progress.json.tmp").empty());
}

TEST_CASE("progress store does not quarantine a file it cannot read")
{
    TempDir t;
    auto const dir = t.path / "data";
    progress::ProgressStore store(dir / "progress.json", dir / "progress.lock");
    REQUIRE(store.update([](progress::Progress& p) { progress::set_active(p, "a/b"); }));
    REQUIRE(store.update([](progress::Progress& p) { progress::set_active(p, "c/d"); }));
    std::filesystem::permissions(store.file(), std::filesystem::perms::none);
    if (platform::read_file(store.file()))
    {
        std::filesystem::permissions(store.file(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        return;
    }
    auto loaded = store.load();
    REQUIRE(!loaded);
    CHECK(loaded.error().message.find("cannot be read") != std::string::npos);
    CHECK(!store.update([](progress::Progress& p) { progress::set_active(p, "e/f"); }));
    CHECK(std::filesystem::exists(store.file()));
    CHECK(entries_with_prefix(dir, "progress.json.corrupt-").empty());
    std::filesystem::permissions(store.file(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    CHECK(progress::is_completed(store.load()->progress, "a/b") == false);
    CHECK_EQ(store.load()->progress.active_exercise, "c/d");
}

TEST_CASE("progress store quarantines corrupt files")
{
    TempDir t;
    auto const dir = t.path / "data";
    progress::ProgressStore store(dir / "progress.json", dir / "progress.lock");
    write_file(store.file(), "{ this is not json");
    auto loaded = store.load();
    REQUIRE(loaded);
    CHECK(loaded->progress == progress::Progress{});
    CHECK_EQ(loaded->warnings.size(), 1u);
    CHECK(loaded->warnings[0].find("corrupt") != std::string::npos);
    auto quarantined = entries_with_prefix(dir, "progress.json.corrupt-");
    CHECK_EQ(quarantined.size(), 1u);
    CHECK_EQ(slurp(quarantined[0]), "{ this is not json");
    auto again = store.load();
    REQUIRE(again);
    CHECK(again->warnings.empty());

    REQUIRE(store.update([](progress::Progress& p) { progress::record_check(p, "a/b", 2, 2, 1, "2026-01-01T00:00:00Z"); }));
    REQUIRE(store.update([](progress::Progress& p) { progress::set_active(p, "a/b"); }));
    write_file(store.file(), "{\"schema\":1,\"exercises\":{\"a/b\":{\"status\":\"bogus\"}}}");
    auto recovered = store.load();
    REQUIRE(recovered);
    CHECK_EQ(recovered->warnings.size(), 1u);
    CHECK(recovered->warnings[0].find("restored from the previous backup") != std::string::npos);
    CHECK(progress::is_completed(recovered->progress, "a/b"));
    CHECK_EQ(entries_with_prefix(dir, "progress.json.corrupt-").size(), 2u);

    write_file(store.file(), "");
    auto empty_file = store.update([](progress::Progress& p) { progress::set_active(p, "x/y"); });
    REQUIRE(empty_file);
    CHECK_EQ(empty_file->warnings.size(), 1u);
    CHECK_EQ(empty_file->progress.active_exercise, "x/y");
    CHECK_EQ(store.load()->progress.active_exercise, "x/y");
}

TEST_CASE("progress store refuses to overwrite a newer schema")
{
    TempDir t;
    progress::ProgressStore store(t.path / "progress.json", t.path / "progress.lock");
    write_file(store.file(), "{\"schema\":2,\"future\":true}");
    auto loaded = store.load();
    REQUIRE(!loaded);
    CHECK(loaded.error().message.find("unsupported progress schema 2") != std::string::npos);
    CHECK(!store.update([](progress::Progress& p) { progress::set_active(p, "a/b"); }));
    CHECK_EQ(slurp(store.file()), "{\"schema\":2,\"future\":true}");
    CHECK(entries_with_prefix(t.path, "progress.json.corrupt-").empty());
}

TEST_CASE("progress store merges concurrent updates from threads")
{
    TempDir t;
    progress::ProgressStore a(t.path / "progress.json", t.path / "progress.lock");
    progress::ProgressStore b(t.path / "progress.json", t.path / "progress.lock");
    constexpr int per_thread = 25;
    std::atomic<int> failures{0};
    auto work = [&](progress::ProgressStore& store, std::string tag) {
        for (int i = 0; i < per_thread; ++i)
        {
            auto r = store.update([&](progress::Progress& p) {
                ++p.exercises["shared/counter"].attempts;
                p.bookmarks.push_back(std::format("{}/{}", tag, i));
            });
            if (!r)
                ++failures;
        }
    };
    std::thread one(work, std::ref(a), std::string("one"));
    std::thread two(work, std::ref(b), std::string("two"));
    one.join();
    two.join();
    CHECK_EQ(failures.load(), 0);
    auto loaded = a.load();
    REQUIRE(loaded);
    CHECK_EQ(loaded->progress.exercises.at("shared/counter").attempts, 2 * per_thread);
    CHECK_EQ(loaded->progress.bookmarks.size(), static_cast<std::size_t>(2 * per_thread));
    CHECK(loaded->warnings.empty());
}

TEST_CASE("progress store merges concurrent updates from processes")
{
    TempDir t;
    auto const file = t.path / "progress.json";
    auto const lock = t.path / "progress.lock";
    constexpr int per_process = 20;
    std::array<dcc::host::process::Result, 2> results;
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < results.size(); ++i)
        threads.emplace_back([&, i] {
            dcc::host::process::Command cmd;
            cmd.argv = {g_self, "--progress-worker", platform::path_to_utf8(file), platform::path_to_utf8(lock), std::format("proc{}", i), std::to_string(per_process)};
            cmd.timeout = std::chrono::seconds(60);
            results[i] = dcc::host::process::run(cmd);
        });
    for (auto& th : threads)
        th.join();
    for (auto const& r : results)
    {
        CHECK(r.started);
        CHECK_EQ(r.exit_code, 0);
    }
    progress::ProgressStore store(file, lock);
    auto loaded = store.load();
    REQUIRE(loaded);
    CHECK_EQ(loaded->progress.exercises.at("shared/counter").attempts, 2 * per_process);
    CHECK_EQ(loaded->progress.bookmarks.size(), static_cast<std::size_t>(2 * per_process));
}

TEST_CASE("progress export parses and rejects foreign files")
{
    progress::Progress p;
    progress::record_check(p, "a/b", 2, 2, 1, "2026-01-02T00:00:00Z");
    progress::toggle_bookmark(p, "a/c");
    auto const exported = progress::export_text(p, "2026-03-01T00:00:00Z");
    auto parsed = json::parse(exported);
    REQUIRE(parsed);
    CHECK_EQ(parsed->find("format")->as_string().value(), "learndc-progress");
    CHECK_EQ(parsed->find("schema")->as_int().value(), 1);
    CHECK_EQ(parsed->find("exported_at")->as_string().value(), "2026-03-01T00:00:00Z");
    auto back = progress::parse_export(exported);
    REQUIRE(back);
    CHECK(*back == p);

    CHECK(!progress::parse_export("nonsense"));
    CHECK(!progress::parse_export("{\"schema\":1,\"progress\":{}}"));
    CHECK(!progress::parse_export("{\"format\":\"other\",\"schema\":1,\"progress\":{\"schema\":1}}"));
    auto future = progress::parse_export("{\"format\":\"learndc-progress\",\"schema\":2,\"progress\":{\"schema\":1}}");
    REQUIRE(!future);
    CHECK(future.error().message.find("unsupported export schema 2") != std::string::npos);
    CHECK(!progress::parse_export("{\"format\":\"learndc-progress\",\"schema\":1}"));
    CHECK(!progress::parse_export("{\"format\":\"learndc-progress\",\"schema\":1,\"progress\":{\"schema\":1,\"exercises\":5}}"));
}

TEST_CASE("progress import merges and reports conflicts")
{
    progress::Progress current;
    progress::record_check(current, "a/one", 1, 2, 1, "2026-01-05T00:00:00Z");
    progress::record_check(current, "a/two", 2, 2, 1, "2026-01-06T00:00:00Z");
    progress::set_active(current, "a/one");
    current.bookmarks = {"a/one"};
    current.workspaces["a/one"] = "/work/one";

    progress::Progress incoming;
    progress::record_check(incoming, "a/one", 2, 2, 2, "2026-01-03T00:00:00Z");
    incoming.exercises["a/one"].attempts = 7;
    incoming.exercises["a/one"].hint_level = 2;
    incoming.exercises["a/one"].first_started_at = "2025-12-31T00:00:00Z";
    progress::record_check(incoming, "a/two", 1, 2, 1, "2026-01-07T00:00:00Z");
    incoming.exercises["a/two"].attempts = 1;
    progress::record_check(incoming, "a/three", 2, 2, 1, "2026-01-08T00:00:00Z");
    incoming.bookmarks = {"a/two", "a/one"};
    incoming.workspaces["a/one"] = "/other/one";
    incoming.workspaces["a/three"] = "/work/three";
    progress::set_active(incoming, "a/three");

    auto const snapshot = current;
    auto plan = progress::plan_import(current, incoming);
    CHECK(current == snapshot);

    auto const& one = plan.merged.exercises.at("a/one");
    CHECK(one.status == progress::Status::Completed);
    CHECK_EQ(one.attempts, 7);
    CHECK_EQ(one.hint_level, 2);
    CHECK_EQ(one.completed_at, "2026-01-03T00:00:00Z");
    CHECK_EQ(one.completed_revision, 2);
    CHECK_EQ(one.first_started_at, "2025-12-31T00:00:00Z");
    CHECK_EQ(one.last_checked_at, "2026-01-05T00:00:00Z");
    CHECK_EQ(one.last_passed, 1);
    auto const& two = plan.merged.exercises.at("a/two");
    CHECK(two.status == progress::Status::Completed);
    CHECK_EQ(two.last_checked_at, "2026-01-07T00:00:00Z");
    CHECK_EQ(two.last_passed, 1);
    CHECK(plan.merged.exercises.contains("a/three"));
    CHECK_EQ(plan.merged.active_exercise, "a/one");
    CHECK_EQ(plan.merged.bookmarks, (std::vector<std::string>{"a/one", "a/two"}));
    CHECK_EQ(plan.merged.workspaces.at("a/one"), "/work/one");
    CHECK_EQ(plan.merged.workspaces.at("a/three"), "/work/three");

    auto has_conflict = [&](std::string_view exercise, std::string_view field) {
        return std::ranges::any_of(plan.conflicts, [&](progress::Conflict const& c) { return c.exercise == exercise && c.field == field; });
    };
    CHECK(has_conflict("a/one", "status"));
    CHECK(!has_conflict("a/one", "completed_revision"));
    CHECK(has_conflict("a/one", "workspace"));
    CHECK(has_conflict("a/two", "status"));
    CHECK(has_conflict("", "active_exercise"));
    for (auto const& c : plan.conflicts)
        CHECK(!c.describe().empty());

    auto has_change = [&](std::string_view exercise, std::string_view field) {
        return std::ranges::any_of(plan.changes, [&](progress::Change const& c) { return c.exercise == exercise && c.field == field; });
    };
    CHECK(has_change("a/one", "attempts"));
    CHECK(has_change("a/one", "completed_at"));
    CHECK(has_change("a/three", "status"));
    CHECK(has_change("", "bookmark"));
    CHECK(has_change("a/three", "workspace"));
    CHECK(!has_change("a/two", "status"));
    for (auto const& c : plan.changes)
        CHECK(!c.describe().empty());

    progress::apply_import(current, plan);
    CHECK(current == plan.merged);
    auto idempotent = progress::plan_import(current, incoming);
    CHECK(idempotent.merged == current);
    CHECK(idempotent.changes.empty());
}

TEST_CASE("progress import status conflict keeps the completed side")
{
    progress::Progress current;
    progress::mark_started(current, "a/one", "2026-01-01T00:00:00Z");
    progress::Progress incoming;
    progress::record_check(incoming, "a/one", 2, 2, 1, "2026-01-02T00:00:00Z");
    auto plan = progress::plan_import(current, incoming);
    CHECK(plan.merged.exercises.at("a/one").status == progress::Status::Completed);
    CHECK_EQ(plan.conflicts.size(), 1u);
    CHECK_EQ(plan.conflicts[0].field, "status");
    CHECK_EQ(plan.conflicts[0].resolution, "completed");

    TempDir t;
    progress::ProgressStore store(t.path / "progress.json", t.path / "progress.lock");
    REQUIRE(store.update([&](progress::Progress& p) { p = current; }));
    auto imported = progress::import_into(store, incoming);
    REQUIRE(imported);
    CHECK(progress::is_completed(store.load()->progress, "a/one"));
}

SECTION("learndc.workspace");

TEST_CASE("workspace start creates the starter files and metadata")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "learn dc" / "\xC3\xBC" "bung \xE5\xB7\xA5\xE5\x85\xB7";
    auto started = workspace::start(ex, root);
    REQUIRE(started);
    CHECK(started->created);
    CHECK(!started->outdated);
    CHECK_EQ(started->workspace.meta.exercise, "basics/one");
    CHECK_EQ(started->workspace.meta.revision, 1);
    CHECK(!started->workspace.meta.created_at.empty());
    CHECK_EQ(started->workspace.meta.starter.size(), 3u);
    CHECK_EQ(started->workspace.meta.starter.at("src/main.dc"), workspace::hash_hex(ex.starter["src/main.dc"]));
    CHECK_EQ(slurp(root / "src" / "main.dc"), ex.starter["src/main.dc"]);
    CHECK_EQ(slurp(root / "src" / "util" / "helper.dc"), ex.starter["src/util/helper.dc"]);
    CHECK(std::filesystem::is_regular_file(root / ".learndc" / "workspace.json"));

    auto loaded = workspace::load(root);
    REQUIRE(loaded);
    CHECK_EQ(loaded->meta.exercise, "basics/one");
    CHECK(loaded->meta.starter == started->workspace.meta.starter);
    CHECK_EQ(loaded->meta.created_at, started->workspace.meta.created_at);
    CHECK((workspace::files(*loaded) == std::vector<std::string>{"project.dcon", "src/main.dc", "src/util/helper.dc"}));
    CHECK(workspace::modified(*loaded).empty());
    CHECK(workspace::modified(*loaded, ex).empty());
}

TEST_CASE("workspace fnv1a64 matches known vectors")
{
    CHECK_EQ(workspace::fnv1a64(""), 14695981039346656037ull);
    CHECK_EQ(workspace::fnv1a64("a"), 0xaf63dc4c8601ec8cull);
    CHECK_EQ(workspace::fnv1a64("foobar"), 0x85944171f73967e8ull);
    CHECK_EQ(workspace::hash_hex("a"), "af63dc4c8601ec8c");
    CHECK_EQ(workspace::hash_hex("").size(), 16u);
}

TEST_CASE("workspace start is idempotent and preserves edits")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "ws";
    REQUIRE(workspace::start(ex, root));
    write_file(root / "src" / "main.dc", "my edit\n");
    write_file(root / "notes.txt", "mine\n");

    auto again = workspace::start(ex, root);
    REQUIRE(again);
    CHECK(!again->created);
    CHECK(again->restored.empty());
    CHECK_EQ(slurp(root / "src" / "main.dc"), "my edit\n");
    CHECK_EQ(slurp(root / "notes.txt"), "mine\n");

    std::filesystem::remove(root / "src" / "util" / "helper.dc");
    auto healed = workspace::start(ex, root);
    REQUIRE(healed);
    CHECK(!healed->created);
    CHECK_EQ(healed->restored.size(), 1u);
    CHECK_EQ(slurp(root / "src" / "util" / "helper.dc"), ex.starter["src/util/helper.dc"]);
    CHECK_EQ(slurp(root / "src" / "main.dc"), "my edit\n");

    auto ex2 = make_exercise("basics/one", 2);
    auto outdated = workspace::start(ex2, root);
    REQUIRE(outdated);
    CHECK(outdated->outdated);
    CHECK_EQ(slurp(root / "src" / "main.dc"), "my edit\n");
}

TEST_CASE("workspace start refuses foreign directories")
{
    TempDir t;
    auto ex = make_exercise();
    auto other = make_exercise("basics/two");

    write_file(t.path / "occupied" / "something.txt", "x");
    auto refused = workspace::start(ex, t.path / "occupied");
    REQUIRE(!refused);
    CHECK(refused.error().kind == workspace::ErrorKind::Refused);
    CHECK_EQ(slurp(t.path / "occupied" / "something.txt"), "x");
    CHECK(!std::filesystem::exists(t.path / "occupied" / "project.dcon"));
    CHECK(!std::filesystem::exists(t.path / "occupied" / ".learndc"));

    REQUIRE(workspace::start(ex, t.path / "ws"));
    auto wrong = workspace::start(other, t.path / "ws");
    REQUIRE(!wrong);
    CHECK(wrong.error().kind == workspace::ErrorKind::Refused);
    CHECK(wrong.error().message.find("basics/one") != std::string::npos);

    write_file(t.path / "afile", "x");
    auto onto_file = workspace::start(ex, t.path / "afile");
    REQUIRE(!onto_file);
    CHECK(onto_file.error().kind == workspace::ErrorKind::Refused);

    std::filesystem::create_directories(t.path / "empty");
    auto into_empty = workspace::start(ex, t.path / "empty");
    REQUIRE(into_empty);
    CHECK(into_empty->created);

    write_file(t.path / "broken" / ".learndc" / "workspace.json", "{ nope");
    auto corrupt = workspace::start(ex, t.path / "broken");
    REQUIRE(!corrupt);
    CHECK(corrupt.error().kind == workspace::ErrorKind::Corrupt);

    CHECK(!workspace::start(ex, std::filesystem::path{}));
}

TEST_CASE("workspace start rejects unsafe starter paths before writing anything")
{
    TempDir t;
    std::vector<std::string> const bad = {
        "../escape.dc", "src/../../escape.dc", "/abs/escape.dc", "a\\b.dc", "C:/escape.dc", "C:escape.dc", ".learndc/workspace.json", ".dpm/out", "a//b.dc", "./a.dc", "", "a/./b.dc",
        "trail/", "name.",
    };
    int index = 0;
    for (auto const& path : bad)
    {
        auto ex = make_exercise();
        ex.starter[path] = "evil";
        auto const root = t.path / std::format("ws{}", index++);
        auto r = workspace::start(ex, root);
        REQUIRE(!r);
        CHECK(r.error().kind == workspace::ErrorKind::InvalidPath);
        CHECK(!std::filesystem::exists(root));
    }
    CHECK(!std::filesystem::exists(t.path / "escape.dc"));
    CHECK(!std::filesystem::exists(t.path.parent_path() / "escape.dc"));
}

TEST_CASE("workspace refuses symbolic links")
{
    TempDir t;
    std::filesystem::create_directories(t.path / "outside");
    if (!symlink_supported(t.path / "outside"))
        return;
    auto ex = make_exercise();

    std::error_code ec;
    std::filesystem::create_directory_symlink(t.path / "outside", t.path / "linkroot", ec);
    REQUIRE(!ec);
    auto through_root = workspace::start(ex, t.path / "linkroot");
    REQUIRE(!through_root);
    CHECK(through_root.error().kind == workspace::ErrorKind::Unsafe);
    CHECK(std::filesystem::is_empty(t.path / "outside"));

    auto const root = t.path / "ws";
    REQUIRE(workspace::start(ex, root));
    std::filesystem::remove_all(root / "src");
    std::filesystem::create_directory_symlink(t.path / "outside", root / "src", ec);
    REQUIRE(!ec);
    auto resumed = workspace::start(ex, root);
    REQUIRE(!resumed);
    CHECK(resumed.error().kind == workspace::ErrorKind::Unsafe);
    CHECK(std::filesystem::is_empty(t.path / "outside"));

    auto ws = workspace::load(root);
    REQUIRE(ws);
    auto reset = workspace::reset(*ws, ex, t.path / "backups");
    REQUIRE(!reset);
    CHECK(reset.error().kind == workspace::ErrorKind::Unsafe);
    CHECK(std::filesystem::is_empty(t.path / "outside"));

    std::filesystem::remove(root / "src");
    std::filesystem::create_directories(root / "src");
    write_file(t.path / "secret.txt", "secret");
    std::filesystem::create_symlink(t.path / "secret.txt", root / "src" / "main.dc", ec);
    REQUIRE(!ec);
    auto resumed_file = workspace::start(ex, root);
    REQUIRE(!resumed_file);
    CHECK(resumed_file.error().kind == workspace::ErrorKind::Unsafe);
    CHECK_EQ(slurp(t.path / "secret.txt"), "secret");

    std::filesystem::remove(root / "src" / "main.dc");
    write_file(root / "src" / "main.dc", "x");
    std::filesystem::create_symlink(t.path / "secret.txt", root / "linked.txt", ec);
    auto ws2 = workspace::load(root);
    REQUIRE(ws2);
    auto listed = workspace::files(*ws2);
    CHECK(std::ranges::find(listed, std::string("linked.txt")) == listed.end());
}

TEST_CASE("workspace find_enclosing walks up from nested directories")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "learn dc" / "\xC3\xBC" "bung";
    REQUIRE(workspace::start(ex, root));
    auto found = workspace::find_enclosing(root / "src" / "util");
    REQUIRE(found.has_value());
    CHECK_EQ(found->meta.exercise, "basics/one");
    CHECK(found->root == root);
    auto at_root = workspace::find_enclosing(root);
    CHECK(at_root.has_value());
    std::filesystem::create_directories(t.path / "elsewhere" / "deeper");
    CHECK(!workspace::find_enclosing(t.path / "elsewhere" / "deeper").has_value());
    write_file(root / "sub" / ".learndc" / "workspace.json", "{ broken");
    auto skipped = workspace::find_enclosing(root / "sub");
    REQUIRE(skipped.has_value());
    CHECK(skipped->root == root);
    CHECK(!workspace::load(t.path / "elsewhere"));
    CHECK(workspace::load(t.path / "elsewhere").error().kind == workspace::ErrorKind::NotFound);
}

TEST_CASE("workspace refuses a root nested inside another workspace and files skips nested workspaces")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "ws";
    REQUIRE(workspace::start(ex, root));
    auto nested = workspace::start(ex, root / "sub");
    REQUIRE(!nested);
    CHECK(nested.error().kind == workspace::ErrorKind::Refused);
    CHECK(!std::filesystem::exists(root / "sub" / ".learndc"));
    write_file(root / "inner" / ".learndc" / "workspace.json", "{}");
    write_file(root / "inner" / "mine.txt", "x");
    write_file(root / "own.txt", "y");
    auto const listed = workspace::files(workspace::load(root).value());
    CHECK(std::ranges::find(listed, "own.txt") != listed.end());
    CHECK(std::ranges::find(listed, "inner/mine.txt") == listed.end());
}

TEST_CASE("workspace files applies the ignore rules")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "ws";
    auto started = workspace::start(ex, root);
    REQUIRE(started);
    write_file(root / ".dpm" / "out" / "prog", "binary");
    write_file(root / "src" / ".main.dc.swp", "x");
    write_file(root / "src" / "main.dc~", "x");
    write_file(root / "src" / ".#main.dc", "x");
    write_file(root / "4913", "x");
    write_file(root / "scratch.tmp", "x");
    write_file(root / "docs" / "notes.md", "x");
    write_file(root / "docs" / ".hidden", "x");
    write_file(root / "\xC3\xBC" "ber.dc", "x");
    auto listed = workspace::files(started->workspace);
    CHECK((listed == std::vector<std::string>{"docs/.hidden", "docs/notes.md", "project.dcon", "src/main.dc", "src/util/helper.dc", "\xC3\xBC" "ber.dc"}));
}

TEST_CASE("workspace modified reports edits additions and removals")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "ws";
    auto started = workspace::start(ex, root);
    REQUIRE(started);
    write_file(root / "src" / "main.dc", "changed\n");
    write_file(root / "extra.dc", "new\n");
    std::filesystem::remove(root / "project.dcon");
    auto changes = workspace::modified(started->workspace, ex);
    CHECK_EQ(changes.size(), 3u);
    auto find = [&](std::string_view path) -> workspace::FileChange const* {
        for (auto const& c : changes)
            if (c.path == path)
                return &c;
        return nullptr;
    };
    REQUIRE(find("src/main.dc") != nullptr);
    CHECK(find("src/main.dc")->kind == workspace::ChangeKind::Modified);
    REQUIRE(find("extra.dc") != nullptr);
    CHECK(find("extra.dc")->kind == workspace::ChangeKind::Added);
    REQUIRE(find("project.dcon") != nullptr);
    CHECK(find("project.dcon")->kind == workspace::ChangeKind::Missing);
    CHECK_EQ(workspace::modified(started->workspace).size(), 3u);
}

TEST_CASE("workspace reset takes a backup before restoring")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "ws";
    auto const backups = t.path / "backups";
    auto started = workspace::start(ex, root);
    REQUIRE(started);
    auto ws = started->workspace;
    write_file(root / "src" / "main.dc", "my work\n");
    write_file(root / "notes" / "todo.txt", "remember\n");
    write_file(root / ".dpm" / "out" / "prog", "build output");

    auto result = workspace::reset(ws, ex, backups);
    REQUIRE(result);
    REQUIRE(!result->backup_path.empty());
    CHECK(result->backup_path.lexically_relative(backups / "basics" / "one").native().find("..") == std::string::npos);
    CHECK_EQ(slurp(result->backup_path / "src" / "main.dc"), "my work\n");
    CHECK_EQ(slurp(result->backup_path / "notes" / "todo.txt"), "remember\n");
    CHECK_EQ(slurp(result->backup_path / "project.dcon"), ex.starter["project.dcon"]);
    CHECK(!std::filesystem::exists(result->backup_path / ".dpm"));
    CHECK(!std::filesystem::exists(result->backup_path / ".learndc"));

    CHECK_EQ(slurp(root / "src" / "main.dc"), ex.starter["src/main.dc"]);
    CHECK(!std::filesystem::exists(root / "notes" / "todo.txt"));
    CHECK(!std::filesystem::exists(root / "notes"));
    CHECK(std::filesystem::exists(root / ".dpm" / "out" / "prog"));
    CHECK_EQ(result->removed.size(), 1u);
    CHECK_EQ(result->removed[0], "notes/todo.txt");
    CHECK(result->kept.empty());
    CHECK(workspace::modified(ws, ex).empty());

    auto second = workspace::reset(ws, ex, backups);
    REQUIRE(second);
    CHECK(second->backup_path != result->backup_path);
    CHECK(std::filesystem::exists(second->backup_path));
    CHECK_EQ(entries_with_prefix(backups / "basics" / "one", "").size(), 2u);
}

TEST_CASE("workspace reset names the backup when restoring a starter file fails")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "ws";
    auto const backups = t.path / "backups";
    auto started = workspace::start(ex, root);
    REQUIRE(started);
    auto ws = started->workspace;
    std::filesystem::remove(root / "src" / "main.dc");
    write_file(root / "src" / "main.dc" / "inner.txt", "work\n");
    auto result = workspace::reset(ws, ex, backups);
    REQUIRE(!result);
    CHECK(result.error().message.find("your files were saved in") != std::string::npos);
    CHECK(result.error().message.find(platform::path_to_utf8(backups)) != std::string::npos);
    CHECK_EQ(entries_with_prefix(backups / "basics" / "one", "").size(), 1u);
}

TEST_CASE("workspace reset refuses a backup directory inside the workspace")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "ws";
    auto started = workspace::start(ex, root);
    REQUIRE(started);
    auto ws = started->workspace;
    write_file(root / "src" / "main.dc", "my work\n");
    auto result = workspace::reset(ws, ex, root / "backups");
    REQUIRE(!result);
    CHECK(result.error().kind == workspace::ErrorKind::Refused);
    CHECK_EQ(slurp(root / "src" / "main.dc"), "my work\n");
    CHECK(!std::filesystem::exists(root / "backups"));

    ws.meta.exercise = "basics/other";
    auto wrong = workspace::reset(ws, ex, t.path / "backups");
    REQUIRE(!wrong);
    CHECK(wrong.error().kind == workspace::ErrorKind::Refused);
    CHECK_EQ(slurp(root / "src" / "main.dc"), "my work\n");
}

TEST_CASE("workspace reset without a backup keeps learner files")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "ws";
    auto started = workspace::start(ex, root);
    REQUIRE(started);
    auto ws = started->workspace;
    write_file(root / "src" / "main.dc", "my work\n");
    write_file(root / "notes.txt", "mine\n");
    auto result = workspace::reset(ws, ex, t.path / "backups", false);
    REQUIRE(result);
    CHECK(result->backup_path.empty());
    CHECK(!std::filesystem::exists(t.path / "backups"));
    CHECK_EQ(slurp(root / "src" / "main.dc"), ex.starter["src/main.dc"]);
    CHECK_EQ(slurp(root / "notes.txt"), "mine\n");
    CHECK_EQ(result->kept.size(), 1u);
}

TEST_CASE("workspace reset brings an outdated workspace to the new revision")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "ws";
    auto started = workspace::start(ex, root);
    REQUIRE(started);
    auto ws = started->workspace;
    auto ex2 = make_exercise("basics/one", 2);
    ex2.starter["src/main.dc"] = "module main;\n// new\n";
    auto result = workspace::reset(ws, ex2, t.path / "backups");
    REQUIRE(result);
    CHECK_EQ(ws.meta.revision, 2);
    CHECK_EQ(workspace::load(root)->meta.revision, 2);
    CHECK_EQ(slurp(root / "src" / "main.dc"), ex2.starter["src/main.dc"]);
    CHECK(workspace::modified(ws).empty());
}

TEST_CASE("workspace fresh_copy makes a sibling for a new revision")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "learn dc" / "one";
    auto started = workspace::start(ex, root);
    REQUIRE(started);
    write_file(root / "src" / "main.dc", "old work\n");
    auto ex2 = make_exercise("basics/one", 2);
    ex2.starter["src/main.dc"] = "module main;\n// v2\n";
    auto copy = workspace::fresh_copy(started->workspace, ex2);
    REQUIRE(copy);
    CHECK(copy->created);
    CHECK_EQ(platform::path_to_utf8(copy->workspace.root.filename()), "one.r2");
    CHECK_EQ(copy->workspace.meta.revision, 2);
    CHECK_EQ(slurp(copy->workspace.root / "src" / "main.dc"), ex2.starter["src/main.dc"]);
    CHECK_EQ(slurp(root / "src" / "main.dc"), "old work\n");
    auto twice = workspace::fresh_copy(*workspace::load(root), ex2);
    REQUIRE(twice);
    CHECK(!twice->created);
    auto ex3 = make_exercise("basics/one", 3);
    auto third = workspace::fresh_copy(copy->workspace, ex3);
    REQUIRE(third);
    CHECK_EQ(platform::path_to_utf8(third->workspace.root.filename()), "one.r3");
    CHECK(!workspace::fresh_copy(started->workspace, make_exercise("basics/other")));
}

TEST_CASE("workspace default_root follows track and name")
{
    TempDir t;
    auto dirs = platform::resolve_dirs(platform::Overrides{t.path / "home dir"}, platform::Env{});
    REQUIRE(dirs);
    auto ex = make_exercise("types/option-basics");
    CHECK(workspace::default_root(*dirs, ex) == dirs->workspaces_dir / "types" / "option-basics");
}

TEST_CASE("workspace diffs against starter and solution")
{
    TempDir t;
    auto ex = make_exercise();
    auto const root = t.path / "ws";
    auto started = workspace::start(ex, root);
    REQUIRE(started);
    CHECK(workspace::starter_diff(started->workspace, ex).empty());
    CHECK(!workspace::solution_diff(started->workspace, ex).empty());

    write_file(root / "src" / "main.dc", "module main;\npublic i32 main() { return 0; }\n");
    write_file(root / "extra.dc", "added\n");
    auto starter = workspace::starter_diff(started->workspace, ex);
    CHECK(starter.find("--- starter/src/main.dc") != std::string::npos);
    CHECK(starter.find("+++ yours/src/main.dc") != std::string::npos);
    CHECK(starter.find("-public i32 main() { return 1; }") != std::string::npos);
    CHECK(starter.find("+public i32 main() { return 0; }") != std::string::npos);
    CHECK(starter.find("yours/extra.dc") != std::string::npos);
    CHECK(starter.find("--- /dev/null") != std::string::npos);
    CHECK(starter.find("project.dcon") == std::string::npos);
    CHECK(workspace::solution_diff(started->workspace, ex).empty());

    write_file(root / "src" / "main.dc", "module main;\n");
    auto solution = workspace::solution_diff(started->workspace, ex);
    CHECK(solution.find("--- yours/src/main.dc") != std::string::npos);
    CHECK(solution.find("+++ solution/src/main.dc") != std::string::npos);
    CHECK(solution.find("+public i32 main() { return 0; }") != std::string::npos);
    CHECK(solution.find("extra.dc") == std::string::npos);
    std::filesystem::remove(root / "src" / "main.dc");
    CHECK(workspace::solution_diff(started->workspace, ex).find("--- /dev/null") != std::string::npos);

    write_file(root / "src" / "main.dc", std::string("bin\0ary", 7));
    CHECK(workspace::starter_diff(started->workspace, ex).find("Binary files") != std::string::npos);
}

SECTION("learndc.toolchain");

namespace
{
    std::filesystem::path make_fake(std::filesystem::path const& dir, std::string const& name)
    {
        std::filesystem::create_directories(dir);
        auto const target = dir / name;
        std::filesystem::copy_file(g_self, target, std::filesystem::copy_options::overwrite_existing);
        return target;
    }

    toolchain::Inputs inputs_for(std::filesystem::path const& path_dir, std::optional<std::filesystem::path> self = std::nullopt)
    {
        toolchain::Inputs in;
        in.env["PATH"] = platform::path_to_utf8(path_dir);
        in.self_exe = std::move(self);
        in.os = platform::Os::Linux;
        return in;
    }
}

TEST_CASE("relative tool paths are made absolute")
{
    TempDir t;
    auto const fake = make_fake(t.path / "bin", "dpm");
    auto in = inputs_for(t.path / "nowhere");
    in.env["LEARNDC_DPM"] = platform::path_to_utf8(std::filesystem::relative(fake, std::filesystem::current_path()));
    REQUIRE(in.env["LEARNDC_DPM"].find("..") != std::string::npos);
    auto located = toolchain::locate("dpm", "", "LEARNDC_DPM", in);
    REQUIRE(located.found());
    CHECK(located.path->is_absolute());
    CHECK_EQ(std::filesystem::weakly_canonical(*located.path), std::filesystem::weakly_canonical(fake));
}

TEST_CASE("toolchain precedence is settings then environment then sibling then PATH")
{
    TempDir t;
    auto const via_settings = make_fake(t.path / "s", "my-dcc");
    auto const via_env = make_fake(t.path / "e", "env-dcc");
    auto const sibling = make_fake(t.path / "bin", "dcc");
    make_fake(t.path / "bin", "learndc");
    auto const via_path = make_fake(t.path / "p", "dcc");

    auto in = inputs_for(t.path / "p", t.path / "bin" / "learndc");
    auto found = toolchain::locate("dcc", "", "LEARNDC_DCC", in);
    REQUIRE(found.found());
    CHECK(*found.path == sibling);
    CHECK(found.source == toolchain::Source::Sibling);

    auto no_sibling = inputs_for(t.path / "p", t.path / "nothing" / "learndc");
    found = toolchain::locate("dcc", "", "LEARNDC_DCC", no_sibling);
    REQUIRE(found.found());
    CHECK(*found.path == via_path);
    CHECK(found.source == toolchain::Source::Path);

    in.env["LEARNDC_DCC"] = platform::path_to_utf8(via_env);
    found = toolchain::locate("dcc", "", "LEARNDC_DCC", in);
    REQUIRE(found.found());
    CHECK(*found.path == via_env);
    CHECK(found.source == toolchain::Source::Environment);

    found = toolchain::locate("dcc", platform::path_to_utf8(via_settings), "LEARNDC_DCC", in);
    REQUIRE(found.found());
    CHECK(*found.path == via_settings);
    CHECK(found.source == toolchain::Source::Settings);
    CHECK(found.warnings.empty());
    CHECK_EQ(found.searched.size(), 4u);
}

TEST_CASE("toolchain falls through invalid overrides with a warning")
{
    TempDir t;
    auto const via_path = make_fake(t.path / "p", "dpm");
    auto in = inputs_for(t.path / "p");
    in.env["LEARNDC_DPM"] = platform::path_to_utf8(t.path / "missing" / "dpm");
    write_file(t.path / "plain.txt", "not executable");
    std::filesystem::permissions(t.path / "plain.txt", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    auto found = toolchain::locate("dpm", platform::path_to_utf8(t.path / "plain.txt"), "LEARNDC_DPM", in);
    REQUIRE(found.found());
    CHECK(*found.path == via_path);
    CHECK(found.source == toolchain::Source::Path);
    CHECK_EQ(found.warnings.size(), 2u);
    auto const report = found.searched_text();
    CHECK(report.find("tools.dpm") != std::string::npos);
    CHECK(report.find("LEARNDC_DPM") != std::string::npos);
    CHECK(report.find("PATH") != std::string::npos);
    CHECK(report.find("next to learndc") != std::string::npos);
}

TEST_CASE("toolchain reports every location when nothing is found")
{
    TempDir t;
    std::filesystem::create_directories(t.path / "empty");
    auto in = inputs_for(t.path / "empty", t.path / "bin" / "learndc");
    auto found = toolchain::locate("dcc", "", "LEARNDC_DCC", in);
    CHECK(!found.found());
    CHECK(found.source == toolchain::Source::None);
    CHECK_EQ(found.searched.size(), 4u);
    auto const report = found.searched_text();
    CHECK(report.find("tools.dcc (not set)") != std::string::npos);
    CHECK(report.find("LEARNDC_DCC (not set)") != std::string::npos);
    CHECK(report.find(platform::path_to_utf8(t.path / "bin" / "dcc")) != std::string::npos);
    CHECK(report.find("PATH for 'dcc'") != std::string::npos);
    in.self_exe.reset();
    CHECK(toolchain::locate("dcc", "", "LEARNDC_DCC", in).searched_text().find("unknown") != std::string::npos);
}

TEST_CASE("toolchain resolves a bare name from settings through PATH")
{
    TempDir t;
    auto const target = make_fake(t.path / "p", "dcc-nightly");
    auto in = inputs_for(t.path / "p");
    auto found = toolchain::locate("dcc", "dcc-nightly", "LEARNDC_DCC", in);
    REQUIRE(found.found());
    CHECK(*found.path == target);
    CHECK(found.source == toolchain::Source::Settings);
}

TEST_CASE("toolchain handles Windows executable names")
{
    TempDir t;
    auto const sibling = make_fake(t.path / "bin", "dcc.exe");
    make_fake(t.path / "bin", "dpm.exe");
    make_fake(t.path / "p", "dccd.exe");
    toolchain::Inputs in;
    in.os = platform::Os::Windows;
    in.env["Path"] = platform::path_to_utf8(t.path / "p");
    in.env["PATHEXT"] = ".COM;.EXE";
    in.self_exe = t.path / "bin" / "learndc.exe";
    auto dcc = toolchain::locate("dcc", "", "LEARNDC_DCC", in);
    REQUIRE(dcc.found());
    CHECK(*dcc.path == sibling);
    CHECK(dcc.source == toolchain::Source::Sibling);
    auto dccd = toolchain::locate("dccd", "", "LEARNDC_DCCD", in);
    REQUIRE(dccd.found());
    CHECK(dccd.source == toolchain::Source::Path);
    CHECK(platform::path_to_utf8(dccd.path->filename()) == "dccd.exe");
    in.env["learndc_dpm"] = platform::path_to_utf8(t.path / "bin" / "dpm");
    auto dpm = toolchain::locate("dpm", "", "LEARNDC_DPM", in);
    REQUIRE(dpm.found());
    CHECK(dpm.source == toolchain::Source::Environment);
    CHECK(platform::path_to_utf8(dpm.path->filename()) == "dpm.exe");
}

TEST_CASE("toolchain resolve covers dcc dpm and dccd")
{
    TempDir t;
    make_fake(t.path / "bin", "dcc");
    make_fake(t.path / "bin", "dpm");
    make_fake(t.path / "bin", "learndc");
    toolchain::Inputs in = inputs_for(t.path / "empty", t.path / "bin" / "learndc");
    auto tc = toolchain::resolve(in);
    CHECK(tc.dcc.found());
    CHECK(tc.dpm.found());
    CHECK(!tc.dccd.found());
    in.settings.tools_dcc = platform::path_to_utf8(t.path / "bin" / "dpm");
    tc = toolchain::resolve(in);
    CHECK(tc.dcc.source == toolchain::Source::Settings);
    CHECK(platform::path_to_utf8(tc.dcc.path->filename()) == "dpm");
}

TEST_CASE("toolchain runs version with a timeout")
{
    TempDir t;
    auto const good = make_fake(t.path, "gooddcc");
    auto info = toolchain::version(good);
    REQUIRE(info);
    CHECK_EQ(info->line, "fakedcc 1.2.3 (test)");
    CHECK_EQ(info->number.value(), "1.2.3");

    auto const failing = make_fake(t.path, "faildcc");
    auto failed = toolchain::version(failing);
    REQUIRE(!failed);
    CHECK(failed.error().message.find("status 3") != std::string::npos);

    auto const silent = make_fake(t.path, "silentdcc");
    CHECK(!toolchain::version(silent));

    auto const hung = make_fake(t.path, "hangdcc");
    auto const start = std::chrono::steady_clock::now();
    auto slow = toolchain::version(hung, std::chrono::milliseconds(300));
    auto const elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(!slow);
    CHECK(slow.error().message.find("did not finish") != std::string::npos);
    CHECK(elapsed < std::chrono::seconds(5));

    auto missing = toolchain::version(t.path / "nonexistent");
    REQUIRE(!missing);
    CHECK(missing.error().message.find("cannot run") != std::string::npos);
}

TEST_CASE("toolchain version parsing and comparison")
{
    CHECK_EQ(toolchain::parse_version("dcc 0.6.0-dev (5be14a3)").value(), "0.6.0");
    CHECK_EQ(toolchain::parse_version("v1.22.3").value(), "1.22.3");
    CHECK_EQ(toolchain::parse_version("tool version 2.0").value(), "2.0");
    CHECK(!toolchain::parse_version("no digits here"));
    CHECK(!toolchain::parse_version("build 12"));
    CHECK(toolchain::version_at_least("0.6.0", "0.6.0"));
    CHECK(toolchain::version_at_least("0.10.0", "0.9.5"));
    CHECK(toolchain::version_at_least("1.0", "0.9.9"));
    CHECK(!toolchain::version_at_least("0.5.9", "0.6.0"));
    CHECK(!toolchain::version_at_least("garbage", "0.6.0"));
}

SECTION("learndc.runner");

namespace
{
    using namespace std::chrono_literals;

    std::string hex_encode(std::string_view bytes)
    {
        std::string out;
        for (char c : bytes)
            out += std::format("{:02x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        return out;
    }

    runner::ProgramSpec fake_spec(std::vector<std::string> args)
    {
        runner::ProgramSpec spec;
        spec.argv = {g_self};
        spec.argv.insert(spec.argv.end(), args.begin(), args.end());
        spec.timeout = 10s;
        spec.max_output = 65536;
        return spec;
    }

    catalog::Check run_check(std::optional<std::string> expected, int exit = 0)
    {
        catalog::Check c;
        c.kind = catalog::CheckKind::Run;
        c.name = "fake";
        c.expect_stdout = std::move(expected);
        c.expect_exit = exit;
        return c;
    }

    runner::CheckResult judge_fake(catalog::Check const& check, std::vector<std::string> args, std::size_t max_output = 65536, std::chrono::milliseconds timeout = 10s)
    {
        auto spec = fake_spec(std::move(args));
        spec.max_output = max_output;
        spec.timeout = timeout;
        return runner::judge_run(check, runner::execute_program(spec));
    }

    std::filesystem::path bin_dir()
    {
        return platform::executable_path().value_or(std::filesystem::path("build/bin/tests/test_learndc")).parent_path().parent_path();
    }

    bool real_tools_present()
    {
        return std::filesystem::exists(bin_dir() / "dcc") && std::filesystem::exists(bin_dir() / "dpm");
    }

    runner::RunnerOptions make_options(std::filesystem::path const& home, runner::Backend backend = runner::Backend::Dpm)
    {
        runner::RunnerOptions o;
        platform::Overrides ov;
        ov.home = home;
        o.dirs = platform::resolve_dirs(ov, platform::Env{}).value();
        o.settings.tools_dcc = platform::path_to_utf8(bin_dir() / "dcc");
        o.settings.tools_dpm = platform::path_to_utf8(bin_dir() / "dpm");
        toolchain::Inputs in;
        in.settings = o.settings;
        in.env = platform::current_env();
        o.tools = toolchain::resolve(in);
        o.backend = backend;
        return o;
    }

    std::optional<catalog::Exercise> hello_exercise()
    {
        auto const* dir = std::getenv("DCC_TEST_LEARNDC_CONTENT");
        if (!dir)
            return std::nullopt;
        auto full = catalog::load_directory(dir);
        if (!full)
            return std::nullopt;
        catalog::ContentSource s;
        s.label = "hello";
        s.add("catalog.toml", "schema = 1\ntitle = \"T\"\ntracks = [\"fundamentals\"]\n");
        s.add("tracks/fundamentals/track.toml", "schema = 1\nid = \"fundamentals\"\ntitle = \"F\"\nsummary = \"s\"\nexercises = [\"hello-world\"]\n");
        for (auto const& [path, bytes] : full->files)
            if (path.starts_with("tracks/fundamentals/hello-world/"))
                s.add(path, bytes);
        auto loaded = catalog::Catalog::load(s);
        if (!loaded)
            return std::nullopt;
        auto const* ex = loaded->find("fundamentals/hello-world");
        if (!ex)
            return std::nullopt;
        return *ex;
    }

    void write_tree(std::filesystem::path const& root, std::map<std::string, std::string> const& files)
    {
        for (auto const& [rel, bytes] : files)
            write_file(root / platform::path_from_utf8(rel), bytes);
    }

    catalog::Exercise inline_exercise(std::string const& id, std::string const& main_source)
    {
        catalog::Exercise ex;
        ex.id = id;
        ex.track = id.substr(0, id.find('/'));
        ex.name = id.substr(id.find('/') + 1);
        ex.entry_file = "src/main.dc";
        ex.product = "prog";
        ex.starter["project.dcon"] =
            "dpm::v1::Project {\n    name = \"prog\",\n    version = \"0.1.0\",\n    products = {\n        { name = \"prog\", entry = \"src/main.dc\" },\n    },\n}\n";
        ex.starter["src/main.dc"] = main_source;
        catalog::Check build;
        build.kind = catalog::CheckKind::Build;
        build.name = "Compiles";
        ex.checks.push_back(build);
        return ex;
    }

    catalog::Check stdout_check(std::string const& name, std::string expected)
    {
        catalog::Check c;
        c.kind = catalog::CheckKind::Run;
        c.name = name;
        c.expect_stdout = std::move(expected);
        c.max_seconds = 10;
        return c;
    }
}

TEST_CASE("output matching modes and line endings")
{
    using catalog::LineEndings;
    using catalog::StdoutMatch;
    CHECK(runner::output_matches("a\nb\n", "a\nb\n", StdoutMatch::Exact, LineEndings::Portable));
    CHECK(!runner::output_matches("a\nb\n", "a\nb", StdoutMatch::Exact, LineEndings::Portable));
    CHECK(runner::output_matches("a\nb\n", "a\r\nb\r\n", StdoutMatch::Exact, LineEndings::Portable));
    CHECK(!runner::output_matches("a\nb\n", "a\r\nb\r\n", StdoutMatch::Exact, LineEndings::Exact));
    CHECK(!runner::output_matches("a\nb", "a\rb", StdoutMatch::Exact, LineEndings::Portable));
    CHECK(runner::output_matches("a\rb", "a\rb", StdoutMatch::Exact, LineEndings::Portable));
    CHECK(runner::output_matches("a\nb", "  a\nb\n\n", StdoutMatch::Trimmed, LineEndings::Portable));
    CHECK(!runner::output_matches("a b", "a  b", StdoutMatch::Trimmed, LineEndings::Portable));
    CHECK(runner::output_matches("needle", "hay needle stack", StdoutMatch::Contains, LineEndings::Portable));
    CHECK(!runner::output_matches("needle", "hay stack", StdoutMatch::Contains, LineEndings::Portable));
    CHECK(runner::output_matches("", "anything", StdoutMatch::Contains, LineEndings::Portable));
}

TEST_CASE("parse_locations reads error and warning pointers")
{
    std::string const diag =
        "error: expected expression, found ';'\n"
        "  --> /ws/src/main.dc:2:39\n"
        "   |\n"
        "warning: unused variable `x`\n"
        "  --> C:\\Users\\a b\\src\\main.dc:10:5\n"
        "error: no pointer here\n"
        "note: something\n"
        "  --> src/x.dc:3:1\n";
    auto const locs = runner::parse_locations(diag);
    REQUIRE((locs.size()) == 3u);
    CHECK_EQ(locs[0].file, "/ws/src/main.dc");
    CHECK_EQ(locs[0].line, 2);
    CHECK_EQ(locs[0].column, 39);
    CHECK_EQ(locs[0].severity, "error");
    CHECK_EQ(locs[0].message, "expected expression, found ';'");
    CHECK_EQ(locs[1].file, "C:\\Users\\a b\\src\\main.dc");
    CHECK_EQ(locs[1].line, 10);
    CHECK_EQ(locs[1].severity, "warning");
    CHECK_EQ(locs[2].severity, "note");
    CHECK(runner::parse_locations("nothing to see\n").empty());
}

TEST_CASE("diagnostics are sanitized and staging paths rewritten")
{
    runner::detail::Rewrites rw;
    rw.add("/stage/x/project", "/ws/root");
    rw.add("/stage/x/harness/tests", "checks");
    auto const out = runner::detail::clean_diagnostics(
        "    Building hello (debug, host)\n\x1b[1;91merror\x1b[0m: bad\x1b]0;title\x07 thing\n\x1b[1;94m  --> \x1b[0m/stage/x/project/src/main.dc:2:39\n"
        "  --> /stage/x/harness/tests/a.dc:1:1\n",
        rw);
    CHECK(out.find('\x1b') == std::string::npos);
    CHECK(out.find("Building") == std::string::npos);
    CHECK(out.find("/stage/") == std::string::npos);
    CHECK(out.find("/ws/root/src/main.dc:2:39") != std::string::npos);
    CHECK(out.find("checks/a.dc:1:1") != std::string::npos);
    CHECK(out.starts_with("error: bad thing"));
}

TEST_CASE("staging sync keeps unchanged files and prunes stale ones")
{
    TempDir t;
    auto const dest = t.path / "stage";
    std::map<std::string, std::string> files = {{"src/a.dc", "one"}, {"src/b.dc", "two"}, {"project.dcon", "p"}};
    REQUIRE(runner::detail::sync_tree(dest, files, {".dpm"}).has_value());
    write_file(dest / ".dpm" / "out" / "bin", "built");
    write_file(dest / "stale.dc", "old");
    auto const before = std::filesystem::last_write_time(dest / "src" / "a.dc");
    std::this_thread::sleep_for(30ms);
    files["src/b.dc"] = "changed";
    files.erase("project.dcon");
    REQUIRE(runner::detail::sync_tree(dest, files, {".dpm"}).has_value());
    CHECK(std::filesystem::last_write_time(dest / "src" / "a.dc") == before);
    CHECK_EQ(slurp(dest / "src" / "b.dc"), "changed");
    CHECK(!std::filesystem::exists(dest / "stale.dc"));
    CHECK(!std::filesystem::exists(dest / "project.dcon"));
    CHECK(std::filesystem::exists(dest / ".dpm" / "out" / "bin"));
}

TEST_CASE("fake program exact output passes and mismatch reports expected and observed")
{
    auto ok = judge_fake(run_check("hello\n"), {"--emit", hex_encode("hello\n")});
    CHECK(ok.verdict == runner::Verdict::Pass);
    CHECK(ok.stage == runner::Stage::Passed);
    CHECK_EQ(ok.exit_code.value_or(-1), 0);

    auto bad = judge_fake(run_check("hello\n"), {"--emit", hex_encode("help\n")});
    CHECK(bad.verdict == runner::Verdict::Fail);
    CHECK(bad.stage == runner::Stage::Compare);
    CHECK_EQ(bad.expected, "hello\n");
    CHECK_EQ(bad.observed, "help\n");
    CHECK(bad.expected_visible.empty());
}

TEST_CASE("whitespace only differences are shown visibly")
{
    auto r = judge_fake(run_check("a\nb\n"), {"--emit", hex_encode("a \nb\n")});
    CHECK(r.verdict == runner::Verdict::Fail);
    CHECK(r.stage == runner::Stage::Compare);
    CHECK(r.observed_visible.find("\xC2\xB7") != std::string::npos);
    CHECK(r.expected_visible.find("\xC2\xB7") == std::string::npos);
    CHECK(r.summary.find("whitespace") != std::string::npos);

    auto tab = judge_fake(run_check("a b\n"), {"--emit", hex_encode("a\tb\n")});
    CHECK(tab.observed_visible.find("\xE2\x86\x92") != std::string::npos);
}

TEST_CASE("CRLF output passes with portable endings and fails visibly with exact endings")
{
    auto portable = judge_fake(run_check("a\nb\n"), {"--emit", hex_encode("a\r\nb\r\n")});
    CHECK(portable.verdict == runner::Verdict::Pass);

    auto strict = run_check("a\nb\n");
    strict.line_endings = catalog::LineEndings::Exact;
    auto r = judge_fake(strict, {"--emit", hex_encode("a\r\nb\r\n")});
    CHECK(r.verdict == runner::Verdict::Fail);
    CHECK(r.observed_visible.find("\xE2\x90\x8D") != std::string::npos);
}

TEST_CASE("contains and trimmed matching in judge_run")
{
    auto contains = run_check("world");
    contains.stdout_match = catalog::StdoutMatch::Contains;
    CHECK(judge_fake(contains, {"--emit", hex_encode("hello world\n")}).verdict == runner::Verdict::Pass);
    auto miss = judge_fake(contains, {"--emit", hex_encode("hello there\n")});
    CHECK(miss.verdict == runner::Verdict::Fail);
    CHECK(miss.summary.find("does not contain") != std::string::npos);

    auto trimmed = run_check("x");
    trimmed.stdout_match = catalog::StdoutMatch::Trimmed;
    CHECK(judge_fake(trimmed, {"--emit", hex_encode("\n  x \n\n")}).verdict == runner::Verdict::Pass);
}

TEST_CASE("exit codes signals and launch failures are classified")
{
    auto exit3 = judge_fake(run_check(std::nullopt, 0), {"--exit", "3"});
    CHECK(exit3.verdict == runner::Verdict::Fail);
    CHECK(exit3.stage == runner::Stage::Run);
    CHECK(exit3.summary.find("code 3") != std::string::npos);
    CHECK_EQ(exit3.exit_code.value_or(-1), 3);
    CHECK(judge_fake(run_check(std::nullopt, 3), {"--exit", "3"}).verdict == runner::Verdict::Pass);

    auto aborted = judge_fake(run_check(std::nullopt), {"--abort"});
    CHECK(aborted.verdict == runner::Verdict::Fail);
    CHECK(aborted.stage == runner::Stage::Run);
    CHECK(aborted.signal.has_value());
    CHECK(aborted.summary.find("SIGABRT") != std::string::npos);

    auto segv = judge_fake(run_check(std::nullopt), {"--segv"});
    CHECK(segv.verdict == runner::Verdict::Fail);
    CHECK(segv.summary.find("SIGSEGV") != std::string::npos);

    runner::ProgramSpec missing;
    missing.argv = {"/nonexistent/learndc-no-such-program"};
    auto launch = runner::judge_run(run_check(std::nullopt), runner::execute_program(missing));
    CHECK(launch.verdict == runner::Verdict::Error);
    CHECK(launch.stage == runner::Stage::Launch);
}

TEST_CASE("stderr expectations and stdin fixtures")
{
    auto check = run_check(std::nullopt);
    check.expect_stderr_contains = "boom";
    CHECK(judge_fake(check, {"--emit-err", hex_encode("a boom here\n")}).verdict == runner::Verdict::Pass);
    auto miss = judge_fake(check, {"--emit-err", hex_encode("quiet\n")});
    CHECK(miss.verdict == runner::Verdict::Fail);
    CHECK(miss.stage == runner::Stage::Compare);

    TempDir t;
    write_file(t.path / "in.txt", "line one\nline two\n");
    auto spec = fake_spec({"--echo-stdin"});
    spec.stdin_file = t.path / "in.txt";
    auto echoed = runner::judge_run(run_check("line one\nline two\n"), runner::execute_program(spec));
    CHECK(echoed.verdict == runner::Verdict::Pass);
}

TEST_CASE("flooding output stops the program at the output limit")
{
    auto r = judge_fake(run_check(std::nullopt), {"--flood"}, 4096);
    CHECK(r.verdict == runner::Verdict::Fail);
    CHECK(r.stage == runner::Stage::OutputLimit);
    CHECK(r.summary.find("more than") != std::string::npos);
}

TEST_CASE("a sleeping program times out")
{
    auto const begin = std::chrono::steady_clock::now();
    auto r = judge_fake(run_check(std::nullopt), {"--sleep"}, 65536, 300ms);
    auto const elapsed = std::chrono::steady_clock::now() - begin;
    CHECK(r.verdict == runner::Verdict::Fail);
    CHECK(r.stage == runner::Stage::Timeout);
    CHECK(r.timed_out);
    CHECK(elapsed < 5s);
}

TEST_CASE("cancelling a sleeping program returns within two seconds")
{
    std::atomic<bool> cancel{false};
    auto spec = fake_spec({"--sleep"});
    spec.cancel = &cancel;
    spec.timeout = 60s;
    std::thread canceller(
        [&]
        {
            std::this_thread::sleep_for(250ms);
            cancel = true;
        });
    auto const begin = std::chrono::steady_clock::now();
    auto r = runner::judge_run(run_check(std::nullopt), runner::execute_program(spec));
    auto const elapsed = std::chrono::steady_clock::now() - begin;
    canceller.join();
    CHECK(r.verdict == runner::Verdict::Cancelled);
    CHECK(r.stage == runner::Stage::Cancelled);
    CHECK(elapsed < 2s);
}

namespace
{
    struct EnvGuard
    {
        explicit EnvGuard(char const* value)
        {
            ::setenv("LEARNDC_TEST_FAKE_DPM", value, 1);
        }

        ~EnvGuard()
        {
            ::unsetenv("LEARNDC_TEST_FAKE_DPM");
        }
    };

    runner::BuildSpec fake_dpm_spec(std::filesystem::path const& dir)
    {
        runner::BuildSpec spec;
        spec.backend = runner::Backend::Dpm;
        spec.dpm = std::filesystem::path(g_self);
        spec.project_dir = dir;
        spec.entry = "src/main.dc";
        spec.product = "prog";
        spec.timeout = 10s;
        return spec;
    }
}

TEST_CASE("a stale dpm binary is not run when the build produced none")
{
    TempDir t;
    auto const stale = t.path / ".dpm" / "out" / "debug" / "host" / "prog";
    write_file(stale, "stale");
    EnvGuard guard("ok");
    auto outcome = runner::build_program(fake_dpm_spec(t.path));
    CHECK(!outcome.ok);
    CHECK(!std::filesystem::exists(stale));
    CHECK_EQ(outcome.exit_code, 0);
}

namespace
{
    catalog::Check diagnostic_check(bool expect_success, std::string contains)
    {
        catalog::Check c;
        c.kind = catalog::CheckKind::Diagnostic;
        c.name = "Rejected";
        c.expect_build_success = expect_success;
        c.diagnostic_contains = std::move(contains);
        return c;
    }

    runner::CheckResult run_diagnostic(char const* mode, std::chrono::seconds timeout = 20s)
    {
        TempDir t;
        auto o = make_options(t.path);
        o.tools.dpm.path = std::filesystem::path(g_self);
        o.settings.checks_build_timeout_seconds = static_cast<int>(timeout.count());
        auto ex = inline_exercise("basics/diag", "module main;\n");
        ex.checks.clear();
        ex.checks.push_back(diagnostic_check(false, ""));
        auto const root = t.path / "ws";
        write_tree(root, ex.starter);
        EnvGuard guard(mode);
        auto report = runner::run_checks(ex, root, o);
        return report.results.at(0);
    }
}

TEST_CASE("diagnostic checks do not pass when the build tool crashes or produces no result")
{
    auto crashed = run_diagnostic("abort");
    CHECK(crashed.verdict == runner::Verdict::Error);
    CHECK(crashed.summary.find("SIGABRT") != std::string::npos);
    auto empty = run_diagnostic("ok");
    CHECK(empty.verdict == runner::Verdict::Error);
    auto rejected = run_diagnostic("reject");
    CHECK(rejected.verdict == runner::Verdict::Pass);
}

TEST_CASE("diagnostic checks do not pass when the build times out")
{
    auto r = run_diagnostic("sleep", 1s);
    CHECK(r.verdict == runner::Verdict::Fail);
    CHECK(r.stage == runner::Stage::Timeout);
}

TEST_CASE("source lines that mention linker words do not make a compile error a link failure")
{
    CHECK(!runner::detail::is_link_failure("error: initializer type mismatch\n 4 | println(\"undefined symbol\")\n   | ^^^"));
    CHECK(runner::detail::is_link_failure("ld.lld: error: undefined symbol: foo"));
}

TEST_CASE("missing toolchain is reported with the setting names")
{
    TempDir t;
    runner::RunnerOptions o;
    platform::Overrides ov;
    ov.home = t.path;
    o.dirs = platform::resolve_dirs(ov, platform::Env{}).value();
    auto ex = inline_exercise("basics/none", "module main;\n");
    ex.checks.push_back(stdout_check("Prints", "x\n"));
    auto const root = t.path / "ws";
    write_tree(root, ex.starter);
    auto report = runner::run_checks(ex, root, o);
    CHECK(report.outcome == runner::Outcome::Error);
    CHECK_EQ(report.total, 2);
    CHECK_EQ(report.passed, 0);
    CHECK(report.results[0].stage == runner::Stage::Toolchain);
    CHECK(report.results[0].summary.find("tools.dpm") != std::string::npos);
    CHECK(report.results[0].summary.find("LEARNDC_DPM") != std::string::npos);
    CHECK(report.results[1].verdict == runner::Verdict::Skipped);
    CHECK_EQ(report.first_failure_stage, "toolchain");
}

TEST_CASE("a held check lock refuses a second check")
{
    TempDir t;
    auto o = make_options(t.path);
    auto ex = inline_exercise("basics/locked", "module main;\n");
    auto const root = t.path / "ws";
    write_tree(root, ex.starter);
    std::filesystem::create_directories(o.dirs.staging_dir);
    auto held = platform::FileLock::try_acquire(runner::lock_path(o.dirs, ex));
    REQUIRE(held.has_value());
    auto report = runner::run_checks(ex, root, o);
    CHECK(report.outcome == runner::Outcome::Error);
    CHECK(report.results[0].stage == runner::Stage::Busy);
    CHECK(report.results[0].summary.find("already running") != std::string::npos);
    held->release();
    o.cancel = nullptr;
    std::atomic<bool> cancel{true};
    o.cancel = &cancel;
    auto after = runner::run_checks(ex, root, o);
    CHECK(after.results[0].stage != runner::Stage::Busy);
}

TEST_CASE("a pre-set cancel flag cancels every check without building")
{
    TempDir t;
    auto o = make_options(t.path);
    std::atomic<bool> cancel{true};
    o.cancel = &cancel;
    auto ex = inline_exercise("basics/cancelled", "module main;\n");
    ex.checks.push_back(stdout_check("Prints", "x\n"));
    auto const root = t.path / "ws";
    write_tree(root, ex.starter);
    auto report = runner::run_checks(ex, root, o);
    CHECK(report.outcome == runner::Outcome::Cancelled);
    CHECK_EQ(report.passed, 0);
    for (auto const& r : report.results)
        CHECK(r.verdict == runner::Verdict::Cancelled);
}

TEST_CASE("hello-world starter builds but fails the run check with expected and observed")
{
    REQUIRE(real_tools_present());
    auto ex = hello_exercise();
    REQUIRE(ex.has_value());
    TempDir t;
    auto o = make_options(t.path);
    auto const root = t.path / "ws";
    REQUIRE(workspace::start(*ex, root).has_value());
    std::vector<std::string> stages;
    o.on_stage = [&](std::string_view s) { stages.emplace_back(s); };
    o.include_safety_note = true;
    auto report = runner::run_checks(*ex, root, o);
    REQUIRE((report.results.size()) == 2u);
    CHECK(report.results[0].verdict == runner::Verdict::Pass);
    CHECK(report.results[1].verdict == runner::Verdict::Fail);
    CHECK(report.results[1].stage == runner::Stage::Compare);
    CHECK_EQ(report.results[1].expected, "Hello, Ada!\nWelcome to DC.\n");
    CHECK(report.results[1].observed.find("Hello, world!") != std::string::npos);
    CHECK(report.outcome == runner::Outcome::Failed);
    CHECK_EQ(report.passed, 1);
    CHECK_EQ(report.total, 2);
    CHECK_EQ(report.first_failure_stage, "compare");
    CHECK(!report.safety_note.empty());
    CHECK(!stages.empty());
    CHECK(std::filesystem::exists(report.log_path));
    auto const log = slurp(report.log_path);
    CHECK(log.find("Hello, world!") != std::string::npos);
    auto const text = runner::render_text(report, {});
    CHECK(text.find("1 of 2 checks passed") != std::string::npos);
    CHECK(text.find("Expected:") != std::string::npos);
    CHECK(text.find("Observed:") != std::string::npos);
    CHECK(text.find("final full stop") != std::string::npos);
    CHECK(text.find("not a sandbox") != std::string::npos);
    CHECK(text.find('\x1b') == std::string::npos);
    auto colored = runner::render_text(report, {true, false, false});
    CHECK(colored.find('\x1b') != std::string::npos);
    auto j = runner::to_json(report);
    CHECK_EQ(j.find("outcome")->as_string().value_or(""), "failed");
    CHECK_EQ(j.find("results")->size(), 2u);
}

TEST_CASE("hello-world solution passes on dpm and both direct backends")
{
    REQUIRE(real_tools_present());
    auto ex = hello_exercise();
    REQUIRE(ex.has_value());
    for (auto backend : {runner::Backend::Dpm, runner::Backend::DirectLlvm, runner::Backend::DirectCustom})
    {
        TempDir t;
        auto o = make_options(t.path, backend);
        auto const root = t.path / "ws";
        REQUIRE(workspace::start(*ex, root).has_value());
        write_tree(root, ex->solution);
        auto report = runner::run_checks(*ex, root, o);
        CHECK_EQ(report.passed, report.total);
        CHECK(report.outcome == runner::Outcome::Passed);
        CHECK_EQ(report.backend, std::string(runner::to_string(backend)));
        CHECK(runner::render_text(report, {}).find("2 of 2 checks passed") != std::string::npos);
    }
}

TEST_CASE("a broken program fails at the build stage with workspace locations")
{
    REQUIRE(real_tools_present());
    TempDir t;
    auto o = make_options(t.path);
    auto ex = inline_exercise("basics/broken", "module main;\n\npublic Status(Error) main() { int x = ; }\n");
    ex.checks.push_back(stdout_check("Prints", "x\n"));
    ex.checks[0].hint = "Look at the line the compiler points to.";
    auto const root = t.path / "ws dir";
    write_tree(root, ex.starter);
    auto report = runner::run_checks(ex, root, o);
    REQUIRE((report.results.size()) == 2u);
    auto const& build = report.results[0];
    CHECK(build.verdict == runner::Verdict::Fail);
    CHECK(build.stage == runner::Stage::Build);
    CHECK(build.diagnostics.find('\x1b') == std::string::npos);
    CHECK(build.diagnostics.find("staging") == std::string::npos);
    REQUIRE(!build.locations.empty());
    auto const& loc = build.locations[0];
    CHECK(loc.file.starts_with(platform::path_to_utf8(root)));
    CHECK(loc.file.ends_with("src/main.dc"));
    CHECK_EQ(loc.line, 3);
    CHECK_EQ(loc.severity, "error");
    CHECK(report.results[1].verdict == runner::Verdict::Skipped);
    CHECK_EQ(report.first_failure_stage, "build");
    auto const text = runner::render_text(report, {});
    CHECK(text.find("0 of 2 checks passed") != std::string::npos);
    CHECK(text.find("src/main.dc:3:") != std::string::npos);
    CHECK(text.find("Look at the line") != std::string::npos);
}

TEST_CASE("an unresolved symbol is reported at the link stage")
{
    REQUIRE(real_tools_present());
    TempDir t;
    auto o = make_options(t.path);
    auto ex = inline_exercise("basics/linkfail", "module main;\n\n@nomangle extern i32 missing_symbol_for_learndc();\n\npublic i32 main() {\n    return missing_symbol_for_learndc();\n}\n");
    auto const root = t.path / "ws";
    write_tree(root, ex.starter);
    auto report = runner::run_checks(ex, root, o);
    REQUIRE((report.results.size()) == 1u);
    CHECK(report.results[0].verdict == runner::Verdict::Fail);
    CHECK(report.results[0].stage == runner::Stage::Link || report.results[0].stage == runner::Stage::Build);
}

TEST_CASE("stdin fixtures arguments and exit codes reach the learner program")
{
    REQUIRE(real_tools_present());
    TempDir t;
    auto o = make_options(t.path);
    auto ex = inline_exercise("basics/exitcode", "module main;\n\npublic i32 main() {\n    return 7;\n}\n");
    catalog::Check seven = stdout_check("Exit seven", "");
    seven.expect_stdout = std::nullopt;
    seven.expect_exit = 7;
    ex.checks.push_back(seven);
    catalog::Check wrong = stdout_check("Exit zero", "");
    wrong.expect_stdout = std::nullopt;
    wrong.expect_exit = 0;
    ex.checks.push_back(wrong);
    auto const root = t.path / "ws";
    write_tree(root, ex.starter);
    auto report = runner::run_checks(ex, root, o);
    REQUIRE((report.results.size()) == 3u);
    CHECK(report.results[0].verdict == runner::Verdict::Pass);
    CHECK(report.results[1].verdict == runner::Verdict::Pass);
    CHECK(report.results[2].verdict == runner::Verdict::Fail);
    CHECK(report.results[2].summary.find("code 7") != std::string::npos);
}

namespace
{
    catalog::Exercise harness_exercise(std::string const& id, std::string const& area_body)
    {
        auto ex = inline_exercise(id, "module main;\n\nimport shapes;\n\npublic i32 main() {\n    return 0;\n}\n");
        ex.starter["src/shapes.dc"] = std::format("module shapes;\n\npublic i32 area(i32 w, i32 h) {{\n    return {};\n}}\n", area_body);
        ex.check_files["checks/area_check.dc"] =
            "module area_check;\nimport shapes;\n\npublic i32 main() {\n    if shapes::area(2, 3) != 6 {\n        println(\"area(2, 3): expected 6, got {}\", shapes::area(2, 3));\n        return 1;\n    }\n    println(\"area ok\");\n    return 0;\n}\n";
        catalog::Check harness;
        harness.kind = catalog::CheckKind::Harness;
        harness.name = "area works";
        harness.harness = "checks/area_check.dc";
        harness.expect_stdout = "area ok\n";
        harness.hint = "Multiply the width by the height.";
        ex.checks.push_back(harness);
        return ex;
    }
}

TEST_CASE("harness exercise passes and fails against learner code")
{
    REQUIRE(real_tools_present());
    {
        TempDir t;
        auto o = make_options(t.path);
        auto ex = harness_exercise("basics/shapes-pass", "w * h");
        auto const root = t.path / "ws";
        write_tree(root, ex.starter);
        auto report = runner::run_checks(ex, root, o);
        REQUIRE((report.results.size()) == 2u);
        CHECK(report.results[0].verdict == runner::Verdict::Pass);
        CHECK(report.results[1].verdict == runner::Verdict::Pass);
        CHECK(report.outcome == runner::Outcome::Passed);
    }
    {
        TempDir t;
        auto o = make_options(t.path);
        auto ex = harness_exercise("basics/shapes-fail", "w + h");
        auto const root = t.path / "ws";
        write_tree(root, ex.starter);
        auto report = runner::run_checks(ex, root, o);
        REQUIRE((report.results.size()) == 2u);
        CHECK(report.results[0].verdict == runner::Verdict::Pass);
        CHECK(report.results[1].verdict == runner::Verdict::Fail);
        CHECK(report.results[1].stage == runner::Stage::Harness);
        CHECK(report.results[1].exit_code.value_or(-1) == 1);
        CHECK(report.results[1].stdout_preview.find("expected 6, got 5") != std::string::npos);
        CHECK(runner::render_text(report, {}).find("Multiply the width") != std::string::npos);
    }
    {
        TempDir t;
        auto o = make_options(t.path);
        auto ex = harness_exercise("basics/shapes-missing", "w * h");
        ex.starter["src/shapes.dc"] = "module shapes;\n\npublic i32 other() {\n    return 1;\n}\n";
        auto const root = t.path / "ws";
        write_tree(root, ex.starter);
        auto report = runner::run_checks(ex, root, o);
        REQUIRE((report.results.size()) == 2u);
        CHECK(report.results[0].verdict == runner::Verdict::Pass);
        CHECK(report.results[1].verdict == runner::Verdict::Fail);
        CHECK(report.results[1].stage == runner::Stage::Build);
        REQUIRE(!report.results[1].locations.empty());
        CHECK(report.results[1].diagnostics.find("staging") == std::string::npos);
    }
}

TEST_CASE("diagnostic checks compare compiler behaviour")
{
    REQUIRE(real_tools_present());
    TempDir t;
    auto o = make_options(t.path);
    auto ex = inline_exercise("basics/diag", "module main;\n\npublic Status(Error) main() { int x = ; }\n");
    ex.checks.clear();
    catalog::Check expect_fail;
    expect_fail.kind = catalog::CheckKind::Diagnostic;
    expect_fail.name = "Rejected";
    expect_fail.expect_build_success = false;
    expect_fail.diagnostic_contains = "expected expression";
    ex.checks.push_back(expect_fail);
    catalog::Check wrong_text = expect_fail;
    wrong_text.name = "Wrong text";
    wrong_text.diagnostic_contains = "no such message anywhere";
    ex.checks.push_back(wrong_text);
    catalog::Check expect_ok = expect_fail;
    expect_ok.name = "Compiles";
    expect_ok.expect_build_success = true;
    expect_ok.diagnostic_contains.clear();
    ex.checks.push_back(expect_ok);
    auto const root = t.path / "ws";
    write_tree(root, ex.starter);
    auto report = runner::run_checks(ex, root, o);
    REQUIRE((report.results.size()) == 3u);
    CHECK(report.results[0].verdict == runner::Verdict::Pass);
    CHECK(report.results[1].verdict == runner::Verdict::Fail);
    CHECK(report.results[1].stage == runner::Stage::Compare);
    CHECK(report.results[2].verdict == runner::Verdict::Fail);
    CHECK(report.results[2].stage == runner::Stage::Build);
}

TEST_CASE("repeated checks reuse unchanged staging files")
{
    REQUIRE(real_tools_present());
    auto ex = hello_exercise();
    REQUIRE(ex.has_value());
    TempDir t;
    auto o = make_options(t.path);
    auto const root = t.path / "ws";
    REQUIRE(workspace::start(*ex, root).has_value());
    auto first = runner::run_checks(*ex, root, o);
    auto const staged = o.dirs.staging_dir / "fundamentals-hello-world" / "project" / "src" / "main.dc";
    REQUIRE(std::filesystem::exists(staged));
    auto const before = std::filesystem::last_write_time(staged);
    std::this_thread::sleep_for(30ms);
    auto second = runner::run_checks(*ex, root, o);
    CHECK(std::filesystem::last_write_time(staged) == before);
    CHECK_EQ(first.passed, second.passed);
    write_file(root / "src" / "main.dc", "module main;\n\npublic Status(Error) main() {\n    println(\"changed\")?;\n    return Status::Ok;\n}\n");
    auto third = runner::run_checks(*ex, root, o);
    CHECK(third.results[1].observed.find("changed") != std::string::npos);
}

TEST_CASE("interactive run reports build errors and program exit")
{
    REQUIRE(real_tools_present());
    TempDir t;
    auto o = make_options(t.path);
    {
        auto ex = inline_exercise("basics/irun-bad", "module main;\n\npublic Status(Error) main() { int x = ; }\n");
        auto const root = t.path / "bad";
        write_tree(root, ex.starter);
        auto out = runner::build_and_run_interactive(ex, root, o);
        CHECK(!out.ok);
        CHECK(out.stage == runner::Stage::Build);
        CHECK(!out.locations.empty());
    }
    {
        auto ex = inline_exercise("basics/irun-ok", "module main;\n\npublic i32 main() {\n    return 4;\n}\n");
        auto const root = t.path / "ok";
        write_tree(root, ex.starter);
        auto out = runner::build_and_run_interactive(ex, root, o);
        CHECK(!out.ok);
        CHECK_EQ(out.exit_code.value_or(-1), 4);
        CHECK(out.stage == runner::Stage::Passed);
    }
}

SECTION("learndc.watch");

namespace
{
    void touch_with(std::filesystem::path const& p, std::string_view data)
    {
        write_file(p, data);
    }
}

TEST_CASE("watch ignore rules")
{
    CHECK(watch::ignored(".dpm/out/x"));
    CHECK(watch::ignored(".learndc/workspace.json"));
    CHECK(watch::ignored("src/.main.dc.swp"));
    CHECK(watch::ignored("src/main.dc~"));
    CHECK(watch::ignored("src/.#main.dc"));
    CHECK(watch::ignored("src/4913"));
    CHECK(watch::ignored("src/main.dc.tmp"));
    CHECK(!watch::ignored("src/main.dc"));
    CHECK(!watch::ignored("project.dcon"));
    CHECK(!watch::ignored("src/.dpm/keep.dc"));
}

TEST_CASE("watch snapshot compare finds added removed and modified files")
{
    TempDir t;
    touch_with(t.path / "src" / "a.dc", "one");
    touch_with(t.path / "src" / "b.dc", "two");
    touch_with(t.path / ".dpm" / "out" / "x", "built");
    touch_with(t.path / "src" / ".a.dc.swp", "swap");
    auto const first = watch::snapshot(t.path);
    CHECK_EQ(first.size(), 2u);
    CHECK(first.contains("src/a.dc"));
    CHECK(!first.contains(".dpm/out/x"));
    touch_with(t.path / "src" / "a.dc", "three!");
    std::filesystem::remove(t.path / "src" / "b.dc");
    touch_with(t.path / "src" / "c.dc", "new");
    touch_with(t.path / ".dpm" / "out" / "y", "ignored");
    auto const changes = watch::compare(first, watch::snapshot(t.path));
    CHECK_EQ(changes.modified.size(), 1u);
    CHECK_EQ(changes.modified[0], "src/a.dc");
    CHECK_EQ(changes.removed.size(), 1u);
    CHECK_EQ(changes.added.size(), 1u);
    CHECK_EQ(changes.added[0], "src/c.dc");
    CHECK(watch::compare(first, first).empty());
}

TEST_CASE("watch detects atomic rename saves with identical size and mtime")
{
    TempDir t;
    auto const target = t.path / "src" / "main.dc";
    touch_with(target, "aaaa");
    auto const before = watch::snapshot(t.path);
    auto const mtime = std::filesystem::last_write_time(target);
    auto const tmp = t.path / "src" / "main.dc.tmp";
    touch_with(tmp, "bbbb");
    std::filesystem::last_write_time(tmp, mtime);
    std::filesystem::rename(tmp, target);
    auto const changes = watch::compare(before, watch::snapshot(t.path));
    REQUIRE((changes.modified.size()) == 1u);
    CHECK_EQ(changes.modified[0], "src/main.dc");
    CHECK(changes.added.empty());
    CHECK(changes.removed.empty());
}

TEST_CASE("watcher debounces bursts of changes")
{
    TempDir t;
    auto const file = t.path / "src" / "main.dc";
    touch_with(file, "v1");
    watch::Options options;
    options.debounce = 400ms;
    watch::Watcher w(t.path, options);
    auto const t0 = watch::Watcher::Clock::now();
    CHECK(!w.poll(t0).has_value());
    touch_with(file, "version two");
    CHECK(!w.poll(t0 + 10ms).has_value());
    CHECK(w.pending());
    CHECK(!w.poll(t0 + 200ms).has_value());
    touch_with(file, "version three!");
    CHECK(!w.poll(t0 + 300ms).has_value());
    CHECK(!w.poll(t0 + 600ms).has_value());
    auto const done = w.poll(t0 + 800ms);
    REQUIRE(done.has_value());
    REQUIRE((done->modified.size()) == 1u);
    CHECK_EQ(done->modified[0], "src/main.dc");
    CHECK(!w.poll(t0 + 1500ms).has_value());
    CHECK(!w.pending());
}

TEST_CASE("watcher wait returns on change and stops on cancel and never writes")
{
    TempDir t;
    auto const file = t.path / "src" / "main.dc";
    touch_with(file, "v1");
    std::atomic<bool> cancel{false};
    watch::Options options;
    options.debounce = 50ms;
    options.interval = 20ms;
    options.cancel = &cancel;
    watch::Watcher w(t.path, options);
    std::thread editor_thread(
        [&]
        {
            std::this_thread::sleep_for(150ms);
            touch_with(file, "version two");
        });
    auto const changes = w.wait();
    editor_thread.join();
    REQUIRE(changes.has_value());
    CHECK_EQ(changes->modified.size(), 1u);

    std::thread canceller(
        [&]
        {
            std::this_thread::sleep_for(150ms);
            cancel = true;
        });
    auto const begin = std::chrono::steady_clock::now();
    CHECK(!w.wait().has_value());
    canceller.join();
    CHECK(std::chrono::steady_clock::now() - begin < 2s);
    std::size_t files = 0;
    for (auto const& e : std::filesystem::recursive_directory_iterator(t.path))
        if (e.is_regular_file())
            ++files;
    CHECK_EQ(files, 1u);
}

SECTION("learndc.editor");

TEST_CASE("command line parsing posix rules")
{
    using editor::Syntax;
    auto a = editor::parse_command_line("nvim +{line} \"{file}\"", Syntax::Posix);
    REQUIRE(a.has_value());
    CHECK_EQ(a->size(), 3u);
    CHECK_EQ((*a)[1], "+{line}");
    CHECK_EQ((*a)[2], "{file}");
    auto b = editor::parse_command_line("code --goto 'a b' c\\ d \"e \\\" f\" ''", Syntax::Posix);
    REQUIRE(b.has_value());
    REQUIRE((b->size()) == 6u);
    CHECK_EQ((*b)[2], "a b");
    CHECK_EQ((*b)[3], "c d");
    CHECK_EQ((*b)[4], "e \" f");
    CHECK_EQ((*b)[5], "");
    auto c = editor::parse_command_line("\"/opt/Editor (x86)/ed\" -x", Syntax::Posix);
    REQUIRE(c.has_value());
    CHECK_EQ((*c)[0], "/opt/Editor (x86)/ed");
    for (std::string_view const bad : {"vim | cat", "vim; rm x", "vim $HOME", "vim `x`", "vim > f", "vim < f", "vim & x", "vim (x)", "vim \"$HOME\"", "", "   ", "'unterminated",
                                       "vim \\", "vim\nfoo"})
    {
        auto r = editor::parse_command_line(bad, Syntax::Posix);
        CHECK(!r.has_value());
        if (!r)
            CHECK(!r.error().message.empty());
    }
}

TEST_CASE("command line parsing windows rules")
{
    using editor::Syntax;
    auto a = editor::parse_command_line("\"C:\\Program Files (x86)\\Vim\\vim.exe\" +{line} {file}", Syntax::Windows);
    REQUIRE(a.has_value());
    REQUIRE((a->size()) == 3u);
    CHECK_EQ((*a)[0], "C:\\Program Files (x86)\\Vim\\vim.exe");
    auto b = editor::parse_command_line("notepad.exe C:\\dir\\file.txt \"a\\\"b\" \"c\\\\\"", Syntax::Windows);
    REQUIRE(b.has_value());
    REQUIRE((b->size()) == 4u);
    CHECK_EQ((*b)[1], "C:\\dir\\file.txt");
    CHECK_EQ((*b)[2], "a\"b");
    CHECK_EQ((*b)[3], "c\\");
    auto c = editor::parse_command_line("it's 'quoted' x", Syntax::Windows);
    REQUIRE(c.has_value());
    CHECK_EQ((*c)[0], "it's");
    CHECK(!editor::parse_command_line("a | b", Syntax::Windows).has_value());
    CHECK(!editor::parse_command_line("a & b", Syntax::Windows).has_value());
    CHECK(!editor::parse_command_line("\"open", Syntax::Windows).has_value());
}

TEST_CASE("placeholder expansion stays inside single arguments")
{
    editor::Placeholders ph;
    ph.file = "/tmp/My Files/h\xC3\xA9llo w\xC3\xB6rld/\xE5\xB7\xA5\xE5\x85\xB7 {line}.dc";
    ph.workspace = "/tmp/My Files/h\xC3\xA9llo w\xC3\xB6rld";
    ph.line = 12;
    ph.column = 3;
    auto argv = editor::expand({"ed", "+{line},{column}", "{file}", "--root={workspace}", "{unknown}", "{{file}", "plain"}, ph);
    REQUIRE((argv.size()) == 7u);
    CHECK_EQ(argv[1], "+12,3");
    CHECK_EQ(argv[2], ph.file);
    CHECK_EQ(argv[3], "--root=" + ph.workspace);
    CHECK_EQ(argv[4], "{unknown}");
    CHECK_EQ(argv[5], "{" + ph.file);
    CHECK_EQ(argv[6], "plain");
}

TEST_CASE("editor presets and folder variants")
{
    auto nvim = editor::preset("nvim", false);
    REQUIRE(nvim.has_value());
    CHECK(nvim->kind == editor::Kind::Terminal);
    CHECK_EQ(nvim->argv_template[1], "+{line}");
    auto nano = editor::preset("nano", false);
    CHECK_EQ(nano->argv_template[1], "+{line},{column}");
    auto code = editor::preset("code", false);
    CHECK(code->kind == editor::Kind::Gui);
    CHECK_EQ(code->argv_template[1], "--goto");
    CHECK_EQ(code->argv_template[2], "{file}:{line}:{column}");
    CHECK_EQ(code->folder_template, (std::vector<std::string>{"code", "{workspace}"}));
    auto waiting = editor::preset("code", true);
    CHECK_EQ(waiting->argv_template[1], "--wait");
    auto notepad = editor::preset("notepad", false);
    CHECK_EQ(notepad->argv_template[0], "notepad.exe");
    CHECK(!editor::preset("emacs", false).has_value());
    editor::Placeholders ph;
    ph.workspace = "/w s";
    CHECK_EQ(editor::expand(code->folder_template, ph), (std::vector<std::string>{"code", "/w s"}));
}

TEST_CASE("editor resolution precedence")
{
    TempDir t;
    auto const bin = t.path / "bin";
    make_fake(bin, "nvim");
    make_fake(bin, "vim");
    make_fake(bin, "nano");
    make_fake(bin, "code");
    platform::Env env;
    env["PATH"] = platform::path_to_utf8(bin);
    settings::Settings s;
    auto const os = platform::Os::Linux;

    auto detected = editor::resolve(s, env, std::nullopt, os);
    CHECK(detected.source == editor::Source::Detected);
    CHECK_EQ(detected.label, "nvim");
    CHECK(detected.kind == editor::Kind::Terminal);

    env["EDITOR"] = "nano";
    auto from_editor = editor::resolve(s, env, std::nullopt, os);
    CHECK(from_editor.source == editor::Source::Editor);
    CHECK_EQ(from_editor.label, "nano");
    CHECK_EQ(from_editor.argv_template[1], "+{line},{column}");

    env["VISUAL"] = "vim";
    auto from_visual = editor::resolve(s, env, std::nullopt, os);
    CHECK(from_visual.source == editor::Source::Visual);
    CHECK_EQ(from_visual.label, "vim");

    s.editor_preset = "code";
    auto from_settings = editor::resolve(s, env, std::nullopt, os);
    CHECK(from_settings.source == editor::Source::Settings);
    CHECK(from_settings.kind == editor::Kind::Gui);

    auto from_override = editor::resolve(s, env, std::string("myed --flag {file}"), os);
    CHECK(from_override.source == editor::Source::Override);
    CHECK_EQ(from_override.argv_template, (std::vector<std::string>{"myed", "--flag", "{file}"}));

    auto appended = editor::resolve(s, env, std::string("myed --flag"), os);
    CHECK_EQ(appended.argv_template.back(), "{file}");

    auto bad = editor::resolve(s, env, std::string("myed | cat"), os);
    CHECK(bad.manual());
    CHECK(bad.source == editor::Source::Override);
    CHECK(!bad.problem.empty());

    s.editor_preset = "custom";
    s.editor_command = {"my editor", "--at", "{file}:{line}"};
    auto custom = editor::resolve(s, env, std::nullopt, os);
    CHECK(custom.source == editor::Source::Settings);
    CHECK_EQ(custom.argv_template[0], "my editor");

    s.editor_command.clear();
    auto empty_custom = editor::resolve(s, env, std::nullopt, os);
    CHECK(empty_custom.manual());
    CHECK(!empty_custom.problem.empty());

    s.editor_preset = "manual";
    CHECK(editor::resolve(s, env, std::nullopt, os).manual());

    s.editor_preset = "auto";
    s.editor_kind = "gui";
    s.editor_wait = true;
    auto forced = editor::resolve(s, env, std::nullopt, os);
    CHECK(forced.kind == editor::Kind::Gui);
    CHECK(forced.wait);

    platform::Env empty;
    empty["PATH"] = platform::path_to_utf8(t.path / "nothing");
    s = settings::Settings{};
    auto none = editor::resolve(s, empty, std::nullopt, os);
    CHECK(none.manual());
    CHECK(none.source == editor::Source::Manual);

    platform::Env broken;
    broken["PATH"] = platform::path_to_utf8(bin);
    broken["VISUAL"] = "bad | cmd";
    auto skipped = editor::resolve(s, broken, std::nullopt, os);
    CHECK(skipped.source == editor::Source::Detected);
    CHECK_EQ(skipped.warnings.size(), 1u);
}

namespace
{
    editor::EditorChoice fake_editor_choice(editor::Kind kind, bool wait)
    {
        settings::Settings s;
        s.editor_preset = "custom";
        s.editor_command = {g_self, "--fake-editor", "{file}", "two words", "line={line}", "col={column}", "\xE5\xB7\xA5\xE5\x85\xB7"};
        s.editor_kind = kind == editor::Kind::Terminal ? "terminal" : "gui";
        s.editor_wait = wait;
        return editor::resolve(s, platform::Env{}, std::nullopt, platform::Os::Linux);
    }
}

TEST_CASE("terminal editor launch passes arguments unsplit")
{
    TempDir t;
    auto const dir = t.path / "My Work" / "h\xC3\xA9llo \xE5\xB7\xA5";
    auto const file = dir / "main file.dc";
    write_file(file, "start\n");
    auto choice = fake_editor_choice(editor::Kind::Terminal, false);
    CHECK(editor::mode_of(choice) == editor::Mode::Terminal);
    editor::Placeholders ph;
    ph.file = platform::path_to_utf8(file);
    ph.workspace = platform::path_to_utf8(dir);
    ph.line = 9;
    ph.column = 4;
    auto r = editor::launch(choice, ph);
    CHECK(r.ok);
    CHECK(r.mode == editor::Mode::Terminal);
    CHECK_EQ(r.exit_code.value_or(-1), 0);
    CHECK_EQ(slurp(file), "start\nedited\n");
    auto const argv_dump = slurp(platform::path_to_utf8(file) + ".argv");
    auto const expected = std::format("{}\ntwo words\nline=9\ncol=4\n\xE5\xB7\xA5\xE5\x85\xB7\n", ph.file);
    CHECK_EQ(argv_dump, expected);
}

TEST_CASE("gui editors can launch blocking or detached")
{
    TempDir t;
    auto const file = t.path / "a b" / "main.dc";
    write_file(file, "x\n");
    editor::Placeholders ph;
    ph.file = platform::path_to_utf8(file);
    ph.workspace = platform::path_to_utf8(t.path / "a b");
    auto waiting = fake_editor_choice(editor::Kind::Gui, true);
    CHECK(editor::mode_of(waiting) == editor::Mode::GuiWait);
    auto r = editor::launch(waiting, ph);
    CHECK(r.ok);
    CHECK_EQ(slurp(file), "x\nedited\n");

    write_file(file, "y\n");
    auto detached = fake_editor_choice(editor::Kind::Gui, false);
    CHECK(editor::mode_of(detached) == editor::Mode::GuiDetached);
    auto d = editor::launch(detached, ph);
    CHECK(d.ok);
    bool seen = false;
    for (int i = 0; i < 100 && !seen; ++i)
    {
        seen = slurp(file) == "y\nedited\n";
        if (!seen)
            std::this_thread::sleep_for(50ms);
    }
    CHECK(seen);
}

TEST_CASE("manual editors and unstartable editors report instead of failing silently")
{
    editor::EditorChoice manual;
    editor::Placeholders ph;
    ph.file = "/tmp/x.dc";
    ph.workspace = "/tmp";
    auto m = editor::launch(manual, ph);
    CHECK(!m.ok);
    CHECK(m.mode == editor::Mode::Manual);
    CHECK_EQ(m.shown_path, "/tmp/x.dc");
    CHECK(m.message.find("/tmp/x.dc") != std::string::npos);

    settings::Settings s;
    s.editor_preset = "custom";
    s.editor_command = {"/nonexistent/learndc-editor", "{file}"};
    s.editor_kind = "terminal";
    auto choice = editor::resolve(s, platform::Env{}, std::nullopt, platform::Os::Linux);
    auto r = editor::launch(choice, ph);
    CHECK(!r.ok);
    CHECK(r.message.find("could not start") != std::string::npos);

    auto nvim = editor::preset("nvim", false);
    editor::LaunchOptions folder;
    folder.folder = true;
    auto f = editor::launch(*nvim, ph, folder);
    CHECK(!f.ok);
    CHECK(f.message.find("folder") != std::string::npos);
}

SECTION("learndc.doctor");

TEST_CASE("doctor reports missing tools with actionable fixes")
{
    TempDir t;
    doctor::DoctorOptions o;
    platform::Overrides ov;
    ov.home = t.path;
    o.dirs = platform::resolve_dirs(ov, platform::Env{}).value();
    o.env["PATH"] = platform::path_to_utf8(t.path / "none");
    o.os = platform::Os::Linux;
    o.stdin_tty = false;
    o.stdout_tty = false;
    auto content = base_source();
    o.content = &content;
    auto report = doctor::run_doctor(o);
    auto const* dcc = report.find("dcc");
    auto const* dpm = report.find("dpm");
    REQUIRE(dcc != nullptr);
    REQUIRE(dpm != nullptr);
    CHECK(dcc->status == doctor::Status::Error);
    CHECK(dcc->fix.find("tools.dcc") != std::string::npos);
    CHECK(dcc->fix.find("LEARNDC_DCC") != std::string::npos);
    CHECK(dpm->fix.find("tools.dpm") != std::string::npos);
    CHECK(dpm->fix.find("LEARNDC_DPM") != std::string::npos);
    CHECK(report.find("dccd")->status == doctor::Status::Info);
    CHECK(report.find("dir.data")->status == doctor::Status::Ok);
    CHECK(report.find("dir.cache")->status == doctor::Status::Ok);
    CHECK(report.find("content")->status == doctor::Status::Ok);
    CHECK(report.find("editor")->status == doctor::Status::Warn);
    CHECK(report.find("editor")->fix.find("editor.preset") != std::string::npos);
    CHECK(report.find("terminal.tty")->status == doctor::Status::Info);
    CHECK(report.find("terminal.term")->status == doctor::Status::Warn);
    CHECK(report.find("safety")->detail.find("not a sandbox") != std::string::npos);
    CHECK(report.has_errors());
    auto const text = doctor::render_text(report, {});
    CHECK(text.find("Fix:") != std::string::npos);
    CHECK(text.find("2 errors") != std::string::npos);
    auto j = doctor::to_json(report);
    CHECK_EQ(j.find("ok")->as_bool().value_or(true), false);
    CHECK_EQ(j.find("items")->size(), report.items.size());
}

TEST_CASE("doctor flags broken content unwritable dirs and terminal problems")
{
    TempDir t;
    doctor::DoctorOptions o;
    platform::Overrides ov;
    ov.home = t.path;
    o.dirs = platform::resolve_dirs(ov, platform::Env{}).value();
    write_file(t.path / "blocker", "file");
    o.dirs.cache_dir = t.path / "blocker" / "cache";
    o.env["TERM"] = "dumb";
    o.env["NO_COLOR"] = "1";
    o.os = platform::Os::Linux;
    o.terminal = platform::TerminalSize{40, 10};
    auto content = base_source();
    edit(content, "tracks/basics/one/exercise.toml", "schema = 1", "schema = 9");
    o.content = &content;
    auto report = doctor::run_doctor(o);
    CHECK(report.find("content")->status == doctor::Status::Error);
    CHECK(report.find("dir.cache")->status == doctor::Status::Error);
    CHECK(report.find("dir.cache")->fix.find("LEARNDC_HOME") != std::string::npos);
    CHECK(report.find("terminal.term")->status == doctor::Status::Warn);
    CHECK(report.find("terminal.size")->status == doctor::Status::Warn);
    CHECK(report.find("terminal.color")->detail.find("NO_COLOR") != std::string::npos);
}

TEST_CASE("doctor with the real toolchain finds versions and can build")
{
    REQUIRE(real_tools_present());
    TempDir t;
    auto const run = make_options(t.path);
    doctor::DoctorOptions o;
    o.dirs = run.dirs;
    o.settings = run.settings;
    o.tools = run.tools;
    o.env = platform::current_env();
    o.stdin_tty = false;
    o.stdout_tty = false;
    o.build = true;
    auto report = doctor::run_doctor(o);
    CHECK(report.find("dcc")->status == doctor::Status::Ok);
    CHECK(report.find("dpm")->status == doctor::Status::Ok);
    auto const* build = report.find("build");
    REQUIRE(build != nullptr);
    CHECK(build->status == doctor::Status::Ok);
}

SECTION("learndc.cli and learndc.app");

namespace
{
    struct CliRun
    {
        int code = -1;
        std::string out;
        std::string err;
    };

    void write_source(catalog::ContentSource const& source, std::filesystem::path const& dir)
    {
        for (auto const& [path, data] : source.files)
            write_file(dir / platform::path_from_utf8(path), data);
    }

    struct CliFixture
    {
        TempDir temp;
        std::filesystem::path home;
        std::filesystem::path content;

        CliFixture()
        {
            home = temp.path / "home";
            content = temp.path / "content";
            write_source(base_source(), content);
        }

        CliRun run(std::vector<std::string> args, std::optional<std::filesystem::path> cwd = std::nullopt, bool tty = false) const
        {
            std::ostringstream out;
            std::ostringstream err;
            std::istringstream in;
            learndc::cli::Io io;
            io.out = &out;
            io.err = &err;
            io.in = &in;
            io.env = platform::Env{{"PATH", platform::path_to_utf8(temp.path / "nowhere")}, {"LANG", "C.UTF-8"}};
            io.cwd = cwd.value_or(temp.path);
            io.stdin_tty = tty;
            io.stdout_tty = tty;
            io.stderr_tty = false;
            io.self_exe = std::nullopt;
            args.insert(args.begin(), {"--home", platform::path_to_utf8(home), "--content-dir", platform::path_to_utf8(content)});
            CliRun r;
            r.code = learndc::cli::run(args, io);
            r.out = out.str();
            r.err = err.str();
            return r;
        }
    };

    std::expected<learndc::cli::Parsed, std::string> parse_args(std::vector<std::string> args)
    {
        return learndc::cli::parse(args);
    }
} // namespace

TEST_CASE("cli parsing of commands options and positionals")
{
    auto p = parse_args({"check", "basics/one", "--json", "--backend", "llvm"});
    REQUIRE(p.has_value());
    CHECK_EQ(p->command, "check");
    CHECK(p->flag("json"));
    CHECK_EQ(p->value("backend").value_or(""), "llvm");
    CHECK_EQ(p->positional(0).value_or(""), "basics/one");
    CHECK(!p->positional(1).has_value());

    p = parse_args({"--home", "/x y", "list", "--track=basics", "--no-color"});
    REQUIRE(p.has_value());
    CHECK_EQ(p->value("home").value_or(""), "/x y");
    CHECK_EQ(p->value("track").value_or(""), "basics");
    CHECK(p->flag("no-color"));

    p = parse_args({"run", "basics/one", "--", "--flag", "-x", "arg"});
    REQUIRE(p.has_value());
    CHECK_EQ(p->positionals.size(), 1u);
    CHECK((p->rest == std::vector<std::string>{"--flag", "-x", "arg"}));

    p = parse_args({"search", "two", "words", "here"});
    REQUIRE(p.has_value());
    CHECK_EQ(p->positionals.size(), 3u);

    p = parse_args({"--version"});
    REQUIRE(p.has_value());
    CHECK(p->version);
    CHECK(p->command.empty());

    p = parse_args({"check", "--help"});
    REQUIRE(p.has_value());
    CHECK(p->help);
    p = parse_args({"show", "-h"});
    REQUIRE(p.has_value());
    CHECK(p->help);
}

TEST_CASE("cli parsing rejects bad input with a message")
{
    auto p = parse_args({"frobnicate"});
    CHECK(!p.has_value());
    CHECK(p.error().find("unknown command") != std::string::npos);
    p = parse_args({"chec"});
    REQUIRE(!p.has_value());
    CHECK(p.error().find("did you mean 'check'") != std::string::npos);
    p = parse_args({"list", "--bogus"});
    REQUIRE(!p.has_value());
    CHECK(p.error().find("--bogus") != std::string::npos);
    CHECK(!parse_args({"list", "--track"}).has_value());
    CHECK(!parse_args({"list", "--json=1"}).has_value());
    CHECK(!parse_args({"show"}).has_value());
    CHECK(!parse_args({"show", "a", "b"}).has_value());
    CHECK(!parse_args({"list", "--", "x"}).has_value());
    CHECK(!parse_args({"-x"}).has_value());
    CHECK(parse_args({"show"}).error().find("usage: learndc show") != std::string::npos);
}

TEST_CASE("every command has help and a usable spec")
{
    for (auto const& c : learndc::cli::commands())
    {
        auto const h = learndc::cli::command_help(c);
        CHECK(h.find(c.name) != std::string::npos);
        CHECK(h.find("Global options") != std::string::npos);
        CHECK(c.min_positionals <= c.max_positionals);
    }
    auto const help = learndc::cli::help_text();
    for (auto const* word : {"list", "search", "show", "start", "next", "edit", "run", "check", "watch", "hint", "solution", "reset", "progress", "review",
                             "bookmark", "config", "doctor", "export", "import", "dev", "Exit codes"})
        CHECK(help.find(word) != std::string::npos);
}

TEST_CASE("cli exit codes for usage content and lookup errors")
{
    CliFixture f;
    CHECK_EQ(f.run({"--help"}).code, 0);
    CHECK_EQ(f.run({"check", "--help"}).code, 0);
    CHECK(f.run({"help", "reset"}).out.find("--no-backup") != std::string::npos);
    CHECK_EQ(f.run({"help", "nothing"}).code, 2);
    CHECK_EQ(f.run({"nonsense"}).code, 2);
    CHECK_EQ(f.run({"show"}).code, 2);
    CHECK_EQ(f.run({"list", "--status", "bogus"}).code, 2);
    CHECK_EQ(f.run({"list", "--track", "missing"}).code, 2);
    CHECK_EQ(f.run({"show", "basics/missing"}).code, 5);
    CHECK_EQ(f.run({"start", "nope/nope"}).code, 5);
    CHECK_EQ(f.run({"check"}).code, 5);
    CHECK_EQ(f.run({"edit"}).code, 5);
    CHECK_EQ(f.run({"hint"}).code, 5);
    CHECK_EQ(f.run({"solution", "--diff"}).code, 5);
    auto const no_args = f.run({});
    CHECK_EQ(no_args.code, 0);
    CHECK(no_args.out.find("Usage:") != std::string::npos);
    auto const version = f.run({"--version"});
    CHECK_EQ(version.code, 0);
    CHECK(version.out.find("3 exercises") != std::string::npos);

    learndc::cli::Io io;
    std::ostringstream out;
    std::ostringstream err;
    io.out = &out;
    io.err = &err;
    io.stdin_tty = true;
    io.stdout_tty = true;
    bool launched = false;
    io.tui = [&](learndc::app::Context&) {
        launched = true;
        return 7;
    };
    io.env = platform::Env{};
    std::vector<std::string> const args = {"--home", platform::path_to_utf8(f.home), "--content-dir", platform::path_to_utf8(f.content)};
    CHECK_EQ(learndc::cli::run(args, io), 7);
    CHECK(launched);

    std::filesystem::remove(f.content / "catalog.toml");
    CHECK_EQ(f.run({"list"}).code, 3);
    CHECK_EQ(f.run({"--version"}).code, 3);
    CHECK_EQ(f.run({"config", "list"}).code, 0);
    write_file(f.home / "config.json", "{broken");
    auto const moved = f.run({"config", "list"});
    CHECK_EQ(moved.code, 0);
    CHECK(moved.err.find("damaged") != std::string::npos);
}

TEST_CASE("cli list search and show output and json")
{
    CliFixture f;
    auto const list = f.run({"list"});
    CHECK_EQ(list.code, 0);
    CHECK(list.out.find("basics/one") != std::string::npos);
    CHECK(list.out.find("\x1b[") == std::string::npos);
    auto const as_json = f.run({"list", "--json"});
    auto parsed = json::parse(as_json.out);
    REQUIRE(parsed.has_value());
    auto const* arr = parsed->find("exercises");
    REQUIRE(arr != nullptr);
    CHECK_EQ(arr->size(), 3u);
    CHECK_EQ(f.run({"list", "--track", "more", "--json"}).code, 0);
    auto const none = f.run({"list", "--status", "completed"});
    CHECK(none.out.find("No exercises match") != std::string::npos);
    auto const search = f.run({"search", "Title", "two", "--json"});
    auto sj = json::parse(search.out);
    REQUIRE(sj.has_value());
    CHECK_EQ(sj->find("query")->as_string().value_or(""), "Title two");
    auto const show = f.run({"show", "one", "--json"});
    auto shj = json::parse(show.out);
    REQUIRE(shj.has_value());
    CHECK_EQ(shj->find("id")->as_string().value_or(""), "basics/one");
    CHECK(shj->find("readme") != nullptr);
    CHECK(shj->find("workspace")->is_null());
}

TEST_CASE("cli start reset hint solution bookmark config flows and exit codes")
{
    CliFixture f;
    auto const started = f.run({"start", "basics/one"});
    CHECK_EQ(started.code, 0);
    CHECK(started.out.find("Created the workspace") != std::string::npos);
    auto const root = f.home / "workspaces" / "basics" / "one";
    CHECK(std::filesystem::is_regular_file(root / "src" / "main.dc"));
    CHECK(f.run({"start", "basics/one"}).out.find("Resumed") != std::string::npos);

    write_file(f.temp.path / "occupied" / "file.txt", "x");
    CHECK_EQ(f.run({"start", "basics/two", "--dir", platform::path_to_utf8(f.temp.path / "occupied")}).code, 6);

    write_file(root / "src" / "main.dc", "module main;\n// edited\n");
    auto const refused = f.run({"reset", "basics/one"});
    CHECK_EQ(refused.code, 6);
    CHECK(refused.err.find("--yes") != std::string::npos);
    CHECK(std::filesystem::exists(root / "src" / "main.dc"));
    CHECK(platform::read_file(root / "src" / "main.dc").value_or("").find("edited") != std::string::npos);
    CHECK_EQ(f.run({"reset", "basics/one", "--yes"}).code, 0);
    CHECK(platform::read_file(root / "src" / "main.dc").value_or("").find("edited") == std::string::npos);
    bool backed_up = false;
    for (auto const& e : std::filesystem::recursive_directory_iterator(f.home / "backups"))
        if (e.is_regular_file() && platform::read_file(e.path()).value_or("").find("edited") != std::string::npos)
            backed_up = true;
    CHECK(backed_up);

    auto const h1 = f.run({"hint", "basics/one"});
    CHECK_EQ(h1.code, 0);
    CHECK(h1.out.find("Hint 1 of 3") != std::string::npos);
    CHECK(f.run({"hint", "basics/one"}).out.find("Hint 2 of 3") != std::string::npos);
    CHECK_EQ(f.run({"hint", "basics/one", "--level", "7"}).code, 2);
    auto prog = json::parse(f.run({"progress", "--json"}).out);
    REQUIRE(prog.has_value());
    CHECK_EQ(prog->find("in_progress")->as_int().value_or(0), 1);
    CHECK_EQ(prog->find("active_exercise")->as_string().value_or(""), "basics/one");

    CHECK_EQ(f.run({"solution", "basics/one"}).code, 2);
    CHECK_EQ(f.run({"solution", "basics/one", "--show", "--diff"}).code, 2);
    auto const diff = f.run({"solution", "basics/one", "--diff"});
    CHECK_EQ(diff.code, 0);
    CHECK(diff.out.find("solved") != std::string::npos);
    CHECK_EQ(f.run({"bookmark", "basics/two"}).code, 0);
    CHECK_EQ(f.run({"bookmark", "basics/two", "--remove"}).code, 0);
    CHECK_EQ(f.run({"bookmark", "basics/zzz"}).code, 5);

    CHECK_EQ(f.run({"config", "set", "ui.theme", "light"}).code, 0);
    CHECK_EQ(f.run({"config", "get", "ui.theme"}).out, "light\n");
    CHECK_EQ(f.run({"config", "set", "ui.theme", "blue"}).code, 2);
    CHECK_EQ(f.run({"config", "get", "nothing"}).code, 2);
    CHECK_EQ(f.run({"config", "unset", "ui.theme"}).code, 0);
    CHECK_EQ(f.run({"config", "get", "ui.theme"}).out, "dark\n");
    CHECK_EQ(f.run({"config", "wat"}).code, 2);
}

TEST_CASE("cli check and run report a missing toolchain with exit code 4")
{
    CliFixture f;
    REQUIRE(f.run({"start", "basics/one"}).code == 0);
    auto const check = f.run({"check", "basics/one"});
    CHECK_EQ(check.code, 4);
    auto const json_check = f.run({"check", "basics/one", "--json"});
    CHECK_EQ(json_check.code, 4);
    auto j = json::parse(json_check.out);
    REQUIRE(j.has_value());
    CHECK_EQ(j->find("outcome")->as_string().value_or(""), "error");
    CHECK_EQ(f.run({"check", "basics/one", "--backend", "x"}).code, 2);
    CHECK_EQ(f.run({"run", "basics/one"}).code, 4);
    CHECK_EQ(f.run({"doctor"}).code, 4);
    CHECK_EQ(f.run({"dev", "validate"}).code, 1);
    CHECK_EQ(f.run({"dev", "validate", "--id", "x/y"}).code, 5);
    CHECK_EQ(f.run({"dev", "validate", "--backend", "q"}).code, 2);
    CHECK_EQ(f.run({"dev", "other"}).code, 2);
}

TEST_CASE("cli export and import round trip with preview and apply")
{
    CliFixture f;
    REQUIRE(f.run({"start", "basics/one"}).code == 0);
    REQUIRE(f.run({"bookmark", "basics/one"}).code == 0);
    auto const file = f.temp.path / "exports" / "p.json";
    CHECK_EQ(f.run({"export"}).code, 2);
    CHECK_EQ(f.run({"export", "--output", platform::path_to_utf8(file)}).code, 0);
    CHECK_EQ(f.run({"export", "--output", platform::path_to_utf8(file)}).code, 6);
    CHECK_EQ(f.run({"export", "--output", platform::path_to_utf8(file), "--yes"}).code, 0);
    CliFixture g;
    g.content = f.content;
    CHECK_EQ(g.run({"import"}).code, 2);
    auto const preview = g.run({"import", "--input", platform::path_to_utf8(file)});
    CHECK_EQ(preview.code, 0);
    CHECK(preview.out.find("Preview only") != std::string::npos);
    CHECK(g.run({"list", "--status", "in-progress"}).out.find("No exercises match") != std::string::npos);
    auto const applied = g.run({"import", "--input", platform::path_to_utf8(file), "--apply"});
    CHECK_EQ(applied.code, 0);
    CHECK(g.run({"list", "--status", "in-progress"}).out.find("basics/one") != std::string::npos);
    write_file(f.temp.path / "bad.json", "{oops");
    CHECK_EQ(g.run({"import", "--input", platform::path_to_utf8(f.temp.path / "bad.json")}).code, 3);
    CHECK_EQ(g.run({"import", "--input", platform::path_to_utf8(f.temp.path / "none.json")}).code, 1);
}

TEST_CASE("cli export refuses to overwrite learndc's own data files")
{
    CliFixture f;
    REQUIRE(f.run({"start", "basics/one"}).code == 0);
    REQUIRE(f.run({"bookmark", "basics/one"}).code == 0);
    auto const progress_file = f.home / "progress.json";
    REQUIRE(std::filesystem::exists(progress_file));
    auto const before = slurp(progress_file);
    for (auto const& target : {progress_file, f.home / "progress.json.bak", f.home / "config.json", f.home / "sub" / ".." / "progress.json"})
    {
        auto r = f.run({"export", "--output", platform::path_to_utf8(target), "--yes"});
        CHECK_EQ(r.code, 6);
        CHECK(r.err.find("own data files") != std::string::npos);
    }
    CHECK_EQ(slurp(progress_file), before);
    CHECK(!std::filesystem::exists(f.home / "config.json"));
    CHECK_EQ(f.run({"export", "--output", platform::path_to_utf8(f.temp.path / "ok.json")}).code, 0);
}

TEST_CASE("cli watch checks again after an edit saved while a check was running")
{
    CliFixture f;
    auto const ws = f.temp.path / "ws";
    REQUIRE(f.run({"start", "basics/one", "--dir", platform::path_to_utf8(ws)}).code == 0);
    EnvGuard guard("brief");
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in;
    std::atomic<bool> cancel{false};
    learndc::cli::Io io;
    io.out = &out;
    io.err = &err;
    io.in = &in;
    io.env = platform::Env{{"PATH", platform::path_to_utf8(f.temp.path / "nowhere")}, {"LANG", "C.UTF-8"}, {"LEARNDC_DPM", g_self}, {"LEARNDC_DCC", g_self}};
    io.cwd = f.temp.path;
    io.stdin_tty = false;
    io.stdout_tty = false;
    io.stderr_tty = false;
    io.self_exe = std::nullopt;
    io.cancel = &cancel;
    std::vector<std::string> const args = {"--home", platform::path_to_utf8(f.home), "--content-dir", platform::path_to_utf8(f.content), "watch", "basics/one"};
    std::thread runner_thread([&] { (void)learndc::cli::run(args, io); });
    std::this_thread::sleep_for(500ms);
    write_file(ws / "src" / "main.dc", "edited while the first check runs\n");
    std::this_thread::sleep_for(5s);
    cancel = true;
    runner_thread.join();
    CHECK(out.str().find("changed; checking again") != std::string::npos);
}

TEST_CASE("cli edit does not launch a detected terminal editor without a terminal")
{
    CliFixture f;
    auto const ws = f.temp.path / "ws";
    REQUIRE(f.run({"start", "basics/one", "--dir", platform::path_to_utf8(ws)}).code == 0);
    make_fake(f.temp.path / "bin", "nvim");
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in;
    learndc::cli::Io io;
    io.out = &out;
    io.err = &err;
    io.in = &in;
    io.env = platform::Env{{"PATH", platform::path_to_utf8(f.temp.path / "bin")}, {"LANG", "C.UTF-8"}};
    io.cwd = f.temp.path;
    io.stdin_tty = false;
    io.stdout_tty = false;
    io.stderr_tty = false;
    io.self_exe = std::nullopt;
    std::vector<std::string> const args = {"--home", platform::path_to_utf8(f.home), "--content-dir", platform::path_to_utf8(f.content), "edit", "basics/one"};
    CHECK_EQ(learndc::cli::run(args, io), 1);
    CHECK(err.str().find("needs an interactive session") != std::string::npos);
    CHECK(err.str().find("main.dc") != std::string::npos);
}

TEST_CASE("cli hints and solutions do not start an exercise")
{
    CliFixture f;
    REQUIRE(f.run({"hint", "basics/one"}).code == 0);
    auto after_hint = f.run({"progress", "--json"});
    CHECK(after_hint.out.find("\"in_progress\": 0") != std::string::npos);
    f.run({"solution", "basics/one", "--show"});
    auto after_solution = f.run({"progress", "--json"});
    CHECK(after_solution.out.find("\"in_progress\": 0") != std::string::npos);
    REQUIRE(f.run({"start", "basics/one"}).code == 0);
    CHECK(f.run({"progress", "--json"}).out.find("\"in_progress\": 1") != std::string::npos);
}

TEST_CASE("cli moves a damaged settings file aside and keeps working")
{
    CliFixture f;
    std::filesystem::create_directories(f.home);
    write_file(f.home / "config.json", "{bad");
    auto r = f.run({"config", "set", "ui.theme", "light"});
    CHECK_EQ(r.code, 0);
    CHECK(r.err.find("damaged") != std::string::npos);
    CHECK(r.err.find("config.json.corrupt-") != std::string::npos);
    CHECK_EQ(entries_with_prefix(f.home, "config.json.corrupt-").size(), 1u);
    CHECK_EQ(slurp(entries_with_prefix(f.home, "config.json.corrupt-")[0]), "{bad");
    CHECK(f.run({"config", "get", "ui.theme"}).out.find("light") != std::string::npos);
    CHECK_EQ(f.run({"doctor"}).code == 3, false);
}

TEST_CASE("app resolves the exercise from the argument then the enclosing workspace then the active exercise")
{
    CliFixture f;
    learndc::app::OpenOptions oo;
    oo.home = f.home;
    oo.content_dir = f.content;
    oo.env = platform::Env{{"PATH", ""}};
    auto ctx = learndc::app::open(oo);
    REQUIRE(ctx.has_value());
    CHECK(ctx->has_content());
    auto const* one = ctx->catalog.find("basics/one");
    auto const* two = ctx->catalog.find("basics/two");
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    progress::Progress empty;
    auto none = learndc::app::resolve_target(*ctx, empty, std::nullopt, f.temp.path, false);
    REQUIRE(!none.has_value());
    CHECK_EQ(none.error().code, learndc::app::exit_not_found);

    auto started = learndc::app::start_exercise(*ctx, *one, std::nullopt);
    REQUIRE(started.has_value());
    CHECK(started->result.created);
    auto p = learndc::app::load_progress(*ctx);
    REQUIRE(p.has_value());
    CHECK_EQ(p->active_exercise, "basics/one");

    auto active = learndc::app::resolve_target(*ctx, *p, std::nullopt, f.temp.path, true);
    REQUIRE(active.has_value());
    CHECK_EQ(active->exercise->id, "basics/one");
    CHECK(active->how == learndc::app::Resolved::Active);
    CHECK(active->workspace.has_value());

    auto named = learndc::app::resolve_target(*ctx, *p, std::string("two"), f.temp.path, false);
    REQUIRE(named.has_value());
    CHECK_EQ(named->exercise->id, "basics/two");
    CHECK(named->how == learndc::app::Resolved::Argument);
    CHECK(!named->workspace.has_value());
    auto needed = learndc::app::resolve_target(*ctx, *p, std::string("basics/two"), f.temp.path, true);
    REQUIRE(!needed.has_value());
    CHECK_EQ(needed.error().code, learndc::app::exit_not_found);

    REQUIRE(learndc::app::start_exercise(*ctx, *two, std::nullopt).has_value());
    p = learndc::app::load_progress(*ctx);
    REQUIRE(p.has_value());
    auto from_cwd = learndc::app::resolve_target(*ctx, *p, std::nullopt, started->root / "src", true);
    REQUIRE(from_cwd.has_value());
    CHECK_EQ(from_cwd->exercise->id, "basics/one");
    CHECK(from_cwd->how == learndc::app::Resolved::WorkingDirectory);
    auto cwd_two = learndc::app::resolve_target(*ctx, *p, std::nullopt, ctx->dirs.workspaces_dir / "basics" / "two" / "src", true);
    REQUIRE(cwd_two.has_value());
    CHECK_EQ(cwd_two->exercise->id, "basics/two");
    CHECK_EQ(p->active_exercise, "basics/two");

    auto unknown = learndc::app::resolve_target(*ctx, *p, std::string("basics/tw0"), f.temp.path, false);
    REQUIRE(!unknown.has_value());
    CHECK_EQ(unknown.error().code, learndc::app::exit_not_found);
    CHECK(unknown.error().message.find("unknown exercise") != std::string::npos);
}

TEST_CASE("app records check reports completion and recommends the next exercise")
{
    CliFixture f;
    learndc::app::OpenOptions oo;
    oo.home = f.home;
    oo.content_dir = f.content;
    oo.env = platform::Env{{"PATH", ""}};
    auto ctx = learndc::app::open(oo);
    REQUIRE(ctx.has_value());
    auto const* one = ctx->catalog.find("basics/one");
    REQUIRE(one != nullptr);

    runner::CheckReport failed;
    failed.outcome = runner::Outcome::Failed;
    failed.passed = 1;
    failed.total = 2;
    auto rec = learndc::app::record_report(*ctx, *one, failed);
    REQUIRE(rec.has_value());
    CHECK(rec->recorded);
    CHECK(!rec->completed_now);
    CHECK_EQ(learndc::app::exit_code_for(failed), 1);

    runner::CheckReport errored;
    errored.outcome = runner::Outcome::Error;
    rec = learndc::app::record_report(*ctx, *one, errored);
    REQUIRE(rec.has_value());
    CHECK(!rec->recorded);

    runner::CheckReport passed;
    passed.outcome = runner::Outcome::Passed;
    passed.passed = 2;
    passed.total = 2;
    rec = learndc::app::record_report(*ctx, *one, passed);
    REQUIRE(rec.has_value());
    CHECK(rec->completed_now);
    CHECK_EQ(rec->next_id, "basics/two");
    rec = learndc::app::record_report(*ctx, *one, passed);
    REQUIRE(rec.has_value());
    CHECK(!rec->completed_now);

    auto p = learndc::app::load_progress(*ctx);
    REQUIRE(p.has_value());
    auto const& e = p->exercises.at("basics/one");
    CHECK_EQ(e.attempts, 3);
    CHECK_EQ(e.passes, 2);
    CHECK(progress::is_completed(*p, "basics/one"));
    auto sum = learndc::app::summarize(*ctx, *p);
    CHECK_EQ(sum.total, 3);
    CHECK_EQ(sum.completed, 1);

    runner::CheckReport cancelled;
    cancelled.outcome = runner::Outcome::Cancelled;
    CHECK_EQ(learndc::app::exit_code_for(cancelled), 130);
    runner::CheckReport tool_missing;
    tool_missing.outcome = runner::Outcome::Error;
    runner::CheckResult r;
    r.stage = runner::Stage::Toolchain;
    tool_missing.results.push_back(r);
    CHECK_EQ(learndc::app::exit_code_for(tool_missing), 4);

    auto hint = learndc::app::reveal_hint(*ctx, *one, std::nullopt);
    REQUIRE(hint.has_value());
    CHECK_EQ(hint->level, 1);
    hint = learndc::app::reveal_hint(*ctx, *one, 3);
    REQUIRE(hint.has_value());
    hint = learndc::app::reveal_hint(*ctx, *one, std::nullopt);
    REQUIRE(hint.has_value());
    CHECK(hint->all_shown);
    CHECK(!learndc::app::reveal_hint(*ctx, *one, 0).has_value());

    auto review = learndc::app::review_list(*ctx, *learndc::app::load_progress(*ctx));
    CHECK(review.empty() || review.front().exercise != nullptr);
}

TEST_CASE("cli never colors without a terminal and honors NO_COLOR")
{
    CliFixture f;
    REQUIRE(f.run({"start", "basics/one"}).code == 0);
    CHECK(f.run({"list"}).out.find("\x1b[") == std::string::npos);
    CHECK(f.run({"list"}, std::nullopt, true).out.find("\x1b[") != std::string::npos);
    CHECK(f.run({"list", "--no-color"}, std::nullopt, true).out.find("\x1b[") == std::string::npos);
}

SECTION("learndc.tui");

namespace
{
    namespace tm = learndc::tui::model;

    std::string tui_exercise_toml(std::string_view track, std::string_view name, std::string_view title, std::string_view summary, std::string_view difficulty,
                                  std::string_view kind, int minutes, std::string_view prerequisites)
    {
        return std::format(R"(schema = 1
id = "{0}/{1}"
revision = 1
title = "{2}"
summary = "{3}"
kind = "{5}"
chapter = "intro"
difficulty = "{4}"
estimated_minutes = {6}
concepts = ["{1}", "output"]
tags = []
objectives = ["Make every check pass", "Read the diagnostics before changing code"]
entry_file = "src/main.dc"
product = "prog"
prerequisites = {7}
requires = ["dcc", "dpm"]
min_dcc = "0.6.0"
platforms = ["linux", "windows"]
backends = ["llvm", "custom"]
starter_state = "fails-checks"

[[checks]]
kind = "build"
name = "Compiles"

[[checks]]
kind = "run"
name = "Prints the total"
expect_stdout = "Total: 42\n"

[[checks]]
kind = "run"
name = "Handles zero tax"
expect_stdout = "Total: 40\n"
)",
                           track, name, title, summary, difficulty, kind, minutes, prerequisites);
    }

    constexpr std::string_view tui_readme =
        "# Variables and mutation\n\nA variable gives a name to a value. Declare it with its type, then use the name wherever you need the value.\n\n"
        "- Declare `price` and `tax` as `i32`.\n- Add them together into `total`.\n\n```dc\ni32 total = 40 + 2;\n```\n\n"
        "Run the checks when you think you are done.\n";

    constexpr std::string_view tui_main_source =
        "module main;\n\nimport std::io;\n\npublic i32 main()\n{\n    i32 price = 40;\n    i32 tax = 2;\n    i32 total = price - tax;\n"
        "    io::println(\"Total: {}\", total);\n    return 0;\n}\n";

    constexpr std::string_view tui_solution_source =
        "module main;\n\nimport std::io;\n\npublic i32 main()\n{\n    i32 price = 40;\n    i32 tax = 2;\n    i32 total = price + tax;\n"
        "    io::println(\"Total: {}\", total);\n    return 0;\n}\n";

    void add_tui_exercise(catalog::ContentSource& s, std::string_view track, std::string_view name, std::string_view title, std::string_view summary,
                          std::string_view difficulty, std::string_view kind, int minutes, std::string_view prerequisites)
    {
        auto const dir = std::format("tracks/{}/{}/", track, name);
        s.add(dir + "exercise.toml", tui_exercise_toml(track, name, title, summary, difficulty, kind, minutes, prerequisites));
        s.add(dir + "README.md", std::string(tui_readme));
        s.add(dir + "hints.md",
              "## Hint 1\n\nRead the failing check: it names the value it expected.\n\n## Hint 2\n\nLook at the operator on the line that computes total.\n\n"
              "## Hint 3\n\nTax is added to the price, not subtracted from it.\n");
        s.add(dir + "explanation.md", "Adding the tax to the price gives the total. The minus sign subtracted it instead.\n");
        s.add(dir + "starter/project.dcon", "dpm::v1::Project { name = \"prog\" }\n");
        s.add(dir + "starter/src/main.dc", std::string(tui_main_source));
        s.add(dir + "solution/src/main.dc", std::string(tui_solution_source));
    }

    catalog::ContentSource tui_source()
    {
        catalog::ContentSource s;
        s.label = "fixture";
        s.add("catalog.toml", "schema = 1\ntitle = \"Learn DC\"\ntracks = [\"fundamentals\", \"types\"]\n");
        s.add("tracks/fundamentals/track.toml",
              "schema = 1\nid = \"fundamentals\"\ntitle = \"Fundamentals\"\nsummary = \"First steps\"\nexercises = [\"hello-world\", \"variables\", \"branches\", \"loops\"]\n");
        s.add("tracks/types/track.toml", "schema = 1\nid = \"types\"\ntitle = \"Types and data\"\nsummary = \"Model your data\"\nexercises = [\"structs\", \"enums\"]\n");
        add_tui_exercise(s, "fundamentals", "hello-world", "Your first DC program", "Print a greeting from a fallible main.", "beginner", "complete-program", 5, "[]");
        add_tui_exercise(s, "fundamentals", "variables", "Variables and mutation", "Fix the arithmetic so the program prints the right total.", "beginner",
                         "fix-behavior", 10, "[\"fundamentals/hello-world\"]");
        add_tui_exercise(s, "fundamentals", "branches", "Making decisions with if", "Choose between two paths with a condition.", "beginner",
                         "implement-function", 12, "[\"fundamentals/variables\"]");
        add_tui_exercise(s, "fundamentals", "loops", "Repeating work with loops", "Sum a range with a for loop.", "intermediate", "implement-function", 15,
                         "[\"fundamentals/branches\"]");
        add_tui_exercise(s, "types", "structs", "Grouping data in structs", "Bundle related values into one type.", "intermediate", "complete-program", 20,
                         "[\"fundamentals/hello-world\"]");
        add_tui_exercise(s, "types", "enums", "Choosing with enums", "Model a closed set of cases.", "advanced", "refactor", 25, "[\"types/structs\"]");
        return s;
    }

    struct TuiWorld
    {
        TempDir temp;
        std::filesystem::path home;
        std::filesystem::path content;
        std::unique_ptr<learndc::app::Context> ctx;

        explicit TuiWorld(bool with_progress = true, std::function<void(settings::Settings&)> const& tweak = {}, platform::Env env_in = {})
        {
            home = temp.path / "home";
            content = temp.path / "content";
            write_source(tui_source(), content);
            std::filesystem::create_directories(home);
            settings::Settings st;
            st.onboarding_done = true;
            st.ui_open_continue = false;
            st.ui_ascii = "off";
            if (tweak)
                tweak(st);
            REQUIRE(settings::save(home / "config.json", st).has_value());
            learndc::app::OpenOptions oo;
            oo.home = home;
            oo.content_dir = content;
            oo.env = env_in.empty() ? platform::Env{{"PATH", platform::path_to_utf8(temp.path / "nowhere")}, {"LANG", "C.UTF-8"}} : std::move(env_in);
            oo.self_exe = std::nullopt;
            auto opened = learndc::app::open(oo);
            REQUIRE(opened.has_value());
            ctx = std::make_unique<learndc::app::Context>(std::move(*opened));
            if (with_progress)
                seed();
        }

        void seed()
        {
            constexpr std::string_view stamp = "2026-01-01T00:00:00Z";
            auto const* hello = ctx->catalog.find("fundamentals/hello-world");
            auto const* variables = ctx->catalog.find("fundamentals/variables");
            REQUIRE(hello != nullptr);
            REQUIRE(variables != nullptr);
            REQUIRE(learndc::app::start_exercise(*ctx, *hello, std::nullopt).has_value());
            REQUIRE(learndc::app::start_exercise(*ctx, *variables, std::nullopt).has_value());
            auto done = ctx->store.update([&](progress::Progress& p) {
                progress::record_check(p, "fundamentals/hello-world", 3, 3, 1, stamp);
                progress::record_check(p, "fundamentals/variables", 1, 3, 1, stamp);
                progress::record_check(p, "fundamentals/variables", 2, 3, 1, stamp);
                progress::set_active(p, "fundamentals/variables");
                progress::toggle_bookmark(p, "types/structs");
            });
            REQUIRE(done.has_value());
        }

        [[nodiscard]] std::string exercise_file(std::string_view id, std::string_view rel) const
        {
            auto const* ex = ctx->catalog.find(id);
            auto const root = learndc::workspace::default_root(ctx->dirs, *ex);
            return platform::read_file(root / platform::path_from_utf8(rel)).value_or("<missing>");
        }
    };

    std::vector<tm::Effect> press(tm::Model& m, std::string_view key)
    {
        return m.handle(tm::Key{std::string(key)});
    }

    void type_text(tm::Model& m, std::string_view s)
    {
        for (std::size_t i = 0; i < s.size();)
        {
            auto const d = text::decode_utf8(s, i);
            press(m, s.substr(i, d.length));
            i += d.length;
        }
    }

    template <typename E>
    bool has_effect(std::vector<tm::Effect> const& fx)
    {
        return std::ranges::any_of(fx, [](tm::Effect const& e) { return std::holds_alternative<E>(e); });
    }

    template <typename E>
    E const* effect_of(std::vector<tm::Effect> const& fx)
    {
        for (auto const& e : fx)
            if (auto const* p = std::get_if<E>(&e))
                return p;
        return nullptr;
    }

    runner::CheckReport failing_report(std::string const& root)
    {
        runner::CheckReport r;
        r.exercise = "fundamentals/variables";
        r.workspace = root;
        r.backend = "dpm";
        r.passed = 2;
        r.total = 3;
        r.outcome = runner::Outcome::Failed;
        r.duration_ms = 310;
        runner::CheckResult build;
        build.name = "Compiles";
        build.kind = catalog::CheckKind::Build;
        build.verdict = runner::Verdict::Pass;
        runner::CheckResult compare;
        compare.name = "Prints the total";
        compare.kind = catalog::CheckKind::Run;
        compare.verdict = runner::Verdict::Fail;
        compare.stage = runner::Stage::Compare;
        compare.summary = "The program output differs from what the exercise expects.";
        compare.expected = "Total: 42\n";
        compare.observed = "Total: 38 \n";
        compare.expected_visible = text::visible_whitespace(compare.expected);
        compare.observed_visible = text::visible_whitespace(compare.observed);
        compare.hint = "Check which operator combines the price and the tax.";
        runner::CheckResult zero;
        zero.name = "Handles zero tax";
        zero.kind = catalog::CheckKind::Run;
        zero.verdict = runner::Verdict::Pass;
        r.results = {build, compare, zero};
        return r;
    }

    runner::CheckReport build_error_report(std::string const& root)
    {
        runner::CheckReport r;
        r.exercise = "fundamentals/variables";
        r.workspace = root;
        r.backend = "dpm";
        r.passed = 0;
        r.total = 3;
        r.outcome = runner::Outcome::Failed;
        r.duration_ms = 90;
        runner::CheckResult build;
        build.name = "Compiles";
        build.kind = catalog::CheckKind::Build;
        build.verdict = runner::Verdict::Fail;
        build.stage = runner::Stage::Build;
        build.summary = "The program does not compile.";
        build.diagnostics = "error: unknown name `totl`\n  --> " + root + "/src/main.dc:10:32\n   |\n10 |     io::println(\"Total: {}\", totl);\n   |                               ^^^^\n";
        runner::Location loc;
        loc.file = root + "/src/main.dc";
        loc.line = 10;
        loc.column = 32;
        loc.severity = "error";
        loc.message = "unknown name `totl`";
        build.locations.push_back(loc);
        runner::CheckResult second;
        second.name = "Prints the total";
        second.verdict = runner::Verdict::Skipped;
        second.summary = "not run because the build failed";
        runner::CheckResult third = second;
        third.name = "Handles zero tax";
        r.results = {build, second, third};
        return r;
    }

    doctor::DoctorReport fixed_doctor()
    {
        doctor::DoctorReport r;
        r.items.push_back({"dcc", "dcc", doctor::Status::Ok, "found next to learndc, version 0.6.0", ""});
        r.items.push_back({"dpm", "dpm", doctor::Status::Ok, "found next to learndc, version 0.6.0", ""});
        r.items.push_back({"dccd", "dccd", doctor::Status::Info, "not found (optional)", ""});
        r.items.push_back({"editor", "editor", doctor::Status::Warn, "no editor was found, so learndc shows file paths instead", "Install nvim, vim or nano, or set one in Settings."});
        r.items.push_back({"terminal", "terminal", doctor::Status::Ok, "interactive terminal, 100x30, UTF-8", ""});
        r.items.push_back({"safety", "safety", doctor::Status::Info, std::string(runner::safety_note_text), ""});
        return r;
    }

    std::filesystem::path snapshot_dir()
    {
        if (char const* dir = std::getenv("DCC_TEST_LEARNDC_SNAPSHOTS"); dir && *dir)
            return platform::path_from_utf8(dir);
        return std::filesystem::path(__FILE__).parent_path().parent_path() / "learndc" / "snapshots";
    }

    void check_snapshot(std::string const& name, std::string const& rendered, int width, int height)
    {
        auto const lines = text::split_lines(rendered);
        CHECK_EQ(static_cast<int>(lines.size()), height);
        for (auto const& line : lines)
            if (static_cast<int>(text::display_width(line)) > width)
            {
                CHECK(false);
                std::println(std::cerr, "      {}: line wider than {}: {}", name, width, line);
                break;
            }
        CHECK(rendered.find('\x1b') == std::string::npos);
        auto const file = snapshot_dir() / (name + ".txt");
        if (char const* update = std::getenv("LEARNDC_UPDATE_SNAPSHOTS"); update && *update && std::string_view(update) != "0")
        {
            std::filesystem::create_directories(file.parent_path());
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << rendered;
            return;
        }
        auto expected = platform::read_file(file);
        if (!expected)
        {
            std::println(std::cerr, "      missing snapshot {} (run with LEARNDC_UPDATE_SNAPSHOTS=1)", platform::path_to_utf8(file));
            CHECK(false);
            return;
        }
        if (*expected != rendered)
            std::println(std::cerr, "      snapshot {} differs\n--- expected\n{}--- actual\n{}", name, *expected, rendered);
        CHECK(*expected == rendered);
    }

    void snap(tm::Model& m, std::string const& base, std::vector<std::pair<int, int>> const& sizes)
    {
        for (auto const& [w, h] : sizes)
        {
            m.handle(tm::Resize{w, h});
            check_snapshot(std::format("{}-{}x{}", base, w, h), learndc::tui::render_snapshot(m, w, h), w, h);
        }
    }

    std::vector<std::pair<int, int>> const standard_sizes = {{100, 30}, {80, 24}, {60, 20}, {30, 10}};
} // namespace

TEST_CASE("tui layout modes follow the terminal size thresholds")
{
    CHECK(tm::layout_for({100, 30}) == tm::LayoutMode::Wide);
    CHECK(tm::layout_for({140, 50}) == tm::LayoutMode::Wide);
    CHECK(tm::layout_for({99, 30}) == tm::LayoutMode::Normal);
    CHECK(tm::layout_for({100, 29}) == tm::LayoutMode::Normal);
    CHECK(tm::layout_for({80, 24}) == tm::LayoutMode::Normal);
    CHECK(tm::layout_for({79, 24}) == tm::LayoutMode::Compact);
    CHECK(tm::layout_for({80, 23}) == tm::LayoutMode::Compact);
    CHECK(tm::layout_for({40, 12}) == tm::LayoutMode::Compact);
    CHECK(tm::layout_for({39, 12}) == tm::LayoutMode::TooSmall);
    CHECK(tm::layout_for({40, 11}) == tm::LayoutMode::TooSmall);
    CHECK(tm::layout_for({200, 8}) == tm::LayoutMode::TooSmall);
}

TEST_CASE("tui starts on onboarding, the active exercise or home")
{
    {
        TuiWorld w(false, [](settings::Settings& s) { s.onboarding_done = false; });
        tm::Model m(*w.ctx, {100, 30});
        CHECK(m.screen == tm::Screen::Onboarding);
    }
    {
        TuiWorld w(true, [](settings::Settings& s) { s.ui_open_continue = true; });
        tm::Model m(*w.ctx, {100, 30});
        CHECK(m.screen == tm::Screen::Exercise);
        REQUIRE(m.exercise.ex != nullptr);
        CHECK_EQ(m.exercise.ex->id, "fundamentals/variables");
        press(m, "Esc");
        CHECK(m.screen == tm::Screen::Home);
    }
    {
        TuiWorld w;
        tm::Model m(*w.ctx, {100, 30});
        CHECK(m.screen == tm::Screen::Home);
    }
}

TEST_CASE("tui onboarding saves choices, can be skipped and then routes home")
{
    TuiWorld w(false, [](settings::Settings& s) { s.onboarding_done = false; });
    tm::Model m(*w.ctx, {100, 30});
    REQUIRE(m.screen == tm::Screen::Onboarding);
    press(m, "Down");
    press(m, "Enter");
    CHECK_EQ(w.ctx->settings.learner_level, "experienced");
    CHECK_EQ(m.onboarding.step, 1);
    press(m, "s");
    CHECK_EQ(m.onboarding.step, 2);
    press(m, "Down");
    press(m, "Enter");
    CHECK_EQ(w.ctx->settings.ui_theme, "light");
    CHECK(w.ctx->settings.onboarding_done);
    CHECK(m.screen == tm::Screen::Home);
    auto saved = settings::load(w.home / "config.json");
    REQUIRE(saved.has_value());
    CHECK(saved->onboarding_done);
    CHECK_EQ(saved->learner_level, "experienced");

    TuiWorld skipped(false, [](settings::Settings& s) { s.onboarding_done = false; });
    tm::Model m2(*skipped.ctx, {100, 30});
    press(m2, "Esc");
    CHECK(m2.screen == tm::Screen::Home);
    CHECK(skipped.ctx->settings.onboarding_done);
}

TEST_CASE("tui navigation uses the number keys and an escape stack")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "2");
    CHECK(m.screen == tm::Screen::Learn);
    press(m, "4");
    CHECK(m.screen == tm::Screen::Progress);
    press(m, "5");
    CHECK(m.screen == tm::Screen::Bookmarks);
    press(m, "6");
    CHECK(m.screen == tm::Screen::Doctor);
    press(m, "7");
    CHECK(m.screen == tm::Screen::Settings);
    press(m, "1");
    CHECK(m.screen == tm::Screen::Home);
    press(m, "3");
    CHECK(m.screen == tm::Screen::Exercise);
    REQUIRE(m.exercise.ex != nullptr);
    press(m, "?");
    CHECK(m.screen == tm::Screen::Help);
    CHECK(m.help_from == tm::Screen::Exercise);
    press(m, "Esc");
    CHECK(m.screen == tm::Screen::Exercise);
    press(m, "Esc");
    CHECK(m.screen == tm::Screen::Home);
    press(m, "Esc");
    CHECK(m.screen == tm::Screen::Home);
}

TEST_CASE("tui keeps list focus across screens filters and resizes")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "2");
    press(m, "Down");
    press(m, "Down");
    auto const picked = m.learn_id;
    CHECK(!picked.empty());
    press(m, "Enter");
    CHECK(m.screen == tm::Screen::Exercise);
    CHECK_EQ(m.exercise.ex->id, picked);
    press(m, "Esc");
    CHECK(m.screen == tm::Screen::Learn);
    CHECK_EQ(m.learn_id, picked);
    m.handle(tm::Resize{60, 20});
    CHECK_EQ(m.learn_id, picked);
    m.handle(tm::Resize{140, 40});
    CHECK_EQ(m.learn_id, picked);
    CHECK(m.layout() == tm::LayoutMode::Wide);
    press(m, "f");
    CHECK(m.filters.status == tm::StatusFilter::NotStarted);
    auto const list = m.learn_list();
    CHECK(!list.empty());
    CHECK(std::ranges::all_of(list, [&](auto const* e) { return m.status_of(e->id) == progress::Status::NotStarted; }));
    CHECK(std::ranges::any_of(list, [&](auto const* e) { return e->id == m.learn_id; }));
    press(m, "0");
    CHECK(!m.filters.any());
    press(m, "1");
    press(m, "2");
    CHECK(m.screen == tm::Screen::Learn);
}

TEST_CASE("tui search captures keys until escape or enter")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "2");
    auto const before = m.learn_list().size();
    auto fx = press(m, "/");
    CHECK(m.searching);
    fx = press(m, "q");
    CHECK(m.screen == tm::Screen::Learn);
    CHECK(!has_effect<tm::Quit>(fx));
    CHECK_EQ(m.query, "q");
    press(m, "Backspace");
    type_text(m, "struct");
    CHECK(m.searching);
    CHECK_EQ(m.query, "struct");
    auto const list = m.learn_list();
    REQUIRE(!list.empty());
    CHECK_EQ(list.front()->id, "types/structs");
    CHECK_EQ(m.learn_id, "types/structs");
    press(m, "5");
    CHECK(m.screen == tm::Screen::Learn);
    CHECK_EQ(m.query, "struct5");
    press(m, "Backspace");
    press(m, "Enter");
    CHECK(!m.searching);
    CHECK_EQ(m.query, "struct");
    fx = press(m, "q");
    CHECK(has_effect<tm::Quit>(fx));
    TuiWorld w2;
    tm::Model m2(*w2.ctx, {100, 30});
    press(m2, "2");
    press(m2, "/");
    type_text(m2, "zzzz");
    CHECK(m2.learn_list().empty());
    press(m2, "Esc");
    CHECK(!m2.searching);
    CHECK(m2.query.empty());
    CHECK_EQ(m2.learn_list().size(), before);
    CHECK(m2.screen == tm::Screen::Learn);
}

TEST_CASE("tui exercise keys emit effects for edit check cancel run and watch")
{
    TuiWorld w(true, [](settings::Settings& s) { s.editor_preset = "nvim"; });
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    REQUIRE(m.screen == tm::Screen::Exercise);
    REQUIRE(m.exercise.ws.has_value());

    auto fx = press(m, "e");
    auto const* edit = effect_of<tm::LaunchEditor>(fx);
    REQUIRE(edit != nullptr);
    CHECK(edit->file.ends_with("src/main.dc"));
    CHECK(!edit->folder);
    CHECK(edit->choice.kind == editor::Kind::Terminal);

    fx = press(m, "o");
    CHECK(!has_effect<tm::LaunchEditor>(fx));
    CHECK(!m.toasts.empty());

    fx = press(m, "c");
    auto const* check = effect_of<tm::StartCheck>(fx);
    REQUIRE(check != nullptr);
    CHECK_EQ(check->id, "fundamentals/variables");
    CHECK(m.exercise.phase == tm::CheckPhase::Running);
    fx = press(m, "c");
    CHECK(fx.empty());
    fx = press(m, "r");
    CHECK(fx.empty());
    m.handle(tm::CheckStage{"Building"});
    CHECK_EQ(m.exercise.stage_text, "Building");
    fx = press(m, "x");
    CHECK(has_effect<tm::CancelCheck>(fx));
    CHECK(m.exercise.phase == tm::CheckPhase::Cancelling);
    runner::CheckReport cancelled;
    cancelled.exercise = "fundamentals/variables";
    cancelled.outcome = runner::Outcome::Cancelled;
    cancelled.total = 3;
    fx = m.handle(tm::CheckDone{cancelled});
    CHECK(m.exercise.phase == tm::CheckPhase::Idle);
    CHECK(fx.empty());

    fx = press(m, "r");
    CHECK(has_effect<tm::RunProgram>(fx));

    fx = press(m, "w");
    CHECK(has_effect<tm::StartWatch>(fx));
    CHECK(m.exercise.watching);
    fx = press(m, "w");
    CHECK(has_effect<tm::StopWatch>(fx));
    CHECK(!m.exercise.watching);

    fx = press(m, "c");
    REQUIRE(has_effect<tm::StartCheck>(fx));
    fx = press(m, "CtrlC");
    CHECK(has_effect<tm::CancelCheck>(fx));
    CHECK(!has_effect<tm::Quit>(fx));
    CHECK(!m.quit_requested);
    fx = press(m, "CtrlC");
    CHECK(has_effect<tm::Quit>(fx));
    CHECK(m.quit_requested);
}

TEST_CASE("tui ctrl c and q behave when no check is running or one is")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    auto fx = press(m, "CtrlC");
    CHECK(has_effect<tm::Quit>(fx));

    tm::Model running(*w.ctx, {100, 30});
    press(running, "3");
    press(running, "c");
    REQUIRE(running.exercise.phase == tm::CheckPhase::Running);
    fx = press(running, "q");
    CHECK(!has_effect<tm::Quit>(fx));
    CHECK(running.dialog.kind == tm::DialogKind::QuitRunning);
    fx = press(running, "n");
    CHECK(!running.dialog.open());
    CHECK(!has_effect<tm::Quit>(fx));
    fx = press(running, "q");
    fx = press(running, "y");
    CHECK(has_effect<tm::CancelCheck>(fx));
    CHECK(has_effect<tm::Quit>(fx));
}

TEST_CASE("tui marks results stale when files change and coalesces watch reruns")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    press(m, "c");
    auto const root = m.exercise.ws->root;
    auto fx = m.handle(tm::CheckDone{failing_report(platform::path_to_utf8(root))});
    CHECK(m.exercise.report.has_value());
    CHECK(!m.exercise.stale);
    CHECK(m.exercise.phase == tm::CheckPhase::Idle);
    {
        auto p = learndc::app::load_progress(*w.ctx);
        REQUIRE(p.has_value());
        CHECK_EQ(p->exercises.at("fundamentals/variables").attempts, 3);
        CHECK_EQ(p->exercises.at("fundamentals/variables").last_passed, 2);
    }
    fx = m.handle(tm::FilesChanged{});
    CHECK(m.exercise.stale);
    CHECK(fx.empty());

    press(m, "w");
    fx = m.handle(tm::FilesChanged{});
    auto const* start = effect_of<tm::StartCheck>(fx);
    REQUIRE(start != nullptr);
    CHECK(m.exercise.phase == tm::CheckPhase::Running);
    fx = m.handle(tm::FilesChanged{});
    CHECK(fx.empty());
    fx = m.handle(tm::FilesChanged{});
    CHECK(fx.empty());
    CHECK(m.exercise.rerun_pending);
    fx = m.handle(tm::CheckDone{failing_report(platform::path_to_utf8(root))});
    CHECK(has_effect<tm::StartCheck>(fx));
    CHECK(m.exercise.phase == tm::CheckPhase::Running);
    CHECK(m.exercise.stale);
    fx = m.handle(tm::CheckDone{failing_report(platform::path_to_utf8(root))});
    CHECK(fx.empty());
    CHECK(!m.exercise.stale);
    fx = press(m, "Esc");
    CHECK(has_effect<tm::StopWatch>(fx));
    CHECK(!m.exercise.watching);
}

TEST_CASE("tui detects edits made by an external editor")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    press(m, "c");
    auto const root = m.exercise.ws->root;
    m.handle(tm::CheckDone{failing_report(platform::path_to_utf8(root))});
    CHECK(!m.exercise.stale);
    m.handle(tm::EditorDone{true, ""});
    CHECK(!m.exercise.stale);
    write_file(root / "src" / "main.dc", "module main;\n// changed\n");
    m.handle(tm::EditorDone{false, "the editor exited with code 1"});
    CHECK(m.exercise.stale);
    REQUIRE(!m.toasts.empty());
    CHECK(m.toasts.front().error);
    CHECK(m.toasts.front().text.find("code 1") != std::string::npos);
    bool found = false;
    for (auto const& line : m.exercise.preview_lines)
        found = found || line == "// changed";
    CHECK(found);
}

TEST_CASE("tui completing an exercise records progress and offers the next one")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    press(m, "c");
    runner::CheckReport pass;
    pass.exercise = "fundamentals/variables";
    pass.outcome = runner::Outcome::Passed;
    pass.passed = 3;
    pass.total = 3;
    pass.duration_ms = 420;
    m.handle(tm::CheckDone{pass});
    CHECK(m.status_of("fundamentals/variables") == progress::Status::Completed);
    REQUIRE(!m.toasts.empty());
    CHECK(m.toasts.front().text.find("completed") != std::string::npos);
    auto fx = press(m, "n");
    CHECK(fx.empty());
    CHECK(m.exercise.ex->id != "fundamentals/variables");
}

TEST_CASE("tui hints persist and the solution is shown after a confirmation without touching files")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    auto const before = w.exercise_file("fundamentals/variables", "src/main.dc");
    press(m, "h");
    CHECK_EQ(m.exercise.hints_shown, 1);
    press(m, "h");
    CHECK_EQ(m.exercise.hints_shown, 2);
    {
        auto p = learndc::app::load_progress(*w.ctx);
        REQUIRE(p.has_value());
        CHECK_EQ(p->exercises.at("fundamentals/variables").hint_level, 2);
        CHECK(!p->exercises.at("fundamentals/variables").solution_viewed);
    }
    press(m, "s");
    CHECK(m.dialog.kind == tm::DialogKind::Solution);
    CHECK(!m.exercise.solution.has_value());
    press(m, "n");
    CHECK(!m.dialog.open());
    CHECK(!m.exercise.solution.has_value());
    press(m, "s");
    press(m, "y");
    REQUIRE(m.exercise.solution.has_value());
    REQUIRE(m.exercise.solution->files.size() == 1);
    CHECK(!m.exercise.solution->files[0].rows.empty());
    CHECK(m.exercise.solution->files[0].unified.find("+    i32 total = price + tax;") != std::string::npos);
    CHECK(m.exercise.solution->explanation.find("Adding the tax") != std::string::npos);
    CHECK_EQ(w.exercise_file("fundamentals/variables", "src/main.dc"), before);
    {
        auto p = learndc::app::load_progress(*w.ctx);
        REQUIRE(p.has_value());
        CHECK(p->exercises.at("fundamentals/variables").solution_viewed);
    }
    press(m, "Esc");
    CHECK(!m.exercise.solution.has_value());
    CHECK(m.screen == tm::Screen::Exercise);
}

TEST_CASE("tui reset needs a typed y and always makes a backup")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    auto const root = m.exercise.ws->root;
    write_file(root / "src" / "main.dc", "module main;\n// my work\n");
    press(m, "R");
    CHECK(m.dialog.kind == tm::DialogKind::Reset);
    press(m, "Enter");
    CHECK(m.dialog.open());
    CHECK_EQ(w.exercise_file("fundamentals/variables", "src/main.dc"), "module main;\n// my work\n");
    press(m, "x");
    press(m, "Enter");
    CHECK(m.dialog.open());
    press(m, "Backspace");
    press(m, "y");
    CHECK_EQ(m.dialog.input, "y");
    press(m, "Enter");
    CHECK(!m.dialog.open());
    CHECK_EQ(w.exercise_file("fundamentals/variables", "src/main.dc"), std::string(tui_main_source));
    REQUIRE(!m.toasts.empty());
    auto const& message = m.toasts.back().text;
    CHECK(message.find("saved in") != std::string::npos);
    CHECK(std::filesystem::exists(w.ctx->dirs.backups_dir));
    bool backed_up = false;
    std::error_code ec;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(w.ctx->dirs.backups_dir, ec))
        if (entry.is_regular_file() && platform::read_file(entry.path()).value_or("") == "module main;\n// my work\n")
            backed_up = true;
    CHECK(backed_up);
    press(m, "R");
    press(m, "Esc");
    CHECK(!m.dialog.open());
}

TEST_CASE("tui settings are changed through validated selection lists")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "7");
    auto rows = m.settings_rows();
    std::size_t theme_row = 0;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].key == "ui.theme")
            theme_row = i;
    for (std::size_t i = 0; i < theme_row; ++i)
        press(m, "Down");
    press(m, "Enter");
    REQUIRE(m.dialog.kind == tm::DialogKind::Choice);
    CHECK_EQ(m.dialog.target, "ui.theme");
    CHECK(std::ranges::find(m.dialog.values, "light") != m.dialog.values.end());
    while (m.dialog.values[m.dialog.selected] != "light")
        press(m, "Down");
    press(m, "Enter");
    CHECK(!m.dialog.open());
    CHECK_EQ(w.ctx->settings.ui_theme, "light");
    CHECK(m.appearance.theme == tm::ThemeId::Light);
    auto saved = settings::load(w.home / "config.json");
    REQUIRE(saved.has_value());
    CHECK_EQ(saved->ui_theme, "light");

    while (m.settings_rows()[m.settings_sel].key != "checks.timeout_seconds")
        press(m, "Down");
    press(m, "Enter");
    REQUIRE(m.dialog.kind == tm::DialogKind::Choice);
    CHECK(std::ranges::find(m.dialog.values, "30") != m.dialog.values.end());
    while (m.dialog.values[m.dialog.selected] != "30")
        press(m, "Down");
    press(m, "Enter");
    CHECK_EQ(w.ctx->settings.checks_timeout_seconds, 30);

    while (m.settings_rows()[m.settings_sel].key != "editor.command")
        press(m, "Up");
    press(m, "Enter");
    CHECK(!m.dialog.open());
    CHECK(!m.toasts.empty());
}

TEST_CASE("tui honors NO_COLOR and the locale for appearance")
{
    TuiWorld plain(false, {}, platform::Env{{"PATH", ""}, {"LANG", "C"}, {"NO_COLOR", "1"}});
    tm::Model m(*plain.ctx, {100, 30});
    CHECK(m.appearance.theme == tm::ThemeId::NoColor);
    TuiWorld ascii(false, [](settings::Settings& s) { s.ui_ascii = "auto"; }, platform::Env{{"PATH", ""}, {"LANG", "C"}});
    tm::Model m2(*ascii.ctx, {100, 30});
    CHECK(m2.appearance.ascii);
    TuiWorld utf(false, [](settings::Settings& s) { s.ui_ascii = "auto"; });
    tm::Model m3(*utf.ctx, {100, 30});
    CHECK(!m3.appearance.ascii);
}

TEST_CASE("tui exercise needs files before checking and starts the workspace on enter")
{
    TuiWorld w(false);
    tm::Model m(*w.ctx, {100, 30});
    press(m, "2");
    press(m, "Enter");
    REQUIRE(m.screen == tm::Screen::Exercise);
    CHECK(!m.exercise.ws.has_value());
    auto fx = press(m, "c");
    CHECK(fx.empty());
    CHECK(!m.toasts.empty());
    press(m, "Enter");
    REQUIRE(m.exercise.ws.has_value());
    CHECK(!m.exercise.files.empty());
    CHECK(m.status_of(m.exercise.ex->id) == progress::Status::InProgress);
    fx = press(m, "c");
    CHECK(has_effect<tm::StartCheck>(fx));
}

TEST_CASE("tui manual editors show the path instead of launching")
{
    TuiWorld w(true, [](settings::Settings& s) { s.editor_preset = "manual"; });
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    auto fx = press(m, "e");
    CHECK(!has_effect<tm::LaunchEditor>(fx));
    REQUIRE(!m.toasts.empty());
    CHECK(m.toasts.front().text.find("src/main.dc") != std::string::npos);
}

TEST_CASE("tui renders untrusted text safely and within the terminal width")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {80, 24});
    press(m, "3");
    auto report = failing_report("/work/variables");
    report.results[1].observed = "\x1b[31mred\x1b[0m \x1b]0;title\x07 bell\x07 \xe6\xbc\xa2\xe5\xad\x97\xe6\xbc\xa2\xe5\xad\x97\xe6\xbc\xa2\xe5\xad\x97\xe6\xbc\xa2\xe5\xad\x97\xe6\xbc\xa2\xe5\xad\x97\xe6\xbc\xa2\xe5\xad\x97\xe6\xbc\xa2\xe5\xad\x97\xe6\xbc\xa2\xe5\xad\x97\n";
    report.results[1].observed_visible = report.results[1].observed;
    report.results[1].diagnostics = "\x1b[1;31merror\x1b[0m: boom \x1b[2J\x1b[H\n";
    m.handle(tm::CheckDone{report});
    for (auto const& size : std::vector<std::pair<int, int>>{{100, 30}, {80, 24}, {60, 20}, {44, 13}})
    {
        m.handle(tm::Resize{size.first, size.second});
        auto const out = learndc::tui::render_snapshot(m, size.first, size.second);
        CHECK(out.find('\x1b') == std::string::npos);
        for (auto const& line : text::split_lines(out))
            CHECK(static_cast<int>(text::display_width(line)) <= size.first);
        CHECK_EQ(static_cast<int>(text::split_lines(out).size()), size.second);
    }
    m.handle(tm::Resize{100, 30});
    auto const wide = learndc::tui::render_snapshot(m, 100, 30);
    CHECK(wide.find("red") != std::string::npos);
    CHECK(wide.find("\xe6\xbc\xa2") != std::string::npos);
}

TEST_CASE("tui snapshots of home")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    snap(m, "home", standard_sizes);
}

TEST_CASE("tui snapshots of learn")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "2");
    press(m, "Down");
    press(m, "Down");
    snap(m, "learn", standard_sizes);
    press(m, "/");
    type_text(m, "struct");
    snap(m, "learn-search", {{100, 30}, {60, 20}});
}

TEST_CASE("tui snapshots of the exercise screen")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    snap(m, "exercise-fresh", {{100, 30}, {80, 24}, {60, 20}});
    press(m, "c");
    m.handle(tm::CheckDone{failing_report("/work/variables")});
    snap(m, "exercise", standard_sizes);
    press(m, "Tab");
    snap(m, "exercise-files", {{100, 30}, {80, 24}, {60, 20}});
    press(m, "Tab");
    snap(m, "exercise-results", {{100, 30}, {80, 24}, {60, 20}});
    press(m, "Tab");
    press(m, "h");
    press(m, "h");
    snap(m, "exercise-hints", {{100, 30}, {80, 24}});
}

TEST_CASE("tui snapshots of check states")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    std::string const root = "/work/variables";
    m.handle(tm::Tick{1000});
    press(m, "c");
    m.handle(tm::Tick{4600});
    m.handle(tm::CheckStage{"Running the program"});
    snap(m, "exercise-running", {{100, 30}, {80, 24}});
    m.handle(tm::CheckDone{build_error_report(root)});
    press(m, "Tab");
    press(m, "Tab");
    snap(m, "exercise-build-error", {{100, 30}, {80, 24}, {60, 20}});
    press(m, "l");
    snap(m, "exercise-raw-log", {{100, 30}});
    m.handle(tm::FilesChanged{});
    snap(m, "exercise-stale", {{100, 30}});
}

TEST_CASE("tui snapshots of toasts")
{
    TuiWorld w(false);
    tm::Model m(*w.ctx, {100, 30});
    press(m, "2");
    press(m, "Enter");
    press(m, "Enter");
    snap(m, "toast-started", {{100, 30}, {60, 20}});
    press(m, "c");
    press(m, "x");
    snap(m, "toast-cancelling", {{100, 30}});
    m.handle(tm::Tick{9000});
    CHECK_EQ(m.toasts.size(), 1u);
    m.handle(tm::Tick{20000});
    CHECK(m.toasts.empty());
}

TEST_CASE("tui snapshots of dialogs and the solution")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    m.handle(tm::CheckDone{failing_report("/work/variables")});
    press(m, "s");
    snap(m, "dialog-solution", {{100, 30}, {60, 20}});
    press(m, "y");
    snap(m, "solution", {{140, 40}, {100, 30}, {80, 24}, {60, 20}});
    press(m, "Esc");
    press(m, "R");
    type_text(m, "y");
    snap(m, "dialog-reset", {{100, 30}});
}

TEST_CASE("tui snapshots of help doctor progress bookmarks and settings")
{
    TuiWorld w;
    tm::Model m(*w.ctx, {100, 30});
    press(m, "3");
    press(m, "?");
    snap(m, "help", standard_sizes);
    press(m, "Esc");
    press(m, "6");
    m.handle(tm::DoctorDone{fixed_doctor()});
    snap(m, "doctor", standard_sizes);
    press(m, "4");
    snap(m, "progress", {{100, 30}, {80, 24}, {60, 20}});
    press(m, "5");
    snap(m, "bookmarks", {{100, 30}, {60, 20}});
    press(m, "7");
    snap(m, "settings", {{100, 30}, {80, 24}, {60, 20}});
    press(m, "Enter");
    snap(m, "settings-dialog", {{100, 30}});
}

TEST_CASE("tui snapshots of onboarding")
{
    TuiWorld w(false, [](settings::Settings& s) { s.onboarding_done = false; });
    tm::Model m(*w.ctx, {100, 30});
    snap(m, "onboarding", {{100, 30}, {60, 20}});
}

TEST_CASE("tui snapshots of the ascii and no color variants")
{
    TuiWorld w(true, [](settings::Settings& s) { s.ui_ascii = "on"; s.ui_theme = "no-color"; });
    tm::Model m(*w.ctx, {100, 30});
    snap(m, "ascii-home", {{100, 30}, {60, 20}});
    press(m, "2");
    snap(m, "ascii-learn", {{100, 30}});
    press(m, "3");
    m.handle(tm::CheckDone{failing_report("/work/variables")});
    snap(m, "ascii-exercise", {{100, 30}, {80, 24}, {60, 20}});
    auto const colored = learndc::tui::render_snapshot(m, 100, 30, true);
    CHECK(colored.find("\x1b[3") == std::string::npos);
    CHECK(colored.find("\x1b[38") == std::string::npos);
    TuiWorld c(true, [](settings::Settings& s) { s.ui_theme = "dark"; });
    tm::Model m2(*c.ctx, {100, 30});
    CHECK(learndc::tui::render_snapshot(m2, 100, 30, true).find("\x1b[") != std::string::npos);
}

int main(int argc, char** argv)
{
    g_self = platform::path_to_utf8(platform::executable_path().value_or(std::filesystem::path(argv[0])));
    if (argc > 1)
    {
        std::string_view const mode{argv[1]};
        auto const stem = std::filesystem::path(argv[0]).filename().string();
        if (mode == "build")
        {
            if (char const* fake = std::getenv("LEARNDC_TEST_FAKE_DPM"))
            {
                std::string_view const kind{fake};
                if (kind == "abort")
                    std::abort();
                if (kind == "sleep")
                    std::this_thread::sleep_for(std::chrono::seconds(60));
                if (kind == "brief")
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                    return 1;
                }
                if (kind == "reject")
                {
                    std::cerr << "error: initializer type mismatch\n   | \n 4 | println(\"undefined symbol\")\n";
                    return 1;
                }
                return 0;
            }
        }
        if (mode == "--version")
        {
            if (stem.starts_with("hang"))
                std::this_thread::sleep_for(std::chrono::seconds(20));
            if (stem.starts_with("fail"))
                return 3;
            if (stem.starts_with("silent"))
                return 0;
            std::println("fakedcc 1.2.3 (test)");
            return 0;
        }
        if (mode == "--emit" && argc == 3)
        {
            auto const hex = std::string_view(argv[2]);
            std::string bytes;
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
                bytes += static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16));
            std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            std::cout.flush();
            return 0;
        }
        if (mode == "--emit-err" && argc == 3)
        {
            auto const hex = std::string_view(argv[2]);
            std::string bytes;
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
                bytes += static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16));
            std::cerr.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            std::cerr.flush();
            return 0;
        }
        if (mode == "--flood")
        {
            std::string const chunk(8192, 'x');
            for (int i = 0; i < 4096; ++i)
            {
                std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                std::cout.flush();
            }
            return 0;
        }
        if (mode == "--sleep")
        {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return 0;
        }
        if (mode == "--exit" && argc == 3)
            return std::stoi(argv[2]);
        if (mode == "--abort")
            std::abort();
        if (mode == "--segv")
        {
            *static_cast<volatile int*>(nullptr) = 1;
            return 0;
        }
        if (mode == "--echo-stdin")
        {
            std::string all{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
            std::cout << all;
            std::cout.flush();
            return 0;
        }
        if (mode == "--fake-editor" && argc >= 3)
        {
            std::filesystem::path const file = platform::path_from_utf8(argv[2]);
            {
                std::ofstream out(file, std::ios::binary | std::ios::app);
                out << "edited\n";
            }
            std::ofstream dump(platform::path_from_utf8(std::string(argv[2]) + ".argv"), std::ios::binary | std::ios::trunc);
            for (int i = 2; i < argc; ++i)
                dump << argv[i] << "\n";
            return 0;
        }
        if (mode == "--progress-worker" && argc == 6)
        {
            progress::ProgressStore store(argv[2], argv[3]);
            int const n = std::stoi(argv[5]);
            for (int i = 0; i < n; ++i)
            {
                auto r = store.update([&](progress::Progress& p) {
                    ++p.exercises["shared/counter"].attempts;
                    p.bookmarks.push_back(std::format("{}/{}", argv[4], i));
                });
                if (!r)
                    return 1;
            }
            return 0;
        }
        return 2;
    }
    if (char const* only = std::getenv("LEARNDC_TEST_ONLY"); only && *only)
        std::erase_if(test::registry(), [&](test::Case const& c) { return !c.name.contains(std::string_view(only)); });
    return test::run();
}
