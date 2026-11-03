export module dpm.cli;

import std;
import dcc.help;

export namespace dpm::cli
{
    enum class Opt : std::uint8_t
    {
        ManifestPath,
        Profile,
        Target,
        Optimize,
        Offline,
        Refresh,
        Locked,
        Verbose,
        Dev,
        DryRun,
        Check,
        Lib,
        Name,
        Runner,
        Timeout,
        Jobs,
        RegistryDir,
        Location,
        Base,
        NoBuild,
        VerifySources,
        Undo,
        Tree,
        All,
        Template,
        Set,
        Ref,
        Pin,
    };

    enum class Value : std::uint8_t
    {
        None,
        Text,
        Int,
    };

    struct OptionSpec
    {
        Opt id;
        std::string_view name;
        Value value;
        std::string_view metavar;
        std::string_view help;
        std::string_view long_help;
        std::span<std::string_view const> choices;
        std::string_view default_text;
        int min;
        int max;
    };

    inline constexpr std::string_view k_target_choices[] = {"host", "x86_64-elf", "x86_64-coff", "x86-elf", "x86-coff", "i8086-binary", "i8086-mz"};
    inline constexpr std::string_view k_optimize_choices[] = {"0", "1", "2", "s"};

    inline constexpr auto k_options = std::to_array<OptionSpec>({
        {Opt::ManifestPath, "--manifest-path", Value::Text, "<file>", "use this project.dcon instead of the one in the current directory",
         "The file must be named project.dcon; dpm works in its directory.", {}, "", 0, 0},
        {Opt::Profile, "--profile", Value::Text, "<name>", "build profile (debug, release, or one defined in project.dcon)", "", {}, "debug", 0, 0},
        {Opt::Target, "--target", Value::Text, "<triple>", "override the target of every product", "", k_target_choices, "from the manifest", 0, 0},
        {Opt::Optimize, "-O", Value::Text, "<level>", "override the optimization level", "", k_optimize_choices, "from the profile", 0, 0},
        {Opt::Offline, "--offline", Value::None, "", "never use the network; use only what is in the cache",
         "The same as DPM_OFFLINE=1. Something that is missing from the cache is an error (exit code 5).", {}, "", 0, 0},
        {Opt::Refresh, "--refresh", Value::None, "", "ignore the 24-hour freshness window of cached registry files", "", {}, "", 0, 0},
        {Opt::Locked, "--locked", Value::None, "", "fail instead of updating project.lock.dcon when it is missing or out of date", "", {}, "", 0, 0},
        {Opt::Verbose, "-v", Value::None, "", "print the dcc command lines", "", {}, "", 0, 0},
        {Opt::Dev, "--dev", Value::None, "", "use dev_dependencies instead of dependencies", "", {}, "", 0, 0},
        {Opt::DryRun, "--dry-run", Value::None, "", "show what would change and write nothing", "", {}, "", 0, 0},
        {Opt::Check, "--check", Value::None, "", "exit with code 4 instead of writing when the lock would change", "", {}, "", 0, 0},
        {Opt::Lib, "--lib", Value::None, "", "create a library (src/<name>/lib.dc) instead of an executable", "", {}, "", 0, 0},
        {Opt::Name, "--name", Value::Text, "<name>", "name to use instead of the directory name", "", {}, "", 0, 0},
        {Opt::Runner, "--runner", Value::Text, "<command>", "run programs through this command",
         "Required for products whose target is not the host. The command is split at spaces (no quoting) and gets the program as its last argument.", {}, "", 0, 0},
        {Opt::Timeout, "--timeout", Value::Int, "<seconds>", "stop a program that runs longer than this", "", {}, "60", 1, 86400},
        {Opt::Jobs, "-j", Value::Int, "<n>", "run this many tests at once", "", {}, "1", 1, 256},
        {Opt::RegistryDir, "--registry-dir", Value::Text, "<dir>", "registry clone to read and write", "The default is ./dc-registry if that directory exists.", {}, "", 0, 0},
        {Opt::Location, "--location", Value::Text, "<location>", "canonical location of the package, for an alias the registry does not have yet", "", {}, "", 0, 0},
        {Opt::Base, "--base", Value::Text, "<rev>", "with --verify-sources, skip versions that are already in this git revision of the registry", "", {}, "", 0, 0},
        {Opt::NoBuild, "--no-build", Value::None, "", "do not build the products first", "", {}, "", 0, 0},
        {Opt::VerifySources, "--verify-sources", Value::None, "",
         "fetch every version and check its commit, hash, alias claim and dependency summary",
         "This needs network access and fills the dpm cache.", {}, "", 0, 0},
        {Opt::Undo, "--undo", Value::None, "", "unyank instead of yank", "", {}, "", 0, 0},
        {Opt::Tree, "--tree", Value::None, "", "also print the decoded tree, with defaults", "", {}, "", 0, 0},
        {Opt::All, "--all", Value::None, "", "also remove installed packages and registry files", "", {}, "", 0, 0},
        {Opt::Template, "--template", Value::Text, "<source>", "generate the project from this template",
         "A built-in template (builtin:executable, builtin:library) or a directory that holds a template.dcon.", {}, "builtin:executable", 0, 0},
        {Opt::Set, "--set", Value::Text, "<var>=<value>", "give a template variable a value",
         "May be repeated. 'dpm template info <source>' lists the variables. --name is the same as --set name=<name>.", {}, "", 0, 0},
        {Opt::Ref, "--ref", Value::Text, "<ref>", "fetch this branch, tag or full commit id of a git template",
         "Without it the latest v<version> tag is used, or the tag named by <source>@<version>. A repository without version tags needs --ref.", {}, "", 0, 0},
        {Opt::Pin, "--pin", Value::Text, "<hash>", "require the fetched template to have this tree hash",
         "The hash is printed by every fetch ('sha256:' and 64 hex digits). A mismatch is an integrity error (exit code 4) and nothing is generated.", {}, "", 0, 0},
    });

