import std;
import dpm.manifest;

#include "harness.hh"

#include <stdlib.h>

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("dpm_test_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write_file(std::filesystem::path const& relative, std::string const& content) const
        {
            auto full = path / relative;
            std::filesystem::create_directories(full.parent_path());
            std::ofstream f{full, std::ios::binary};
            f << content;
        }
    };

    struct Section
    {
        std::string header;
        std::string body;
    };

    [[nodiscard]] std::vector<Section> parse_sections(std::string const& text)
    {
        std::vector<Section> sections;
        std::size_t pos = 0;
        while (pos < text.size())
        {
            auto eol = text.find('\n', pos);
            auto line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos = eol == std::string::npos ? text.size() : eol + 1;
            if (line.starts_with("=== ") && line.ends_with(" ==="))
            {
                sections.push_back({line.substr(4, line.size() - 8), {}});
                continue;
            }
            if (sections.empty())
                continue;
            sections.back().body += line;
            if (eol != std::string::npos)
                sections.back().body += "\n";
        }
        for (auto& s : sections)
            if (s.body.ends_with("\n"))
                s.body.pop_back();
        return sections;
    }

    [[nodiscard]] std::string strip_trailing_newlines(std::string s)
    {
        while (s.ends_with("\n"))
            s.pop_back();
        return s;
    }

    [[nodiscard]] std::filesystem::path env_path(char const* name)
    {
        char const* value = ::getenv(name);
        return value ? std::filesystem::path{value} : std::filesystem::path{};
    }

    [[nodiscard]] std::optional<dpm::manifest::FileKind> kind_from(std::string_view name)
    {
        using K = dpm::manifest::FileKind;
        for (auto kind : {K::Project, K::Lock, K::RegistryAlias, K::RegistryLocation, K::RegistryConfig, K::UserConfig, K::Template, K::RegistryTemplate})
            if (dpm::manifest::type_name(kind) == name)
                return kind;
        return std::nullopt;
    }

    void run_fixture(std::filesystem::path const& fixture, std::filesystem::path const& schema_root)
    {
        std::ifstream in{fixture, std::ios::binary};
        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        auto name = fixture.filename().string();
        auto sections = parse_sections(text);

        TempDir td;
        dpm::manifest::LoadOptions options;
        options.schema_root = schema_root;
        options.project_root = td.path;
        std::string manifest;
        bool have_manifest = false;
        bool explicit_kind = false;
        std::optional<std::string> expect_tree;
        std::optional<std::string> expect_full;
        std::optional<std::string> expect_errors;
        for (auto const& s : sections)
        {
            if (s.header.starts_with("FILE: "))
            {
                auto file = s.header.substr(6);
                if (file == "project.dcon" || file == "other.dcon" || file == "project.lock.dcon" || file == "template.dcon")
                {
                    manifest = s.body;
                    have_manifest = true;
                    options.file_name = file;
                }
                else
                    td.write_file(file, s.body);
            }
            else if (s.header.starts_with("KIND: "))
            {
                if (auto kind = kind_from(s.header.substr(6)))
                {
                    options.kind = *kind;
                    explicit_kind = true;
                }
                else
                    test::check(false, name + ": unknown KIND");
            }
            else if (s.header == "EXPECT-TREE")
                expect_tree = s.body;
            else if (s.header == "EXPECT-TREE-FULL")
                expect_full = s.body;
            else if (s.header == "EXPECT-ERRORS")
                expect_errors = s.body;
            else
                test::check(false, name + ": unknown header " + s.header);
        }
        if (!have_manifest)
        {
            test::check(false, name + ": no manifest FILE section");
            return;
        }

        if (!explicit_kind)
            options.kind = dpm::manifest::detect_kind(manifest, options.file_name);
        auto result = dpm::manifest::load(manifest, options);
        std::string actual_errors;
        for (auto const& d : result.diagnostics)
            actual_errors += d.format() + "\n";
        actual_errors = strip_trailing_newlines(actual_errors);

        if (expect_errors)
        {
            bool same = actual_errors == strip_trailing_newlines(*expect_errors);
            if (!test::check(same, name + ": diagnostics differ"))
                std::println(std::cerr, "      expected:\n{}\n      actual:\n{}", *expect_errors, actual_errors);
            test::check(!result.ok, name + ": expected failure");
            return;
        }
        if (!test::check(result.ok, name + ": expected success"))
        {
            std::println(std::cerr, "      actual errors:\n{}", actual_errors);
            return;
        }
        if (expect_tree)
        {
            auto actual = strip_trailing_newlines(result.tree.dump(false));
            if (!test::check(actual == strip_trailing_newlines(*expect_tree), name + ": tree differs"))
                std::println(std::cerr, "      expected:\n{}\n      actual:\n{}", *expect_tree, actual);
        }
        if (expect_full)
        {
            auto actual = strip_trailing_newlines(result.tree.dump(true));
            if (!test::check(actual == strip_trailing_newlines(*expect_full), name + ": full tree differs"))
                std::println(std::cerr, "      expected:\n{}\n      actual:\n{}", *expect_full, actual);
        }
    }
} // namespace

