import std;
import dpm.manifest;
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

namespace
{
    namespace m = dpm::manifest;

    m::TemplateVariable var(std::string name, m::VariableKind kind = m::VariableKind::String, std::string default_value = "")
    {
        m::TemplateVariable v;
        v.name = std::move(name);
        v.kind = kind;
        v.default_value = std::move(default_value);
        return v;
    }

    m::ProjectTemplate make_template(std::vector<m::TemplateVariable> variables = {})
    {
        m::ProjectTemplate tmpl;
        tmpl.name = "sample";
        tmpl.root = "files";
        tmpl.variables = std::move(variables);
        return tmpl;
    }

    bool mentions(t::Rendered const& rendered, std::string_view needle)
    {
        return std::ranges::any_of(rendered.problems, [&](t::Problem const& p) { return p.message.find(needle) != std::string::npos || p.path.find(needle) != std::string::npos; });
    }

    bool mentions(std::vector<t::Problem> const& problems, std::string_view needle)
    {
        return std::ranges::any_of(problems, [&](t::Problem const& p) { return p.message.find(needle) != std::string::npos; });
    }
} // namespace

TEST_CASE("variables take defaults, given values and built-in names")
{
    auto tmpl = make_template({var("title", m::VariableKind::String, "Hello"), var("count", m::VariableKind::Integer, "3")});
    auto r = t::resolve(tmpl, {{"name", "kern"}}, "out/dir");
    REQUIRE(r.ok());
    CHECK_EQ(r.files.at("name"), std::string{"kern"});
    CHECK_EQ(r.files.at("title"), std::string{"Hello"});
    CHECK_EQ(r.files.at("count"), std::string{"3"});
    CHECK(!r.files.contains("dir"));
    CHECK_EQ(r.steps.at("dir"), std::string{"out/dir"});
    auto g = t::resolve(tmpl, {{"name", "kern"}, {"title", "Mine"}, {"count", "-4"}}, "d");
    REQUIRE(g.ok());
    CHECK_EQ(g.files.at("title"), std::string{"Mine"});
    CHECK_EQ(g.files.at("count"), std::string{"-4"});
}

TEST_CASE("missing, unknown and invalid variables are all reported together")
{
    auto owner = var("owner");
    owner.required = true;
    owner.description = "who owns it";
    auto tmpl = make_template({owner, var("count", m::VariableKind::Integer, "3"), var("flag", m::VariableKind::Bool, "false")});
    auto r = t::resolve(tmpl, {{"name", "1bad"}, {"count", "x"}, {"flag", "maybe"}, {"extra", "1"}, {"dir", "x"}}, "d");
    CHECK(!r.ok());
    CHECK(mentions(r.problems, "variable 'owner' is required: who owns it"));
    CHECK(mentions(r.problems, "variable 'count'"));
    CHECK(mentions(r.problems, "variable 'flag'"));
    CHECK(mentions(r.problems, "unknown variable 'extra'"));
    CHECK(mentions(r.problems, "'dir' is built in"));
    CHECK(mentions(r.problems, "variable 'name'"));
    auto no_name = t::resolve(tmpl, {{"owner", "me"}}, "d");
    CHECK(mentions(no_name.problems, "variable 'name' is required"));
}

TEST_CASE("a name that is not an identifier is refused")
{
    auto tmpl = make_template();
    for (std::string bad : {"", "..", "a/b", "1x", "a-b", "enum", "a b", "x\"y"})
    {
        auto r = t::resolve(tmpl, {{"name", bad}}, "d");
        CHECK(!r.ok());
    }
    for (std::string good : {"kern", "_k", "K1"})
        CHECK(t::resolve(tmpl, {{"name", good}}, "d").ok());
}

TEST_CASE("the name of a shipped library is refused as a package name")
{
    auto tmpl = make_template();
    for (std::string bad : {"std", "core", "assert", "dcext", "dpm"})
    {
        auto r = t::resolve(tmpl, {{"name", bad}}, "d");
        CHECK(!r.ok());
        CHECK(mentions(r.problems, "namespace of a library that ships with dcc"));
    }
    for (std::string good : {"Std", "std_ext", "app"})
        CHECK(t::resolve(tmpl, {{"name", good}}, "d").ok());
}

TEST_CASE("sample values satisfy every required variable")
{
    auto a = var("a");
    a.required = true;
    auto b = var("b", m::VariableKind::Integer);
    b.required = true;
    b.min = 5;
    b.max = 9;
    auto c = var("c", m::VariableKind::Choice);
    c.required = true;
    c.choices = {"p", "q"};
    auto d = var("d", m::VariableKind::Bool);
    d.required = true;
    auto e = var("e", m::VariableKind::Identifier);
    e.required = true;
    auto tmpl = make_template({a, b, c, d, e});
    CHECK(t::resolve(tmpl, t::sample_values(tmpl), "d").ok());
}

