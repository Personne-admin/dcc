import std;
import dpm.cache;
import dpm.edit;
import dpm.manifest;
import dpm.publish;
import dpm.registry;

#include "harness.hh"

#include <stdlib.h>

namespace
{
    namespace pub = dpm::publish;
    namespace fs = std::filesystem;

    struct TempDir
    {
        fs::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("dpm_publish_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
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

        [[nodiscard]] std::string read(std::string const& rel) const
        {
            std::ifstream f{path / rel, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
        }
    };

    fs::path schema_root()
    {
        char const* value = ::getenv("DCC_TEST_DPM_SCHEMA");
        return value ? fs::path{value} : fs::path{};
    }

    pub::Context context(bool allow_file = false)
    {
        pub::Context ctx;
        ctx.schema_root = schema_root();
        ctx.allow_file = allow_file;
        return ctx;
    }

    std::string const k_commit(40, 'c');
    std::string const k_hash = "sha256:" + std::string(64, 'a');

    dpm::manifest::RegistryVersion make_version(std::string version)
    {
        dpm::manifest::RegistryVersion v;
        v.version = std::move(version);
        v.source = "git+https://github.com/u/json5.git";
        v.commit = k_commit;
        v.hash = k_hash;
        return v;
    }

    dpm::manifest::RegistryAlias make_alias()
    {
        dpm::manifest::RegistryAlias a;
        a.alias = "json5";
        a.location = "github.com/u/json5";
        a.description = "a \"quoted\" description";
        a.versions.push_back(make_version("1.0.0"));
        auto second = make_version("1.1.0");
        second.requires_dcc = "0.3.0";
        dpm::manifest::Dependency dep;
        dep.location = "github.com/u/dep";
        dep.version = "2.0.0";
        second.dependencies.push_back(dep);
        second.yanked = true;
        a.versions.push_back(second);
        return a;
    }

    bool contains(std::string const& text, std::string_view needle)
    {
        return text.find(needle) != std::string::npos;
    }

    void make_registry(TempDir const& td)
    {
        td.write("registry.dcon", "dpm::v1::RegistryConfig { name = \"test\" }\n");
    }

    bool has_problem(std::vector<pub::Problem> const& problems, std::string_view path, std::string_view fragment)
    {
        return std::ranges::any_of(problems, [&](pub::Problem const& p) { return p.path == path && contains(p.message, fragment); });
    }
} // namespace

SECTION("dpm publish");

TEST_CASE("an alias record prints canonically and loads back to the same record")
{
    if (schema_root().empty())
        return;
    auto record = make_alias();
    auto text = pub::print_alias(record);
    dpm::manifest::LoadOptions options;
    options.schema_root = schema_root();
    options.kind = dpm::manifest::FileKind::RegistryAlias;
    auto loaded = dpm::manifest::load(text, options);
    if (!loaded.ok)
        for (auto const& d : loaded.diagnostics)
            std::println(std::cerr, "{}", d.format());
    REQUIRE(loaded.ok);
    REQUIRE(loaded.registry_alias.has_value());
    auto const& back = *loaded.registry_alias;
    CHECK_EQ(back.alias, record.alias);
    CHECK_EQ(back.location, record.location);
    CHECK_EQ(back.description, record.description);
    REQUIRE(back.versions.size() == std::size_t{2});
    CHECK(!back.versions[0].yanked);
    CHECK(back.versions[1].yanked);
    CHECK_EQ(back.versions[1].requires_dcc, std::string{"0.3.0"});
    REQUIRE(back.versions[1].dependencies.size() == std::size_t{1});
    CHECK_EQ(back.versions[1].dependencies[0].location, std::string{"github.com/u/dep"});
    CHECK_EQ(pub::print_alias(back), text);
    record.versions.clear();
    auto empty = dpm::manifest::load(pub::print_alias(record), options);
    CHECK(empty.ok);
    CHECK_EQ(pub::print_pointer("json5"), std::string{"dpm::v1::RegistryLocation { alias = \"json5\" }\n"});
}

TEST_CASE("yanking edits one literal and keeps comments, spacing and neighbours")
{
    if (schema_root().empty())
        return;
    std::string text =
        "// registry record\n"
        "dpm::v1::RegistryAlias {\n"
        "    alias = \"json5\", location = \"github.com/u/json5\",\n"
        "    versions = {\n"
        "        // first\n"
        "        { version = \"1.0.0\", source = \"git+https://github.com/u/json5.git\", commit = \"" + k_commit + "\", hash = \"" + k_hash + "\" },\n"
        "        {\n"
        "            version = \"1.1.0\",\n"
        "            source = \"git+https://github.com/u/json5.git\",\n"
        "            commit = \"" + k_commit + "\",\n"
        "            hash = \"" + k_hash + "\",\n"
        "            yanked = false,\n"
        "        },\n"
        "    },\n"
        "}\n";
    dpm::manifest::LoadOptions options;
    options.schema_root = schema_root();
    auto first = dpm::edit::set_registry_yanked(text, options, "1.0.0", true);
    REQUIRE(first.ok);
    CHECK(contains(first.text, "// registry record"));
    CHECK(contains(first.text, "// first"));
    CHECK(contains(first.text, "yanked = true"));
    CHECK_EQ(first.text.size() - text.size(), std::string{", yanked = true"}.size());
    auto second = dpm::edit::set_registry_yanked(first.text, options, "1.1.0", true);
    REQUIRE(second.ok);
    CHECK(contains(second.text, "yanked = true,\n        },"));
    CHECK(!contains(second.text, "yanked = false"));
    auto undone = dpm::edit::set_registry_yanked(second.text, options, "1.1.0", false);
    REQUIRE(undone.ok);
    CHECK(contains(undone.text, "yanked = false,"));
    auto again = dpm::edit::set_registry_yanked(undone.text, options, "1.1.0", false);
    REQUIRE(again.ok);
    CHECK_EQ(again.text, undone.text);
    CHECK(!dpm::edit::set_registry_yanked(text, options, "9.9.9", true).ok);
    auto crlf = text;
    for (std::size_t pos = 0; (pos = crlf.find('\n', pos)) != std::string::npos; pos += 2)
        crlf.insert(pos, "\r");
    auto crlf_edit = dpm::edit::set_registry_yanked(crlf, options, "1.1.0", true);
    REQUIRE(crlf_edit.ok);
    CHECK(contains(crlf_edit.text, "yanked = true,\r\n"));
}

TEST_CASE("a registry check accepts a consistent registry and counts nothing wrong")
{
    if (schema_root().empty())
        return;
    TempDir td;
    make_registry(td);
    auto record = make_alias();
    td.write(dpm::registry::alias_path("json5"), pub::print_alias(record));
    td.write("locations/github.com/u/json5.dcon", pub::print_pointer("json5"));
    auto scan = pub::scan_registry(td.path, context());
    for (auto const& p : scan.problems)
        std::println(std::cerr, "{}", p.format());
    CHECK(scan.problems.empty());
    CHECK_EQ(scan.aliases.size(), std::size_t{1});
    CHECK_EQ(scan.pointers.size(), std::size_t{1});
}

TEST_CASE("a registry check reports every kind of inconsistency")
{
    if (schema_root().empty())
        return;
    TempDir td;
    auto record = make_alias();
    td.write(dpm::registry::alias_path("json5"), pub::print_alias(record));
    {
        auto scan = pub::scan_registry(td.path, context());
        CHECK(has_problem(scan.problems, "registry.dcon", "missing"));
        CHECK(has_problem(scan.problems, "aliases/js/json5.dcon", "missing location pointer"));
    }
    make_registry(td);
    td.write("locations/github.com/u/json5.dcon", pub::print_pointer("other"));
    td.write("locations/github.com/u/ghost.dcon", pub::print_pointer("json5"));
    td.write("locations/github.com/u/bad name.dcon", pub::print_pointer("json5"));
    td.write("locations/github.com/u/readme.txt", "x");
    auto other = make_alias();
    other.alias = "other";
    td.write(dpm::registry::alias_path("other"), pub::print_alias(other));
    td.write("aliases/zz/misplaced.dcon", pub::print_alias(make_alias()));
    td.write("aliases/js/notes.txt", "x");
    td.write("aliases/js/broken.dcon", "dpm::v1::RegistryAlias { alias = ");
    auto moved = make_alias();
    moved.alias = "mover";
    moved.location = "github.com/u/mover";
    moved.versions[0].source = "git+https://evil.example/u/mover.git";
    td.write(dpm::registry::alias_path("mover"), pub::print_alias(moved));
    td.write("locations/github.com/u/mover.dcon", pub::print_pointer("mover"));
    std::error_code ec;
    fs::create_symlink("/etc/passwd", td.path / "aliases/js/link.dcon", ec);
    auto scan = pub::scan_registry(td.path, context());
    auto const& p = scan.problems;
    CHECK(has_problem(p, "locations/github.com/u/json5.dcon", "names alias 'other' but github.com/u/json5 belongs to alias 'json5'"));
    CHECK(has_problem(p, "locations/github.com/u/ghost.dcon", "belongs to github.com/u/json5"));
    CHECK(has_problem(p, "locations/github.com/u/bad name.dcon", "not a valid location pointer path"));
    CHECK(has_problem(p, "locations/github.com/u/readme.txt", "must end in .dcon"));
    CHECK(has_problem(p, "aliases/zz/misplaced.dcon", "does not belong at this path"));
    CHECK(has_problem(p, "aliases/js/notes.txt", "must end in .dcon"));
    CHECK(has_problem(p, "aliases/js/broken.dcon", ""));
    CHECK(has_problem(p, "aliases/js/link.dcon", "not a regular file"));
    CHECK(has_problem(p, "aliases/mo/mover.dcon", "does not belong to github.com/u/mover"));
    CHECK(has_problem(p, "aliases/ot/other.dcon", "is already claimed") || has_problem(p, "aliases/js/json5.dcon", "is already claimed"));
    CHECK(!scan.problems.empty());
}

TEST_CASE("a registry check refuses oversized files and unreadable directories")
{
    if (schema_root().empty())
        return;
    TempDir td;
    make_registry(td);
    td.write(dpm::registry::alias_path("json5"), pub::print_alias(make_alias()) + std::string(dpm::registry::k_max_file + 1, ' '));
    td.write("locations/github.com/u/json5.dcon", pub::print_pointer("json5"));
    auto scan = pub::scan_registry(td.path, context());
    CHECK(has_problem(scan.problems, "aliases/js/json5.dcon", "larger than"));
}

TEST_CASE("creating an alias writes the record and the pointer and refuses duplicates")
{
    if (schema_root().empty())
        return;
    TempDir td;
    make_registry(td);
    std::string error;
    auto written = pub::create_alias(td.path, "github.com/u/json5", "json5", "parse JSON5", context(), false, error);
    REQUIRE(written.has_value());
    CHECK(written->changed);
    REQUIRE(written->pointer_rel.has_value());
    CHECK_EQ(*written->pointer_rel, std::string{"locations/github.com/u/json5.dcon"});
    CHECK(fs::exists(td.path / "aliases/js/json5.dcon"));
    CHECK(fs::exists(td.path / "locations/github.com/u/json5.dcon"));
    CHECK(pub::scan_registry(td.path, context()).problems.empty());

    CHECK(!pub::create_alias(td.path, "github.com/u/other", "json5", {}, context(), false, error).has_value());
    CHECK(contains(error, "already taken"));
    CHECK(!pub::create_alias(td.path, "github.com/u/json5", "second", {}, context(), false, error).has_value());
    CHECK(contains(error, "already claimed"));
    CHECK(!pub::create_alias(td.path, "github.com/u/x", "Bad", {}, context(), false, error).has_value());
    CHECK(!pub::create_alias(td.path, "github.com/u/x", "std", {}, context(), false, error).has_value());
    CHECK(!pub::create_alias(td.path, "json5", "valid", {}, context(), false, error).has_value());
    CHECK(!pub::create_alias(td.path, "path:../x", "valid", {}, context(), false, error).has_value());
    CHECK(!pub::create_alias(td.path, "git+file:///tmp/x", "valid", {}, context(), false, error).has_value());
    auto before = td.read("aliases/js/json5.dcon");
    auto dry = pub::create_alias(td.path, "github.com/u/dry", "dryrun", {}, context(), true, error);
    REQUIRE(dry.has_value());
    CHECK(dry->changed);
    CHECK(!fs::exists(td.path / "aliases/dr/dryrun.dcon"));
    CHECK_EQ(td.read("aliases/js/json5.dcon"), before);

    TempDir plain;
    CHECK(!pub::create_alias(plain.path, "github.com/u/x", "valid", {}, context(), false, error).has_value());
    CHECK(contains(error, "not a registry"));
}

TEST_CASE("set_yanked edits the record on disk and reports no change when it already matches")
{
    if (schema_root().empty())
        return;
    TempDir td;
    make_registry(td);
    td.write(dpm::registry::alias_path("json5"), pub::print_alias(make_alias()));
    td.write("locations/github.com/u/json5.dcon", pub::print_pointer("json5"));
    std::string error;
    auto first = pub::set_yanked(td.path, "json5", "1.0.0", true, context(), false, error);
    REQUIRE(first.has_value());
    CHECK(first->changed);
    CHECK(contains(td.read("aliases/js/json5.dcon"), "yanked = true"));
    auto again = pub::set_yanked(td.path, "json5", "1.0.0", true, context(), false, error);
    REQUIRE(again.has_value());
    CHECK(!again->changed);
    auto undo = pub::set_yanked(td.path, "json5", "1.1.0", false, context(), false, error);
    REQUIRE(undo.has_value());
    CHECK(undo->changed);
    CHECK(!pub::set_yanked(td.path, "json5", "7.7.7", true, context(), false, error).has_value());
    CHECK(!pub::set_yanked(td.path, "missing", "1.0.0", true, context(), false, error).has_value());
    CHECK(contains(error, "not in"));
    CHECK(pub::scan_registry(td.path, context()).problems.empty());
}

namespace
{
    dpm::manifest::RegistryTemplate make_template_record()
    {
        dpm::manifest::RegistryTemplate t;
        t.name = "limine-x86-64";
        t.location = "github.com/u/tpl";
        t.description = "a \"quoted\" description";
        auto first = make_version("1.0.0");
        first.source = "git+https://github.com/u/tpl.git";
        auto second = make_version("1.1.0");
        second.source = "git+https://github.com/u/tpl.git";
        second.requires_dcc = "0.3.0";
        second.yanked = true;
        t.versions = {first, second};
        return t;
    }

