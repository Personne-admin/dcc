import std;
import dpm.cache;
import dpm.extract;
import dpm.fetch;
import dpm.git;
import dpm.hash;
import dpm.lock;
import dpm.manifest;
import dpm.process;
import dpm.resolve;
import dpm.tar;

#include "harness.hh"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
    namespace fs = std::filesystem;

    struct TempDir
    {
        fs::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("dpm_fetch_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
            fs::create_directories(path);
        }

        ~TempDir() { dpm::cache::detail::remove_tree(path); }

        void write(fs::path const& relative, std::string const& content, bool exec = false) const
        {
            auto full = path / relative;
            fs::create_directories(full.parent_path());
            {
                std::ofstream f{full, std::ios::binary};
                f << content;
            }
            ::chmod(full.c_str(), exec ? 0755 : 0644);
        }
    };

    [[nodiscard]] fs::path env_path(char const* name)
    {
        char const* value = ::getenv(name);
        return value ? fs::path{value} : fs::path{};
    }

    struct TarBuilder
    {
        std::string data;

        static void octal(char* dst, std::size_t width, std::uint64_t value)
        {
            auto text = std::format("{:0{}o}", value, width - 1);
            std::memcpy(dst, text.data(), width - 1);
            dst[width - 1] = '\0';
        }

        void header(std::string const& name, char type, std::uint64_t size, std::string const& link = {}, unsigned mode = 0644)
        {
            std::string h(512, '\0');
            std::memcpy(h.data(), name.data(), std::min<std::size_t>(name.size(), 100));
            octal(h.data() + 100, 8, mode);
            octal(h.data() + 108, 8, 0);
            octal(h.data() + 116, 8, 0);
            octal(h.data() + 124, 12, size);
            octal(h.data() + 136, 12, 0);
            std::memset(h.data() + 148, ' ', 8);
            h[156] = type;
            std::memcpy(h.data() + 157, link.data(), std::min<std::size_t>(link.size(), 100));
            std::memcpy(h.data() + 257, "ustar", 5);
            std::memcpy(h.data() + 263, "00", 2);
            std::uint32_t sum = 0;
            for (char c : h)
                sum += static_cast<unsigned char>(c);
            auto text = std::format("{:06o}", sum);
            std::memcpy(h.data() + 148, text.data(), 6);
            h[154] = '\0';
            h[155] = ' ';
            data += h;
        }

        void body(std::string_view content)
        {
            data.append(content);
            data.append((512 - content.size() % 512) % 512, '\0');
        }

        TarBuilder& file(std::string const& name, std::string const& content, unsigned mode = 0644)
        {
            header(name, '0', content.size(), {}, mode);
            body(content);
            return *this;
        }

        TarBuilder& dir(std::string const& name)
        {
            header(name, '5', 0, {}, 0755);
            return *this;
        }

        TarBuilder& symlink(std::string const& name, std::string const& target)
        {
            header(name, '2', 0, target);
            return *this;
        }

        TarBuilder& hardlink(std::string const& name, std::string const& target)
        {
            header(name, '1', 0, target);
            return *this;
        }

        TarBuilder& special(std::string const& name, char type)
        {
            header(name, type, 0);
            return *this;
        }

        TarBuilder& pax(std::map<std::string, std::string> const& records)
        {
            std::string payload;
            for (auto const& [key, value] : records)
            {
                std::string rest = " " + key + "=" + value + "\n";
                std::size_t length = rest.size();
                for (int i = 0; i < 4; ++i)
                    length = rest.size() + std::to_string(length).size();
                payload += std::to_string(length) + rest;
            }
            header("PaxHeader", 'x', payload.size());
            body(payload);
            return *this;
        }

        TarBuilder& long_name(std::string const& name)
        {
            header("././@LongLink", 'L', name.size() + 1);
            body(name + std::string(1, '\0'));
            return *this;
        }

        std::string finish() const { return data + std::string(1024, '\0'); }
    };

    bool write_binary(fs::path const& path, std::string const& bytes)
    {
        std::ofstream f{path, std::ios::binary};
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(f);
    }

    fs::path compress(fs::path const& tar_file, std::string const& program, std::string const& suffix, char const* level = "-6")
    {
        auto out = fs::path{tar_file.string() + suffix};
        dpm::process::Command cmd;
        cmd.argv = {program, "-c", level};
        cmd.stdin_file = tar_file;
        cmd.stdout_file = out;
        cmd.env["PATH"] = std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin";
        auto r = dpm::process::run(cmd);
        CHECK(r.ok());
        return out;
    }

    fs::path make_archive(TempDir const& td, std::string const& name, std::string const& tar_bytes, bool xz = false, char const* level = "-6")
    {
        auto raw = td.path / (name + ".tar");
        write_binary(raw, tar_bytes);
        (void)xz;
        return compress(raw, "gzip", ".gz", level);
    }

    dpm::fetch::Policy test_policy()
    {
        dpm::fetch::Policy policy;
        policy.allow_file = true;
        return policy;
    }

    std::size_t count_entries(fs::path const& dir)
    {
        std::error_code ec;
        if (!fs::exists(dir, ec))
            return 0;
        return static_cast<std::size_t>(std::distance(fs::recursive_directory_iterator{dir, ec}, fs::recursive_directory_iterator{}));
    }

    bool contains(std::string const& text, std::string_view needle)
    {
        return text.find(needle) != std::string::npos;
    }

    dpm::fetch::Extracted extract_to(fs::path const& archive, fs::path const& staging, dpm::fetch::Policy const& policy = test_policy())
    {
        return dpm::fetch::extract_tarball(archive, staging, policy);
    }

    struct Env
    {
        TempDir home;
        dpm::cache::Cache cache{home.path / "cache"};
    };

    dpm::process::Result git(fs::path const& dir, std::vector<std::string> args)
    {
        dpm::process::Command cmd;
        cmd.argv = {"git", "-C", dir.string(), "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", "-c", "protocol.file.allow=always"};
        for (auto& a : args)
            cmd.argv.push_back(std::move(a));
        cmd.env["PATH"] = std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin";
        cmd.env["HOME"] = dir.string();
        cmd.env["GIT_CONFIG_GLOBAL"] = "/dev/null";
        cmd.env["GIT_CONFIG_SYSTEM"] = "/dev/null";
        cmd.env["GIT_AUTHOR_NAME"] = "t";
        cmd.env["GIT_AUTHOR_EMAIL"] = "t@example.invalid";
        cmd.env["GIT_COMMITTER_NAME"] = "t";
        cmd.env["GIT_COMMITTER_EMAIL"] = "t@example.invalid";
        cmd.env["GIT_AUTHOR_DATE"] = "2020-01-01T00:00:00Z";
        cmd.env["GIT_COMMITTER_DATE"] = "2020-01-01T00:00:00Z";
        return dpm::process::run(cmd);
    }

    std::string git_out(fs::path const& dir, std::vector<std::string> args)
    {
        auto r = git(dir, std::move(args));
        auto out = r.out;
        while (!out.empty() && out.back() == '\n')
            out.pop_back();
        return out;
    }

    void init_repo(TempDir const& repo)
    {
        REQUIRE(git(repo.path, {"init", "--quiet", "--initial-branch=main"}).ok());
    }

    std::string commit_all(TempDir const& repo, std::string const& message = "c")
    {
        CHECK(git(repo.path, {"add", "-A"}).ok());
        CHECK(git(repo.path, {"commit", "--quiet", "--allow-empty", "-m", message}).ok());
        return git_out(repo.path, {"rev-parse", "HEAD"});
    }

    std::string url_of(TempDir const& repo)
    {
        return "file://" + repo.path.string();
    }

    std::string project_text(std::string const& name, std::string const& version, std::string const& deps = {})
    {
        return "dpm::v1::Project { name = \"" + name + "\", version = \"" + version + "\"" + (deps.empty() ? "" : ", dependencies = { " + deps + " }") + " }\n";
    }

    dpm::fetch::GitRequest request_for(TempDir const& repo, std::vector<std::string> tags, std::string commit = {}, std::string hash = {}, std::string subdir = {})
    {
        dpm::fetch::GitRequest request;
        request.spec.url = url_of(repo);
        request.spec.repo = fs::path{"_file"} / "test.git";
        request.spec.subdir = std::move(subdir);
        request.tags = std::move(tags);
        request.commit = std::move(commit);
        request.hash = std::move(hash);
        return request;
    }

    std::string tree_hash_of_worktree(TempDir const& repo)
    {
        auto copy = fs::path{repo.path.string() + "-copy"};
        fs::create_directories(copy);
        fs::copy(repo.path, copy, fs::copy_options::recursive);
        fs::remove_all(copy / ".git");
        auto h = dpm::hash::tree_hash(copy).hash;
        fs::remove_all(copy);
        return h;
    }
} // namespace