    inline constexpr std::size_t k_unlimited = std::numeric_limits<std::size_t>::max();

    struct CommandSpec
    {
        std::string_view name;
        std::string_view synopsis;
        std::string_view summary;
        std::string_view description;
        std::span<Opt const> options;
        std::size_t min_args;
        std::size_t max_args;
        bool passthrough;
        std::uint8_t exit_codes;
        bool group;
    };

    namespace detail
    {
        constexpr std::uint8_t codes(std::initializer_list<int> list)
        {
            std::uint8_t mask = 0;
            for (int c : list)
                mask = static_cast<std::uint8_t>(mask | (1u << c));
            return mask;
        }

        inline constexpr Opt init_opts[] = {Opt::Lib, Opt::Name};
        inline constexpr Opt new_opts[] = {Opt::Template, Opt::Set, Opt::Lib, Opt::Name, Opt::Ref, Opt::Pin, Opt::Offline, Opt::Refresh};
        inline constexpr Opt template_info_opts[] = {Opt::Ref, Opt::Pin, Opt::Offline, Opt::Refresh};
        inline constexpr Opt add_opts[] = {Opt::ManifestPath, Opt::Dev, Opt::Offline, Opt::Refresh};
        inline constexpr Opt update_opts[] = {Opt::ManifestPath, Opt::DryRun, Opt::Offline, Opt::Refresh};
        inline constexpr Opt lock_opts[] = {Opt::ManifestPath, Opt::Check, Opt::Offline, Opt::Refresh};
        inline constexpr Opt fetch_opts[] = {Opt::ManifestPath, Opt::Offline, Opt::Refresh};
        inline constexpr Opt check_opts[] = {Opt::Tree};
        inline constexpr Opt build_opts[] = {Opt::ManifestPath, Opt::Profile, Opt::Target, Opt::Optimize, Opt::Offline, Opt::Refresh, Opt::Locked, Opt::Verbose};
        inline constexpr Opt run_opts[] = {Opt::ManifestPath, Opt::Profile, Opt::Target, Opt::Optimize, Opt::Offline, Opt::Refresh, Opt::Locked, Opt::Verbose, Opt::Runner};
        inline constexpr Opt test_opts[] = {Opt::ManifestPath, Opt::Profile, Opt::Target, Opt::Optimize, Opt::Offline, Opt::Refresh, Opt::Locked, Opt::Verbose,
                                            Opt::Runner, Opt::Timeout, Opt::Jobs};
        inline constexpr Opt task_opts[] = {Opt::ManifestPath, Opt::Profile, Opt::Target, Opt::Optimize, Opt::Offline, Opt::Refresh, Opt::Locked, Opt::Verbose, Opt::DryRun};
        inline constexpr Opt manifest_only[] = {Opt::ManifestPath};
        inline constexpr Opt graph_opts[] = {Opt::ManifestPath, Opt::Offline, Opt::Refresh};
        inline constexpr Opt cache_clean_opts[] = {Opt::All};
        inline constexpr Opt publish_opts[] = {Opt::ManifestPath, Opt::Profile, Opt::Offline, Opt::Refresh, Opt::Locked, Opt::Verbose, Opt::RegistryDir,
                                               Opt::Location, Opt::NoBuild, Opt::DryRun};
        inline constexpr Opt registry_init_opts[] = {Opt::Name};
        inline constexpr Opt registry_alias_opts[] = {Opt::RegistryDir, Opt::DryRun};
        inline constexpr Opt registry_check_opts[] = {Opt::RegistryDir, Opt::Offline, Opt::VerifySources, Opt::Base};
        inline constexpr Opt registry_yank_opts[] = {Opt::RegistryDir, Opt::DryRun, Opt::Undo};
    } // namespace detail

