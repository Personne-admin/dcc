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
        for (auto kind : {K::Project, K::Lock, K::RegistryAlias, K::RegistryLocation, K::RegistryConfig, K::UserConfig})
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
        std::optional<std::string> expect_tree;
        std::optional<std::string> expect_full;
        std::optional<std::string> expect_errors;
        for (auto const& s : sections)
        {
            if (s.header.starts_with("FILE: "))
            {
                auto file = s.header.substr(6);
                if (file == "project.dcon" || file == "other.dcon")
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
                    options.kind = *kind;
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