SECTION("dpm tar reader");

TEST_CASE("the reader lists members of a plain ustar stream")
{
    TarBuilder b;
    b.dir("top/").file("top/a.txt", "hello", 0755).symlink("top/l", "a.txt").hardlink("top/h", "top/a.txt").special("top/dev", '3').special("top/fifo", '6');
    auto bytes = b.finish();
    std::vector<dpm::extract::Member> members;
    std::string data;
    dpm::tar::Reader::Handlers h;
    h.on_member = [&](dpm::extract::Member const& m)
    {
        members.push_back(m);
        return true;
    };
    h.on_data = [&](std::string_view d)
    {
        data.append(d);
        return true;
    };
    dpm::tar::Reader reader{h};
    for (std::size_t i = 0; i < bytes.size(); i += 100)
        REQUIRE(reader.feed(std::string_view{bytes}.substr(i, 100)));
    CHECK(reader.finish());
    REQUIRE(members.size() == 6);
    CHECK(members[0].type == dpm::extract::MemberType::Directory);
    CHECK_EQ(members[0].path, std::string{"top"});
    CHECK(members[1].type == dpm::extract::MemberType::File);
    CHECK(members[1].exec);
    CHECK_EQ(members[1].size, std::uint64_t{5});
    CHECK(members[2].type == dpm::extract::MemberType::Symlink);
    CHECK_EQ(members[2].target, std::string{"a.txt"});
    CHECK(members[3].type == dpm::extract::MemberType::Hardlink);
    CHECK(members[4].type == dpm::extract::MemberType::CharDevice);
    CHECK(members[5].type == dpm::extract::MemberType::Fifo);
    CHECK_EQ(data, std::string{"hello"});
}

TEST_CASE("pax and GNU long names are honored and cannot hide unsafe names")
{
    std::string long_path(150, 'a');
    TarBuilder b;
    b.pax({{"path", "dir/" + long_path}}).file("short", "x").long_name("gnu/" + long_path).file("short2", "y").pax({{"path", "../escape"}}).file("innocent", "z");
    auto bytes = b.finish();
    std::vector<dpm::extract::Member> members;
    dpm::tar::Reader::Handlers h;
    h.on_member = [&](dpm::extract::Member const& m)
    {
        members.push_back(m);
        return true;
    };
    dpm::tar::Reader reader{h};
    REQUIRE(reader.feed(bytes));
    REQUIRE(members.size() == 3);
    CHECK_EQ(members[0].path, "dir/" + long_path);
    CHECK_EQ(members[1].path, "gnu/" + long_path);
    CHECK_EQ(members[2].path, std::string{"../escape"});
    CHECK(dpm::extract::member_name_problem(members[2].path).has_value());
}

TEST_CASE("the reader rejects bad checksums, truncation and oversized streams")
{
    TarBuilder b;
    b.file("a", "x");
    auto bytes = b.finish();
    auto bad = bytes;
    bad[10] ^= 1;
    dpm::tar::Reader r1{{}};
    CHECK(!r1.feed(bad));
    CHECK(contains(r1.error(), "checksum"));
    dpm::tar::Reader r2{{}};
    REQUIRE(r2.feed(std::string_view{bytes}.substr(0, 700)));
    CHECK(!r2.finish());
    dpm::tar::Reader r3{{}, 600};
    CHECK(!r3.feed(bytes));
    TarBuilder g;
    g.pax({{"path", "x"}});
    auto global = g.data;
    global[156] = 'g';
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < 512; ++i)
        sum += (i >= 148 && i < 156) ? 32u : static_cast<unsigned char>(global[i]);
    auto text = std::format("{:06o}", sum);
    std::memcpy(global.data() + 148, text.data(), 6);
    dpm::tar::Reader r4{{}};
    CHECK(!r4.feed(global));
}

SECTION("dpm tarball safety");

TEST_CASE("a valid archive extracts with its single top directory stripped")
{
    {
        TempDir td;
        TarBuilder b;
        b.dir("pkg-1.0/").file("pkg-1.0/project.dcon", project_text("pkg", "1.0.0")).file("pkg-1.0/bin/run", "#!/bin/sh\n", 0755).dir("pkg-1.0/empty/");
        auto archive = make_archive(td, "ok", b.finish());
        auto staging = td.path / "staging";
        auto r = extract_to(archive, staging);
        if (!r.ok)
            std::println(std::cerr, "{}", r.error);
        REQUIRE(r.ok);
        CHECK(fs::is_regular_file(staging / "project.dcon"));
        CHECK(fs::is_regular_file(staging / "bin/run"));
        CHECK(fs::is_directory(staging / "empty"));
        CHECK(::access((staging / "bin/run").c_str(), X_OK) == 0);
        CHECK(::access((staging / "project.dcon").c_str(), X_OK) != 0);
        CHECK_EQ(r.files, std::size_t{2});
    }
}

TEST_CASE("an archive with several top-level entries is not stripped")
{
    TempDir td;
    TarBuilder b;
    b.file("a.txt", "1").file("sub/b.txt", "2");
    auto staging = td.path / "staging";
    REQUIRE(extract_to(make_archive(td, "multi", b.finish()), staging).ok);
    CHECK(fs::is_regular_file(staging / "a.txt"));
    CHECK(fs::is_regular_file(staging / "sub/b.txt"));
}

TEST_CASE("a symlink followed by a file written through it is rejected before anything is written")
{
    TempDir td;
    auto outside = td.path / "outside";
    fs::create_directories(outside);
    TarBuilder b;
    b.dir("pkg/").file("pkg/innocent", "ok").symlink("pkg/link", outside.string()).file("pkg/link/pwned", "owned");
    auto staging = td.path / "staging";
    auto r = extract_to(make_archive(td, "link", b.finish()), staging);
    CHECK(!r.ok);
    CHECK(contains(r.error, "symbolic link not allowed"));
    CHECK_EQ(count_entries(staging), std::size_t{0});
    CHECK_EQ(count_entries(outside), std::size_t{0});
    TarBuilder relative;
    relative.symlink("link", "..").file("link/escaped", "owned");
    auto staging2 = td.path / "staging2";
    CHECK(!extract_to(make_archive(td, "link2", relative.finish()), staging2).ok);
    CHECK_EQ(count_entries(staging2), std::size_t{0});
    CHECK(!fs::exists(td.path / "escaped"));
}

TEST_CASE("absolute paths and parent components are rejected before anything is written")
{
    for (std::string name : {"/abs/file", "a/../../escape", "../escape", "pkg/../../x", "C:/evil", "a\\b"})
    {
        TempDir td;
        TarBuilder b;
        b.file("fine", "ok").file(name, "bad");
        auto staging = td.path / "staging";
        auto r = extract_to(make_archive(td, "hostile", b.finish()), staging);
        CHECK(!r.ok);
        CHECK(contains(r.error, "unsafe archive"));
        CHECK_EQ(count_entries(staging), std::size_t{0});
        CHECK(!fs::exists(td.path / "escape"));
        CHECK(!fs::exists("/abs"));
    }
}

