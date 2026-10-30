import std;
import dpm.analyze;
import dpm.cache;
import dpm.lock;
import dpm.manifest;
import dpm.schema;

#include "harness.hh"

#include <stdlib.h>

namespace
{
    namespace an = dpm::analyze;
    namespace fs = std::filesystem;

    struct TempDir
    {
        fs::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("dpm_analyze_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
            fs::create_directories(path);
        }

        ~TempDir() { dpm::cache::detail::remove_tree(path); }

        void write(std::string const& rel, std::string const& content) const
        {
            auto full = path / rel;
            fs::create_directories(full.parent_path());
            std::ofstream f{full, std::ios::binary};
            f << content;
        }
    };

    fs::path schema_root()
    {
        char const* value = ::getenv("DCC_TEST_DPM_SCHEMA");
        return value ? fs::path{value} : fs::path{};
    }

    struct Marked
    {
        std::string text;
        std::uint32_t offset{};
    };

    Marked mark(std::string text)
    {
        auto at = text.find('|');
        Marked m;
        m.offset = static_cast<std::uint32_t>(at);
        text.erase(at, 1);
        m.text = std::move(text);
        return m;
    }

    an::Options options_for(TempDir const& dir, std::string file = "project.dcon")
    {
        an::Options options;
        options.schema_root = schema_root();
        options.file = dir.path / file;
        options.dcc_version = "0.3.0";
        options.cache_root = dir.path / "cache";
        return options;
    }

    bool contains(std::string const& text, std::string_view needle)
    {
        return text.find(needle) != std::string::npos;
    }

    std::string slice(std::string const& text, an::Finding const& f)
    {
        return text.substr(f.begin, f.end - f.begin);
    }

    an::Finding const* find_message(an::Result const& result, std::string_view fragment)
    {
        for (auto const& f : result.findings)
            if (contains(f.message, fragment))
                return &f;
        return nullptr;
    }

    std::string const k_hash = "sha256:" + std::string(64, 'a');

    std::string lock_text(dpm::manifest::Project const& project, std::vector<dpm::manifest::LockedPackage> packages)
    {
        dpm::manifest::Lock lock;
        lock.manifest = dpm::lock::fingerprint(project);
        lock.packages = std::move(packages);
        return dpm::lock::print(lock);
    }

    dpm::manifest::LockedPackage locked(std::string location, std::string version)
    {
        dpm::manifest::LockedPackage p;
        p.location = std::move(location);
        p.version = std::move(version);
        p.source = "git+https://github.com/u/a.git";
        p.commit = std::string(40, 'c');
        p.hash = k_hash;
        return p;
    }

    dpm::manifest::Project project_of(std::string const& text)
    {
        dpm::manifest::LoadOptions options;
        options.schema_root = schema_root();
        auto loaded = dpm::manifest::load(text, options);
        if (!loaded.ok || !loaded.project)
            return {};
        return *loaded.project;
    }
} // namespace

SECTION("dpm analyze");

TEST_CASE("the schema file parses into types, fields, variants and docs")
{
    auto root = schema_root();
    if (root.empty())
        return;
    auto schema = an::load_schema(root);
    REQUIRE(schema.has_value());
    auto const* project = schema->find("Project");
    REQUIRE(project != nullptr);
    CHECK(!project->is_enum);
    auto const* deps = project->field("dependencies");
    REQUIRE(deps != nullptr);
    CHECK_EQ(deps->type, std::string{"[]const Dependency"});
    CHECK(deps->has_default);
    CHECK(!deps->doc.empty());
    auto const* name = project->field("name");
    REQUIRE(name != nullptr);
    CHECK(!name->has_default);
    auto const* tri = schema->find("Tri");
    REQUIRE(tri != nullptr);
    CHECK(tri->is_enum);
    CHECK_EQ(tri->variants.size(), std::size_t{3});
    REQUIRE(tri->variant("On") != nullptr);
    CHECK(!tri->variant("On")->doc.empty());
    CHECK(schema->find("Nothing") == nullptr);

    std::ifstream in{root / "dpm" / "v1.dc"};
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    for (auto const& [type_name, type] : schema->types)
    {
        CHECK(contains(lines[type.line], type_name));
        for (auto const& f : type.fields)
            CHECK(contains(lines[f.line], f.name));
        for (auto const& v : type.variants)
            CHECK(contains(lines[v.line], v.name));
    }
}

