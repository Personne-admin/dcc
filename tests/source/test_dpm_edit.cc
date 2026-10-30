import std;
import dpm.manifest;
import dpm.edit;

#include "harness.hh"

#include <stdlib.h>

namespace
{
    namespace m = dpm::manifest;
    namespace e = dpm::edit;

    [[nodiscard]] m::LoadOptions options()
    {
        m::LoadOptions o;
        char const* root = ::getenv("DCC_TEST_DPM_SCHEMA");
        if (root)
            o.schema_root = root;
        return o;
    }

    [[nodiscard]] bool enabled()
    {
        return ::getenv("DCC_TEST_DPM_SCHEMA") != nullptr;
    }

    [[nodiscard]] m::Dependency dep(std::string location, std::string version = "")
    {
        m::Dependency d;
        d.location = std::move(location);
        d.version = std::move(version);
        return d;
    }

    [[nodiscard]] std::string add(std::string const& text, m::Dependency const& d, e::List list = e::List::Dependencies)
    {
        auto r = e::add_dependency(text, options(), list, d);
        if (!r.ok)
        {
            std::println(std::cerr, "      add failed: {}", r.error);
            for (auto const& diagnostic : r.diagnostics)
                std::println(std::cerr, "      {}", diagnostic.format());
            return "<failed>";
        }
        return r.text;
    }

    [[nodiscard]] std::string remove(std::string const& text, std::string_view location, e::List list = e::List::Dependencies)
    {
        auto r = e::remove_dependency(text, options(), list, location);
        if (!r.ok)
        {
            std::println(std::cerr, "      remove failed: {}", r.error);
            return "<failed>";
        }
        return r.text;
    }

    [[nodiscard]] std::string set_version(std::string const& text, std::string_view location, std::string_view version)
    {
        auto r = e::set_dependency_version(text, options(), e::List::Dependencies, location, version);
        if (!r.ok)
        {
            std::println(std::cerr, "      set failed: {}", r.error);
            return "<failed>";
        }
        return r.text;
    }

    void expect_text(std::string const& actual, std::string const& expected, std::string_view what)
    {
        if (!test::check(actual == expected, what))
            std::println(std::cerr, "      expected:\n{}\n      actual:\n{}", expected, actual);
    }
} // namespace

SECTION("dpm manifest editing");

TEST_CASE("add appends after the last element of a multi-line list and keeps the trailing comma style")
{
    if (!enabled())
        return;
    std::string text =
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" },\n    },\n}\n";
    std::string expected = "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" },\n"
                           "        { location = \"github.com/x/y\", version = \"1.0.0\" },\n    },\n}\n";
    expect_text(add(text, dep("github.com/x/y", "1.0.0")), expected, "trailing comma");
    std::string no_comma =
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" }\n    }\n}\n";
    std::string expected_no_comma =
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" },\n"
        "        { location = \"github.com/x/y\", version = \"1.0.0\" }\n    }\n}\n";
    expect_text(add(no_comma, dep("github.com/x/y", "1.0.0")), expected_no_comma, "no trailing comma");
}

TEST_CASE("add keeps a trailing comment on the previous element's line")
{
    if (!enabled())
        return;
    std::string text =
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" }, // json\n    },\n}\n";
    std::string expected =
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" }, // json\n"
        "        { location = \"path:../u\" },\n    },\n}\n";
    expect_text(add(text, dep("path:../u")), expected, "comment kept");
}

TEST_CASE("add to a single-line list inserts inline")
{
    if (!enabled())
        return;
    std::string text = "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.2.0\" } } }";
    std::string expected =
        "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.2.0\" }, { location = \"path:../u\" } } }";
    expect_text(add(text, dep("path:../u")), expected, "inline");
    std::string trailing = "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.2.0\" }, } }";
    std::string expected_trailing =
        "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.2.0\" }, { location = \"path:../u\" }, } }";
    expect_text(add(trailing, dep("path:../u")), expected_trailing, "inline trailing comma");
}

TEST_CASE("add to an empty list replaces it")
{
    if (!enabled())
        return;
    std::string multi = "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {},\n}\n";
    std::string expected_multi = "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"path:../u\" },\n    },\n}\n";
    expect_text(add(multi, dep("path:../u")), expected_multi, "empty multi-line");
    std::string inline_text = "{ name = \"a\", version = \"1.0.0\", dependencies = {} }";
    expect_text(add(inline_text, dep("path:../u")), "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"path:../u\" } } }", "empty inline");
}