TEST_CASE("hard links, device nodes and fifos are rejected")
{
    struct Case
    {
        std::string label;
        std::function<void(TarBuilder&)> build;
        std::string expected;
    };
    std::vector<Case> cases = {
        {"hard link", [](TarBuilder& b) { b.file("a", "x").hardlink("b", "a"); }, "hard link not allowed"},
        {"hard link to outside", [](TarBuilder& b) { b.hardlink("b", "/etc/passwd"); }, "hard link not allowed"},
        {"char device", [](TarBuilder& b) { b.file("a", "x").special("null", '3'); }, "character device not allowed"},
        {"block device", [](TarBuilder& b) { b.special("disk", '4'); }, "block device not allowed"},
        {"fifo", [](TarBuilder& b) { b.special("pipe", '6'); }, "fifo not allowed"},
        {"sparse", [](TarBuilder& b) { b.special("sparse", 'S'); }, "special member not allowed"},
    };
    for (auto const& c : cases)
    {
        TempDir td;
        TarBuilder b;
        c.build(b);
        auto staging = td.path / "staging";
        auto r = extract_to(make_archive(td, "bad", b.finish()), staging);
        CHECK(!r.ok);
        CHECK(contains(r.error, c.expected));
        CHECK_EQ(count_entries(staging), std::size_t{0});
    }
}

TEST_CASE("a size bomb is rejected from the headers without expanding it")
{
    TempDir td;
    std::string big(64u << 20, '\0');
    TarBuilder b;
    b.file("small", "x").file("pkg/bomb.bin", big);
    auto archive = make_archive(td, "bomb", b.finish(), false, "-9");
    CHECK(fs::file_size(archive) < (1u << 20));
    auto policy = test_policy();
    policy.limits.max_bytes = 1u << 20;
    auto staging = td.path / "staging";
    auto start = std::chrono::steady_clock::now();
    auto r = extract_to(archive, staging, policy);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(!r.ok);
    CHECK(contains(r.error, "more than"));
    CHECK_EQ(count_entries(staging), std::size_t{0});
    CHECK(elapsed < std::chrono::seconds(30));
}

TEST_CASE("many small members and padding bombs hit the limits")
{
    TempDir td;
    TarBuilder b;
    for (int i = 0; i < 50; ++i)
        b.file(std::format("f{}", i), "x");
    auto policy = test_policy();
    policy.limits.max_files = 10;
    auto staging = td.path / "staging";
    auto r = extract_to(make_archive(td, "many", b.finish()), staging, policy);
    CHECK(!r.ok);
    CHECK(contains(r.error, "more than 10 files"));
    CHECK_EQ(count_entries(staging), std::size_t{0});

    TempDir td2;
    TarBuilder pad;
    pad.file("a", "x");
    auto padded = pad.finish() + std::string(8u << 20, '\0');
    auto policy2 = test_policy();
    policy2.limits.max_bytes = 1024;
    policy2.limits.max_files = 4;
    auto r2 = extract_to(make_archive(td2, "padded", padded, false, "-9"), td2.path / "staging", policy2);
    CHECK(!r2.ok);
    CHECK_EQ(count_entries(td2.path / "staging"), std::size_t{0});
}

TEST_CASE("duplicates, case collisions and files used as directories are rejected")
{
    for (int variant = 0; variant < 3; ++variant)
    {
        TempDir td;
        TarBuilder b;
        if (variant == 0)
            b.file("a", "1").file("a", "2");
        else if (variant == 1)
            b.file("Readme", "1").file("README", "2");
        else
            b.file("a", "1").file("a/b", "2");
        auto staging = td.path / "staging";
        CHECK(!extract_to(make_archive(td, "dup", b.finish()), staging).ok);
        CHECK_EQ(count_entries(staging), std::size_t{0});
    }
}

TEST_CASE("corrupt and truncated archives fail without leaving partial output beyond staging")
{
    TempDir td;
    TarBuilder b;
    b.file("pkg/a", std::string(5000, 'x'));
    auto bytes = b.finish();
    auto truncated = make_archive(td, "trunc", bytes.substr(0, 2000));
    auto r = extract_to(truncated, td.path / "s1");
    CHECK(!r.ok);
    auto garbage = td.path / "garbage.tar.gz";
    write_binary(garbage, "this is not gzip");
    CHECK(!extract_to(garbage, td.path / "s2").ok);
}

SECTION("dpm tarball download");

TEST_CASE("only https and, in tests, file URLs can be downloaded")
{
    TempDir td;
    dpm::cache::Cache cache{td.path / "cache"};
    std::string error;
    dpm::fetch::Policy strict;
    CHECK(!dpm::fetch::download(cache, "http://example.invalid/p.tar.gz", strict, error).has_value());
    CHECK(contains(error, "only https"));
    CHECK(!dpm::fetch::download(cache, "ftp://example.invalid/p.tar.gz", strict, error).has_value());
    CHECK(!dpm::fetch::download(cache, "file:///etc/passwd.tar.gz", strict, error).has_value());
    CHECK(contains(error, "only https"));
}

TEST_CASE("a file tarball installs into the cache by hash and mismatches leave nothing behind")
{
    Env env;
    TarBuilder b;
    b.dir("pkg-1/").file("pkg-1/project.dcon", project_text("pkg", "1.0.0")).file("pkg-1/src/pkg/x.dc", "module pkg::x;\n");
    auto archive = make_archive(env.home, "pkg", b.finish());
    auto url = "file://" + archive.string();
    auto first = dpm::fetch::fetch_tarball(env.cache, url, {}, test_policy());
    if (!first.ok)
        std::println(std::cerr, "{}", first.error);
    REQUIRE(first.ok);
    CHECK(dpm::hash::is_hash_string(first.hash));
    CHECK(fs::is_regular_file(first.path / "src/pkg/x.dc"));
    CHECK_EQ(env.cache.verify().size(), std::size_t{1});
    CHECK(env.cache.verify()[0].ok);
    auto again = dpm::fetch::fetch_tarball(env.cache, url, first.hash, test_policy());
    REQUIRE(again.ok);
    CHECK(again.existed);

    Env other;
    auto wrong = dpm::fetch::fetch_tarball(other.cache, url, "sha256:" + std::string(64, '0'), test_policy());
    CHECK(!wrong.ok);
    CHECK(contains(wrong.error, "hash mismatch"));
    CHECK_EQ(count_entries(other.cache.src_dir()), std::size_t{0});
    CHECK_EQ(count_entries(other.cache.tmp_dir()), std::size_t{0});
}

TEST_CASE("offline mode never downloads")
{
    Env env;
    TarBuilder b;
    b.file("project.dcon", project_text("pkg", "1.0.0"));
    auto archive = make_archive(env.home, "pkg", b.finish());
    auto url = "file://" + archive.string();
    auto policy = test_policy();
    policy.offline = true;
    auto r = dpm::fetch::fetch_tarball(env.cache, url, {}, policy);
    CHECK(!r.ok);
    CHECK(contains(r.error, "not in cache"));
    policy.offline = false;
    REQUIRE(dpm::fetch::fetch_tarball(env.cache, url, {}, policy).ok);
    fs::remove(archive);
    policy.offline = true;
    CHECK(dpm::fetch::fetch_tarball(env.cache, url, {}, policy).ok);
}

TEST_CASE("downloads over the size limit are refused")
{
    Env env;
    TarBuilder b;
    std::mt19937 rng{99};
    std::string noise(4000, '\0');
    for (auto& c : noise)
        c = static_cast<char>(rng());
    b.file("project.dcon", noise);
    auto archive = make_archive(env.home, "pkg", b.finish(), false, "-1");
    auto policy = test_policy();
    policy.max_download = 100;
    auto r = dpm::fetch::fetch_tarball(env.cache, "file://" + archive.string(), {}, policy);
    CHECK(!r.ok);
    CHECK(contains(r.error, "download"));
    CHECK_EQ(count_entries(env.cache.src_dir()), std::size_t{0});
}

TEST_CASE("a malicious file tarball is rejected through the full fetch path")
{
    Env env;
    auto outside = env.home.path / "outside";
    fs::create_directories(outside);
    TarBuilder b;
    b.dir("p/").symlink("p/link", outside.string()).file("p/link/pwned", "x");
    auto archive = make_archive(env.home, "evil", b.finish());
    auto r = dpm::fetch::fetch_tarball(env.cache, "file://" + archive.string(), {}, test_policy());
    CHECK(!r.ok);
    CHECK_EQ(count_entries(outside), std::size_t{0});
    CHECK_EQ(count_entries(env.cache.src_dir()), std::size_t{0});
    CHECK_EQ(count_entries(env.cache.tmp_dir()), std::size_t{0});
}