TEST_CASE("the schema scanner agrees with the fields the loader decodes")
{
    auto root = schema_root();
    if (root.empty())
        return;
    auto schema = an::load_schema(root);
    REQUIRE(schema.has_value());
    std::string const sample_lock = "{ manifest = \"" + k_hash + "\", packages = {} }";
    std::vector<std::pair<std::string, std::pair<dpm::manifest::FileKind, std::string>>> cases{
        {"Project", {dpm::manifest::FileKind::Project, "{ name = \"a\", version = \"1.0.0\" }"}},
        {"Lock", {dpm::manifest::FileKind::Lock, sample_lock}},
        {"RegistryConfig", {dpm::manifest::FileKind::RegistryConfig, "{ name = \"r\" }"}},
        {"RegistryLocation", {dpm::manifest::FileKind::RegistryLocation, "{ alias = \"json5\" }"}},
        {"UserConfig", {dpm::manifest::FileKind::UserConfig, "{ }"}},
        {"RegistryAlias", {dpm::manifest::FileKind::RegistryAlias, "{ alias = \"json5\", location = \"github.com/u/p\", versions = {} }"}},
    };
    for (auto const& [type_name, entry] : cases)
    {
        dpm::manifest::LoadOptions options;
        options.schema_root = root;
        options.kind = entry.first;
        auto loaded = dpm::manifest::load(entry.second, options);
        REQUIRE(loaded.ok);
        auto const* type = schema->find(type_name);
        REQUIRE(type != nullptr);
        std::vector<std::string> from_tree;
        for (auto const& [field, node] : loaded.tree.fields)
            from_tree.push_back(field);
        std::vector<std::string> from_schema;
        for (auto const& f : type->fields)
            from_schema.push_back(f.name);
        CHECK_EQ(from_tree, from_schema);
    }
}

TEST_CASE("type strings split into a base and a list depth")
{
    auto a = dpm::schema::parse_type("[]const char");
    CHECK_EQ(a.base, std::string{"string"});
    CHECK_EQ(a.list_depth, 0);
    auto b = dpm::schema::parse_type("[]const []const char");
    CHECK_EQ(b.base, std::string{"string"});
    CHECK_EQ(b.list_depth, 1);
    auto c = dpm::schema::parse_type("[]const Dependency");
    CHECK_EQ(c.base, std::string{"Dependency"});
    CHECK_EQ(c.list_depth, 1);
    CHECK_EQ(dpm::schema::parse_type("Tri").list_depth, 0);
    CHECK(!dpm::schema::parse("public struct X\nnot a brace\n").has_value());
    CHECK(!dpm::schema::parse("public struct X\n{\n    broken line\n}\n").has_value());
    CHECK(!dpm::schema::parse("public struct X\n{\n    i32 a;\n").has_value());
    CHECK(dpm::schema::parse("").has_value());
}

TEST_CASE("manifest errors become findings with exact byte ranges")
{
    if (schema_root().empty())
        return;
    TempDir dir;
    std::string text = "dpm::v1::Project {\n    name = \"a\",\n    version = \"1.0.0\",\n    bogus = 1,\n}\n";
    auto result = an::analyze(text, options_for(dir));
    CHECK(!result.loaded);
    REQUIRE(!result.findings.empty());
    CHECK(result.findings[0].severity == an::Severity::Error);
    CHECK(contains(slice(text, result.findings[0]), "bogus"));
    auto clean = an::analyze("{ name = \"a\", version = \"1.0.0\" }", options_for(dir));
    CHECK(clean.loaded);
    CHECK(clean.findings.empty());
    auto broken = an::analyze("{ name = ", options_for(dir));
    CHECK(!broken.loaded);
    CHECK(!broken.findings.empty());
    std::string non_utf8 = "{ name = \"\xff\" }";
    CHECK(!an::analyze(non_utf8, options_for(dir)).loaded);
}