    std::string template_file(std::string const& name, std::string const& location, std::string const& versions = {})
    {
        return std::format("dpm::v1::RegistryTemplate {{ name = \"{}\", location = \"{}\", versions = {{ {} }} }}\n", name, location, versions);
    }

    std::string template_version_text(std::string const& version, std::string const& source)
    {
        return std::format("{{ version = \"{}\", source = \"{}\", commit = \"{}\", hash = \"{}\" }}", version, source, k_commit, k_hash);
    }
} // namespace

TEST_CASE("a template record prints canonically and loads back to the same record")
{
    if (schema_root().empty())
        return;
    auto record = make_template_record();
    auto text = pub::print_template(record);
    CHECK(text.starts_with("dpm::v1::RegistryTemplate {\n    name = \"limine-x86-64\",\n"));
    CHECK(!contains(text, "alias"));
    dpm::manifest::LoadOptions options;
    options.schema_root = schema_root();
    options.kind = dpm::manifest::FileKind::RegistryTemplate;
    auto loaded = dpm::manifest::load(text, options);
    if (!loaded.ok)
        for (auto const& d : loaded.diagnostics)
            std::println(std::cerr, "{}", d.format());
    REQUIRE(loaded.ok);
    REQUIRE(loaded.registry_template.has_value());
    auto const& back = *loaded.registry_template;
    CHECK_EQ(back.name, record.name);
    CHECK_EQ(back.description, record.description);
    REQUIRE(back.versions.size() == std::size_t{2});
    CHECK(back.versions[1].yanked);
    CHECK_EQ(back.versions[1].requires_dcc, std::string{"0.3.0"});
    CHECK_EQ(pub::print_template(back), text);
    record.versions.clear();
    auto empty = dpm::manifest::load(pub::print_template(record), options);
    REQUIRE(empty.ok);
    CHECK(empty.registry_template->versions.empty());
}

TEST_CASE("the registry check reads the templates directory")
{
    if (schema_root().empty())
        return;
    TempDir td;
    make_registry(td);
    td.write(dpm::registry::template_path("limine-x86-64"), pub::print_template(make_template_record()));
    td.write(dpm::registry::template_path("kernel"), template_file("kernel", "github.com/u/kernel-tpl"));
    auto scan = pub::scan_registry(td.path, context());
    CHECK(scan.problems.empty());
    CHECK_EQ(scan.templates.size(), std::size_t{2});
    CHECK(scan.templates.contains("kernel"));
    CHECK_EQ(scan.templates.at("limine-x86-64").versions.size(), std::size_t{2});
}

TEST_CASE("the registry check reports every kind of template inconsistency")
{
    if (schema_root().empty())
        return;
    TempDir td;
    make_registry(td);
    td.write("templates/zz/zzz.dcon", template_file("other", "github.com/u/a"));
    td.write(dpm::registry::template_path("kernel"), template_file("kernel", "github.com/u/k"));
    td.write("templates/ke/kernel2.dcon", template_file("kernel", "github.com/u/k2"));
    td.write(dpm::registry::template_path("twin"), template_file("twin", "github.com/u/k"));
    td.write(dpm::registry::template_path("moved"), template_file("moved", "github.com/u/moved", template_version_text("1.0.0", "git+https://evil.example/u/moved.git")));
    td.write("templates/bd/bad.dcon", "dpm::v1::RegistryTemplate { name = ");
    td.write("templates/notes.txt", "stray");
    td.write("templates/ke/readme.md", "stray");
    td.write(dpm::registry::template_path("ok-one"), template_file("ok-one", "github.com/u/ok"));
    auto scan = pub::scan_registry(td.path, context());
    CHECK(has_problem(scan.problems, "templates/zz/zzz.dcon", "does not belong at this path"));
    CHECK(has_problem(scan.problems, "templates/ke/kernel2.dcon", "does not belong at this path"));
    CHECK(has_problem(scan.problems, "templates/ke/kernel2.dcon", "is already defined"));
    bool claimed = has_problem(scan.problems, dpm::registry::template_path("kernel"), "already claimed by template") || has_problem(scan.problems, dpm::registry::template_path("twin"), "already claimed by template");
    CHECK(claimed);
    CHECK(has_problem(scan.problems, dpm::registry::template_path("moved"), "does not belong to github.com/u/moved"));
    CHECK(has_problem(scan.problems, "templates/bd/bad.dcon", ""));
    CHECK(has_problem(scan.problems, "templates/notes.txt", "unexpected entry"));
    CHECK(has_problem(scan.problems, "templates/ke/readme.md", "template records must end in .dcon"));
    CHECK(!has_problem(scan.problems, dpm::registry::template_path("ok-one"), ""));
}

TEST_CASE("a templates path that is a file is reported")
{
    if (schema_root().empty())
        return;
    TempDir td;
    make_registry(td);
    td.write("templates", "not a directory");
    auto scan = pub::scan_registry(td.path, context());
    CHECK(has_problem(scan.problems, "templates", "not a directory"));
}

TEST_CASE("creating a template writes one record, no pointer, and refuses duplicates")
{
    if (schema_root().empty())
        return;
    TempDir td;
    make_registry(td);
    std::string error;
    auto first = pub::create_template(td.path, "github.com/u/tpl", "kernel", "a kernel", context(), false, error);
    REQUIRE(first.has_value());
    CHECK(first->changed);
    CHECK_EQ(first->files, std::vector<std::string>{dpm::registry::template_path("kernel")});
    CHECK(!first->pointer_rel.has_value());
    CHECK(!fs::exists(td.path / "locations"));
    CHECK(contains(td.read(dpm::registry::template_path("kernel")), "description = \"a kernel\""));
    CHECK(pub::scan_registry(td.path, context()).problems.empty());
    CHECK(!pub::create_template(td.path, "github.com/u/other", "kernel", {}, context(), false, error).has_value());
    CHECK(contains(error, "already taken"));
    CHECK(!pub::create_template(td.path, "github.com/u/other", "KERNEL", {}, context(), false, error).has_value());
    CHECK(!pub::create_template(td.path, "github.com/u/tpl", "second", {}, context(), false, error).has_value());
    CHECK(contains(error, "already claimed by template 'kernel'"));
    CHECK(!pub::create_template(td.path, "someothername", "third", {}, context(), false, error).has_value());
    CHECK(!pub::create_template(td.path, "path:../x", "fourth", {}, context(), false, error).has_value());
    CHECK(!pub::create_template(td.path, "github.com/u/ok", "Bad Name", {}, context(), false, error).has_value());
    CHECK(contains(error, "not a valid template name"));
    CHECK(!fs::exists(td.path / dpm::registry::template_path("second")));
    auto dry = pub::create_template(td.path, "github.com/u/dry", "dry-run", {}, context(), true, error);
    REQUIRE(dry.has_value());
    CHECK(!fs::exists(td.path / dpm::registry::template_path("dry-run")));
    TempDir not_a_registry;
    CHECK(!pub::create_template(not_a_registry.path, "github.com/u/x", "kernel", {}, context(), false, error).has_value());
    CHECK(contains(error, "not a registry"));
}

TEST_CASE("a template alias and a package alias with the same name live side by side")
{
    if (schema_root().empty())
        return;
    TempDir td;
    make_registry(td);
    std::string error;
    REQUIRE(pub::create_alias(td.path, "github.com/u/json5", "shared", {}, context(), false, error).has_value());
    REQUIRE(pub::create_template(td.path, "github.com/u/shared-tpl", "shared", {}, context(), false, error).has_value());
    auto scan = pub::scan_registry(td.path, context());
    CHECK(scan.problems.empty());
    CHECK(scan.aliases.contains("shared"));
    CHECK(scan.templates.contains("shared"));
}

TEST_CASE("set_template_yanked edits the record on disk and reports no change when it already matches")
{
    if (schema_root().empty())
        return;
    TempDir td;
    make_registry(td);
    auto record = make_template_record();
    record.versions[1].yanked = false;
    td.write(dpm::registry::template_path(record.name), pub::print_template(record));
    std::string error;
    auto yanked = pub::set_template_yanked(td.path, record.name, "1.1.0", true, context(), false, error);
    REQUIRE(yanked.has_value());
    CHECK(yanked->changed);
    auto text = td.read(dpm::registry::template_path(record.name));
    CHECK(contains(text, "yanked = true"));
    CHECK(pub::scan_registry(td.path, context()).problems.empty());
    auto again = pub::set_template_yanked(td.path, record.name, "1.1.0", true, context(), false, error);
    REQUIRE(again.has_value());
    CHECK(!again->changed);
    auto undone = pub::set_template_yanked(td.path, record.name, "1.1.0", false, context(), false, error);
    REQUIRE(undone.has_value());
    CHECK(undone->changed);
    CHECK(contains(td.read(dpm::registry::template_path(record.name)), "yanked = false"));
    auto scan_after = pub::scan_registry(td.path, context());
    CHECK(scan_after.problems.empty());
    CHECK(!scan_after.templates.at(record.name).versions[1].yanked);
    CHECK(!pub::set_template_yanked(td.path, record.name, "9.9.9", true, context(), false, error).has_value());
    CHECK(!pub::set_template_yanked(td.path, "missing", "1.0.0", true, context(), false, error).has_value());
    CHECK(contains(error, "is not in"));
}

TEST_CASE("printed git commands quote every argument and are never run")
{
    pub::Written written;
    written.changed = true;
    written.files = {"aliases/js/json5.dcon", "it's a file"};
    auto commands = pub::git_commands("/tmp/a b", written, "publish-json5-1.0.0", "add json5 1.0.0");
    REQUIRE(commands.size() == std::size_t{5});
    CHECK(contains(commands[0], "cd '/tmp/a b'"));
    CHECK_EQ(commands[1], std::string{"git checkout -b 'publish-json5-1.0.0'"});
    CHECK(contains(commands[2], "'it'\\''s a file'"));
    CHECK_EQ(commands[3], std::string{"git commit -m 'add json5 1.0.0'"});
    CHECK_EQ(commands[4], std::string{"gh pr create --fill"});
}

TEST_CASE("init creates a registry that passes the check and grows its directories lazily")
{
    if (schema_root().empty())
        return;
    TempDir td;
    std::string error;
    auto dir = td.path / "my-registry";
    auto written = pub::init_registry(dir, {}, context(), error);
    REQUIRE(written.has_value());
    CHECK(written->changed);
    CHECK_EQ(td.read("my-registry/registry.dcon"), std::string{"dpm::v1::RegistryConfig {\n    format = 1,\n    name = \"my-registry\",\n}\n"});
    CHECK(fs::is_directory(dir / "aliases"));
    CHECK(fs::is_directory(dir / "locations"));
    auto scan = pub::scan_registry(dir, context());
    CHECK(scan.problems.empty());
    CHECK(scan.aliases.empty());

    fs::remove(dir / "aliases");
    fs::remove(dir / "locations");
    CHECK(pub::scan_registry(dir, context()).problems.empty());
    auto alias = pub::create_alias(dir, "github.com/u/json5", "json5", {}, context(), false, error);
    REQUIRE(alias.has_value());
    CHECK(fs::exists(dir / "aliases/js/json5.dcon"));
    CHECK(fs::exists(dir / "locations/github.com/u/json5.dcon"));
    CHECK(pub::scan_registry(dir, context()).problems.empty());

    auto named = pub::init_registry(td.path / "other", "dc \"registry\"", context(), error);
    REQUIRE(named.has_value());
    CHECK(contains(td.read("other/registry.dcon"), "name = \"dc \\\"registry\\\"\""));
    CHECK(pub::scan_registry(td.path / "other", context()).problems.empty());
    auto trailing = pub::init_registry(td.path / "third" / "", {}, context(), error);
    REQUIRE(trailing.has_value());
    CHECK(contains(td.read("third/registry.dcon"), "name = \"third\""));
}

TEST_CASE("init accepts what a fresh GitHub repository contains and nothing else")
{
    if (schema_root().empty())
        return;
    TempDir td;
    std::string error;
    td.write("fresh/README.md", "# registry\n");
    td.write("fresh/LICENSE", "text\n");
    td.write("fresh/.gitignore", "x\n");
    td.write("fresh/.git/HEAD", "ref: refs/heads/main\n");
    auto ok = pub::init_registry(td.path / "fresh", "dc-registry", context(), error);
    REQUIRE(ok.has_value());
    CHECK_EQ(td.read("fresh/README.md"), std::string{"# registry\n"});
    CHECK(fs::exists(td.path / "fresh/.git/HEAD"));
    CHECK(pub::scan_registry(td.path / "fresh", context()).problems.empty());

    td.write("busy/notes.txt", "x");
    CHECK(!pub::init_registry(td.path / "busy", {}, context(), error).has_value());
    CHECK(contains(error, "notes.txt"));
    CHECK(!fs::exists(td.path / "busy/registry.dcon"));

    td.write("again/registry.dcon", "dpm::v1::RegistryConfig { name = \"x\" }\n");
    CHECK(!pub::init_registry(td.path / "again", {}, context(), error).has_value());
    td.write("file", "x");
    CHECK(!pub::init_registry(td.path / "file", {}, context(), error).has_value());
    CHECK(contains(error, "not a directory"));
    CHECK(!pub::init_registry(td.path / "named", std::string(65, 'x'), context(), error).has_value());
    CHECK(!pub::init_registry(td.path / "named", "bad\nname", context(), error).has_value());
    CHECK(!fs::exists(td.path / "named"));
    td.write("link/real", "x");
    std::error_code ec;
    fs::create_directories(td.path / "linked");
    fs::create_symlink("/etc", td.path / "linked/etc", ec);
    CHECK(!pub::init_registry(td.path / "linked", {}, context(), error).has_value());
}