    inline constexpr std::string_view k_exit_code_text[] = {
        "success",
        "a build or test failed, a precondition was not met, or a file could not be written",
        "usage error",
        "invalid manifest, lock or configuration file",
        "resolution or integrity error (conflicting versions, hash mismatch, registry problems)",
        "network error, or something is missing from the cache while offline",
    };

    inline constexpr auto k_commands = std::to_array<CommandSpec>({
        {"init", "[DIR]", "create a package in DIR (default: the current directory)",
         "Writes project.dcon, src/main.dc (or src/<name>/lib.dc with --lib) and adds .dpm/ to .gitignore. The package name is the directory name unless "
         "--name is given. An existing project.dcon is never overwritten.",
         detail::init_opts, 0, 1, false, detail::codes({0, 1, 2}), false},
        {"new", "[DIR]", "create a project from a template",
         "Generates a project in DIR (default: the current directory, which must be empty) from a template and prints what to do next. Without --template "
         "the built-in executable template is used, or the library template with --lib. The template is a directory (path:DIR, ./DIR or /DIR), a git "
         "location (github.com/user/repo[/subdir], git+https://host/repo.git[#subdir], optionally followed by @version), an https .tar.gz, or the name "
         "of a registry template (name[@version]). A fetched template is checked like a dependency, cached by tree hash, and its commit and hash are "
         "printed; a registry template is fetched at the commit and hash its registry record lists. A template is data: nothing in it is run. The "
         "project is written into a temporary directory first and moved into place only when everything succeeded. Afterwards dpm lists the tasks and "
         "assets the new project declares; review project.dcon before running a task.",
         detail::new_opts, 0, 1, false, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"add", "<location>[@version]", "add a dependency",
         "Resolves the requirement, edits project.dcon in place (comments and layout are kept), writes project.lock.dcon and fetches the package. "
         "Without @version the latest release is used: from the registry for an alias, from the repository's version tags for a location. A failed "
         "resolution changes nothing.",
         detail::add_opts, 1, 1, false, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"remove", "<location>", "remove a dependency",
         "Removes the entry from project.dcon in place and writes the new project.lock.dcon. Packages stay in the cache.",
         detail::add_opts, 1, 1, false, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"update", "[<location>...]", "raise minimum versions to the latest release",
         "Raises the minimum version of the named dependencies (default: all of them) to the newest release of the same series, then writes "
         "project.lock.dcon. Registry aliases are looked up in the registry, locations in the repository's tags.",
         detail::update_opts, 0, k_unlimited, false, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"lock", "", "write project.lock.dcon",
         "Resolves the dependencies and assets in project.dcon and writes project.lock.dcon, fetching and checking every package and asset. Versions "
         "and assets already in the lock keep the commit and hash they were locked with.",
         detail::lock_opts, 0, 0, false, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"fetch", "", "download every locked package",
         "Downloads and verifies the packages and assets named in project.lock.dcon so that later builds and tasks work offline.",
         detail::fetch_opts, 0, 0, false, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"check", "[<file>]", "validate a manifest, lock or registry file",
         "Loads the file (default: project.dcon) the way dpm does and prints diagnostics. The kind of file comes from its typed prefix, else from its name. "
         "Nothing is fetched or run.",
         detail::check_opts, 0, 1, false, detail::codes({0, 2, 3}), false},
        {"build", "[<product>]", "build the products of the package",
         "Resolves and fetches the dependencies if the lock is not current, then runs dcc for the named product (default: all products). Output goes to "
         ".dpm/out/<profile>/<target>/, and compile_commands.json is updated.",
         detail::build_opts, 0, 1, false, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"run", "[<product>] [-- <args>...]", "build and run an executable product",
         "Builds the product, then runs it with the arguments after --. The exit code is the program's own when it ran. Programs for a target other than "
         "the host need --runner. A product whose run field names a task is built and then that task runs instead, as 'dpm task <name> -- <args>' would.",
         detail::run_opts, 0, 1, true, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"test", "[<name>...]", "build and run the test programs",
         "Builds and runs every <test_dir>/*.dc (or the named ones) with the dependencies and dev_dependencies. A test passes when it exits with 0.",
         detail::test_opts, 0, k_unlimited, false, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"task", "[<name>] [-- <args>...]", "run a task declared in project.dcon",
         "Without a name, lists the tasks of the project. With a name, builds the products the task and the tasks it needs list in build, fetches the "
         "assets they use, runs the needed tasks first and then the task, with the arguments after -- appended to its command. Commands run directly, "
         "without a shell, in the user's environment. A task with outputs is skipped while its command, inputs and outputs are unchanged. Only the "
         "tasks of this project run; tasks of dependencies and templates never do, and no other dpm command runs a task. With --dry-run the expanded "
         "commands are printed and nothing is built or run.",
         detail::task_opts, 0, 1, true, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"clean", "", "remove .dpm/out and .dpm/tasks", "Removes the build output and the task directories and stamps of this package. The cache is not touched; see 'dpm cache clean'.", detail::manifest_only, 0, 0, false,
         detail::codes({0, 1, 2}), false},
        {"tree", "", "print the resolved dependency graph", "Resolves the dependencies and prints the build list as a tree, followed by the assets and their pins from project.lock.dcon.", detail::graph_opts, 0, 0, false,
         detail::codes({0, 2, 3, 4, 5}), false},
        {"why", "<location>", "explain why a package is in the build list", "Prints the requirement paths that led to the package.", detail::graph_opts, 1, 1,
         false, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"publish", "", "write the registry entry for the current version",
         "Checks the package (alias, no path dependencies, requires_dcc, a clean git tree, a tag at HEAD, the version at the tag), builds it, computes the "
         "commit and the tree hash, and writes the alias record and location pointer into a registry clone. In a directory that has a template.dcon and "
         "no project.dcon it publishes the template instead: it must pass 'dpm template check', the version is the one tag (v1.2.3 or 1.2.3) that points at "
         "HEAD, and the record goes to templates/. It never pushes and runs no git command in the registry: it prints the commands for you to run.",
         detail::publish_opts, 0, 0, false, detail::codes({0, 1, 2, 3, 4, 5}), false},
        {"template", "<command>", "inspect and check project templates", "A template is a directory with a template.dcon and the files to generate; see 'dpm help new'.", {}, 0, 0,
         false, 0, true},
        {"template info", "<source>", "describe a template and its variables",
         "Loads the template the way 'dpm new' does and prints its name, description and variables with their kinds, defaults and limits. Nothing is generated.",
         detail::template_info_opts, 1, 1, false, detail::codes({0, 2, 3, 4, 5}), false},
        {"template check", "[<dir>]", "validate a template directory",
         "Loads template.dcon and the files under its root, generates the project with default values (sample values for required variables) in memory and "
         "validates it, including the generated project.dcon. Reports unused variables and patterns that match nothing as warnings.",
         {}, 0, 1, false, detail::codes({0, 2, 3}), false},
        {"cache", "<command>", "manage the package cache", "The cache is shared by all projects: DPM_HOME, else $XDG_CACHE_HOME/dpm, else ~/.cache/dpm.", {}, 0, 0, false, 0,
         true},
        {"cache path", "", "print the cache directory", "", {}, 0, 0, false, detail::codes({0, 2}), false},
        {"cache verify", "", "check that every cached package matches its hash",
         "Re-hashes each tree under src/ and compares it with its directory name.", {}, 0, 0, false, detail::codes({0, 2, 4}), false},
        {"cache clean", "", "remove downloads and git data from the cache",
         "Removes downloads, git repositories, staging data and verification stamps. Installed packages and registry files are kept unless --all is given.",
         detail::cache_clean_opts, 0, 0, false, detail::codes({0, 2}), false},
        {"registry", "<command>", "create and maintain a registry clone", "A registry is a git repository of alias records; see 'dpm help publish' for the usual way to add a version.", {}, 0, 0,
         false, 0, true},
        {"registry init", "<dir>", "create an empty registry in DIR",
         "Writes registry.dcon (format 1). The aliases/ and locations/ directories appear with the first alias. The directory may only contain .git, "
         ".gitignore, README* and LICENSE* files.",
         detail::registry_init_opts, 1, 1, false, detail::codes({0, 1, 2}), false},
        {"registry alias", "<location> <alias>", "reserve an alias for a location",
         "Writes an alias record without versions and its location pointer. Fails if the alias or the location is already taken.",
         detail::registry_alias_opts, 2, 2, false, detail::codes({0, 1, 2}), false},
        {"registry template", "<location> <name>", "reserve a name for a template",
         "Writes a template record without versions. Fails if the name or the location is already taken by another template. Templates have their own names: a "
         "template and a package alias may share one.",
         detail::registry_alias_opts, 2, 2, false, detail::codes({0, 1, 2}), false},
        {"registry yank-template", "<name> <version>", "mark a template version as yanked",
         "Sets yanked = true on one version of a template record, keeping the rest of the file as it is. New 'dpm new' runs skip a yanked version unless it is "
         "asked for by name.",
         detail::registry_yank_opts, 2, 2, false, detail::codes({0, 1, 2}), false},
        {"registry check", "[<dir>]", "validate a registry clone",
         "Checks registry.dcon, every alias record, every location pointer and every template record: file placement, sizes, ordering, unique names and "
         "locations, and that pointers and records agree. Nothing is fetched unless --verify-sources is given.",
         detail::registry_check_opts, 0, 1, false, detail::codes({0, 2, 4, 5}), false},
        {"registry yank", "<alias> <version>", "mark a version as yanked",
         "Sets yanked = true on one version of an alias record, keeping the rest of the file as it is. New resolutions skip a yanked version.",
         detail::registry_yank_opts, 2, 2, false, detail::codes({0, 1, 2}), false},
    });