TEST_CASE("requires_dcc newer than the running dcc is a warning on that field")
{
    if (schema_root().empty())
        return;
    TempDir dir;
    std::string text = "{ name = \"a\", version = \"1.0.0\", requires_dcc = \"9.0.0\" }";
    auto result = an::analyze(text, options_for(dir));
    auto const* f = find_message(result, "requires dcc 9.0.0");
    REQUIRE(f != nullptr);
    CHECK(f->severity == an::Severity::Warning);
    CHECK_EQ(slice(text, *f), std::string{"\"9.0.0\""});
    auto older = an::analyze("{ name = \"a\", version = \"1.0.0\", requires_dcc = \"0.1.0\" }", options_for(dir));
    CHECK(older.findings.empty());
}

TEST_CASE("a lock next to the manifest explains mismatches on the dependency elements")
{
    if (schema_root().empty())
        return;
    TempDir dir;
    std::string text = "dpm::v1::Project {\n    name = \"a\",\n    version = \"1.0.0\",\n    dependencies = {\n"
                       "        { location = \"github.com/u/a\", version = \"1.2.0\" },\n"
                       "        { location = \"github.com/u/b\", version = \"1.0.0\" },\n    },\n}\n";
    auto project = project_of(text);
    REQUIRE(project.name == "a");
    dir.write("project.lock.dcon", lock_text(project, {locked("github.com/u/a", "1.1.0")}));
    auto result = an::analyze(text, options_for(dir));
    auto const* lower = find_message(result, "locked version 1.1.0 is lower than the required minimum 1.2.0");
    REQUIRE(lower != nullptr);
    CHECK(contains(slice(text, *lower), "github.com/u/a"));
    CHECK(lower->severity == an::Severity::Warning);
    auto const* missing = find_message(result, "dependency github.com/u/b has no lock entry");
    REQUIRE(missing != nullptr);
    CHECK(contains(slice(text, *missing), "github.com/u/b"));
    CHECK(find_message(result, "fingerprint") == nullptr);

    auto edited = text;
    edited.replace(edited.find("1.0.0\" },\n    },"), 5, "1.0.1");
    auto stale = an::analyze(edited, options_for(dir));
    auto const* fingerprint = find_message(stale, "fingerprint differs");
    REQUIRE(fingerprint != nullptr);
    CHECK(contains(slice(edited, *fingerprint), "dependencies") || contains(slice(edited, *fingerprint), "github.com/u/a"));

    dir.write("project.lock.dcon", lock_text(project, {locked("github.com/u/a", "1.2.0"), locked("github.com/u/b", "1.0.0")}));
    auto fine = an::analyze(text, options_for(dir));
    CHECK(find_message(fine, "no lock entry") == nullptr);
    CHECK(find_message(fine, "lower than") == nullptr);
    auto const* uncached = find_message(fine, "not in the dpm cache");
    REQUIRE(uncached != nullptr);
    CHECK(uncached->severity == an::Severity::Information);
    fs::create_directories(dir.path / "cache" / "src" / std::string(64, 'a'));
    auto cached = an::analyze(text, options_for(dir));
    CHECK(find_message(cached, "not in the dpm cache") == nullptr);
    CHECK(cached.findings.empty());

    dir.write("project.lock.dcon", "not a lock");
    auto broken = an::analyze(text, options_for(dir));
    CHECK(find_message(broken, "project.lock.dcon has errors") != nullptr);
}

TEST_CASE("alias dependencies and path dependencies are matched against the lock where that is possible")
{
    if (schema_root().empty())
        return;
    TempDir dir;
    std::string text = "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"json5\", version = \"1.0.0\" }, { location = \"path:../x\" } } }";
    auto project = project_of(text);
    REQUIRE(project.name == "a");
    auto path_package = locked("path:../x", "0.1.0");
    path_package.source = "path:../x";
    path_package.commit.clear();
    path_package.hash.clear();
    dir.write("project.lock.dcon", lock_text(project, {locked("git+https://example.com/json5.git", "1.0.0"), path_package}));
    auto result = an::analyze(text, options_for(dir));
    CHECK(find_message(result, "no lock entry") == nullptr);
    CHECK(find_message(result, "lower than") == nullptr);
}

