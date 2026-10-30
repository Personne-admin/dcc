import std;
import dpm.cache;
import dpm.hash;
import dpm.stamp;

#include "harness.hh"

namespace
{
    namespace st = dpm::stamp;
    namespace fs = std::filesystem;
    using Clock = fs::file_time_type::clock;

    struct TempDir
    {
        fs::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("dpm_stamp_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
            fs::create_directories(path);
        }

        ~TempDir() { dpm::cache::detail::remove_tree(path); }

        fs::path write(std::string const& rel, std::string const& content, std::chrono::hours age = std::chrono::hours{100}) const
        {
            auto full = path / rel;
            fs::create_directories(full.parent_path());
            {
                std::ofstream f{full, std::ios::binary};
                f << content;
            }
            fs::last_write_time(full, Clock::now() - age);
            return full;
        }
    };

    std::string key(std::string_view tag = "a")
    {
        std::vector<std::string> argv{"dcc", "x.dc", std::string{tag}};
        return st::make_key(argv, "dcc 1", "sha256:" + std::string(64, '0'));
    }
} // namespace

SECTION("dpm stamps");

TEST_CASE("depfiles are read with Make escapes and the target is skipped")
{
    auto paths = st::parse_depfile("out/app: /a/main.dc /b/with\\ space.dc /c/h\\#sh.dc /d/co\\:lon.dc /e/dollar$$.dc /f/100\\%.dc\n");
    std::vector<fs::path> expected{"/a/main.dc", "/b/with space.dc", "/c/h#sh.dc", "/d/co:lon.dc", "/e/dollar$.dc", "/f/100%.dc"};
    CHECK(paths == expected);
    auto escaped_target = st::parse_depfile("out/my\\ app: /a.dc\n");
    CHECK(escaped_target == std::vector<fs::path>{"/a.dc"});
    CHECK(st::parse_depfile("out/app:\n").empty());
    CHECK(st::parse_depfile("").empty());
    auto continued = st::parse_depfile("out: /a.dc \\\n /b.dc\n");
    CHECK(continued == (std::vector<fs::path>{"/a.dc", "/b.dc"}));
    auto tabs = st::parse_depfile("out:\t/a.dc\t/b.dc");
    CHECK_EQ(tabs.size(), std::size_t{2});
}

TEST_CASE("the key depends on every part and cannot be confused by splitting arguments")
{
    std::vector<std::string> a{"dcc", "a b"};
    std::vector<std::string> b{"dcc", "a", "b"};
    std::vector<std::string> c{"dcc", "a b", ""};
    auto fp = std::string{"sha256:"} + std::string(64, '1');
    auto base = st::make_key(a, "v1", fp);
    CHECK(dpm::hash::is_hash_string(base));
    CHECK_EQ(base, st::make_key(a, "v1", fp));
    CHECK_NE(base, st::make_key(b, "v1", fp));
    CHECK_NE(base, st::make_key(c, "v1", fp));
    CHECK_NE(base, st::make_key(a, "v2", fp));
    CHECK_NE(base, st::make_key(a, "v1", "sha256:" + std::string(64, '2')));
}

TEST_CASE("a stamp survives printing and parsing, and damaged stamps are rejected")
{
    TempDir td;
    auto output = td.write("out/app", "binary");
    auto spaced = td.write("src/with space.dc", "module a;\n");
    auto plain = td.write("src/b.dc", "module b;\n");
    std::vector<fs::path> inputs{spaced, plain, plain};
    auto stamp = st::record(key(), output, inputs, Clock::now());
    REQUIRE(stamp.has_value());
    CHECK_EQ(stamp->inputs.size(), std::size_t{2});
    auto text = st::print(*stamp);
    auto back = st::parse(text);
    REQUIRE(back.has_value());
    CHECK_EQ(st::print(*back), text);
    CHECK_EQ(back->inputs[0].path, spaced);
    CHECK(st::write(td.path / "app.stamp", *stamp));
    auto from_disk = st::read(td.path / "app.stamp");
    REQUIRE(from_disk.has_value());
    CHECK_EQ(st::print(*from_disk), text);
    CHECK(!st::read(td.path / "missing.stamp").has_value());

    CHECK(!st::parse("").has_value());
    CHECK(!st::parse("dpm-stamp 2\n").has_value());
    CHECK(!st::parse(text.substr(0, text.size() - 1)).has_value());
    CHECK(!st::parse(text + "junk\n").has_value());
    auto bad_key = text;
    bad_key.replace(bad_key.find("sha256:"), 12, "sha256:zzzzz");
    CHECK(!st::parse(bad_key).has_value());
    for (std::string_view broken : {"input x 1 0 aa p\n", "input 1 1 2 aa p\n", "input 1 1 0 aa\n", "input 1 1\n"})
        CHECK(!st::parse(std::string{"dpm-stamp 1\nkey "} + key() + "\noutput 1 1\n" + std::string{broken}).has_value());
    CHECK(!st::parse(std::string{"dpm-stamp 1\nkey "} + key() + "\noutput x 1\n").has_value());
}

