import std;
import dpm.hash;

#include "harness.hh"

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        static inline int s_counter{0};

        TempDir()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("dpm_hash_" + std::to_string(stamp) + "_" + std::to_string(++s_counter));
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write(std::filesystem::path const& relative, std::string const& content, bool exec = false) const
        {
            auto full = path / relative;
            std::filesystem::create_directories(full.parent_path());
            {
                std::ofstream f{full, std::ios::binary};
                f << content;
            }
            std::filesystem::permissions(full, exec ? std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::others_read
                                                    : std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read,
                                         std::filesystem::perm_options::replace);
        }
    };

    struct Spec
    {
        std::string path;
        std::string content;
        bool exec;
    };

    std::vector<Spec> sample()
    {
        return {{"a.txt", "hello\n", false}, {"bin/run", "#!/bin/sh\n", true}, {"dir/sub/z", "", false}, {"B", "upper", false}};
    }

    void build(TempDir const& td, std::vector<Spec> const& specs)
    {
        for (auto const& s : specs)
            td.write(s.path, s.content, s.exec);
    }

    constexpr auto k_sample_hash = "sha256:26d262d3fd5b502e9dad5dfbeff69165bcf2fd8e7e21e1348b9985b617b6e099";
} // namespace

SECTION("dpm sha256");