TEST_CASE("tar.xz and txz tarballs are refused with a clear error")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    Env env;
    for (std::string url : {"https://example.invalid/p.tar.xz", "https://example.invalid/p.txz", "file:///tmp/p.tar.xz"})
    {
        CHECK(!dpm::fetch::is_tarball_location(url, test_policy()));
        dpm::manifest::Project project;
        project.name = "app";
        project.version = "1.0.0";
        dpm::manifest::Dependency d;
        d.location = url;
        d.version = "1.0.0";
        project.dependencies.push_back(d);
        dpm::fetch::FetchSource source{env.cache, test_policy(), env.home.path, schema_root, nullptr};
        auto r = dpm::resolve::resolve(project, source, {});
        CHECK(!r.ok);
        CHECK(r.ok || contains(r.errors[0], "only .tar.gz and .tgz tarballs are supported"));
    }
}

SECTION("dpm git safety");

TEST_CASE("git locations map to https URLs and cache paths")
{
    std::string error;
    dpm::fetch::Policy policy;
    auto a = dpm::fetch::parse_git_location("github.com/user/repo/sub/dir", policy, error);
    REQUIRE(a.has_value());
    CHECK_EQ(a->url, std::string{"https://github.com/user/repo.git"});
    CHECK_EQ(a->subdir, std::string{"sub/dir"});
    CHECK_EQ(a->repo.string(), std::string{"github.com/user/repo.git"});
    auto b = dpm::fetch::parse_git_location("git+https://git.example.org/some/path.git#pkg", policy, error);
    REQUIRE(b.has_value());
    CHECK_EQ(b->url, std::string{"https://git.example.org/some/path.git"});
    CHECK_EQ(b->subdir, std::string{"pkg"});
    CHECK_EQ(b->repo.string(), std::string{"git.example.org/some/path.git"});
    CHECK(!dpm::fetch::parse_git_location("git+file:///tmp/x", policy, error).has_value());
    CHECK(!dpm::fetch::parse_git_location("git+http://host/x.git", policy, error).has_value());
    CHECK(!dpm::fetch::parse_git_location("git+ssh://host/x.git", policy, error).has_value());
    CHECK(!dpm::fetch::parse_git_location("git+https://host/../x.git", policy, error).has_value());
    CHECK(!dpm::fetch::parse_git_location("github.com/user", policy, error).has_value());
    policy.allow_file = true;
    auto c = dpm::fetch::parse_git_location("git+file:///tmp/x", policy, error);
    REQUIRE(c.has_value());
    CHECK_EQ(c->url, std::string{"file:///tmp/x"});
    CHECK(c->repo.string().starts_with("_file/"));
    auto weird = dpm::fetch::parse_git_location("git+https://host/a b/c%d.git", policy, error);
    REQUIRE(weird.has_value());
    CHECK(weird->repo.string().find(' ') == std::string::npos);
}

TEST_CASE("a tagged repository is fetched, exported without git metadata and hashed")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    repo.write("src/lib/a.dc", "module lib::a;\n");
    repo.write("tools/run.sh", "#!/bin/sh\n", true);
    repo.write("docs/note.txt", "line\r\nline\r\n");
    auto commit = commit_all(repo);
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    Env env;
    auto r = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.0.0"}), test_policy());
    if (!r.ok)
        std::println(std::cerr, "{}", r.error);
    REQUIRE(r.ok);
    CHECK_EQ(r.commit, commit);
    CHECK_EQ(r.hash, tree_hash_of_worktree(repo));
    CHECK(!fs::exists(r.path / ".git"));
    CHECK(::access((r.path / "tools/run.sh").c_str(), X_OK) == 0);
    std::ifstream note{r.path / "docs/note.txt", std::ios::binary};
    std::string content{std::istreambuf_iterator<char>{note}, std::istreambuf_iterator<char>{}};
    CHECK_EQ(content, std::string{"line\r\nline\r\n"});
    CHECK(env.cache.verify()[0].ok);
}

TEST_CASE("a branch is fetched by name and a moved branch gives the new commit")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    repo.write("a.txt", "main\n");
    auto first = commit_all(repo);
    REQUIRE(git(repo.path, {"checkout", "--quiet", "-b", "dev"}).ok());
    repo.write("a.txt", "dev one\n");
    auto second = commit_all(repo, "dev one");
    auto dev_hash = tree_hash_of_worktree(repo);
    REQUIRE(git(repo.path, {"checkout", "--quiet", "main"}).ok());
    Env env;
    auto request = request_for(repo, {});
    request.ref = "dev";
    auto r = dpm::fetch::fetch_git(env.cache, request, test_policy());
    if (!r.ok)
        std::println(std::cerr, "{}", r.error);
    REQUIRE(r.ok);
    CHECK_EQ(r.commit, second);
    CHECK(r.commit != first);
    CHECK_EQ(r.hash, dev_hash);
    REQUIRE(git(repo.path, {"checkout", "--quiet", "dev"}).ok());
    repo.write("a.txt", "dev two\n");
    auto third = commit_all(repo, "dev two");
    auto moved = dpm::fetch::fetch_git(env.cache, request, test_policy());
    REQUIRE(moved.ok);
    CHECK_EQ(moved.commit, third);
    CHECK(moved.hash != r.hash);
    auto pinned = request;
    pinned.commit = second;
    pinned.hash = r.hash;
    auto kept = dpm::fetch::fetch_git(env.cache, pinned, test_policy());
    REQUIRE(kept.ok);
    CHECK_EQ(kept.commit, second);
}

TEST_CASE("a ref can be a branch with slashes, a tag or a full commit id; a tag wins over a branch of the same name")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    repo.write("a.txt", "one\n");
    auto first = commit_all(repo);
    REQUIRE(git(repo.path, {"tag", "v1"}).ok());
    REQUIRE(git(repo.path, {"checkout", "--quiet", "-b", "release/1.0"}).ok());
    repo.write("a.txt", "two\n");
    auto second = commit_all(repo, "two");
    REQUIRE(git(repo.path, {"branch", "v1"}).ok());
    Env env;
    auto with = [&](std::string ref) {
        auto request = request_for(repo, {});
        request.ref = std::move(ref);
        return dpm::fetch::fetch_git(env.cache, request, test_policy());
    };
    auto slashed = with("release/1.0");
    REQUIRE(slashed.ok);
    CHECK_EQ(slashed.commit, second);
    auto tagged = with("v1");
    REQUIRE(tagged.ok);
    CHECK_EQ(tagged.commit, first);
    auto by_id = with(first);
    REQUIRE(by_id.ok);
    CHECK_EQ(by_id.commit, first);
    auto head = with("main");
    REQUIRE(head.ok);
    CHECK_EQ(head.commit, first);
}

TEST_CASE("an unknown or unsafe ref is refused with a message that names it")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    commit_all(repo);
    Env env;
    auto with = [&](std::string ref) {
        auto request = request_for(repo, {});
        request.ref = std::move(ref);
        return dpm::fetch::fetch_git(env.cache, request, test_policy());
    };
    auto missing = with("nowhere");
    CHECK(!missing.ok);
    CHECK(contains(missing.error, "'nowhere'"));
    CHECK(contains(missing.error, "no tag or branch"));
    for (std::string bad : std::initializer_list<std::string>{"--upload-pack=touch /tmp/x", "-x", "a..b", "x.lock", "a@{1}", "a b", "refs/../x", "/abs", "a//b", "a;b", "x/", std::string(201, 'a')})
    {
        auto r = with(bad);
        CHECK(!r.ok);
        CHECK(contains(r.error, "unsafe"));
    }
    CHECK(!with(std::string(40, 'f')).ok);
}

