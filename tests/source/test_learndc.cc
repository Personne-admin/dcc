import std;
import learndc.text;
import learndc.json;
import learndc.toml;
import learndc.platform;
import learndc.catalog;
import learndc.bundle;
import learndc.diff;

#include "harness.hh"

namespace text = learndc::text;
namespace json = learndc::json;
namespace toml = learndc::toml;
namespace platform = learndc::platform;
namespace catalog = learndc::catalog;
namespace bundle = learndc::bundle;
namespace diff = learndc::diff;

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