TEST_CASE("a lock reports a stale manifest fingerprint and entries nothing requires")
{
    if (schema_root().empty())
        return;
    TempDir dir;
    std::string text = "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"github.com/u/a\", version = \"1.0.0\" } } }";
    auto project = project_of(text);
    REQUIRE(project.name == "a");
    dir.write("project.dcon", text);
    auto stale_owner = project;
    stale_owner.dependencies.clear();
    auto lock_a = locked("github.com/u/a", "1.0.0");
    auto lock_old = locked("github.com/u/old", "1.0.0");
    auto lock_dep = locked("github.com/u/dep", "1.0.0");
    lock_a.dependencies = {"github.com/u/dep"};
    auto text_lock = lock_text(project, {lock_a, lock_dep, lock_old});
    auto result = an::analyze(text_lock, options_for(dir, "project.lock.dcon"));
    CHECK(result.kind == dpm::manifest::FileKind::Lock);
    CHECK(result.loaded);
    auto const* hint = find_message(result, "github.com/u/old is no longer required");
    REQUIRE(hint != nullptr);
    CHECK(hint->severity == an::Severity::Hint);
    CHECK(contains(slice(text_lock, *hint), "github.com/u/old"));
    CHECK(find_message(result, "github.com/u/dep") == nullptr);
    CHECK(find_message(result, "different dependencies") == nullptr);

    auto other = lock_text(stale_owner, {lock_a, lock_dep});
    auto mismatch = an::analyze(other, options_for(dir, "project.lock.dcon"));
    auto const* warning = find_message(mismatch, "different dependencies");
    REQUIRE(warning != nullptr);
    CHECK(warning->severity == an::Severity::Warning);
}

TEST_CASE("field names are found in nested literals and list elements")
{
    auto root = schema_root();
    if (root.empty())
        return;
    auto schema = an::load_schema(root);
    REQUIRE(schema.has_value());
    auto at = [&](std::string source)
    {
        auto m = mark(std::move(source));
        return an::locate(m.text, dpm::manifest::FileKind::Project, *schema, m.offset);
    };
    auto top = at("dpm::v1::Project {\n    name = \"a\",\n    |\n}\n");
    CHECK(top.where == an::Where::FieldName);
    CHECK_EQ(top.struct_type, std::string{"Project"});
    CHECK_EQ(top.present, std::vector<std::string>{"name"});

    auto nested = at("{ name = \"a\", settings = { opt| } }");
    CHECK(nested.where == an::Where::FieldName);
    CHECK_EQ(nested.struct_type, std::string{"Settings"});
    CHECK_EQ(nested.prefix, std::string{"opt"});

    auto element = at("{ dependencies = { { location = \"x\", | } } }");
    CHECK(element.where == an::Where::FieldName);
    CHECK_EQ(element.struct_type, std::string{"Dependency"});
    CHECK_EQ(element.present, std::vector<std::string>{"location"});

    auto second = at("{ dependencies = { { location = \"x\" }, { | } } }");
    CHECK_EQ(second.struct_type, std::string{"Dependency"});
    CHECK(second.present.empty());

    auto value = at("{ settings = { optimize = Optimize::| } }");
    CHECK(value.where == an::Where::Value);
    CHECK_EQ(value.field, std::string{"optimize"});
    CHECK_EQ(value.value_type, std::string{"Optimize"});
    CHECK_EQ(value.qualified_variant_type, std::string{"Optimize"});

    auto in_string = at("{ dependencies = { { location = \"gith|ub\" } } }");
    CHECK(in_string.where == an::Where::Value);
    CHECK(in_string.in_string);
    CHECK_EQ(in_string.struct_type, std::string{"Dependency"});

    auto strings = at("{ settings = { extra_flags = { \"-g\", | } } }");
    CHECK(strings.where == an::Where::Value);
    CHECK_EQ(strings.value_type, std::string{"string"});

    auto outside = at("|{ name = \"a\" }");
    CHECK(outside.where == an::Where::None);
    auto after = at("{ name = \"a\" }|");
    CHECK(after.where == an::Where::None);
    auto constants = at("const MIN = \"1.0.0\";\nconst OTHER = 2;\n{ name = | }");
    CHECK_EQ(constants.constants, (std::vector<std::string>{"MIN", "OTHER"}));
    auto garbage = at("{ name = \"a\", ((( | ");
    CHECK(garbage.where == an::Where::None);
}