TEST_CASE("offline mode resolves a ref only from the cache")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    auto commit = commit_all(repo);
    Env env;
    auto request = request_for(repo, {});
    request.ref = "main";
    auto offline = test_policy();
    offline.offline = true;
    auto before = dpm::fetch::fetch_git(env.cache, request, offline);
    CHECK(!before.ok);
    CHECK(contains(before.error, "not in cache"));
    REQUIRE(dpm::fetch::fetch_git(env.cache, request, test_policy()).ok);
    auto after = dpm::fetch::fetch_git(env.cache, request, offline);
    REQUIRE(after.ok);
    CHECK_EQ(after.commit, commit);
    request.ref = commit;
    CHECK(dpm::fetch::fetch_git(env.cache, request, offline).ok);
}

TEST_CASE("a ref and a subdirectory combine")
{
    TempDir repo;
    init_repo(repo);
    repo.write("pkg/project.dcon", project_text("inner", "1.0.0"));
    repo.write("pkg/src/inner/lib.dc", "module inner::lib;\n");
    repo.write("other/x.txt", "x\n");
    commit_all(repo);
    Env env;
    auto request = request_for(repo, {}, {}, {}, "pkg");
    request.ref = "main";
    auto r = dpm::fetch::fetch_git(env.cache, request, test_policy());
    REQUIRE(r.ok);
    CHECK(fs::exists(r.path / "project.dcon"));
    CHECK(!fs::exists(r.path / "other"));
}

TEST_CASE("a pinned commit is used even when a ref is given, and a pin the ref contradicts is refused")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    auto first = commit_all(repo);
    Env env;
    auto request = request_for(repo, {});
    request.ref = "main";
    request.commit = first;
    auto ok = dpm::fetch::fetch_git(env.cache, request, test_policy());
    REQUIRE(ok.ok);
    CHECK_EQ(ok.commit, first);
    request.commit = std::string(40, 'a');
    auto contradicted = dpm::fetch::fetch_git(env.cache, request, test_policy());
    CHECK(!contradicted.ok);
    CHECK(contains(contradicted.error, "main now points to " + first));
    CHECK(contains(contradicted.error, "the pin is " + std::string(40, 'a')));
}

TEST_CASE("annotated tags peel to their commit and later candidates are tried")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.2.0"));
    auto commit = commit_all(repo);
    REQUIRE(git(repo.path, {"tag", "-a", "-m", "release", "1.2.0"}).ok());
    Env env;
    auto r = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.2.0", "1.2.0"}), test_policy());
    REQUIRE(r.ok);
    CHECK_EQ(r.commit, commit);
    auto missing = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v9.9.9"}), test_policy());
    CHECK(!missing.ok);
    CHECK(contains(missing.error, "v9.9.9"));
}

TEST_CASE("subdirectory packages hash only their subdirectory")
{
    TempDir repo;
    init_repo(repo);
    repo.write("other/file", "unrelated");
    repo.write("pkgs/lib/project.dcon", project_text("lib", "1.0.0"));
    repo.write("pkgs/lib/src/lib/a.dc", "module lib::a;\n");
    commit_all(repo);
    REQUIRE(git(repo.path, {"tag", "pkgs/lib/v1.0.0"}).ok());
    Env env;
    auto r = dpm::fetch::fetch_git(env.cache, request_for(repo, {"pkgs/lib/v1.0.0"}, {}, {}, "pkgs/lib"), test_policy());
    REQUIRE(r.ok);
    CHECK(fs::is_regular_file(r.path / "project.dcon"));
    CHECK(!fs::exists(r.path / "other"));
    CHECK_EQ(count_entries(r.path), std::size_t{4});
    auto traversal = dpm::fetch::fetch_git(env.cache, request_for(repo, {"pkgs/lib/v1.0.0"}, {}, {}, "../other"), test_policy());
    CHECK(!traversal.ok);
}

TEST_CASE("a repository containing a symlink is rejected before anything is exported")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    REQUIRE(git(repo.path, {"add", "-A"}).ok());
    fs::create_symlink("/etc", repo.path / "etc-link");
    fs::create_symlink("project.dcon", repo.path / "alias.dcon");
    commit_all(repo);
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    Env env;
    auto r = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.0.0"}), test_policy());
    CHECK(!r.ok);
    CHECK(contains(r.error, "symbolic link not allowed"));
    CHECK_EQ(count_entries(env.cache.src_dir()), std::size_t{0});
    CHECK_EQ(count_entries(env.cache.tmp_dir()), std::size_t{0});
}

TEST_CASE("submodules are rejected")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    repo.write(".gitmodules", "[submodule \"x\"]\n\tpath = x\n\turl = https://example.invalid/x.git\n");
    REQUIRE(git(repo.path, {"add", "-A"}).ok());
    REQUIRE(git(repo.path, {"update-index", "--add", "--cacheinfo", "160000," + std::string(40, 'a') + ",x"}).ok());
    REQUIRE(git(repo.path, {"commit", "--quiet", "-m", "sub"}).ok());
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    Env env;
    auto r = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.0.0"}), test_policy());
    CHECK(!r.ok);
    CHECK(contains(r.error, "submodule not allowed"));
    CHECK_EQ(count_entries(env.cache.src_dir()), std::size_t{0});
}

TEST_CASE("filters, attributes and line-ending conversion never apply to exported files")
{
    TempDir repo;
    init_repo(repo);
    repo.write(".gitattributes", "* filter=evil text eol=crlf export-subst export-ignore\nproject.dcon -export-ignore\n");
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    repo.write("data.txt", "a\nb\n$Format:%H$\n");
    commit_all(repo);
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    Env env;
    auto r = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.0.0"}), test_policy());
    REQUIRE(r.ok);
    std::ifstream in{r.path / "data.txt", std::ios::binary};
    std::string content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    CHECK_EQ(content, std::string{"a\nb\n$Format:%H$\n"});
    CHECK(fs::exists(r.path / ".gitattributes"));
}

TEST_CASE("the cache repository has no hooks and hooks in the source never run")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    auto marker = repo.path / "hook-ran";
    repo.write(".git/hooks/reference-transaction", "#!/bin/sh\ntouch '" + marker.string() + "'\n", true);
    repo.write(".git/hooks/post-checkout", "#!/bin/sh\ntouch '" + marker.string() + "'\n", true);
    repo.write(".git/hooks/pre-upload-pack", "#!/bin/sh\ntouch '" + marker.string() + "'\n", true);
    auto commit = commit_all(repo);
    fs::remove(marker);
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    fs::remove(marker);
    Env env;
    auto request = request_for(repo, {"v1.0.0"});
    auto r = dpm::fetch::fetch_git(env.cache, request, test_policy());
    REQUIRE(r.ok);
    CHECK_EQ(r.commit, commit);
    CHECK(!fs::exists(marker));
    auto bare = env.cache.git_dir() / request.spec.repo / "hooks";
    CHECK_EQ(count_entries(bare), std::size_t{0});
}

TEST_CASE("file URLs are refused unless the policy allows them")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    commit_all(repo);
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    Env env;
    dpm::fetch::Policy strict;
    auto r = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.0.0"}), strict);
    CHECK(!r.ok);
    CHECK_EQ(count_entries(env.cache.src_dir()), std::size_t{0});
}