TEST_CASE("recording needs every input and skips paths that cannot be stored")
{
    TempDir td;
    auto output = td.write("out/app", "binary");
    auto input = td.write("a.dc", "a");
    std::vector<fs::path> missing{input, td.path / "gone.dc"};
    CHECK(!st::record(key(), output, missing, Clock::now()).has_value());
    CHECK(!st::record(key(), td.path / "no-output", std::vector<fs::path>{input}, Clock::now()).has_value());
    CHECK(!st::record(key(), output, std::vector<fs::path>{td.path}, Clock::now()).has_value());
    std::vector<fs::path> with_newline{input, fs::path{(td.path / "new\nline.dc").string()}};
    auto stamp = st::record(key(), output, with_newline, Clock::now());
    REQUIRE(stamp.has_value());
    CHECK_EQ(stamp->inputs.size(), std::size_t{1});
}

TEST_CASE("verification trusts size and mtime, then the content hash, and notices everything else")
{
    TempDir td;
    auto output = td.write("out/app", "binary");
    auto source = td.write("main.dc", "module main; // v1\n");
    auto library = td.write("lib.dc", "module lib;\n");
    std::vector<fs::path> inputs{source, library};
    auto fresh = [&]
    {
        auto s = st::record(key(), output, inputs, Clock::now());
        CHECK(s.has_value());
        return s.value_or(st::Stamp{});
    };

    {
        auto s = fresh();
        CHECK(!s.inputs[0].racy);
        auto v = st::verify(s, key(), output, Clock::now());
        CHECK(v.state == st::State::Fresh);
        CHECK(!v.refreshed);
    }
    {
        auto s = fresh();
        CHECK(st::verify(s, key("b"), output, Clock::now()).state == st::State::KeyChanged);
    }
    {
        auto s = fresh();
        fs::last_write_time(source, Clock::now());
        auto v = st::verify(s, key(), output, Clock::now());
        CHECK(v.state == st::State::Fresh);
        CHECK(v.refreshed);
        CHECK(s.inputs[0].racy);
        CHECK(st::verify(s, key(), output, Clock::now()).state == st::State::Fresh);
        fs::last_write_time(source, Clock::now() - std::chrono::hours{50});
    }
    {
        auto s = fresh();
        fs::last_write_time(source, Clock::now() - std::chrono::hours{1});
        td.write("main.dc", "module main; // v2\n", std::chrono::hours{1});
        auto v = st::verify(s, key(), output, Clock::now());
        CHECK(v.state == st::State::InputChanged);
        CHECK(v.detail.find("main.dc") != std::string::npos);
        td.write("main.dc", "module main; // v1\n");
    }
    {
        auto s = fresh();
        td.write("main.dc", "module main; // v1 longer\n");
        CHECK(st::verify(s, key(), output, Clock::now()).state == st::State::InputChanged);
        td.write("main.dc", "module main; // v1\n");
    }
    {
        auto s = fresh();
        auto mtime = fs::last_write_time(source);
        td.write("main.dc", "module main; // v2\n");
        fs::last_write_time(source, mtime);
        auto v = st::verify(s, key(), output, Clock::now());
        CHECK(v.state == st::State::Fresh);
        td.write("main.dc", "module main; // v1\n");
    }
    {
        auto s = fresh();
        fs::remove(library);
        auto v = st::verify(s, key(), output, Clock::now());
        CHECK(v.state == st::State::InputMissing);
        CHECK(v.detail.find("lib.dc") != std::string::npos);
        library = td.write("lib.dc", "module lib;\n");
    }
    {
        auto s = fresh();
        fs::remove(output);
        CHECK(st::verify(s, key(), output, Clock::now()).state == st::State::OutputChanged);
        output = td.write("out/app", "binary");
    }
    {
        auto s = fresh();
        td.write("out/app", "binary!");
        CHECK(st::verify(s, key(), output, Clock::now()).state == st::State::OutputChanged);
        output = td.write("out/app", "binary");
    }
}

TEST_CASE("an input written around the build time is compared by content even when size and mtime match")
{
    TempDir td;
    auto output = td.write("out/app", "binary");
    auto source = td.write("main.dc", "module main; // a\n", std::chrono::hours{0});
    auto stamp = st::record(key(), output, std::vector<fs::path>{source}, Clock::now());
    REQUIRE(stamp.has_value());
    REQUIRE(stamp->inputs[0].racy);
    auto mtime = fs::last_write_time(source);
    {
        std::ofstream f{source, std::ios::binary | std::ios::trunc};
        f << "module main; // b\n";
    }
    fs::last_write_time(source, mtime);
    auto copy = *stamp;
    CHECK(st::verify(copy, key(), output, Clock::now()).state == st::State::InputChanged);

    {
        std::ofstream f{source, std::ios::binary | std::ios::trunc};
        f << "module main; // a\n";
    }
    fs::last_write_time(source, mtime);
    auto same = *stamp;
    CHECK(st::verify(same, key(), output, Clock::now()).state == st::State::Fresh);
    auto later = *stamp;
    CHECK(st::verify(later, key(), output, Clock::now() + std::chrono::hours{1}).state == st::State::Fresh);
    CHECK(!later.inputs[0].racy);
}