TEST_CASE("add creates an absent field after the last field")
{
    if (!enabled())
        return;
    std::string text = "{\n    name = \"a\",\n    version = \"1.0.0\",\n}\n";
    expect_text(add(text, dep("json5", "1.2.0")),
                "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = { { location = \"json5\", version = \"1.2.0\" } },\n}\n",
                "absent multi-line");
    std::string no_comma = "{\n\tname = \"a\",\n\tversion = \"1.0.0\"\n}\n";
    expect_text(add(no_comma, dep("json5", "1.2.0"), e::List::DevDependencies),
                "{\n\tname = \"a\",\n\tversion = \"1.0.0\",\n\tdev_dependencies = { { location = \"json5\", version = \"1.2.0\" } }\n}\n",
                "absent tabs, no comma");
    std::string inline_text = "{ name = \"a\", version = \"1.0.0\" }";
    expect_text(add(inline_text, dep("json5", "1.2.0")),
                "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.2.0\" } } }", "absent inline");
}

TEST_CASE("CRLF files get CRLF in inserted text")
{
    if (!enabled())
        return;
    std::string text = "{\r\n    name = \"a\",\r\n    version = \"1.0.0\",\r\n    dependencies = {\r\n        { location = \"json5\", version = \"1.2.0\" "
                       "},\r\n    },\r\n}\r\n";
    auto result = add(text, dep("path:../u"));
    std::string expected =
        "{\r\n    name = \"a\",\r\n    version = \"1.0.0\",\r\n    dependencies = {\r\n        { location = \"json5\", version = \"1.2.0\" },\r\n"
        "        { location = \"path:../u\" },\r\n    },\r\n}\r\n";
    expect_text(result, expected, "crlf add");
    expect_text(remove(result, "path:../u"), text, "crlf remove restores");
}

TEST_CASE("remove deletes an element and the line it was alone on")
{
    if (!enabled())
        return;
    std::string text = "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" },\n"
                       "        { location = \"path:../u\" },\n        { location = \"github.com/x/y\", version = \"1.0.0\" },\n    },\n}\n";
    expect_text(remove(text, "path:../u"),
                "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" },\n"
                "        { location = \"github.com/x/y\", version = \"1.0.0\" },\n    },\n}\n",
                "middle");
    expect_text(remove(text, "github.com/x/y"),
                "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" },\n"
                "        { location = \"path:../u\" },\n    },\n}\n",
                "last");
}

TEST_CASE("remove takes the comment directly above but not one after a blank line")
{
    if (!enabled())
        return;
    std::string text = "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        // kept\n\n        // goes with u\n        { location = "
                       "\"path:../u\" },\n"
                       "        { location = \"json5\", version = \"1.2.0\" },\n    },\n}\n";
    expect_text(remove(text, "path:../u"),
                "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        // kept\n\n        { location = \"json5\", version = \"1.2.0\" "
                "},\n    },\n}\n",
                "comment");
}

TEST_CASE("remove from a single-line list")
{
    if (!enabled())
        return;
    std::string text = "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.2.0\" }, { location = \"path:../u\" } } }";
    expect_text(remove(text, "path:../u"), "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.2.0\" } } }",
                "last inline");
    expect_text(remove(text, "json5"), "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"path:../u\" } } }", "first inline");
}

TEST_CASE("removing the only element removes the field")
{
    if (!enabled())
        return;
    std::string text = "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = { { location = \"json5\", version = \"1.2.0\" } },\n}\n";
    expect_text(remove(text, "json5"), "{\n    name = \"a\",\n    version = \"1.0.0\",\n}\n", "only element");
}

TEST_CASE("set version replaces only the string literal")
{
    if (!enabled())
        return;
    std::string text =
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" }, // note\n    },\n}\n";
    expect_text(
        set_version(text, "json5", "1.3.0"),
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.3.0\" }, // note\n    },\n}\n",
        "set version");
}

TEST_CASE("invalid operations fail and report why")
{
    if (!enabled())
        return;
    std::string text = "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.2.0\" } } }";
    CHECK(!e::add_dependency(text, options(), e::List::Dependencies, dep("json5", "1.2.0")).ok);
    auto bad = e::add_dependency(text, options(), e::List::Dependencies, dep("foo.bar/x", "1.0.0"));
    CHECK(!bad.ok);
    CHECK(!bad.diagnostics.empty());
    CHECK(!e::remove_dependency(text, options(), e::List::Dependencies, "nope").ok);
    CHECK(!e::remove_dependency(text, options(), e::List::DevDependencies, "json5").ok);
    CHECK(!e::set_dependency_version(text, options(), e::List::Dependencies, "json5", "not-a-version").ok);
    CHECK(!e::add_dependency("{ name = 3 }", options(), e::List::Dependencies, dep("json5", "1.0.0")).ok);
    std::string indirect = "const []const Dependency L = {};\n{ name = \"a\", version = \"1.0.0\", dependencies = L }";
    auto not_literal = e::add_dependency(indirect, options(), e::List::Dependencies, dep("json5", "1.0.0"));
    CHECK(!not_literal.ok);
    CHECK(not_literal.error.contains("plain list literal"));
}