    struct Parsed
    {
        std::string command;
        std::map<Opt, std::string> values;
        std::set<Opt> flags;
        std::vector<std::string> positional;
        std::vector<std::string> passthrough;
        std::vector<std::string> sets;
        bool help{};
        bool version{};
        std::string help_topic;

        [[nodiscard]] bool has(Opt o) const { return flags.contains(o) || values.contains(o) || (o == Opt::Set && !sets.empty()); }
        [[nodiscard]] std::optional<std::string> value(Opt o) const
        {
            if (auto it = values.find(o); it != values.end())
                return it->second;
            return std::nullopt;
        }
    };

    struct Failure
    {
        std::string message;
        std::string topic;
    };

    [[nodiscard]] inline CommandSpec const* find_command(std::string_view name)
    {
        for (auto const& c : k_commands)
            if (c.name == name)
                return &c;
        return nullptr;
    }

    [[nodiscard]] inline OptionSpec const* find_option(Opt id)
    {
        for (auto const& o : k_options)
            if (o.id == id)
                return &o;
        return nullptr;
    }

    [[nodiscard]] inline bool is_group(std::string_view name)
    {
        auto const* c = find_command(name);
        return c && c->group;
    }

    namespace detail
    {
        inline bool is_help_flag(std::string_view a)
        {
            return a == "--help" || a == "-h";
        }

