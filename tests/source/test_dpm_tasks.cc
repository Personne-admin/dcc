import std;
import dpm.cache;
import dpm.expand;
import dpm.hash;
import dpm.manifest;
import dpm.process;
import dpm.stamp;
import dpm.task;

#include "harness.hh"

namespace
{
    namespace fs = std::filesystem;
    namespace tk = dpm::task;
    namespace st = dpm::stamp;
    using Clock = fs::file_time_type::clock;

    struct TempDir
    {
        fs::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("dpm_tasks_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
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

    tk::Context context(fs::path const& root)
    {
        tk::Context c;
        c.root = root;
        c.profile = "debug";
        c.dcc = "/opt/dcc/bin/dcc";
        c.outs["kern"] = root / ".dpm/out/debug/x86_64-elf/kern";
        c.targets["kern"] = "x86_64-elf";
        c.outs["my-tool"] = root / ".dpm/out/debug/host/my-tool";
        c.targets["my-tool"] = "host";
        c.assets["limine"] = "/cache/src/abc";
        c.asset_hashes["limine"] = "sha256:" + std::string(64, 'a');
        return c;
    }

    dpm::manifest::Task task(std::string name, std::vector<std::string> command)
    {
        dpm::manifest::Task t;
        t.name = std::move(name);
        t.command = std::move(command);
        t.cwd = ".";
        return t;
    }

    std::string join(std::vector<std::string> const& v)
    {
        std::string out;
        for (auto const& s : v)
            out += (out.empty() ? "" : "|") + s;
        return out;
    }
} // namespace

SECTION("dpm task tokens");

TEST_CASE("the scanner accepts exactly the closed token set")
{
    struct Case
    {
        std::string_view text;
        std::string_view error;
        std::size_t pieces;
    };
    Case const cases[] = {
        {"plain", "", 1},
        {"", "", 0},
        {"a$$b", "", 1},
        {"$$", "", 1},
        {"$${root}", "", 1},
        {"${root}", "", 1},
        {"${root}/x", "", 2},
        {"${profile}${dcc}", "", 2},
        {"${out:kern}", "", 1},
        {"${target:my-tool}", "", 1},
        {"${asset:limine}/limine.c", "", 2},
        {"${task_dir}", "", 1},
        {"${task_dir:iso}/x", "", 2},
        {"$", "a '$' must start", 0},
        {"$HOME", "a '$' must start", 0},
        {"x$", "a '$' must start", 0},
        {"${", "unterminated", 0},
        {"${root", "unterminated", 0},
        {"${}", "unknown token", 0},
        {"${ROOT}", "unknown token", 0},
        {"${env:HOME}", "unknown token", 0},
        {"${root:x}", "takes no argument", 0},
        {"${profile:x}", "takes no argument", 0},
        {"${dcc:x}", "takes no argument", 0},
        {"${out}", "needs a name", 0},
        {"${asset}", "needs a name", 0},
        {"${target}", "needs a name", 0},
        {"${out:}", "needs a name of", 0},
        {"${task_dir:}", "needs a name of", 0},
        {"${out:a/b}", "needs a name of", 0},
        {"${asset:../x}", "needs a name of", 0},
        {"${out:a b}", "needs a name of", 0},
        {"${out:${root}}", "needs a name of", 0},
    };
    for (auto const& c : cases)
    {
        auto r = dpm::expand::scan(c.text);
        if (c.error.empty())
        {
            CHECK(r.ok());
            CHECK_EQ(r.pieces.size(), c.pieces);
        }
        else
            CHECK(r.error.find(c.error) != std::string::npos);
    }
    auto r = dpm::expand::scan("a$$b${root}$$");
    REQUIRE(r.ok());
    REQUIRE(r.pieces.size() == 3);
    CHECK_EQ(r.pieces[0].text, std::string{"a$b"});
    CHECK(r.pieces[1].kind == dpm::expand::Kind::Root);
    CHECK_EQ(r.pieces[2].text, std::string{"$"});
}

TEST_CASE("paths are confined to the project, task directories, products and assets")
{
    struct Case
    {
        std::string_view text;
        bool input;
        bool ok;
    };
    Case const cases[] = {
        {"a/b.c", false, true},
        {"src/*.dc", true, true},
        {"${root}", false, true},
        {"${root}/x/y", false, true},
        {"${task_dir}/out.iso", false, true},
        {"${task_dir:iso}", false, true},
        {"${out:kern}", false, true},
        {"${asset:limine}/limine.c", true, true},
        {"${asset:limine}", false, false},
        {"out/${profile}/${target:kern}", false, true},
        {"${profile}/x", false, true},
        {"/etc/passwd", true, false},
        {"../x", true, false},
        {"a/../../x", true, false},
        {"${root}/../x", true, false},
        {"${task_dir}/../../x", true, false},
        {"${root}x", true, false},
        {"a/${root}", true, false},
        {"${dcc}", true, false},
        {"${dcc}/x", true, false},
        {"x/${asset:limine}", true, false},
        {"a\\b", true, false},
        {"", true, false},
    };
    for (auto const& c : cases)
    {
        auto scanned = dpm::expand::scan(c.text);
        REQUIRE(scanned.ok());
        auto problem = dpm::expand::path_problem(scanned, c.input);
        if (problem.has_value() == c.ok)
            std::println(std::cerr, "confinement case '{}': {}", c.text, problem.value_or("accepted"));
        CHECK_EQ(!problem.has_value(), c.ok);
    }
}

SECTION("dpm task expansion");

TEST_CASE("every token expands to path text without rescanning")
{
    auto c = context("/p/k");
    auto t = task("iso", {"sh", "scripts/make-iso.sh", "${out:kern}", "${target:kern}", "${profile}", "${dcc}", "${asset:limine}", "${task_dir}/x.iso",
                          "${task_dir:limine-tool}/limine", "100$$", "${root}"});
    t.env = {{"IMAGE", "${task_dir}/x.iso"}, {"LITERAL", "$${root}"}};
    t.inputs = {"scripts/*.sh", "${out:kern}", "${asset:limine}/limine.c"};
    t.outputs = {"${task_dir}/x.iso"};
    t.build = {"kern"};
    std::vector<std::string> extra{"-m", "4G", "${root}"};
    auto e = tk::expand_task(t, c, extra);
    REQUIRE(e.has_value());
    CHECK_EQ(join(e->argv), std::string{"sh|scripts/make-iso.sh|/p/k/.dpm/out/debug/x86_64-elf/kern|x86_64-elf|debug|/opt/dcc/bin/dcc|/cache/src/abc|"
                                        "/p/k/.dpm/tasks/iso/x.iso|/p/k/.dpm/tasks/limine-tool/limine|100$|/p/k|-m|4G|${root}"});
    CHECK_EQ(e->cwd, fs::path{"/p/k"});
    CHECK_EQ(e->env.at("IMAGE"), std::string{"/p/k/.dpm/tasks/iso/x.iso"});
    CHECK_EQ(e->env.at("LITERAL"), std::string{"${root}"});
    CHECK_EQ(e->env.at("DPM_ROOT"), std::string{"/p/k"});
    CHECK_EQ(e->env.at("DPM_PROFILE"), std::string{"debug"});
    CHECK_EQ(e->env.at("DPM_TASK"), std::string{"iso"});
    CHECK_EQ(e->env.at("DPM_OUT_KERN"), std::string{"/p/k/.dpm/out/debug/x86_64-elf/kern"});
    CHECK_EQ(e->env.at("DPM_OUT_MY_TOOL"), std::string{"/p/k/.dpm/out/debug/host/my-tool"});
    CHECK(!e->env.contains("DPM_OFFLINE"));
    REQUIRE(e->inputs.size() == 3);
    CHECK_EQ(e->inputs[0], fs::path{"/p/k/scripts/*.sh"});
    CHECK_EQ(e->inputs[2], fs::path{"/cache/src/abc/limine.c"});
    REQUIRE(e->outputs.size() == 1);
    CHECK_EQ(e->outputs[0], fs::path{"/p/k/.dpm/tasks/iso/x.iso"});
    CHECK(e->products.contains("kern"));
    CHECK(e->assets.contains("limine"));
    c.offline = true;
    CHECK_EQ(tk::expand_task(t, c)->env.at("DPM_OFFLINE"), std::string{"1"});
}

TEST_CASE("relative programs and directories are anchored at the project root")
{
    auto c = context("/p/k");
    auto t = task("run", {"tools/run.sh", "rel/arg"});
    t.cwd = "sub/dir";
    auto e = tk::expand_task(t, c);
    REQUIRE(e.has_value());
    CHECK_EQ(e->argv[0], std::string{"/p/k/tools/run.sh"});
    CHECK_EQ(e->argv[1], std::string{"rel/arg"});
    CHECK_EQ(e->cwd, fs::path{"/p/k/sub/dir"});
    auto named = tk::expand_task(task("cc", {"cc", "x"}), c);
    CHECK_EQ(named->argv[0], std::string{"cc"});
    auto product = tk::expand_task(task("p", {"${out:kern}"}), c);
    CHECK_EQ(product->argv[0], std::string{"/p/k/.dpm/out/debug/x86_64-elf/kern"});
}

TEST_CASE("unavailable assets fail, or stay visible in a dry run")
{
    auto c = context("/p/k");
    c.assets.clear();
    auto t = task("tool", {"cc", "${asset:limine}/limine.c"});
    auto e = tk::expand_task(t, c);
    CHECK(!e.has_value());
    CHECK(e.error().find("asset 'limine' is not available") != std::string::npos);
    c.keep_unresolved = true;
    auto dry = tk::expand_task(t, c);
    REQUIRE(dry.has_value());
    CHECK_EQ(dry->argv[1], std::string{"${asset:limine}/limine.c"});
    auto bad = tk::expand_task(task("x", {"${out:none}"}), c);
    CHECK(!bad.has_value());
}

SECTION("dpm task order");

TEST_CASE("needs run first, depth first in declaration order, each once")
{
    dpm::manifest::Project p;
    auto a = task("a", {"x"});
    auto b = task("b", {"x"});
    auto c = task("c", {"x"});
    auto d = task("d", {"x"});
    auto e = task("e", {"x"});
    d.needs = {"b", "c"};
    b.needs = {"a"};
    c.needs = {"a", "b"};
    e.needs = {"d", "a"};
    p.tasks = {a, b, c, d, e};
    auto names = [](std::vector<dpm::manifest::Task const*> const& order)
    {
        std::vector<std::string> out;
        for (auto const* t : order)
            out.push_back(t->name);
        return join(out);
    };
    CHECK_EQ(names(tk::order(p, "e")), std::string{"a|b|c|d|e"});
    CHECK_EQ(names(tk::order(p, "c")), std::string{"a|b|c"});
    CHECK_EQ(names(tk::order(p, "a")), std::string{"a"});
    CHECK(tk::order(p, "zz").empty());
    p.tasks[0].needs = {"e"};
    CHECK_EQ(names(tk::order(p, "e")), std::string{"a|b|c|d|e"});
}

TEST_CASE("only the products in build and the assets the tasks name are needed")
{
    dpm::manifest::Project p;
    dpm::manifest::Product k;
    k.name = "kern";
    dpm::manifest::Product other;
    other.name = "other";
    p.products = {k, other};
    auto t1 = task("tool", {"cc", "${asset:limine}/limine.c", "${out:other}"});
    auto t2 = task("iso", {"sh", "x"});
    t2.build = {"kern"};
    t2.env = {{"FW", "${asset:ovmf}/code.fd"}};
    t2.needs = {"tool"};
    p.tasks = {t1, t2};
    auto order = tk::order(p, "iso");
    auto needs = tk::needs_of(p, order);
    CHECK_EQ(join(needs.products), std::string{"kern"});
    CHECK_EQ(needs.assets.size(), std::size_t{2});
    CHECK(needs.assets.contains("limine") && needs.assets.contains("ovmf"));
    auto none = tk::needs_of(p, std::vector<dpm::manifest::Task const*>{&p.tasks[1]});
    CHECK(!none.assets.contains("limine"));
    p.tasks[1].env.clear();
    p.tasks[1].build.clear();
    auto empty = tk::needs_of(p, std::vector<dpm::manifest::Task const*>{&p.tasks[1]});
    CHECK(empty.products.empty() && empty.assets.empty());
}

SECTION("dpm task inputs and keys");

TEST_CASE("inputs expand globs and directories, sorted, and a missing literal input is an error")
{
    TempDir d;
    d.write("scripts/b.sh", "b");
    d.write("scripts/a.sh", "a");
    d.write("scripts/notes.txt", "n");
    d.write("data/x/1.bin", "1");
    d.write("data/x/y/2.bin", "2");
    d.write("data/3.bin", "3");
    std::vector<fs::path> patterns{d.path / "scripts/*.sh", d.path / "data/**/*.bin", d.path / "scripts/notes.txt", d.path / "scripts/a.sh"};
    auto r = tk::collect_inputs(patterns);
    REQUIRE(r.has_value());
    std::vector<std::string> rel;
    for (auto const& p : *r)
        rel.push_back(p.lexically_relative(d.path).generic_string());
    CHECK_EQ(join(rel), std::string{"data/3.bin|data/x/1.bin|data/x/y/2.bin|scripts/a.sh|scripts/b.sh|scripts/notes.txt"});
    auto dir = tk::collect_inputs(std::vector<fs::path>{d.path / "data"});
    REQUIRE(dir.has_value());
    CHECK_EQ(dir->size(), std::size_t{3});
    auto none = tk::collect_inputs(std::vector<fs::path>{d.path / "nowhere/*.c"});
    REQUIRE(none.has_value());
    CHECK(none->empty());
    auto missing = tk::collect_inputs(std::vector<fs::path>{d.path / "gone.c"});
    CHECK(!missing.has_value());
    CHECK(missing.error().find("does not exist") != std::string::npos);
}

TEST_CASE("the key changes with the command, cwd, environment, outputs, products and assets")
{
    TempDir d;
    auto c = context(d.path);
    fs::create_directories(c.outs["kern"].parent_path());
    {
        std::ofstream f{c.outs["kern"], std::ios::binary};
        f << "elf";
    }
    auto t = task("iso", {"sh", "x", "${out:kern}"});
    t.env = {{"A", "1"}};
    t.outputs = {"${task_dir}/x.iso"};
    auto key = [&](dpm::manifest::Task const& task_value, tk::Context const& ctx, std::vector<std::string> extra = {})
    { return tk::make_key(*tk::expand_task(task_value, ctx, extra), ctx); };
    auto base = key(t, c);
    CHECK_EQ(base, key(t, c));
    CHECK(base.starts_with("sha256:"));
    CHECK_NE(base, key(t, c, {"-v"}));
    auto changed = t;
    changed.command[1] = "y";
    CHECK_NE(base, key(changed, c));
    changed = t;
    changed.command = {"sh", "x${out:kern}"};
    CHECK_NE(base, key(changed, c));
    changed = t;
    changed.cwd = "sub";
    CHECK_NE(base, key(changed, c));
    changed = t;
    changed.env[0].value = "2";
    CHECK_NE(base, key(changed, c));
    changed = t;
    changed.outputs = {"${task_dir}/y.iso"};
    CHECK_NE(base, key(changed, c));
    auto profile = c;
    profile.profile = "release";
    CHECK_NE(base, key(t, profile));
    {
        std::ofstream f{c.outs["kern"], std::ios::binary};
        f << "elf2";
    }
    CHECK_NE(base, key(t, c));
    auto with_asset = t;
    with_asset.command.push_back("${asset:limine}");
    auto asset_key = key(with_asset, c);
    auto other_hash = c;
    other_hash.asset_hashes["limine"] = "sha256:" + std::string(64, 'b');
    CHECK_NE(asset_key, key(with_asset, other_hash));
}

SECTION("dpm task stamps");

TEST_CASE("a task stamp prints and parses, and damaged stamps are rejected")
{
    TempDir d;
    auto in = d.write("in.txt", "input");
    auto out = d.write("out.iso", "image");
    d.write("tree/a", "1");
    d.write("tree/b/c", "2");
    std::vector<fs::path> outputs{out, d.path / "tree"};
    std::vector<fs::path> inputs{in};
    auto key = st::make_task_key(std::vector<std::string>{"argv", "x"});
    auto stamp = st::record_task(key, outputs, inputs, Clock::now());
    REQUIRE(stamp.has_value());
    REQUIRE(stamp->outputs.size() == 2);
    CHECK(!stamp->outputs[0].directory);
    CHECK(stamp->outputs[1].directory);
    CHECK(dpm::hash::is_hash_string(stamp->outputs[1].hash));
    auto text = st::print(*stamp);
    CHECK(text.starts_with("dpm-stamp 2\n"));
    auto parsed = st::parse_task(text);
    REQUIRE(parsed.has_value());
    CHECK_EQ(st::print(*parsed), text);
    CHECK(!st::parse(text).has_value());
    CHECK(!st::parse_task("dpm-stamp 1\nkey " + key + "\noutput 1 2\n").has_value());
    CHECK(!st::parse_task("dpm-stamp 2\nkey nope\n").has_value());
    CHECK(!st::parse_task("dpm-stamp 2\nkey " + key + "\noutput dir xyz /a\n").has_value());
    CHECK(!st::parse_task("dpm-stamp 2\nkey " + key + "\noutput file 1\n").has_value());
    CHECK(!st::parse_task("dpm-stamp 2\nkey " + key + "\nbogus\n").has_value());
    CHECK(!st::parse_task("dpm-stamp 2\nkey " + key).has_value());
    auto reordered = text;
    auto input_at = reordered.find("input ");
    auto output_at = reordered.find("output file ");
    REQUIRE(input_at != std::string::npos && output_at != std::string::npos);
    auto input_line = reordered.substr(input_at);
    reordered = reordered.substr(0, input_at);
    reordered.insert(output_at, input_line);
    CHECK(!st::parse_task(reordered).has_value());
    CHECK(!st::record_task(key, std::vector<fs::path>{d.path / "missing"}, inputs, Clock::now()).has_value());
    CHECK(!st::record_task(key, outputs, std::vector<fs::path>{d.path / "missing"}, Clock::now()).has_value());
    auto file = d.path / "t.stamp";
    REQUIRE(st::write_task(file, *stamp));
    auto back = st::read_task(file);
    REQUIRE(back.has_value());
    CHECK_EQ(st::print(*back), text);
    CHECK(!st::read(file).has_value());
}

TEST_CASE("task verification notices the key, outputs, directory contents and inputs")
{
    TempDir d;
    auto in = d.write("in.txt", "input");
    auto out = d.write("out.iso", "image");
    d.write("tree/a", "1");
    std::vector<fs::path> outputs{out, d.path / "tree"};
    std::vector<fs::path> inputs{in};
    auto key = st::make_task_key(std::vector<std::string>{"k"});
    auto fresh = [&](std::string const& k, std::vector<fs::path> const& o, std::vector<fs::path> const& i)
    {
        auto stamp = st::record_task(key, outputs, inputs, Clock::now());
        if (!stamp)
            return st::Verdict{};
        return st::verify_task(*stamp, k, o, i, Clock::now());
    };
    CHECK(fresh(key, outputs, inputs).state == st::State::Fresh);
    CHECK(fresh(st::make_task_key(std::vector<std::string>{"other"}), outputs, inputs).state == st::State::KeyChanged);
    CHECK(fresh(key, {out}, inputs).state != st::State::Fresh);
    CHECK(fresh(key, outputs, {}).state == st::State::InputChanged);
    auto stamp = st::record_task(key, outputs, inputs, Clock::now());
    REQUIRE(stamp.has_value());
    d.write("tree/a", "changed");
    CHECK(st::verify_task(*stamp, key, outputs, inputs, Clock::now()).state == st::State::OutputChanged);
    d.write("tree/a", "1");
    CHECK(st::verify_task(*stamp, key, outputs, inputs, Clock::now()).state == st::State::Fresh);
    d.write("tree/new", "x");
    CHECK(st::verify_task(*stamp, key, outputs, inputs, Clock::now()).state == st::State::OutputChanged);
    fs::remove(d.path / "tree/new");
    d.write("out.iso", "image2", std::chrono::hours{1});
    CHECK(st::verify_task(*stamp, key, outputs, inputs, Clock::now()).state == st::State::OutputChanged);
    stamp = st::record_task(key, outputs, inputs, Clock::now());
    d.write("in.txt", "other", std::chrono::hours{1});
    CHECK(st::verify_task(*stamp, key, outputs, inputs, Clock::now()).state == st::State::InputChanged);
    stamp = st::record_task(key, outputs, inputs, Clock::now());
    fs::remove(in);
    CHECK(st::verify_task(*stamp, key, outputs, inputs, Clock::now()).state == st::State::InputMissing);
    fs::remove(out);
    CHECK(st::verify_task(*stamp, key, outputs, inputs, Clock::now()).state == st::State::OutputChanged);
}

TEST_CASE("a racy input with the same size and mtime but new content is noticed")
{
    TempDir d;
    auto in = d.write("in.txt", "aaaa", std::chrono::hours{0});
    auto out = d.write("out.bin", "o");
    std::vector<fs::path> outputs{out};
    std::vector<fs::path> inputs{in};
    auto key = st::make_task_key(std::vector<std::string>{"k"});
    auto stamp = st::record_task(key, outputs, inputs, Clock::now());
    REQUIRE(stamp.has_value());
    REQUIRE(stamp->inputs.size() == 1);
    CHECK(stamp->inputs[0].racy);
    auto mtime = fs::last_write_time(in);
    {
        std::ofstream f{in, std::ios::binary};
        f << "bbbb";
    }
    fs::last_write_time(in, mtime);
    CHECK(st::verify_task(*stamp, key, outputs, inputs, Clock::now()).state == st::State::InputChanged);
}

SECTION("dpm task process groups");

TEST_CASE("a task runs in its own process group and a timeout stops the whole group")
{
    TempDir d;
    dpm::process::Command cmd;
    cmd.argv = {"sh", "-c", "sleep 30 & echo $! > child.pid; ps -o pgid= -p $$ > group.txt; echo $$ > self.pid; wait"};
    cmd.cwd = d.path;
    cmd.inherit_env = true;
    cmd.passthrough = true;
    cmd.own_group = true;
    cmd.timeout = std::chrono::milliseconds{1500};
    cmd.grace = std::chrono::milliseconds{200};
    auto start = std::chrono::steady_clock::now();
    auto r = dpm::process::run(cmd);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(r.started);
    CHECK(r.timed_out);
    CHECK(elapsed < std::chrono::seconds{10});
    auto read_int = [&](char const* name)
    {
        std::ifstream f{d.path / name};
        long v = 0;
        f >> v;
        return v;
    };
    auto self = read_int("self.pid");
    auto group = read_int("group.txt");
    auto child = read_int("child.pid");
    CHECK(self > 0);
    CHECK_EQ(group, self);
    REQUIRE(child > 0);
    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i)
    {
        std::error_code ec;
        auto stat = fs::path{"/proc"} / std::to_string(child) / "stat";
        std::ifstream f{stat};
        std::string text{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
        auto close = text.rfind(')');
        gone = !fs::exists(stat, ec) || (close != std::string::npos && close + 2 < text.size() && text[close + 2] == 'Z');
        if (!gone)
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    CHECK(gone);
}

TEST_CASE("a task's exit code and signal are reported")
{
    dpm::process::Command cmd;
    cmd.argv = {"sh", "-c", "exit 7"};
    cmd.inherit_env = true;
    cmd.passthrough = true;
    cmd.own_group = true;
    auto r = dpm::process::run(cmd);
    CHECK(r.started);
    CHECK_EQ(r.exit_code, 7);
    cmd.argv = {"sh", "-c", "kill -TERM $$"};
    auto s = dpm::process::run(cmd);
    CHECK_EQ(s.signal, 15);
    cmd.argv = {"definitely-not-a-program-xyz"};
    auto missing = dpm::process::run(cmd);
    CHECK(!missing.started);
}