TEST_CASE("glob patterns match relative paths segment by segment")
{
    struct Row
    {
        std::string_view pattern;
        std::string_view path;
        bool match;
    };
    std::vector<Row> rows{
        {"a", "a", true},         {"a", "b", false},          {"a", "a/b", false},       {"*.md", "README.md", true},     {"*.md", "docs/README.md", false},
        {"*.md", ".md", true},    {"*", "a", true},           {"*", "a/b", false},       {"*", "", true},                 {"a*b", "ab", true},
        {"a*b", "axxb", true},    {"a*b", "axxbx", false},    {"a*b*c", "abxbxc", true}, {"a?c", "abc", true},            {"a?c", "ac", false},
        {"a?c", "a/c", false},    {"?", "\xC3\xA9", true},  {"??", "\xC3\xA9", false}, {"src/**/*.dc", "src/a.dc", true}, {"src/**/*.dc", "src/x/y/a.dc", true},
        {"src/**/*.dc", "src/a.c", false}, {"src/**/*.dc", "other/a.dc", false}, {"**", "a", true}, {"**", "a/b/c", true},  {"**/x", "x", true},
        {"**/x", "a/b/x", true},  {"**/x", "a/b/y", false},   {"a/**", "a", true},       {"a/**", "a/b", true},           {"dir/", "dir", false},
        {"dir/", "dir/f", true},  {"dir/", "dir/s/f", true},  {"dir/", "other/f", false}, {"dir/", "dirx/f", false},      {"**/.keep", "a/.keep", true},
        {"**/.keep", ".keep", true}, {"tmp/*", "tmp/a", true}, {"tmp/*", "tmp/a/b", false}, {"A", "a", false},            {".h*", ".hidden", true},
        {"*.dc", "main.dc.bak", false}, {"x.*", "x.", true},  {"a**", "ab", true},       {"**/**", "a/b", true},
    };
    for (auto const& row : rows)
        CHECK(t::glob_match(row.pattern, row.path) == row.match);
}

namespace
{
    std::string blob()
    {
        return std::string{"\0\1@@name@@", 10};
    }

    std::vector<t::File> sample_tree()
    {
        std::vector<t::File> tree;
        tree.push_back({"project.dcon", "name = \"@@name@@\"\n"});
        tree.push_back({"src", "", false, false, true});
        tree.push_back({"src/@@name@@.dc", "module @@name@@;\n"});
        tree.push_back({"scripts/run.sh", "echo @@title@@\n", true});
        tree.push_back({"data/blob.bin", blob()});
        tree.push_back({"README.md", "# @@title@@\n"});
        return tree;
    }

    m::ProjectTemplate sample_template()
    {
        auto tmpl = make_template({var("title", m::VariableKind::String, "Hello World")});
        tmpl.substitute = {"project.dcon", "src/**", "scripts/*.sh", "*.md"};
        tmpl.next_steps = "cd @@dir@@ and build @@name@@";
        return tmpl;
    }
}

TEST_CASE("generation substitutes only the listed files and every path")
{
    auto generated = t::generate(sample_template(), sample_tree(), {{"name", "kern"}}, "out");
    REQUIRE(generated.rendered.ok());
    auto const& r = generated.rendered;
    CHECK_EQ(text_of(r, "project.dcon"), std::string{"name = \"kern\"\n"});
    CHECK_EQ(text_of(r, "src/kern.dc"), std::string{"module kern;\n"});
    CHECK_EQ(text_of(r, "scripts/run.sh"), std::string{"echo Hello World\n"});
    CHECK_EQ(text_of(r, "README.md"), std::string{"# Hello World\n"});
    CHECK_EQ(text_of(r, "data/blob.bin"), blob());
    CHECK_EQ(generated.next_steps, std::string{"cd out and build kern"});
    bool saw_dir = false;
    bool saw_exec = false;
    for (auto const& file : r.files)
    {
        saw_dir = saw_dir || (file.path == "src" && file.directory);
        saw_exec = saw_exec || (file.path == "scripts/run.sh" && file.executable);
    }
    CHECK(saw_dir);
    CHECK(saw_exec);
}

TEST_CASE("generation order does not depend on the order of the input tree")
{
    auto tree = sample_tree();
    auto a = t::generate(sample_template(), tree, {{"name", "kern"}}, "out");
    std::ranges::reverse(tree);
    auto b = t::generate(sample_template(), tree, {{"name", "kern"}}, "out");
    REQUIRE(a.rendered.ok());
    REQUIRE(b.rendered.ok());
    REQUIRE(a.rendered.files.size() == b.rendered.files.size());
    for (std::size_t i = 0; i < a.rendered.files.size(); ++i)
    {
        CHECK_EQ(a.rendered.files[i].path, b.rendered.files[i].path);
        CHECK_EQ(a.rendered.files[i].content, b.rendered.files[i].content);
    }
}