        inline std::string pointer(std::string_view topic)
        {
            return topic.empty() ? std::string{"see 'dpm --help'"} : std::format("see 'dpm {} --help'", topic);
        }

        inline std::string join_words(std::span<std::string const> words)
        {
            std::string out;
            for (auto const& w : words)
                out += (out.empty() ? "" : " ") + w;
            return out;
        }
    } // namespace detail

    [[nodiscard]] inline std::expected<Parsed, Failure> parse(std::span<char const* const> args)
    {
        Parsed parsed;
        if (args.empty())
            return std::unexpected{Failure{"", ""}};
        std::string_view first{args[0]};
        if (detail::is_help_flag(first))
        {
            parsed.help = true;
            return parsed;
        }
        if (first == "help")
        {
            parsed.help = true;
            std::vector<std::string> words;
            for (auto a : args.subspan(1))
                words.emplace_back(a);
            parsed.help_topic = detail::join_words(words);
            if (!parsed.help_topic.empty() && !find_command(parsed.help_topic))
                return std::unexpected{Failure{std::format("dpm: no help for '{}' (see 'dpm --help')", parsed.help_topic), ""}};
            return parsed;
        }
        if (first == "--version")
        {
            parsed.version = true;
            return parsed;
        }
        if (first.starts_with('-'))
            return std::unexpected{Failure{std::format("dpm: unknown option {} (see 'dpm --help')", first), ""}};

        std::string name{first};
        std::size_t next = 1;
        if (is_group(first))
        {
            if (args.size() < 2)
                return std::unexpected{Failure{"", name}};
            std::string_view second{args[1]};
            if (detail::is_help_flag(second))
            {
                parsed.help = true;
                parsed.help_topic = name;
                return parsed;
            }
            name += " " + std::string{second};
            next = 2;
        }
        auto const* command = find_command(name);
        if (!command || command->group)
            return std::unexpected{Failure{std::format("dpm: unknown command '{}' ({})", name, next == 2 ? detail::pointer(std::string{first}) : detail::pointer("")), ""}};
        parsed.command = name;
        for (std::size_t i = next; i < args.size(); ++i)
        {
            std::string_view arg{args[i]};
            if (arg == "--")
            {
                if (!command->passthrough)
                    return std::unexpected{Failure{std::format("dpm {}: unexpected '--' ({})", name, detail::pointer(name)), ""}};
                for (++i; i < args.size(); ++i)
                    parsed.passthrough.emplace_back(args[i]);
                break;
            }
            if (detail::is_help_flag(arg))
            {
                parsed.help = true;
                parsed.help_topic = name;
                return parsed;
            }
            if (arg.size() > 1 && arg.starts_with('-'))
            {
                OptionSpec const* spec = nullptr;
                for (auto const& o : k_options)
                    if (o.name == arg)
                        spec = &o;
                bool accepted = false;
                if (spec)
                    for (auto id : command->options)
                        if (id == spec->id)
                            accepted = true;
                if (!spec)
                    return std::unexpected{Failure{std::format("dpm {}: unknown option {} ({})", name, arg, detail::pointer(name)), ""}};
                if (!accepted)
                    return std::unexpected{Failure{std::format("dpm {}: the option {} does not apply to this command ({})", name, arg, detail::pointer(name)), ""}};
                if (spec->value == Value::None)
                {
                    parsed.flags.insert(spec->id);
                    continue;
                }
                if (i + 1 >= args.size())
                    return std::unexpected{Failure{std::format("dpm {}: {} needs a value ({})", name, arg, detail::pointer(name)), ""}};
                std::string value{args[++i]};
                if (!spec->choices.empty() && std::ranges::find(spec->choices, value) == spec->choices.end())
                    return std::unexpected{Failure{std::format("dpm {}: invalid value '{}' for {} (expected one of: {})", name, value, arg,
                                                               dcc::help::join(spec->choices, ", ")),
                                                   ""}};
                if (spec->value == Value::Int)
                {
                    int number = 0;
                    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
                    if (ec != std::errc{} || ptr != value.data() + value.size() || number < spec->min || number > spec->max)
                        return std::unexpected{Failure{std::format("dpm {}: {} needs a number from {} to {} ({})", name, arg, spec->min, spec->max, detail::pointer(name)), ""}};
                }
                if (spec->id == Opt::Set)
                    parsed.sets.push_back(std::move(value));
                else
                    parsed.values[spec->id] = std::move(value);
                continue;
            }
            parsed.positional.emplace_back(arg);
        }
        if (parsed.positional.size() < command->min_args || parsed.positional.size() > command->max_args)
        {
            auto usage = std::format("dpm {}{}{}", name, command->synopsis.empty() ? "" : " ", command->synopsis);
            return std::unexpected{Failure{std::format("dpm {}: {} (usage: {}; {})", name, parsed.positional.size() < command->min_args ? "missing argument" : "too many arguments",
                                                       usage, detail::pointer(name)),
                                           ""}};
        }
        return parsed;
    }