TEST_CASE("git limits apply before any file is written")
{
    TempDir repo;
    init_repo(repo);
    for (int i = 0; i < 6; ++i)
        repo.write(std::format("f{}", i), "x");
    commit_all(repo);
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    Env env;
    auto policy = test_policy();
    policy.limits.max_files = 3;
    auto r = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.0.0"}), policy);
    CHECK(!r.ok);
    CHECK(contains(r.error, "more than 3 files"));
    CHECK_EQ(count_entries(env.cache.tmp_dir()), std::size_t{0});
    policy = test_policy();
    policy.limits.max_bytes = 3;
    CHECK(!dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.0.0"}), policy).ok);
}

TEST_CASE("a pinned commit survives a moved tag and offline mode uses the cache")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    auto first = commit_all(repo, "one");
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    Env env;
    auto a = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.0.0"}), test_policy());
    REQUIRE(a.ok);
    repo.write("extra", "new content");
    auto second = commit_all(repo, "two");
    REQUIRE(git(repo.path, {"tag", "-f", "v1.0.0"}).ok());
    CHECK_NE(first, second);

    auto pinned = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.0.0"}, first, a.hash), test_policy());
    REQUIRE(pinned.ok);
    CHECK_EQ(pinned.commit, first);
    CHECK_EQ(pinned.hash, a.hash);

    Env fresh;
    auto fresh_pinned = dpm::fetch::fetch_git(fresh.cache, request_for(repo, {"v1.0.0"}, first, a.hash), test_policy());
    if (fresh_pinned.ok)
        CHECK_EQ(fresh_pinned.commit, first);
    else
        CHECK(contains(fresh_pinned.error, "now points to") || contains(fresh_pinned.error, "cannot fetch"));
    CHECK(!fs::exists(fresh.cache.src_path("sha256:" + std::string(64, '1'))));

    auto moved = dpm::fetch::fetch_git(fresh.cache, request_for(repo, {"v1.0.0"}, std::string(40, '1')), test_policy());
    CHECK(!moved.ok);

    auto policy = test_policy();
    policy.offline = true;
    auto offline_hit = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.0.0"}), policy);
    REQUIRE(offline_hit.ok);
    CHECK_EQ(offline_hit.commit, first);
    Env cold;
    auto offline_miss = dpm::fetch::fetch_git(cold.cache, request_for(repo, {"v1.0.0"}), policy);
    CHECK(!offline_miss.ok);
    CHECK(contains(offline_miss.error, "not in cache"));
}

SECTION("dpm fetch source");

TEST_CASE("git packages resolve into a lock with commit and hash and re-resolve identically")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    TempDir dep_repo;
    init_repo(dep_repo);
    dep_repo.write("project.dcon", project_text("util", "0.2.0"));
    auto dep_commit = commit_all(dep_repo);
    REQUIRE(git(dep_repo.path, {"tag", "v0.2.0"}).ok());
    TempDir lib_repo;
    init_repo(lib_repo);
    auto dep_loc = "git+" + url_of(dep_repo);
    lib_repo.write("project.dcon", project_text("lib", "1.0.0", "{ location = \"" + dep_loc + "\", version = \"0.2.0\" }"));
    lib_repo.write("src/lib/a.dc", "module lib::a;\n");
    auto lib_commit = commit_all(lib_repo);
    REQUIRE(git(lib_repo.path, {"tag", "v1.0.0"}).ok());

    Env env;
    auto lib_loc = "git+" + url_of(lib_repo);
    dpm::manifest::Project project;
    project.name = "app";
    project.version = "1.0.0";
    dpm::manifest::Dependency d;
    d.location = lib_loc;
    d.version = "1.0.0";
    project.dependencies.push_back(d);

    dpm::fetch::FetchSource source{env.cache, test_policy(), env.home.path, schema_root, nullptr};
    auto r = dpm::resolve::resolve(project, source, {});
    if (!r.ok)
        for (auto const& e : r.errors)
            std::println(std::cerr, "{}", e);
    REQUIRE(r.ok);
    REQUIRE(r.lock.packages.size() == 2);
    CHECK_EQ(source.warnings.size(), std::size_t{2});
    CHECK(contains(source.warnings[0], "new package"));
    for (auto const& p : r.lock.packages)
    {
        CHECK(dpm::hash::is_hash_string(p.hash));
        CHECK_EQ(p.commit.size(), std::size_t{40});
        CHECK(p.source.starts_with("git+file://"));
    }
    auto const& util = r.packages.at(dep_loc);
    CHECK_EQ(util.version, std::string{"0.2.0"});
    CHECK_EQ(r.lock.packages[0].commit == lib_commit || r.lock.packages[0].commit == dep_commit, true);

    auto text = dpm::lock::print(r.lock);
    dpm::manifest::LoadOptions options;
    options.schema_root = schema_root;
    options.kind = dpm::manifest::FileKind::Lock;
    options.allow_file_urls = true;
    auto loaded = dpm::manifest::load(text, options);
    if (!loaded.ok)
        for (auto const& diagnostic : loaded.diagnostics)
            std::println(std::cerr, "{}", diagnostic.format());
    REQUIRE(loaded.ok);
    options.allow_file_urls = false;
    CHECK(!dpm::manifest::load(text, options).ok);

    Env second;
    dpm::fetch::FetchSource again{second.cache, test_policy(), second.home.path, schema_root, &*loaded.lock};
    auto r2 = dpm::resolve::resolve(project, again, {});
    REQUIRE(r2.ok);
    CHECK_EQ(dpm::lock::print(r2.lock), text);
    CHECK(again.warnings.empty());

    auto all = dpm::fetch::fetch_locked(env.cache, *loaded.lock, test_policy());
    CHECK(all.ok);
    CHECK_EQ(all.present, std::size_t{2});
    (void)env.cache.clean(true);
    auto refetch = dpm::fetch::fetch_locked(env.cache, *loaded.lock, test_policy());
    if (!refetch.ok)
        for (auto const& e : refetch.errors)
            std::println(std::cerr, "{}", e);
    CHECK(refetch.ok);
    CHECK_EQ(refetch.fetched, std::size_t{2});
}

TEST_CASE("a tag whose project version differs is an error and a tampered pin is detected")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.1"));
    commit_all(repo);
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    Env env;
    dpm::manifest::Project project;
    project.name = "app";
    project.version = "1.0.0";
    dpm::manifest::Dependency d;
    d.location = "git+" + url_of(repo);
    d.version = "1.0.0";
    project.dependencies.push_back(d);
    dpm::fetch::FetchSource source{env.cache, test_policy(), env.home.path, schema_root, nullptr};
    auto r = dpm::resolve::resolve(project, source, {});
    CHECK(!r.ok);
    CHECK(contains(r.errors[0], "contains version 1.0.1"));

    TempDir good;
    init_repo(good);
    good.write("project.dcon", project_text("lib", "1.0.0"));
    auto commit = commit_all(good);
    REQUIRE(git(good.path, {"tag", "v1.0.0"}).ok());
    project.dependencies[0].location = "git+" + url_of(good);
    dpm::manifest::Lock previous;
    dpm::manifest::LockedPackage lp;
    lp.location = project.dependencies[0].location;
    lp.version = "1.0.0";
    lp.source = lp.location;
    lp.commit = commit;
    lp.hash = "sha256:" + std::string(64, '0');
    previous.packages.push_back(lp);
    Env env2;
    dpm::fetch::FetchSource pinned{env2.cache, test_policy(), env2.home.path, schema_root, &previous};
    auto r2 = dpm::resolve::resolve(project, pinned, {});
    CHECK(!r2.ok);
    CHECK(contains(r2.errors[0], "hash mismatch") || contains(r2.errors[0], "integrity mismatch"));
    CHECK_EQ(count_entries(env2.cache.src_dir()), std::size_t{0});
}

TEST_CASE("a tarball package resolves with its own version above the minimum")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    Env env;
    TarBuilder b;
    b.dir("tool-2.3.0/").file("tool-2.3.0/project.dcon", project_text("tool", "2.3.0"));
    auto archive = make_archive(env.home, "tool-2.3.0", b.finish());
    dpm::manifest::Project project;
    project.name = "app";
    project.version = "1.0.0";
    dpm::manifest::Dependency d;
    d.location = "file://" + archive.string();
    d.version = "2.0.0";
    project.dependencies.push_back(d);
    dpm::fetch::FetchSource source{env.cache, test_policy(), env.home.path, schema_root, nullptr};
    auto r = dpm::resolve::resolve(project, source, {});
    if (!r.ok)
        for (auto const& e : r.errors)
            std::println(std::cerr, "{}", e);
    REQUIRE(r.ok);
    CHECK_EQ(r.lock.packages[0].version, std::string{"2.3.0"});
    CHECK(r.lock.packages[0].commit.empty());
    CHECK(dpm::hash::is_hash_string(r.lock.packages[0].hash));
    project.dependencies[0].version = "2.4.0";
    dpm::fetch::FetchSource too_new{env.cache, test_policy(), env.home.path, schema_root, nullptr};
    CHECK(!dpm::resolve::resolve(project, too_new, {}).ok);
}