SECTION("dpm manifest fixtures");

TEST_CASE("every manifest fixture produces its expected tree or diagnostics")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    auto cases = env_path("DCC_TEST_DPM_CASES");
    if (schema_root.empty() || cases.empty())
        return;
    std::vector<std::filesystem::path> fixtures;
    for (auto const& entry : std::filesystem::directory_iterator{cases})
        if (entry.path().extension() == ".dcon-test")
            fixtures.push_back(entry.path());
    std::ranges::sort(fixtures);
    REQUIRE(!fixtures.empty());
    for (auto const& fixture : fixtures)
        run_fixture(fixture, schema_root);
}

TEST_CASE("file kinds come from the typed prefix, then from the file name")
{
    using K = dpm::manifest::FileKind;
    CHECK(dpm::manifest::detect_kind("dpm::v1::Lock { }", "project.dcon") == K::Lock);
    CHECK(dpm::manifest::detect_kind("dpm::v1::Project { }", "project.lock.dcon") == K::Project);
    CHECK(dpm::manifest::detect_kind("const []const char n = \"a\"; dpm::v1::RegistryAlias { }", "x.dcon") == K::RegistryAlias);
    CHECK(dpm::manifest::detect_kind("{ }", "project.lock.dcon") == K::Lock);
    CHECK(dpm::manifest::detect_kind("{ }", "dir/sub/project.lock.dcon") == K::Lock);
    CHECK(dpm::manifest::detect_kind("{ }", "project.dcon") == K::Project);
    CHECK(dpm::manifest::detect_kind("{ }", "registry.dcon") == K::RegistryConfig);
    CHECK(dpm::manifest::detect_kind("{ }", "config.dcon") == K::UserConfig);
    CHECK(dpm::manifest::detect_kind("{ }", "template.dcon") == K::Template);
    CHECK(dpm::manifest::detect_kind("{ }", "dir/template.dcon") == K::Template);
    CHECK(dpm::manifest::detect_kind("dpm::v1::Template { }", "project.dcon") == K::Template);
    CHECK(dpm::manifest::detect_kind("{ }", "other.txt") == K::Project);
    CHECK(dpm::manifest::detect_kind("dpm::v1::Nope { }", "project.lock.dcon") == K::Lock);
    CHECK(dpm::manifest::detect_kind("dpm::v1 { }", "project.lock.dcon") == K::Lock);
    CHECK(dpm::manifest::detect_kind("", "project.dcon") == K::Project);
}

TEST_CASE("decoding fills the project structure")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    dpm::manifest::LoadOptions options;
    options.schema_root = schema_root;
    auto result = dpm::manifest::load(R"(dpm::v1::Project {
    name = "app",
    version = "1.2.3-rc1",
    dependencies = {
        { location = "json5", version = "1.2.0" },
        { location = "path:../util" },
    },
    products = {
        { name = "app", entry = "src/main.dc", settings = { optimize = Optimize::Size, whole_program = Tri::On } },
    },
    settings = { target = Target::I8086Mz, model = Model::Small, extra_flags = { "-Wall" } },
})",
                                      options);
    REQUIRE(result.ok);
    REQUIRE(result.project.has_value());
    auto const& p = *result.project;
    CHECK(p.name == "app");
    CHECK(p.source == "src");
    CHECK(p.test_dir == "tests");
    REQUIRE(p.dependencies.size() == 2);
    CHECK(p.dependencies[0].location == "json5");
    CHECK(p.dependencies[1].version.empty());
    REQUIRE(p.products.size() == 1);
    CHECK(p.products[0].settings.optimize == dpm::manifest::Optimize::Size);
    CHECK(p.products[0].settings.whole_program == dpm::manifest::Tri::On);
    CHECK(p.settings.target == dpm::manifest::Target::I8086Mz);
    CHECK(p.settings.model == dpm::manifest::Model::Small);
    REQUIRE(p.settings.extra_flags.size() == 1);
    CHECK(p.settings.extra_flags[0] == "-Wall");
}