TEST_CASE("add then remove restores the original bytes when the list was not empty")
{
    if (!enabled())
        return;
    std::vector<std::string> corpus{
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" },\n    },\n}\n",
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" }\n    }\n}\n",
        "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.2.0\" } } }",
        "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.2.0\" }, } }",
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n}\n",
        "{\n    name = \"a\",\n    version = \"1.0.0\"\n}\n",
        "{ name = \"a\", version = \"1.0.0\" }",
        "{\r\n\tname = \"a\", // x\r\n\tversion = \"1.0.0\"\r\n}\r\n",
    };
    for (auto const& original : corpus)
    {
        auto added = add(original, dep("path:../u"));
        CHECK(added != "<failed>");
        expect_text(remove(added, "path:../u"), original, "round trip");
    }
}

TEST_CASE("random add and remove sequences keep the manifest valid and the lists in step")
{
    if (!enabled())
        return;
    std::vector<std::string> starts{
        "{\n    name = \"a\",\n    version = \"1.0.0\",\n}\n",
        "{ name = \"a\", version = \"1.0.0\", dependencies = {}, dev_dependencies = {} }",
        "{\n    name = \"a\", // c\n    version = \"1.0.0\",\n    dependencies = {\n        { location = \"json5\", version = \"1.2.0\" },\n    },\n}\n",
        "{\r\n\tname = \"a\",\r\n\tversion = \"1.0.0\"\r\n}\r\n",
    };
    std::mt19937 rng{12345};
    for (auto const& start : starts)
        for (int round = 0; round < 6; ++round)
        {
            std::string text = start;
            std::vector<std::string> deps;
            std::vector<std::string> devs;
            if (start.contains("json5"))
                deps.push_back("json5");
            for (int step = 0; step < 40; ++step)
            {
                bool dev = rng() % 3 == 0;
                auto& names = dev ? devs : deps;
                auto list = dev ? e::List::DevDependencies : e::List::Dependencies;
                bool do_add = names.empty() || rng() % 5 < 3;
                if (do_add)
                {
                    auto name = "path:../p" + std::to_string(rng() % 6);
                    bool exists = std::ranges::find(names, name) != names.end();
                    auto r = e::add_dependency(text, options(), list, dep(name));
                    CHECK(r.ok == !exists);
                    if (r.ok)
                    {
                        text = r.text;
                        names.push_back(name);
                    }
                }
                else
                {
                    auto victim = names[rng() % names.size()];
                    auto r = e::remove_dependency(text, options(), list, victim);
                    CHECK(r.ok);
                    if (!r.ok)
                    {
                        std::println(std::cerr, "      {}\n{}", r.error, text);
                        return;
                    }
                    text = r.text;
                    names.erase(std::ranges::find(names, victim));
                }
                auto loaded = m::load(text, options());
                REQUIRE(loaded.ok);
                std::vector<std::string> got;
                for (auto const& d : loaded.project->dependencies)
                    got.push_back(d.location);
                std::vector<std::string> got_dev;
                for (auto const& d : loaded.project->dev_dependencies)
                    got_dev.push_back(d.location);
                CHECK(got == deps);
                CHECK(got_dev == devs);
            }
        }
}

TEST_CASE("atomic write replaces the file and keeps its mode")
{
    auto dir = std::filesystem::temp_directory_path() / ("dpm_edit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    auto file = dir / "project.dcon";
    {
        std::ofstream out{file};
        out << "old";
    }
    std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read);
    std::string error;
    CHECK(e::write_atomic(file, "new content", error));
    std::ifstream in{file};
    std::string content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    CHECK(content == "new content");
    CHECK((std::filesystem::status(file).permissions() & std::filesystem::perms::group_read) != std::filesystem::perms::none);
    CHECK(!std::filesystem::exists(dir / "project.dcon.dpm-tmp"));
    std::filesystem::remove_all(dir);
}