SECTION("dpm assets");

namespace
{
    dpm::manifest::Asset asset(std::string name, std::string location, std::string version = {})
    {
        dpm::manifest::Asset a;
        a.name = std::move(name);
        a.location = std::move(location);
        a.version = std::move(version);
        return a;
    }

    std::string lock_text(dpm::manifest::Project const& project, std::vector<dpm::manifest::LockedAsset> assets)
    {
        dpm::manifest::Lock lock;
        lock.manifest = dpm::lock::fingerprint(project);
        lock.assets = std::move(assets);
        return dpm::lock::print(lock);
    }

    std::optional<dpm::manifest::Lock> load_lock(std::string const& text, fs::path const& schema_root)
    {
        dpm::manifest::LoadOptions options;
        options.schema_root = schema_root;
        options.kind = dpm::manifest::FileKind::Lock;
        options.allow_file_urls = true;
        auto loaded = dpm::manifest::load(text, options);
        if (!loaded.ok)
            for (auto const& diagnostic : loaded.diagnostics)
                std::println(std::cerr, "{}", diagnostic.format());
        if (!loaded.ok || !loaded.lock)
            return std::nullopt;
        return *loaded.lock;
    }
} // namespace

TEST_CASE("tarball, git and path assets resolve into the lock, reload and re-resolve identically")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    Env env;
    TarBuilder b;
    b.dir("limine-binary/").file("limine-binary/limine.c", "int main(void) { return 0; }\n").file("limine-binary/BOOTX64.EFI", std::string("MZ\0\1", 4));
    auto archive = make_archive(env.home, "limine-binary", b.finish());
    TempDir repo;
    init_repo(repo);
    repo.write("fw.bin", "one");
    auto old_commit = commit_all(repo, "one");
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    repo.write("fw.bin", "two");
    auto new_commit = commit_all(repo, "two");
    REQUIRE(git(repo.path, {"tag", "v1.2.0"}).ok());
    env.home.write("vendor/x/readme", "local");

    dpm::manifest::Project project;
    project.name = "k";
    project.version = "1.0.0";
    project.assets.push_back(asset("limine", "file://" + archive.string(), "v12.9.6"));
    project.assets.push_back(asset("fw", "git+" + url_of(repo)));
    project.assets.push_back(asset("fw_old", "git+" + url_of(repo), "1.0.0"));
    project.assets.push_back(asset("local", "path:vendor/x"));

    auto r = dpm::fetch::resolve_assets(env.cache, project, test_policy(), env.home.path, nullptr);
    for (auto const& e : r.errors)
        std::println(std::cerr, "{}", e);
    REQUIRE(r.ok);
    REQUIRE(r.assets.size() == 4);
    CHECK_EQ(r.warnings.size(), std::size_t{3});
    CHECK(contains(r.warnings[0], "new asset fw: commit " + new_commit));
    CHECK_EQ(r.assets[0].name, std::string{"fw"});
    CHECK_EQ(r.assets[0].source, "git+" + url_of(repo));
    CHECK_EQ(r.assets[0].commit, new_commit);
    CHECK(r.assets[0].version.empty());
    CHECK_EQ(r.assets[1].name, std::string{"fw_old"});
    CHECK_EQ(r.assets[1].commit, old_commit);
    CHECK_EQ(r.assets[1].version, std::string{"1.0.0"});
    CHECK_NE(r.assets[0].hash, r.assets[1].hash);
    CHECK_EQ(r.assets[2].name, std::string{"limine"});
    CHECK(r.assets[2].commit.empty());
    CHECK(dpm::hash::is_hash_string(r.assets[2].hash));
    CHECK_EQ(r.assets[3].source, std::string{"path:vendor/x"});
    CHECK(r.assets[3].hash.empty());

    auto text = lock_text(project, r.assets);
    auto loaded = load_lock(text, schema_root);
    REQUIRE(loaded.has_value());
    CHECK_EQ(dpm::lock::print(*loaded), text);
    CHECK(dpm::lock::is_current(project, *loaded));

    auto all = dpm::fetch::fetch_locked(env.cache, *loaded, test_policy(), {}, &*loaded);
    for (auto const& e : all.errors)
        std::println(std::cerr, "{}", e);
    REQUIRE(all.ok);
    REQUIRE(all.assets.size() == 4);
    CHECK(fs::is_regular_file(all.assets.at("limine") / "limine.c"));
    CHECK(fs::is_regular_file(all.assets.at("fw") / "fw.bin"));
    CHECK(!fs::exists(all.assets.at("fw") / ".git"));
    CHECK_EQ(all.assets.at("local"), fs::path{"vendor/x"});

    repo.write("fw.bin", "three");
    commit_all(repo, "three");
    REQUIRE(git(repo.path, {"tag", "v1.3.0"}).ok());
    auto again = dpm::fetch::resolve_assets(env.cache, project, test_policy(), env.home.path, &*loaded);
    REQUIRE(again.ok);
    CHECK(again.warnings.empty());
    CHECK_EQ(lock_text(project, again.assets), text);

    auto offline = test_policy();
    offline.offline = true;
    auto cached = dpm::fetch::resolve_assets(env.cache, project, offline, env.home.path, &*loaded);
    CHECK(cached.ok);
    CHECK_EQ(lock_text(project, cached.assets), text);
    CHECK(dpm::fetch::fetch_locked(env.cache, *loaded, offline, {}, &*loaded).ok);
    Env cold;
    auto miss = dpm::fetch::fetch_locked(cold.cache, *loaded, offline, {}, &*loaded);
    CHECK(!miss.ok);

    auto changed = project;
    changed.assets[2].version = "1.2.0";
    CHECK(!dpm::lock::is_current(changed, *loaded));
    auto bumped = dpm::fetch::resolve_assets(env.cache, changed, test_policy(), env.home.path, &*loaded);
    REQUIRE(bumped.ok);
    CHECK_EQ(bumped.warnings.size(), std::size_t{1});
    CHECK_EQ(bumped.assets[1].commit, new_commit);
}

TEST_CASE("a tampered or changed asset is fatal against the lock")
{
    auto schema_root = env_path("DCC_TEST_DPM_SCHEMA");
    if (schema_root.empty())
        return;
    Env env;
    TarBuilder b;
    b.dir("ovmf/").file("ovmf/code.fd", "firmware");
    auto archive = make_archive(env.home, "ovmf", b.finish());
    TempDir repo;
    init_repo(repo);
    repo.write("fw.bin", "one");
    auto commit = commit_all(repo, "one");
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    dpm::manifest::Project project;
    project.name = "k";
    project.version = "1.0.0";
    project.assets.push_back(asset("ovmf", "file://" + archive.string()));
    project.assets.push_back(asset("fw", "git+" + url_of(repo), "1.0.0"));
    auto r = dpm::fetch::resolve_assets(env.cache, project, test_policy(), env.home.path, nullptr);
    REQUIRE(r.ok);
    auto good = load_lock(lock_text(project, r.assets), schema_root);
    REQUIRE(good.has_value());

    auto tampered = *good;
    for (auto& a : tampered.assets)
        a.hash = "sha256:" + std::string(64, '1');
    Env fresh;
    auto fetched = dpm::fetch::fetch_locked(fresh.cache, tampered, test_policy(), {}, &tampered);
    CHECK(!fetched.ok);
    REQUIRE(fetched.errors.size() == 2);
    CHECK(contains(fetched.errors[0], "asset fw"));
    CHECK(contains(fetched.errors[1], "asset ovmf") && contains(fetched.errors[1], "mismatch"));
    CHECK(fetched.assets.empty());
    auto again = dpm::fetch::resolve_assets(fresh.cache, project, test_policy(), env.home.path, &tampered);
    CHECK(!again.ok);
    CHECK_EQ(again.errors.size(), std::size_t{2});
    CHECK(again.assets.empty());

    TarBuilder other;
    other.dir("ovmf/").file("ovmf/code.fd", "replaced upstream");
    write_binary(env.home.path / "ovmf.tar", other.finish());
    fs::remove(archive);
    (void)compress(env.home.path / "ovmf.tar", "gzip", ".gz");
    Env cold;
    auto replaced = dpm::fetch::fetch_locked(cold.cache, *good, test_policy(), {}, &*good);
    CHECK(!replaced.ok);
    REQUIRE(replaced.errors.size() == 1);
    CHECK(contains(replaced.errors[0], "asset ovmf") && contains(replaced.errors[0], "mismatch"));
    CHECK(!fs::exists(cold.cache.src_path(r.assets[1].hash)));
    (void)commit;
}