TEST_CASE("excluded files are not looked at and exclude wins over substitute")
{
    auto tmpl = sample_template();
    tmpl.exclude = {"data/**", "README.md"};
    auto tree = sample_tree();
    tree.push_back({"README.md", "@@nonsense@@"});
    tree.push_back({"data/other.txt", "@@nonsense@@"});
    auto generated = t::generate(tmpl, tree, {{"name", "kern"}}, "out");
    REQUIRE(generated.rendered.ok());
    for (auto const& file : generated.rendered.files)
    {
        CHECK(file.path != "README.md");
        CHECK(!file.path.starts_with("data/"));
    }
}

TEST_CASE("a substituted file must be small UTF-8 text")
{
    auto tmpl = sample_template();
    tmpl.substitute.push_back("data/**");
    auto bad = t::generate(tmpl, sample_tree(), {{"name", "kern"}}, "out");
    CHECK(!bad.rendered.ok());
    CHECK(mentions(bad.rendered, "UTF-8"));
    auto tree = sample_tree();
    tree.push_back({"big.txt", std::string((1U << 20) + 1, 'a')});
    tmpl.substitute = {"big.txt"};
    CHECK(mentions(t::generate(tmpl, tree, {{"name", "kern"}}, "out").rendered, "larger than 1 MiB"));
    tree.back().content.resize(1U << 20);
    CHECK(t::generate(tmpl, tree, {{"name", "kern"}}, "out").rendered.ok());
    tree.back().content = "caf\xC3";
    CHECK(mentions(t::generate(tmpl, tree, {{"name", "kern"}}, "out").rendered, "UTF-8"));
}

TEST_CASE("unknown placeholders are errors in contents, paths and next steps")
{
    auto tmpl = sample_template();
    auto tree = sample_tree();
    tree.push_back({"extra.md", "line one\n@@oops@@\n"});
    tmpl.substitute.push_back("extra.md");
    auto in_content = t::generate(tmpl, tree, {{"name", "kern"}}, "out");
    REQUIRE(!in_content.rendered.ok());
    CHECK(mentions(in_content.rendered, "unknown variable 'oops'"));
    bool line_two = false;
    for (auto const& p : in_content.rendered.problems)
        line_two = line_two || (p.path == "extra.md" && p.line == 2);
    CHECK(line_two);
    auto in_path = t::generate(sample_template(), std::vector<t::File>{{"a/@@oops@@/x", "x"}}, {{"name", "kern"}}, "out");
    CHECK(mentions(in_path.rendered, "unknown variable 'oops'"));
    auto steps = sample_template();
    steps.next_steps = "go @@oops@@";
    auto in_steps = t::generate(steps, sample_tree(), {{"name", "kern"}}, "out");
    CHECK(mentions(in_steps.rendered, "unknown variable 'oops'"));
}

TEST_CASE("the built-in dir variable is available to next steps only")
{
    auto tmpl = sample_template();
    auto tree = sample_tree();
    tree.push_back({"where.md", "in @@dir@@\n"});
    tmpl.substitute.push_back("where.md");
    auto generated = t::generate(tmpl, tree, {{"name", "kern"}}, "out");
    CHECK(!generated.rendered.ok());
    CHECK(mentions(generated.rendered, "unknown variable 'dir'"));
}

TEST_CASE("names that collide after substitution are refused")
{
    auto tmpl = make_template({var("a", m::VariableKind::String, "X"), var("b", m::VariableKind::String, "x")});
    std::vector<t::File> tree{{"@@a@@.txt", "1"}, {"@@b@@.txt", "2"}};
    auto folded = t::generate(tmpl, tree, {{"name", "k"}}, "o");
    CHECK(!folded.rendered.ok());
    CHECK(mentions(folded.rendered, "differs only in case"));
    std::vector<t::File> dup{{"@@a@@", "1"}, {"X", "2"}};
    CHECK(mentions(t::generate(tmpl, dup, {{"name", "k"}}, "o").rendered, "generated twice"));
    std::vector<t::File> shadow{{"@@a@@", "1"}, {"X/inner", "2"}};
    CHECK(mentions(t::generate(tmpl, shadow, {{"name", "k"}}, "o").rendered, "used as a directory"));
}

