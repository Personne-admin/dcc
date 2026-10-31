import std;
import dpm.scaffold;

#include "harness.hh"

namespace
{
    namespace t = dpm::scaffold;

    std::string legacy_manifest(std::string const& name, bool lib)
    {
        std::string manifest = std::format("dpm::v1::Project {{\n    name = \"{}\",\n    version = \"0.1.0\",\n", name);
        if (!lib)
            manifest += std::format("    products = {{\n        {{ name = \"{}\", entry = \"src/main.dc\" }},\n    }},\n", name);
        manifest += "}\n";
        return manifest;
    }

    std::string legacy_entry(std::string const& name, bool lib)
    {
        return lib ? std::format("module {}::lib;\n\npublic i32 answer()\n{{\n    return 42;\n}}\n", name) : std::string{"module main;\n\npublic i32 main()\n{\n    return 0;\n}\n"};
    }

    std::string legacy_entry_path(std::string const& name, bool lib)
    {
        return lib ? "src/" + name + "/lib.dc" : "src/main.dc";
    }

    std::string text_of(t::Rendered const& rendered, std::string const& path)
    {
        for (auto const& file : rendered.files)
            if (file.path == path)
                return file.content;
        return "<missing>";
    }

    std::string expanded(std::string_view text, t::Variables const& vars)
    {
        auto out = t::expand(text, vars);
        return out.problem ? "<error: " + out.problem->message + ">" : out.text;
    }
} // namespace

TEST_CASE("built-in templates render exactly what dpm init always wrote")
{
    for (std::string name : {"app", "my_tool", "x1", "a_very_long_package_name_for_a_test"})
    {
        for (bool lib : {false, true})
        {
            auto files = lib ? t::builtin_library() : t::builtin_executable();
            auto rendered = t::render(files, {{"name", name}});
            REQUIRE(rendered.ok());
            CHECK_EQ(rendered.files.size(), std::size_t{3});
            CHECK_EQ(text_of(rendered, "project.dcon"), legacy_manifest(name, lib));
            CHECK_EQ(text_of(rendered, legacy_entry_path(name, lib)), legacy_entry(name, lib));
            CHECK_EQ(text_of(rendered, ".gitignore"), std::string{".dpm/\n"});
        }
    }
}

TEST_CASE("a placeholder is two at signs, a lowercase name and two at signs")
{
    t::Variables vars{{"name", "kernel"}, {"title", "Boot"}};
    CHECK_EQ(expanded("@@name@@", vars), std::string{"kernel"});
    CHECK_EQ(expanded("a @@name@@ b @@title@@ c", vars), std::string{"a kernel b Boot c"});
    CHECK_EQ(expanded("@@name@@@@title@@", vars), std::string{"kernelBoot"});
    CHECK_EQ(expanded("", vars), std::string{});
    CHECK_EQ(expanded("no placeholders", vars), std::string{"no placeholders"});
}

TEST_CASE("four at signs are an escape for two")
{
    t::Variables vars{{"name", "kernel"}};
    CHECK_EQ(expanded("@@@@", vars), std::string{"@@"});
    CHECK_EQ(expanded("@@@@name@@@@", vars), std::string{"@@name@@"});
    CHECK_EQ(expanded("@@@@@@", vars), std::string{"@@@@"});
    CHECK_EQ(expanded("@@@@@@name@@", vars), std::string{"@@kernel"});
}

TEST_CASE("text that is not a complete placeholder stays literal")
{
    t::Variables vars{{"name", "kernel"}};
    CHECK_EQ(expanded("@", vars), std::string{"@"});
    CHECK_EQ(expanded("@@", vars), std::string{"@@"});
    CHECK_EQ(expanded("@@ name@@", vars), std::string{"@@ name@@"});
    CHECK_EQ(expanded("@@name", vars), std::string{"@@name"});
    CHECK_EQ(expanded("@@name@", vars), std::string{"@@name@"});
    CHECK_EQ(expanded("@@Name@@", vars), std::string{"@@Name@@"});
    CHECK_EQ(expanded("@@1name@@", vars), std::string{"@@1name@@"});
    CHECK_EQ(expanded("@@na-me@@", vars), std::string{"@@na-me@@"});
    CHECK_EQ(expanded("user@@example.com", vars), std::string{"user@@example.com"});
    CHECK_EQ(expanded("@@@name@@", vars), std::string{"@kernel"});
}

TEST_CASE("an identifier-shaped placeholder that is not declared is an error with its line")
{
    t::Variables vars{{"name", "kernel"}};
    auto out = t::expand("one\ntwo\n  @@missing@@\n", vars);
    REQUIRE(out.problem.has_value());
    CHECK_EQ(out.problem->line, std::size_t{3});
    CHECK(out.problem->message.find("missing") != std::string::npos);
}

TEST_CASE("a value is inserted as it is and not scanned again")
{
    t::Variables vars{{"name", "@@title@@"}, {"title", "Boot"}};
    CHECK_EQ(expanded("@@name@@", vars), std::string{"@@title@@"});
}

TEST_CASE("generated paths are checked after substitution")
{
    for (std::string bad : {"..", "../x", "", "a\\b", "/abs"})
    {
        auto rendered = t::render(t::builtin_library(), {{"name", bad}});
        CHECK(!rendered.ok());
    }
    auto good = t::render(t::builtin_library(), {{"name", "ok_name"}});
    CHECK(good.ok());
}

TEST_CASE("files that do not substitute are copied byte for byte")
{
    std::vector<t::File> files{{"a.txt", "@@name@@ \r\n @@missing@@", false, false}, {"b.txt", "@@name@@", false, true}};
    auto rendered = t::render(files, {{"name", "k"}});
    REQUIRE(rendered.ok());
    CHECK_EQ(text_of(rendered, "a.txt"), std::string{"@@name@@ \r\n @@missing@@"});
    CHECK_EQ(text_of(rendered, "b.txt"), std::string{"k"});
}

TEST_CASE("two files that render to one path are refused")
{
    std::vector<t::File> files{{"@@name@@.txt", "a"}, {"k.txt", "b"}};
    auto rendered = t::render(files, {{"name", "k"}});
    CHECK(!rendered.ok());
    CHECK_EQ(rendered.problems.size(), std::size_t{1});
}

TEST_CASE("an undeclared variable in a path or a file is reported with the file")
{
    std::vector<t::File> files{{"x/@@nope@@.txt", "a"}, {"y.txt", "line\n@@nope@@"}};
    auto rendered = t::render(files, {{"name", "k"}});
    REQUIRE(rendered.problems.size() == std::size_t{2});
    CHECK_EQ(rendered.problems[1].path, std::string{"y.txt"});
    CHECK_EQ(rendered.problems[1].line, std::size_t{2});
}