    namespace detail
    {
        inline dcc::help::Flag flag_of(OptionSpec const& o)
        {
            return {o.name, "", o.value == Value::None ? dcc::help::ArgKind::None : dcc::help::ArgKind::Required, o.metavar};
        }

        inline dcc::help::Row row_of(OptionSpec const& o, bool long_form)
        {
            std::string help{o.help};
            if (long_form && !o.long_help.empty())
                help += ". " + std::string{o.long_help};
            return {dcc::help::flag_text(flag_of(o)), dcc::help::description_text(help, o.choices, {}, o.default_text)};
        }

        inline std::string usage_line(CommandSpec const& c)
        {
            return std::format("dpm {}{}{}", c.name, c.synopsis.empty() ? "" : " ", c.synopsis);
        }

        inline std::string leaf_flag(CommandSpec const& c)
        {
            return std::format("{}{}{}", c.name, c.synopsis.empty() ? "" : " ", c.synopsis);
        }

        inline std::size_t accepting(Opt id)
        {
            std::size_t n = 0;
            for (auto const& c : k_commands)
                for (auto o : c.options)
                    if (o == id)
                        ++n;
            return n;
        }
    } // namespace detail

    inline constexpr std::size_t k_common_threshold = 5;

    [[nodiscard]] inline std::string overview(int width)
    {
        std::string out = "usage: dpm <command> [options]\n       dpm help <command>\n       dpm <command> --help\n       dpm --version\n\ncommands:\n";
        std::vector<dcc::help::Row> rows;
        for (auto const& c : k_commands)
            if (!c.group)
                rows.push_back({detail::leaf_flag(c), std::string{c.summary}});
        out += dcc::help::format_rows(rows, width);
        std::vector<dcc::help::Row> common;
        for (auto const& o : k_options)
            if (detail::accepting(o.id) >= k_common_threshold)
                common.push_back(detail::row_of(o, false));
        out += "\ncommon options (see 'dpm help <command>' for the options of one command):\n";
        out += dcc::help::format_rows(common, width);
        return out;
    }