TEST_CASE("string values cannot steer a path out of the project")
{
    auto tmpl = make_template({var("part", m::VariableKind::String, "ok")});
    std::vector<t::File> tree{{"@@part@@/file.txt", "x"}};
    for (std::string value : std::initializer_list<std::string>{"..", ".", "a/../..", "/abs", "a//b", "x:y", "../..", std::string(300, 'a')})
    {
        auto generated = t::generate(tmpl, tree, {{"name", "k"}, {"part", value}}, "o");
        CHECK(!generated.rendered.ok());
    }
    for (std::string value : {"a", "a/b", "a.b", "..a", "a..", "...", ".hidden"})
    {
        auto generated = t::generate(tmpl, tree, {{"name", "k"}, {"part", value}}, "o");
        CHECK(generated.rendered.ok());
    }
}

TEST_CASE("an empty value cannot leave an empty path component")
{
    auto tmpl = make_template({var("part", m::VariableKind::String, "")});
    for (std::string path : {"@@part@@", "a/@@part@@", "@@part@@/a", "a/@@part@@/b", "a/@@part@@@@part@@"})
    {
        std::vector<t::File> tree{{path, "x"}};
        CHECK(!t::generate(tmpl, tree, {{"name", "k"}}, "o").rendered.ok());
    }
    std::vector<t::File> fine{{"a@@part@@b", "x"}};
    CHECK(t::generate(tmpl, fine, {{"name", "k"}}, "o").rendered.ok());
}

TEST_CASE("output limits are enforced")
{
    auto tmpl = make_template();
    std::vector<t::File> tree;
    for (int i = 0; i < 20001; ++i)
        tree.push_back({"f" + std::to_string(i), "x", false, false});
    auto generated = t::generate(tmpl, tree, {{"name", "k"}}, "o");
    CHECK(mentions(generated.rendered, "more than 20000 files"));
    tree.resize(20000);
    CHECK(t::generate(tmpl, tree, {{"name", "k"}}, "o").rendered.ok());
    std::vector<t::File> deep{{std::string(4097, 'a'), "x", false, false}};
    CHECK(!t::generate(tmpl, deep, {{"name", "k"}}, "o").rendered.ok());
}

TEST_CASE("generation never produces an unsafe path whatever the template and values")
{
    std::mt19937_64 rng{20261031};
    auto pick = [&](auto const& pool) { return pool[rng() % pool.size()]; };
    std::vector<std::string> pieces{"a", "b", "src", "@@name@@", "@@v@@", "@@@@", "@@", "@@v", "..", ".", "x.dc", "dir", "@@w@@", "Name", "name"};
    std::vector<std::string> values{"ok", "..", ".", "a/b", "a/../b", "/abs", "x:y", "", "a//b", "sp ace", "caf\xC3\xA9", "...", "a.", ".a", "%", "#", "@", "(x)"};
    std::size_t generated_ok = 0;
    std::size_t generated_bad = 0;
    for (int round = 0; round < 3000; ++round)
    {
        auto tmpl = make_template({var("v", m::VariableKind::String, "dv"), var("w", m::VariableKind::Identifier, "wv")});
        tmpl.substitute = {"**"};
        std::vector<t::File> tree;
        int files = 1 + static_cast<int>(rng() % 6);
        for (int f = 0; f < files; ++f)
        {
            std::string path;
            int depth = 1 + static_cast<int>(rng() % 3);
            for (int d = 0; d < depth; ++d)
            {
                if (d)
                    path += '/';
                path += pick(pieces);
                if (rng() % 3 == 0)
                    path += pick(pieces);
            }
            tree.push_back({path, pick(pieces) + pick(pieces), rng() % 4 == 0, true, rng() % 8 == 0});
        }
        t::Variables given{{"name", rng() % 2 ? "kern" : "k_2"}, {"v", pick(values)}};
        if (rng() % 2)
            given["w"] = rng() % 2 ? "id" : "_x1";
        auto first = t::generate(tmpl, tree, given, "out");
        auto second = t::generate(tmpl, tree, given, "out");
        CHECK_EQ(first.rendered.files.size(), second.rendered.files.size());
        CHECK_EQ(first.rendered.problems.size(), second.rendered.problems.size());
        if (!first.rendered.ok())
        {
            ++generated_bad;
            continue;
        }
        ++generated_ok;
        std::set<std::string> seen;
        for (auto const& file : first.rendered.files)
        {
            bool unsafe = file.path.empty() || file.path.front() == '/' || file.path.find("//") != std::string::npos || file.path.find('\\') != std::string::npos ||
                          file.path.find(':') != std::string::npos || file.path.back() == '/';
            for (std::size_t pos = 0; pos <= file.path.size();)
            {
                auto slash = file.path.find('/', pos);
                auto part = file.path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
                if (part.empty() || part == "." || part == "..")
                    unsafe = true;
                if (slash == std::string::npos)
                    break;
                pos = slash + 1;
            }
            CHECK(!unsafe);
            CHECK(seen.insert(file.path).second);
        }
    }
    CHECK(generated_ok > 200);
    CHECK(generated_bad > 200);
}