TEST_CASE("hover shows field, type and variant documentation and locked dependencies")
{
    auto root = schema_root();
    if (root.empty())
        return;
    auto schema = an::load_schema(root);
    REQUIRE(schema.has_value());
    TempDir dir;
    auto hover_at = [&](std::string source) { auto m = mark(std::move(source)); return an::hover(m.text, options_for(dir), *schema, m.offset); };

    auto field = hover_at("{ na|me = \"a\", version = \"1.0.0\" }");
    REQUIRE(field.has_value());
    CHECK(contains(field->markdown, "name"));
    CHECK(contains(field->markdown, "Field of `Project`"));
    CHECK_EQ(field->end - field->begin, std::uint32_t{4});

    auto defaulted = hover_at("{ name = \"a\", version = \"1.0.0\", test_di|r = \"x\" }");
    REQUIRE(defaulted.has_value());
    CHECK(contains(defaulted->markdown, "= \"tests\""));

    auto variant = hover_at("{ settings = { optimize = Optimize::O|2 } }");
    REQUIRE(variant.has_value());
    CHECK(contains(variant->markdown, "Optimize::O2"));
    CHECK(contains(variant->markdown, "-O2"));

    auto type = hover_at("{ settings = { optimize = Opti|mize::O2 } }");
    REQUIRE(type.has_value());
    CHECK(contains(type->markdown, "enum Optimize"));

    auto prefix = hover_at("dpm::v1::Proj|ect { name = \"a\" }");
    REQUIRE(prefix.has_value());
    CHECK(contains(prefix->markdown, "struct Project"));

    CHECK(!hover_at("{ name = \"a|\" }").has_value());
    CHECK(!hover_at("{ nam|e2 = \"a\" }").has_value());
    CHECK(!hover_at("| { }").has_value());

    std::string text = "{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"github.com/u/a\", version = \"1.0.0\" } } }";
    auto project = project_of(text);
    auto package = locked("github.com/u/a", "1.0.3");
    dir.write("project.lock.dcon", lock_text(project, {package}));
    auto marked = mark("{ name = \"a\", version = \"1.0.0\", dependencies = { { location = \"github.com/u|/a\", version = \"1.0.0\" } } }");
    auto dep = an::hover(marked.text, options_for(dir), *schema, marked.offset);
    REQUIRE(dep.has_value());
    CHECK(contains(dep->markdown, "Locked: version 1.0.3"));
    CHECK(contains(dep->markdown, std::string(40, 'c')));
}

TEST_CASE("completion offers missing fields, qualified enumerators, constants and cached aliases")
{
    auto root = schema_root();
    if (root.empty())
        return;
    auto schema = an::load_schema(root);
    REQUIRE(schema.has_value());
    TempDir dir;
    auto complete_at = [&](std::string source) { auto m = mark(std::move(source)); return an::complete(m.text, options_for(dir), *schema, m.offset); };
    auto labels = [](an::CompletionList const& list)
    {
        std::vector<std::string> out;
        for (auto const& c : list.items)
            out.push_back(c.label);
        return out;
    };

    auto fields = complete_at("{ name = \"a\", |}");
    auto names = labels(fields);
    CHECK(std::ranges::find(names, "version") != names.end());
    CHECK(std::ranges::find(names, "name") == names.end());
    CHECK(std::ranges::find(names, "dependencies") != names.end());
    REQUIRE(!fields.items.empty());
    CHECK(fields.items[0].snippet);
    CHECK(contains(fields.items[0].insert, "$0"));
    CHECK(!fields.items[0].documentation.empty() || !fields.items[1].documentation.empty());

    auto partial = complete_at("{ name = \"a\", ver| }");
    CHECK_EQ(partial.end - partial.begin, std::uint32_t{3});

    auto dependency_fields = complete_at("{ dependencies = { { location = \"x\", | } } }");
    CHECK_EQ(labels(dependency_fields), std::vector<std::string>{"version"});

    auto enumerators = complete_at("{ settings = { optimize = | } }");
    auto enum_names = labels(enumerators);
    CHECK(std::ranges::find(enum_names, "Optimize::O2") != enum_names.end());
    CHECK(std::ranges::find(enum_names, "Optimize::Size") != enum_names.end());
    auto qualified = complete_at("{ settings = { optimize = Optimize::| } }");
    auto variants = labels(qualified);
    CHECK(std::ranges::find(variants, "O2") != variants.end());
    CHECK(std::ranges::find(variants, "Optimize::O2") == variants.end());
    auto wrong = complete_at("{ settings = { optimize = Target::| } }");
    CHECK(wrong.items.empty());

    auto constants = complete_at("const LOW = \"1.0.0\";\n{ name = |}");
    CHECK_EQ(labels(constants), std::vector<std::string>{"LOW"});

    CHECK(complete_at("{ name = \"a\", version = \"|\" }").items.empty());

    dir.write("cache/registry/abc/aliases/js/json5.dcon", "x");
    dir.write("cache/registry/abc/aliases/zl/zlib.dcon", "x");
    auto aliases = complete_at("{ dependencies = { { location = \"j|\" } } }");
    CHECK_EQ(labels(aliases), (std::vector<std::string>{"json5", "zlib"}));
    CHECK(aliases.items[0].kind == an::Completion::Kind::Alias);
    CHECK(complete_at("|").items.empty());
}