TEST_CASE("a missing path asset and an unusable location are errors that name the asset")
{
    Env env;
    dpm::manifest::Project project;
    project.name = "k";
    project.version = "1.0.0";
    project.assets.push_back(asset("gone", "path:no/such/dir"));
    project.assets.push_back(asset("bad", "git+https://"));
    auto r = dpm::fetch::resolve_assets(env.cache, project, test_policy(), env.home.path, nullptr);
    CHECK(!r.ok);
    REQUIRE(r.errors.size() == 2);
    CHECK(contains(r.errors[0], "asset bad"));
    CHECK(contains(r.errors[1], "asset gone") && contains(r.errors[1], "does not exist"));
}

SECTION("dpm process runner");

TEST_CASE("the runner captures output, passes only the given environment and reports failures")
{
    dpm::process::Command cmd;
    cmd.argv = {"sh", "-c", "echo out; echo err >&2; echo \"$ONLY\"; echo \"${HOME:-unset}\"; exit 3"};
    cmd.env["PATH"] = "/usr/bin:/bin";
    cmd.env["ONLY"] = "yes";
    auto r = dpm::process::run(cmd);
    CHECK(r.started);
    CHECK_EQ(r.exit_code, 3);
    CHECK(!r.ok());
    CHECK_EQ(r.out, std::string{"out\nyes\nunset\n"});
    CHECK_EQ(r.err, std::string{"err\n"});
    dpm::process::Command missing;
    missing.argv = {"definitely-not-a-program"};
    missing.env["PATH"] = "/usr/bin:/bin";
    auto m = dpm::process::run(missing);
    CHECK(!m.started);
    CHECK(!m.error.empty());
}

TEST_CASE("the runner enforces timeouts, capture limits and file size limits")
{
    dpm::process::Command slow;
    slow.argv = {"sh", "-c", "sleep 30"};
    slow.env["PATH"] = "/usr/bin:/bin";
    slow.timeout = std::chrono::milliseconds(300);
    auto start = std::chrono::steady_clock::now();
    auto r = dpm::process::run(slow);
    CHECK(r.timed_out);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));

    dpm::process::Command loud;
    loud.argv = {"sh", "-c", "yes | head -c 5000000"};
    loud.env["PATH"] = "/usr/bin:/bin";
    loud.max_capture = 1000;
    auto l = dpm::process::run(loud);
    CHECK(l.truncated);
    CHECK(l.out.size() <= 1000);

    TempDir td;
    dpm::process::Command big;
    big.argv = {"sh", "-c", "head -c 100000 /dev/zero"};
    big.env["PATH"] = "/usr/bin:/bin";
    big.stdout_file = td.path / "out";
    big.max_file_size = 1000;
    auto b = dpm::process::run(big);
    CHECK(!b.ok());
    CHECK(fs::file_size(td.path / "out") <= 1000);

    dpm::process::Command stream;
    stream.argv = {"sh", "-c", "yes | head -c 5000000"};
    stream.env["PATH"] = "/usr/bin:/bin";
    std::size_t seen = 0;
    stream.on_stdout = [&](std::string_view chunk)
    {
        seen += chunk.size();
        return seen < 100000;
    };
    auto s = dpm::process::run(stream);
    CHECK(s.aborted);
    CHECK(seen < 5000000);
}

TEST_CASE("a cached hash is only trusted for an exact pair the lock already pins")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    auto first = commit_all(repo, "one");
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    repo.write("project.dcon", project_text("lib", "1.1.0"));
    auto second = commit_all(repo, "two");
    REQUIRE(git(repo.path, {"tag", "v1.1.0"}).ok());
    CHECK_NE(first, second);

    Env env;
    auto a = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.1.0"}), test_policy());
    REQUIRE(a.ok);
    CHECK_EQ(a.commit, second);

    auto wrong = request_for(repo, {"v1.0.0"}, first, a.hash);
    auto refused = dpm::fetch::fetch_git(env.cache, wrong, test_policy());
    CHECK(!refused.ok);
    CHECK(contains(refused.error, "hash mismatch"));

    wrong.allow_cached = true;
    auto trusted = dpm::fetch::fetch_git(env.cache, wrong, test_policy());
    CHECK(trusted.ok);

    auto right = request_for(repo, {"v1.1.0"}, second, a.hash);
    auto verified = dpm::fetch::fetch_git(env.cache, right, test_policy());
    REQUIRE(verified.ok);
    CHECK_EQ(verified.commit, second);
}

TEST_CASE("fetch_locked verifies a new commit and hash pair even when the hash is cached")
{
    TempDir repo;
    init_repo(repo);
    repo.write("project.dcon", project_text("lib", "1.0.0"));
    auto first = commit_all(repo, "one");
    REQUIRE(git(repo.path, {"tag", "v1.0.0"}).ok());
    repo.write("project.dcon", project_text("lib", "1.1.0"));
    auto second = commit_all(repo, "two");
    REQUIRE(git(repo.path, {"tag", "v1.1.0"}).ok());

    Env env;
    auto a = dpm::fetch::fetch_git(env.cache, request_for(repo, {"v1.1.0"}), test_policy());
    REQUIRE(a.ok);

    dpm::manifest::Lock lock;
    dpm::manifest::LockedPackage p;
    p.location = "git+" + url_of(repo);
    p.version = "1.1.0";
    p.source = p.location;
    p.commit = first;
    p.hash = a.hash;
    lock.packages.push_back(p);

    auto unpinned = dpm::fetch::fetch_locked(env.cache, lock, test_policy());
    CHECK(!unpinned.ok);
    REQUIRE(!unpinned.errors.empty());
    CHECK(contains(unpinned.errors[0], "hash mismatch"));

    dpm::manifest::Lock other = lock;
    other.packages[0].commit = second;
    auto different = dpm::fetch::fetch_locked(env.cache, lock, test_policy(), {}, &other);
    CHECK(!different.ok);

    auto pinned = dpm::fetch::fetch_locked(env.cache, lock, test_policy(), {}, &lock);
    CHECK(pinned.ok);
    CHECK_EQ(pinned.present, std::size_t{1});

    lock.packages[0].commit = second;
    auto correct = dpm::fetch::fetch_locked(env.cache, lock, test_policy());
    CHECK(correct.ok);
}

TEST_CASE("a tarball hash that is already cached is verified against the downloaded archive unless the lock pins it")
{
    Env env;
    TarBuilder one;
    one.dir("pkg-1/").file("pkg-1/project.dcon", project_text("pkg", "1.0.0")).file("pkg-1/src/pkg/x.dc", "module pkg::x;\n");
    auto url_one = "file://" + make_archive(env.home, "one", one.finish()).string();
    TarBuilder two;
    two.dir("pkg-2/").file("pkg-2/project.dcon", project_text("pkg", "2.0.0")).file("pkg-2/src/pkg/x.dc", "module pkg::x;\n");
    auto url_two = "file://" + make_archive(env.home, "two", two.finish()).string();
    auto a = dpm::fetch::fetch_tarball(env.cache, url_one, {}, test_policy());
    REQUIRE(a.ok);
    auto refused = dpm::fetch::fetch_tarball(env.cache, url_two, a.hash, test_policy());
    CHECK(!refused.ok);
    CHECK(contains(refused.error, "hash mismatch"));
    auto trusted = dpm::fetch::fetch_tarball(env.cache, url_two, a.hash, test_policy(), true);
    CHECK(trusted.ok);
}