TEST_CASE("every schema enumerator is known to the decoder")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    dpm::manifest::LoadOptions options;
    options.schema_root = schema_root;
    std::vector<std::string> manifests;
    for (std::string target : {"Host", "X86_64Elf", "X86_64Coff", "X86Elf", "X86Coff", "I8086Binary", "I8086Mz"})
        manifests.push_back("{ name = \"a\", version = \"1.0.0\", settings = { target = Target::" + target + " } }");
    for (std::string backend : {"Default", "Llvm", "Custom"})
        manifests.push_back("{ name = \"a\", version = \"1.0.0\", settings = { backend = Backend::" + backend + " } }");
    for (std::string model : {"Default", "Small", "Kernel", "Medium", "Large", "Unreal", "Unreal32"})
        manifests.push_back("{ name = \"a\", version = \"1.0.0\", settings = { model = Model::" + model + " } }");
    for (std::string optimize : {"Default", "O0", "O1", "O2", "Size"})
        manifests.push_back("{ name = \"a\", version = \"1.0.0\", settings = { optimize = Optimize::" + optimize + " } }");
    for (std::string lib : {"Default", "Off", "Linux", "Windows", "Freestanding"})
        manifests.push_back("{ name = \"a\", version = \"1.0.0\", settings = { libdcext = Libdcext::" + lib + " } }");
    for (std::string tri : {"Default", "On", "Off"})
        manifests.push_back("{ name = \"a\", version = \"1.0.0\", settings = { pic = Tri::" + tri + " } }");
    for (auto const& text : manifests)
    {
        auto result = dpm::manifest::load(text, options);
        CHECK(result.ok);
        if (!result.ok)
            for (auto const& d : result.diagnostics)
                std::println(std::cerr, "      {}: {}", text, d.format());
    }
}

TEST_CASE("files over the size bound and with NUL bytes are rejected")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    dpm::manifest::LoadOptions options;
    options.schema_root = schema_root;
    auto big = dpm::manifest::load(std::string(dpm::manifest::k_max_file_size + 1, ' '), options);
    CHECK(!big.ok);
    auto nul = dpm::manifest::load(std::string{"{ name = \"a\"\0 }", 15}, options);
    CHECK(!nul.ok);
    auto bad_utf8 = dpm::manifest::load("{ name = \"\xff\" }", options);
    CHECK(!bad_utf8.ok);
}

TEST_CASE("a missing schema file is an error")
{
    TempDir td;
    dpm::manifest::LoadOptions options;
    options.schema_root = td.path;
    auto result = dpm::manifest::load(R"({ name = "a", version = "1.0.0" })", options);
    CHECK(!result.ok);
    CHECK(!result.diagnostics.empty());
}

namespace
{
    dpm::manifest::TemplateVariable variable(dpm::manifest::VariableKind kind)
    {
        dpm::manifest::TemplateVariable v;
        v.kind = kind;
        return v;
    }

    bool accepts(dpm::manifest::TemplateVariable const& v, std::string_view value)
    {
        return !dpm::manifest::template_value_error(v, value).has_value();
    }
} // namespace

TEST_CASE("the names of the shipped libraries are reserved")
{
    for (std::string_view name : {"std", "core", "assert", "dcext", "dpm"})
        CHECK(dpm::manifest::is_reserved_library_name(name));
    for (std::string_view name : {"Std", "std_ext", "stdx", "my_std", "", "cor", "dcc", "app"})
        CHECK(!dpm::manifest::is_reserved_library_name(name));
}