TEST_CASE("go to definition points into the schema file")
{
    auto root = schema_root();
    if (root.empty())
        return;
    auto schema = an::load_schema(root);
    REQUIRE(schema.has_value());
    TempDir dir;
    std::ifstream in{root / "dpm" / "v1.dc"};
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    auto definition_at = [&](std::string source) { auto m = mark(std::move(source)); return an::definition(m.text, options_for(dir), *schema, m.offset); };

    auto field = definition_at("{ na|me = \"a\" }");
    REQUIRE(field.has_value());
    CHECK_EQ(field->file, root / "dpm" / "v1.dc");
    CHECK(contains(lines[field->line], "name"));
    CHECK(contains(lines[field->line], ";"));

    auto variant = definition_at("{ settings = { optimize = Optimize::O|2 } }");
    REQUIRE(variant.has_value());
    CHECK(contains(lines[variant->line], "O2"));

    auto type = definition_at("{ settings = { optimize = Opti|mize::O2 } }");
    REQUIRE(type.has_value());
    CHECK(contains(lines[type->line], "enum Optimize"));

    CHECK(!definition_at("{ name = \"a|\" }").has_value());
    CHECK(!definition_at("{ unknown| = 1 }").has_value());
}

TEST_CASE("analysis tolerates hostile input")
{
    auto root = schema_root();
    if (root.empty())
        return;
    auto schema = an::load_schema(root);
    REQUIRE(schema.has_value());
    TempDir dir;
    std::string big(dpm::manifest::k_max_file_size + 1, ' ');
    auto huge = an::analyze(big, options_for(dir));
    CHECK(!huge.loaded);
    CHECK(an::locate(big, dpm::manifest::FileKind::Project, *schema, 10).where == an::Where::None);
    std::string deep(5000, '{');
    CHECK(an::locate(deep, dpm::manifest::FileKind::Project, *schema, static_cast<std::uint32_t>(deep.size())).where == an::Where::None);
    std::string text = "{ name = \"a\", settings = { optimize = Optimize::O2 }, dependencies = { { location = \"x\" } } }";
    for (std::uint32_t offset = 0; offset <= text.size(); ++offset)
    {
        (void)an::locate(text, dpm::manifest::FileKind::Project, *schema, offset);
        (void)an::hover(text, options_for(dir), *schema, offset);
        (void)an::complete(text, options_for(dir), *schema, offset);
        (void)an::definition(text, options_for(dir), *schema, offset);
    }
    CHECK(an::locate(text, dpm::manifest::FileKind::Project, *schema, 100000).where == an::Where::None);
    for (std::size_t cut = 0; cut <= text.size(); ++cut)
    {
        auto partial = text.substr(0, cut);
        (void)an::analyze(partial, options_for(dir));
        (void)an::complete(partial, options_for(dir), *schema, static_cast<std::uint32_t>(cut));
    }
}
