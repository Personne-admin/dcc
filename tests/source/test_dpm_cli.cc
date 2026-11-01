import std;
import dpm.build;
import dpm.cli;

#include "harness.hh"

#include <stdlib.h>

namespace
{
    namespace cli = dpm::cli;

    std::vector<std::string> split_words(std::string_view text)
    {
        std::vector<std::string> words;
        std::size_t pos = 0;
        while (pos < text.size())
        {
            auto end = text.find(' ', pos);
            words.emplace_back(text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
            pos = end == std::string_view::npos ? text.size() : end + 1;
        }
        return words;
    }

    std::expected<cli::Parsed, cli::Failure> run(std::vector<std::string> const& words)
    {
        std::vector<char const*> args;
        for (auto const& w : words)
            args.push_back(w.c_str());
        return cli::parse(args);
    }

    std::vector<std::string> command_words(cli::CommandSpec const& c, std::size_t positionals)
    {
        auto words = split_words(c.name);
        for (std::size_t i = 0; i < positionals; ++i)
            words.push_back("arg" + std::to_string(i));
        return words;
    }

    std::string sample_value(cli::OptionSpec const& o)
    {
        if (!o.choices.empty())
            return std::string{o.choices[0]};
        if (o.value == cli::Value::Int)
            return std::to_string(o.min);
        return "value";
    }

    std::set<std::string> option_rows(std::string const& page, std::string_view section)
    {
        std::set<std::string> names;
        auto start = page.find(std::string{section} + ":\n");
        if (start == std::string::npos)
            return names;
        std::size_t pos = start + section.size() + 2;
        while (pos < page.size())
        {
            auto end = page.find('\n', pos);
            auto line = page.substr(pos, end - pos);
            if (line.empty())
                break;
            if (line.starts_with("  -"))
            {
                auto word_end = line.find(' ', 2);
                names.insert(line.substr(2, word_end == std::string::npos ? std::string::npos : word_end - 2));
            }
            pos = end + 1;
        }
        return names;
    }

    std::string read_golden(std::string const& name)
    {
        char const* dir = ::getenv("DCC_TEST_DPM_HELP");
        if (!dir)
            return {};
        std::ifstream in{std::filesystem::path{dir} / name, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }
} // namespace

SECTION("dpm command line");

TEST_CASE("the tables are consistent")
{
    std::set<std::string_view> names;
    for (auto const& c : cli::k_commands)
    {
        CHECK(names.insert(c.name).second);
        CHECK(!c.summary.empty());
        CHECK(c.min_args <= c.max_args);
        if (!c.group)
        {
            CHECK(!c.description.empty() || !c.summary.empty());
            CHECK(c.exit_codes & 1u);
            CHECK(c.exit_codes & 4u);
            std::set<cli::Opt> seen;
            for (auto o : c.options)
            {
                CHECK(seen.insert(o).second);
                CHECK(cli::find_option(o) != nullptr);
            }
        }
        else
            CHECK(c.name.find(' ') == std::string_view::npos);
        if (c.name.find(' ') != std::string_view::npos)
            CHECK(cli::is_group(c.name.substr(0, c.name.find(' '))));
    }
    std::set<std::string_view> option_names;
    for (auto const& o : cli::k_options)
    {
        CHECK(option_names.insert(o.name).second);
        CHECK(o.name.starts_with("-"));
        CHECK(!o.help.empty());
        CHECK_EQ(o.value == cli::Value::None, o.metavar.empty());
        std::size_t users = 0;
        for (auto const& c : cli::k_commands)
            for (auto id : c.options)
                if (id == o.id)
                    ++users;
        CHECK(users > 0);
    }
    for (auto target : cli::k_target_choices)
        CHECK(dpm::build::parse_target(target).has_value());
}

TEST_CASE("every option is accepted by exactly the commands that list it")
{
    for (auto const& c : cli::k_commands)
    {
        if (c.group)
            continue;
        for (auto const& o : cli::k_options)
        {
            bool listed = std::ranges::find(c.options, o.id) != c.options.end();
            auto words = command_words(c, c.min_args);
            words.emplace_back(o.name);
            if (o.value != cli::Value::None)
                words.push_back(sample_value(o));
            auto parsed = run(words);
            CHECK_EQ(parsed.has_value(), listed);
            if (parsed)
                CHECK(parsed->has(o.id));
            else
                CHECK(parsed.error().message.find("'dpm " + std::string{c.name} + " --help'") != std::string::npos);
        }
    }
}

TEST_CASE("the help of a command lists exactly the options it accepts")
{
    for (auto const& c : cli::k_commands)
    {
        if (c.group)
            continue;
        auto page = cli::help_text(c.name, 80);
        REQUIRE(!page.empty());
        std::set<std::string> expected;
        for (auto id : c.options)
            expected.insert(std::string{cli::find_option(id)->name});
        CHECK_EQ(option_rows(page, "options"), expected);
        CHECK(page.starts_with("usage: dpm " + std::string{c.name}));
        for (int code = 0; code < 6; ++code)
        {
            auto line = std::format("\n  {} ", code);
            CHECK_EQ(page.find(line) != std::string::npos, (c.exit_codes & (1u << code)) != 0);
        }
    }
}

TEST_CASE("the overview lists every command and the options most commands share")
{
    auto text = cli::help_text("", 80);
    CHECK(text.starts_with("usage: dpm <command> [options]\n"));
    for (auto const& c : cli::k_commands)
        if (!c.group)
            CHECK(text.find("\n  " + std::string{c.name}) != std::string::npos);
    std::set<std::string> expected;
    for (auto const& o : cli::k_options)
    {
        std::size_t users = 0;
        for (auto const& c : cli::k_commands)
            if (std::ranges::find(c.options, o.id) != c.options.end())
                ++users;
        if (users >= cli::k_common_threshold)
            expected.insert(std::string{o.name});
    }
    CHECK_EQ(option_rows(text, "common options (see 'dpm help <command>' for the options of one command)"), expected);
    CHECK(!expected.empty());
}

TEST_CASE("help works as 'dpm help X' and 'dpm X --help' for every command and group")
{
    auto overview = run({"help"});
    REQUIRE(overview.has_value());
    CHECK(overview->help);
    CHECK(overview->help_topic.empty());
    for (auto flag : {"--help", "-h"})
    {
        auto top = run({flag});
        REQUIRE(top.has_value());
        CHECK(top->help && top->help_topic.empty());
    }
    for (auto const& c : cli::k_commands)
    {
        auto words = split_words(c.name);
        auto topic = std::string{c.name};
        auto help_form = words;
        help_form.insert(help_form.begin(), "help");
        auto a = run(help_form);
        REQUIRE(a.has_value());
        CHECK(a->help);
        CHECK_EQ(a->help_topic, topic);
        auto b_words = words;
        b_words.emplace_back("--help");
        auto b = run(b_words);
        REQUIRE(b.has_value());
        CHECK(b->help);
        CHECK_EQ(b->help_topic, topic);
        auto page = cli::help_text(topic, 80);
        CHECK(page.starts_with(c.group ? "usage: dpm " + topic + " <command>" : "usage: dpm " + topic));
        auto after_options = words;
        after_options.emplace_back("--bogus-but-help-wins");
        after_options.emplace_back("--help");
        if (!c.group)
            CHECK(run(after_options).has_value() == false || run(after_options)->help == false);
    }
    CHECK(!run({"help", "nothing"}).has_value());
    CHECK(run({"help", "nothing"}).error().message.find("dpm --help") != std::string::npos);
}

TEST_CASE("help text fits the width, uses ASCII and has no trailing spaces")
{
    for (int width : {80, 120})
        for (auto const& c : cli::k_commands)
        {
            auto page = cli::help_text(c.name, width);
            for (auto ch : page)
                CHECK(static_cast<unsigned char>(ch) < 0x80);
            std::size_t pos = 0;
            while (pos < page.size())
            {
                auto end = page.find('\n', pos);
                auto line = page.substr(pos, end - pos);
                CHECK(line.empty() || line.back() != ' ');
                CHECK(static_cast<int>(line.size()) <= width);
                pos = end + 1;
            }
        }
}

TEST_CASE("errors are short and point at the right help")
{
    auto bare = run({});
    REQUIRE(!bare.has_value());
    CHECK(bare.error().message.empty());
    CHECK(bare.error().topic.empty());

    auto unknown = run({"frobnicate"});
    REQUIRE(!unknown.has_value());
    CHECK_EQ(unknown.error().message, std::string{"dpm: unknown command 'frobnicate' (see 'dpm --help')"});
    CHECK_EQ(run({"--frobnicate"}).error().message, std::string{"dpm: unknown option --frobnicate (see 'dpm --help')"});

    auto group = run({"cache"});
    REQUIRE(!group.has_value());
    CHECK(group.error().message.empty());
    CHECK_EQ(group.error().topic, std::string{"cache"});
    CHECK_EQ(run({"cache", "frob"}).error().message, std::string{"dpm: unknown command 'cache frob' (see 'dpm cache --help')"});
    CHECK_EQ(run({"registry", "frob"}).error().message, std::string{"dpm: unknown command 'registry frob' (see 'dpm registry --help')"});

    CHECK_EQ(run({"build", "--bogus"}).error().message, std::string{"dpm build: unknown option --bogus (see 'dpm build --help')"});
    CHECK_EQ(run({"init", "--dev"}).error().message, std::string{"dpm init: the option --dev does not apply to this command (see 'dpm init --help')"});
    CHECK(run({"add"}).error().message.find("missing argument (usage: dpm add <location>[@version]; see 'dpm add --help')") != std::string::npos);
    CHECK(run({"add", "a", "b"}).error().message.find("too many arguments") != std::string::npos);
    CHECK(run({"build", "--profile"}).error().message.find("--profile needs a value") != std::string::npos);
    CHECK(run({"build", "-O", "3"}).error().message.find("expected one of: 0, 1, 2, s") != std::string::npos);
    CHECK(run({"test", "-j", "0"}).error().message.find("-j needs a number from 1 to 256") != std::string::npos);
    CHECK(run({"test", "-j", "12x"}).error().message.find("-j needs a number") != std::string::npos);
    CHECK(run({"test", "--timeout", "-5"}).error().message.find("--timeout needs a number") != std::string::npos);
    CHECK(run({"build", "--", "x"}).error().message.find("unexpected '--'") != std::string::npos);
    for (auto const& words : std::vector<std::vector<std::string>>{{"frobnicate"}, {"build", "--bogus"}, {"cache", "frob"}})
        CHECK(run(words).error().message.size() < 100);
}

TEST_CASE("arguments, values and pass-through arguments are collected")
{
    auto build = run({"build", "--profile", "release", "-O", "s", "--offline", "tool", "--target", "i8086-mz", "-v"});
    REQUIRE(build.has_value());
    CHECK_EQ(build->command, std::string{"build"});
    CHECK_EQ(build->value(cli::Opt::Profile).value_or(""), std::string{"release"});
    CHECK_EQ(build->value(cli::Opt::Optimize).value_or(""), std::string{"s"});
    CHECK_EQ(build->value(cli::Opt::Target).value_or(""), std::string{"i8086-mz"});
    CHECK(build->has(cli::Opt::Offline));
    CHECK(build->has(cli::Opt::Verbose));
    CHECK(!build->has(cli::Opt::Locked));
    CHECK_EQ(build->positional, std::vector<std::string>{"tool"});

    auto program = run({"run", "app", "--", "--offline", "-x", "a b"});
    REQUIRE(program.has_value());
    CHECK_EQ(program->positional, std::vector<std::string>{"app"});
    CHECK_EQ(program->passthrough, (std::vector<std::string>{"--offline", "-x", "a b"}));
    CHECK(!program->has(cli::Opt::Offline));

    auto repeated = run({"build", "--profile", "a", "--profile", "b"});
    REQUIRE(repeated.has_value());
    CHECK_EQ(repeated->value(cli::Opt::Profile).value_or(""), std::string{"b"});

    auto dash = run({"add", "-"});
    REQUIRE(dash.has_value());
    CHECK_EQ(dash->positional, std::vector<std::string>{"-"});

    auto sub = run({"registry", "yank", "json5", "1.0.0", "--undo", "--registry-dir", "r"});
    REQUIRE(sub.has_value());
    CHECK_EQ(sub->command, std::string{"registry yank"});
    CHECK(sub->has(cli::Opt::Undo));
    CHECK_EQ(sub->positional, (std::vector<std::string>{"json5", "1.0.0"}));
    auto many = run({"update", "a", "b", "c"});
    REQUIRE(many.has_value());
    CHECK_EQ(many->positional.size(), std::size_t{3});
}

TEST_CASE("--version is a request of its own")
{
    auto parsed = run({"--version"});
    REQUIRE(parsed.has_value());
    CHECK(parsed->version);
    CHECK(!parsed->help);
    CHECK(parsed->command.empty());
    auto plain = run({"build"});
    REQUIRE(plain.has_value());
    CHECK(!plain->version);
    CHECK(!run({"build", "--version"}).has_value());
    CHECK(cli::help_text("", 80).find("dpm --version") != std::string::npos);
}

TEST_CASE("new collects repeated --set values in order and the template source")
{
    auto parsed = run({"new", "proj", "--template", "path:t", "--set", "a=1", "--name", "kern", "--set", "b=two words", "--set", "a=3"});
    REQUIRE(parsed.has_value());
    CHECK_EQ(parsed->command, std::string{"new"});
    CHECK_EQ(parsed->positional, (std::vector<std::string>{"proj"}));
    CHECK_EQ(parsed->value(cli::Opt::Template).value_or(""), std::string{"path:t"});
    CHECK_EQ(parsed->value(cli::Opt::Name).value_or(""), std::string{"kern"});
    CHECK_EQ(parsed->sets, (std::vector<std::string>{"a=1", "b=two words", "a=3"}));
    CHECK(parsed->has(cli::Opt::Set));
    CHECK(!run({"new", "--set"}).has_value());
    CHECK(!run({"new", "a", "b"}).has_value());
    auto bare = run({"new"});
    REQUIRE(bare.has_value());
    CHECK(bare->positional.empty());
    CHECK(!bare->has(cli::Opt::Set));
}

TEST_CASE("--set and --template belong to dpm new only")
{
    for (auto const& command : {"init", "build", "run", "add", "check"})
    {
        auto parsed = run({command, "--set", "a=1"});
        CHECK(!parsed.has_value());
        auto with_template = run({command, "--template", "x"});
        CHECK(!with_template.has_value());
    }
}

TEST_CASE("--ref and --pin belong to commands that fetch templates")
{
    auto fetched = run({"new", "dir", "--template", "github.com/u/r", "--ref", "main", "--pin", "sha256:aa", "--offline"});
    REQUIRE(fetched.has_value());
    CHECK_EQ(fetched->value(cli::Opt::Ref).value_or(""), std::string{"main"});
    CHECK_EQ(fetched->value(cli::Opt::Pin).value_or(""), std::string{"sha256:aa"});
    CHECK(fetched->has(cli::Opt::Offline));
    auto info = run({"template", "info", "github.com/u/r@1.0.0", "--ref", "x", "--pin", "y", "--offline"});
    REQUIRE(info.has_value());
    CHECK_EQ(info->positional, (std::vector<std::string>{"github.com/u/r@1.0.0"}));
    CHECK(!run({"template", "check", "--ref", "x"}).has_value());
    CHECK(!run({"template", "check", "--pin", "x"}).has_value());
    for (auto const& command : {"init", "build", "add", "check", "fetch"})
    {
        CHECK(!run({command, "--ref", "x"}).has_value());
        CHECK(!run({command, "--pin", "x"}).has_value());
    }
    CHECK(!run({"new", "--ref"}).has_value());
    CHECK(!run({"new", "--pin"}).has_value());
}

TEST_CASE("template commands take their arguments")
{
    auto info = run({"template", "info", "src"});
    REQUIRE(info.has_value());
    CHECK_EQ(info->command, std::string{"template info"});
    CHECK(!run({"template", "info"}).has_value());
    CHECK(!run({"template", "info", "a", "b"}).has_value());
    auto check = run({"template", "check"});
    REQUIRE(check.has_value());
    CHECK(check->positional.empty());
    CHECK(run({"template", "check", "dir"}).has_value());
    CHECK(!run({"template", "check", "a", "b"}).has_value());
    CHECK(!run({"template"}).has_value());
}

TEST_CASE("golden help output")
{
    if (!::getenv("DCC_TEST_DPM_HELP"))
        return;
    CHECK_EQ(cli::help_text("", 80), read_golden("overview.txt"));
    CHECK_EQ(cli::help_text("build", 80), read_golden("build.txt"));
    CHECK_EQ(cli::help_text("registry", 80), read_golden("registry.txt"));
    CHECK_EQ(cli::help_text("registry check", 80), read_golden("registry-check.txt"));
    CHECK_EQ(cli::help_text("new", 80), read_golden("new.txt"));
    CHECK_EQ(cli::help_text("template", 80), read_golden("template.txt"));
    CHECK(!read_golden("overview.txt").empty());
}