TEST_CASE("sha256 known vectors")
{
    CHECK_EQ(dpm::hash::sha256_hex(""), std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
    CHECK_EQ(dpm::hash::sha256_hex("abc"), std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
    CHECK_EQ(dpm::hash::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string{"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"});
    dpm::hash::Sha256 h;
    std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i)
        h.update(chunk);
    CHECK_EQ(h.finish_hex(), std::string{"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"});
}

TEST_CASE("sha256 block boundary lengths")
{
    std::vector<std::pair<std::size_t, std::string>> cases = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
        {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
    };
    for (auto const& [length, expected] : cases)
        CHECK_EQ(dpm::hash::sha256_hex(std::string(length, 'a')), expected);
}

TEST_CASE("sha256 chunking does not change the digest")
{
    std::mt19937 rng{12345};
    for (int round = 0; round < 200; ++round)
    {
        std::string data(rng() % 400, '\0');
        for (auto& c : data)
            c = static_cast<char>(rng());
        dpm::hash::Sha256 h;
        std::size_t pos = 0;
        while (pos < data.size())
        {
            auto take = std::min<std::size_t>(rng() % 70 + 1, data.size() - pos);
            h.update(std::string_view{data}.substr(pos, take));
            pos += take;
        }
        CHECK_EQ(h.finish_hex(), dpm::hash::sha256_hex(data));
    }
}

TEST_CASE("a hasher can be reused after finish")
{
    dpm::hash::Sha256 h;
    h.update("abc");
    (void)h.finish_hex();
    h.update("abc");
    CHECK_EQ(h.finish_hex(), dpm::hash::sha256_hex("abc"));
}

TEST_CASE("hash strings are validated")
{
    CHECK(dpm::hash::is_hash_string(k_sample_hash));
    CHECK(!dpm::hash::is_hash_string("sha256:ABC"));
    CHECK(!dpm::hash::is_hash_string(std::string{"sha256:"} + std::string(64, 'A')));
    CHECK(!dpm::hash::is_hash_string(std::string{"sha1:"} + std::string(64, 'a')));
    CHECK(!dpm::hash::is_hash_string(std::string(64, 'a')));
}

TEST_CASE("file hashing reports size and digest")
{
    TempDir td;
    td.write("f", "abc");
    auto r = dpm::hash::hash_file(td.path / "f");
    REQUIRE(r.ok);
    CHECK_EQ(r.hex, dpm::hash::sha256_hex("abc"));
    CHECK_EQ(r.size, std::uint64_t{3});
    CHECK(!dpm::hash::hash_file(td.path / "missing").ok);
}

SECTION("dpm tree hash");

TEST_CASE("tree hash matches an independently computed listing")
{
    TempDir td;
    build(td, sample());
    td.write(".git/config", "ignored");
    auto r = dpm::hash::tree_hash(td.path);
    REQUIRE(r.ok);
    CHECK_EQ(r.hash, std::string{k_sample_hash});
    CHECK_EQ(r.files, std::size_t{4});
    CHECK_EQ(r.bytes, std::uint64_t{6 + 10 + 0 + 5});
}

TEST_CASE("tree hash ignores creation order, timestamps and group or other modes")
{
    std::mt19937 rng{7};
    for (int round = 0; round < 8; ++round)
    {
        TempDir td;
        auto specs = sample();
        std::ranges::shuffle(specs, rng);
        build(td, specs);
        timeval times[2] = {{static_cast<time_t>(1000000 + round * 977), 0}, {static_cast<time_t>(2000000 + round * 131), 0}};
        ::utimes((td.path / "a.txt").c_str(), times);
        std::filesystem::permissions(td.path / "B", std::filesystem::perms::others_write, std::filesystem::perm_options::add);
        auto r = dpm::hash::tree_hash(td.path);
        REQUIRE(r.ok);
        CHECK_EQ(r.hash, std::string{k_sample_hash});
    }
}

TEST_CASE("the exec bit, content, names and empty files change the hash")
{
    auto base = [](std::function<void(TempDir&)> mutate) -> std::string
    {
        TempDir td;
        build(td, sample());
        mutate(td);
        auto r = dpm::hash::tree_hash(td.path);
        CHECK(r.ok);
        return r.hash;
    };
    auto same = base([](TempDir&) {});
    CHECK_NE(base([](TempDir& t) { t.write("a.txt", "hello\n", true); }), same);
    CHECK_NE(base([](TempDir& t) { t.write("a.txt", "hello"); }), same);
    CHECK_NE(base([](TempDir& t) { t.write("extra", ""); }), same);
    CHECK_NE(base([](TempDir& t) { std::filesystem::rename(t.path / "B", t.path / "C"); }), same);
    CHECK_NE(base([](TempDir& t) { std::filesystem::rename(t.path / "a.txt", t.path / "dir" / "a.txt"); }), same);
    CHECK_EQ(base([](TempDir& t) { std::filesystem::create_directories(t.path / "empty" / "dirs"); }), same);
}

TEST_CASE("line endings are hashed as stored")
{
    TempDir lf;
    TempDir crlf;
    lf.write("f", "a\nb\n");
    crlf.write("f", "a\r\nb\r\n");
    CHECK_NE(dpm::hash::tree_hash(lf.path).hash, dpm::hash::tree_hash(crlf.path).hash);
}

TEST_CASE("paths sort bytewise")
{
    TempDir td;
    td.write("b", "1");
    td.write("a/z", "2");
    td.write("a.b", "3");
    td.write("Z", "4");
    td.write("\xc3\xa9", "5");
    auto r = dpm::hash::tree_hash(td.path);
    REQUIRE(r.ok);
    dpm::hash::Sha256 outer;
    std::vector<std::pair<std::string, std::string>> listing = {{"Z", "4"}, {"a.b", "3"}, {"a/z", "2"}, {"b", "1"}, {"\xc3\xa9", "5"}};
    for (auto const& [path, content] : listing)
    {
        outer.update(path);
        outer.update(std::string_view{"\0" "0" "\0", 3});
        outer.update(dpm::hash::sha256_hex(content));
        outer.update("\n");
    }
    CHECK_EQ(r.hash, "sha256:" + outer.finish_hex());
}

TEST_CASE("symbolic links and special files are rejected")
{
    TempDir td;
    build(td, sample());
    std::filesystem::create_symlink("a.txt", td.path / "link");
    auto r = dpm::hash::tree_hash(td.path);
    CHECK(!r.ok);
    CHECK(r.error.find("link") != std::string::npos);

    TempDir dangling;
    build(dangling, sample());
    std::filesystem::create_symlink("nowhere", dangling.path / "dir" / "sub" / "bad");
    CHECK(!dpm::hash::tree_hash(dangling.path).ok);

    TempDir fifo;
    build(fifo, sample());
    ::mkfifo((fifo.path / "pipe").c_str(), 0600);
    auto f = dpm::hash::tree_hash(fifo.path);
    CHECK(!f.ok);
    CHECK(f.error.find("special") != std::string::npos);
}

TEST_CASE("a missing or non-directory root is an error")
{
    TempDir td;
    CHECK(!dpm::hash::tree_hash(td.path / "nope").ok);
    td.write("f", "x");
    CHECK(!dpm::hash::tree_hash(td.path / "f").ok);
}

TEST_CASE("the empty tree hashes the empty listing")
{
    TempDir td;
    auto r = dpm::hash::tree_hash(td.path);
    REQUIRE(r.ok);
    CHECK_EQ(r.hash, "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