    [[nodiscard]] inline std::string command_page(CommandSpec const& c, int width)
    {
        std::string out;
        if (c.group)
        {
            out += std::format("usage: dpm {} <command>\n\n", c.name);
            out += dcc::help::format_wrapped(c.description, 0, width) + "\ncommands:\n";
            std::vector<dcc::help::Row> rows;
            for (auto const& sub : k_commands)
                if (!sub.group && sub.name.starts_with(std::string{c.name} + " "))
                    rows.push_back({std::string{sub.name.substr(c.name.size() + 1)}, std::string{sub.summary}});
            out += dcc::help::format_rows(rows, width);
            out += std::format("\nRun 'dpm help {} <command>' for the options and exit codes of one command.\n", c.name);
            return out;
        }
        out += "usage: " + detail::usage_line(c);
        if (!c.options.empty())
            out += " [options]";
        out += "\n\n";
        out += dcc::help::format_wrapped(c.description.empty() ? c.summary : c.description, 0, width);
        if (!c.options.empty())
        {
            std::vector<dcc::help::Row> rows;
            for (auto id : c.options)
                rows.push_back(detail::row_of(*find_option(id), true));
            out += "\noptions:\n" + dcc::help::format_rows(rows, width);
        }
        std::vector<dcc::help::Row> codes;
        for (int code = 0; code < 6; ++code)
            if (c.exit_codes & (1u << code))
                codes.push_back({std::to_string(code), std::string{k_exit_code_text[static_cast<std::size_t>(code)]}});
        out += "\nexit codes:\n" + dcc::help::format_rows(codes, width);
        return out;
    }

    [[nodiscard]] inline std::string help_text(std::string_view topic, int width)
    {
        if (topic.empty())
            return overview(width);
        auto const* c = find_command(topic);
        return c ? command_page(*c, width) : std::string{};
    }
} // namespace dpm::cli