TEST_CASE("string values use a restricted character set")
{
    using VK = dpm::manifest::VariableKind;
    auto v = variable(VK::String);
    for (std::string_view ok : {"", "a", "Limine Template", "a_b-c.d,e:f/g+h@i(j)k#l%m", "caf\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", "x1"})
        CHECK(accepts(v, ok));
    for (std::string_view bad : std::initializer_list<std::string_view>{"a\"b", "a'b", "a\\b", "a`b", "a$b", "a<b", "a>b", "a&b", "a|b", "a;b", "a{b", "a}b", "a\nb", "a\tb", "a\rb", std::string_view{"a\0b", 3},
                                 "a@@b", "\x7F", "a=b", "a*b", "a?b", "a!b", "a~b", "a^b", "a[b", "a]b"})
        CHECK(!accepts(v, bad));
}

TEST_CASE("string values must be valid UTF-8 and within the length limit")
{
    using VK = dpm::manifest::VariableKind;
    auto v = variable(VK::String);
    for (std::string_view bad : {"\xC3", "\xC3(", "\xE2\x82", "\xF0\x9F\x98", "\x80", "\xC0\xAF", "\xC1\xBF", "\xE0\x80\xAF", "\xED\xA0\x80", "\xF0\x80\x80\xAF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFF"})
        CHECK(!accepts(v, bad));
    CHECK(accepts(v, std::string(128, 'a')));
    CHECK(!accepts(v, std::string(129, 'a')));
    v.max_length = 5;
    CHECK(accepts(v, "abcde"));
    CHECK(!accepts(v, "abcdef"));
    v.max_length = 256;
    CHECK(accepts(v, std::string(256, 'a')));
    CHECK(!accepts(v, std::string(257, 'a')));
}

TEST_CASE("identifier, integer, bool and choice values")
{
    using VK = dpm::manifest::VariableKind;
    auto id = variable(VK::Identifier);
    for (std::string_view ok : {"a", "_a", "A1", "kmain", "my_tool"})
        CHECK(accepts(id, ok));
    for (std::string_view bad : {"", "1a", "a-b", "a b", "a.b", "enum", "struct", "a/b", "..", "a\"b"})
        CHECK(!accepts(id, bad));
    auto num = variable(VK::Integer);
    for (std::string_view ok : {"0", "7", "-7", "123456789", "9223372036854775807", "-9223372036854775808"})
        CHECK(accepts(num, ok));
    for (std::string_view bad : {"", "-", "+1", "01", "-0", "00", "1.5", "1e3", "0x10", " 1", "1 ", "9223372036854775808", "a", "--1"})
        CHECK(!accepts(num, bad));
    num.min = 1;
    num.max = 64;
    CHECK(accepts(num, "1"));
    CHECK(accepts(num, "64"));
    CHECK(!accepts(num, "0"));
    CHECK(!accepts(num, "65"));
    num.min = -5;
    num.max = -1;
    CHECK(accepts(num, "-3"));
    CHECK(!accepts(num, "0"));
    auto flag = variable(VK::Bool);
    CHECK(accepts(flag, "true"));
    CHECK(accepts(flag, "false"));
    for (std::string_view bad : {"", "True", "yes", "1", "true "})
        CHECK(!accepts(flag, bad));
    auto choice = variable(VK::Choice);
    choice.choices = {"x86-64", "aarch64"};
    CHECK(accepts(choice, "x86-64"));
    CHECK(accepts(choice, "aarch64"));
    CHECK(!accepts(choice, "X86-64"));
    CHECK(!accepts(choice, ""));
    CHECK(!accepts(choice, "x86"));
}

TEST_CASE("glob patterns are checked for the supported syntax")
{
    for (std::string_view ok : {"a", "a/b", "*.dc", "src/**/*.dc", "**", "**/x", "dir/", "a?c", ".hidden", "a/.b", "x.*"})
        CHECK(!dpm::manifest::glob_error(ok).has_value());
    for (std::string_view bad : {"", "/", "/a", "!a", "a[1]", "a{b,c}", "a\\b", "a//b", "..", "a/../b", "a**b", "***", "a/**b", "**b/c", "a/", "//"})
    {
        auto problem = dpm::manifest::glob_error(bad);
        if (bad == "a/")
            CHECK(!problem.has_value());
        else
            CHECK(problem.has_value());
    }
}
