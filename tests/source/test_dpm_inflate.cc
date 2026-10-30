import std;
import dpm.inflate;
import dpm.process;

#include "harness.hh"

#include <stdlib.h>

namespace
{
    namespace fs = std::filesystem;

    fs::path env_path(char const* name)
    {
        char const* value = ::getenv(name);
        return value ? fs::path{value} : fs::path{};
    }

    std::string read_file(fs::path const& path)
    {
        std::ifstream in{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    std::uint32_t crc32(std::string_view data)
    {
        std::uint32_t crc = 0xffffffffu;
        for (char ch : data)
        {
            crc ^= static_cast<unsigned char>(ch);
            for (int k = 0; k < 8; ++k)
                crc = (crc & 1) ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
        }
        return ~crc;
    }

    struct Outcome
    {
        dpm::inflate::Result result;
        std::string data;
    };

    Outcome inflate(std::string_view gz, dpm::inflate::Limits limits = {})
    {
        Outcome o;
        o.result = dpm::inflate::gunzip(gz, limits,
                                        [&](std::string_view chunk)
                                        {
                                            o.data.append(chunk);
                                            return true;
                                        });
        return o;
    }

    void le32(std::string& s, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }

    struct BitWriter
    {
        std::string bytes;
        int used{};

        void bit(unsigned b)
        {
            if (used == 0)
                bytes.push_back('\0');
            bytes.back() = static_cast<char>(static_cast<unsigned char>(bytes.back()) | (b << used));
            used = (used + 1) % 8;
        }

        void value(unsigned v, int n)
        {
            for (int i = 0; i < n; ++i)
                bit((v >> i) & 1);
        }

        void code(unsigned c, int n)
        {
            for (int i = n - 1; i >= 0; --i)
                bit((c >> i) & 1);
        }
    };

    std::string gzip_wrap(std::string const& deflated, std::string_view raw)
    {
        std::string out = std::string{"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10} + deflated;
        le32(out, crc32(raw));
        le32(out, static_cast<std::uint32_t>(raw.size()));
        return out;
    }

    void fixed_literal(BitWriter& w, unsigned c)
    {
        if (c < 144)
            w.code(0x30 + c, 8);
        else
            w.code(0x190 + (c - 144), 9);
    }

    std::vector<fs::path> fixtures(fs::path const& dir)
    {
        std::vector<fs::path> out;
        for (auto const& entry : fs::directory_iterator{dir})
            if (entry.path().extension() == ".gz")
                out.push_back(entry.path());
        std::ranges::sort(out);
        return out;
    }

    std::optional<std::string> gzip_tool(std::string const& raw, char const* level, fs::path const& scratch)
    {
        auto in = scratch / "in.bin";
        auto out = scratch / "out.gz";
        {
            std::ofstream f{in, std::ios::binary};
            f.write(raw.data(), static_cast<std::streamsize>(raw.size()));
        }
        dpm::process::Command cmd;
        cmd.argv = {"gzip", "-n", "-c", level};
        cmd.stdin_file = in;
        cmd.stdout_file = out;
        cmd.env["PATH"] = std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin";
        auto r = dpm::process::run(cmd);
        if (!r.ok())
            return std::nullopt;
        return read_file(out);
    }

    struct Scratch
    {
        fs::path path;

        Scratch()
        {
            path = fs::temp_directory_path() / ("dpm_inflate_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(path);
        }

        ~Scratch()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };
} // namespace

SECTION("dpm inflate");

TEST_CASE("every fixture inflates to its original bytes")
{
    auto dir = env_path("DCC_TEST_DPM_GZIP");
    if (dir.empty())
        return;
    auto files = fixtures(dir);
    REQUIRE(files.size() >= 15);
    for (auto const& gz : files)
    {
        auto raw_path = gz;
        raw_path.replace_extension(".raw");
        auto expected = read_file(raw_path);
        auto compressed = read_file(gz);
        auto o = inflate(compressed);
        if (!o.result.ok)
            std::println(std::cerr, "{}: {}", gz.filename().string(), o.result.error);
        CHECK(o.result.ok);
        CHECK(o.data == expected);
        CHECK_EQ(o.result.output, static_cast<std::uint64_t>(expected.size()));
    }
}

TEST_CASE("a large file round trips against the gzip tool")
{
    Scratch scratch;
    std::mt19937 rng{4242};
    std::vector<std::string> words;
    for (int i = 0; i < 500; ++i)
    {
        std::string w;
        for (unsigned j = 0, n = 3 + rng() % 9; j < n; ++j)
            w.push_back(static_cast<char>('a' + rng() % 26));
        words.push_back(w);
    }
    std::string raw;
    raw.reserve(24u << 20);
    while (raw.size() < (24u << 20))
    {
        raw += words[rng() % words.size()];
        raw += (rng() % 12 == 0) ? "\n" : " ";
        if (rng() % 5000 == 0)
            for (int i = 0; i < 300; ++i)
                raw.push_back(static_cast<char>(rng()));
    }
    for (char const* level : {"-1", "-9"})
    {
        auto gz = gzip_tool(raw, level, scratch.path);
        if (!gz)
        {
            CHECK(false);
            return;
        }
        auto o = inflate(*gz);
        CHECK(o.result.ok);
        CHECK(o.data == raw);
    }
}

TEST_CASE("the sink can stop inflation")
{
    auto dir = env_path("DCC_TEST_DPM_GZIP");
    if (dir.empty())
        return;
    auto compressed = read_file(dir / "level6.gz");
    std::size_t seen = 0;
    auto r = dpm::inflate::gunzip(compressed, {},
                                  [&](std::string_view chunk)
                                  {
                                      seen += chunk.size();
                                      return false;
                                  });
    CHECK(!r.ok);
    CHECK(r.aborted);
    CHECK(r.error.empty());
    CHECK(seen > 0);
}

TEST_CASE("size and ratio limits stop a bomb early")
{
    Scratch scratch;
    std::string zeros(64u << 20, '\0');
    auto gz = gzip_tool(zeros, "-9", scratch.path);
    REQUIRE(gz.has_value());
    CHECK(gz->size() < (1u << 20));

    dpm::inflate::Limits size_limit;
    size_limit.max_output = 1u << 20;
    auto a = inflate(*gz, size_limit);
    CHECK(!a.result.ok);
    CHECK(a.result.error.find("expanded size") != std::string::npos);
    CHECK(a.data.size() <= (1u << 20));

    dpm::inflate::Limits ratio_limit;
    ratio_limit.max_ratio = 100;
    auto b = inflate(*gz, ratio_limit);
    CHECK(!b.result.ok);
    CHECK(b.result.error.find("ratio") != std::string::npos);
    CHECK(b.data.size() < (8u << 20));

    auto c = inflate(*gz);
    CHECK(c.result.ok);
    CHECK_EQ(c.data.size(), zeros.size());
}

TEST_CASE("container errors are reported")
{
    auto dir = env_path("DCC_TEST_DPM_GZIP");
    if (dir.empty())
        return;
    auto good = read_file(dir / "dynamic.gz");
    auto expect_error = [&](std::string gz, std::string_view fragment)
    {
        auto o = inflate(gz);
        CHECK(!o.result.ok);
        CHECK(o.result.error.find(fragment) != std::string::npos);
        if (o.result.error.find(fragment) == std::string::npos)
            std::println(std::cerr, "      got: {}", o.result.error);
    };
    auto bad_crc = good;
    bad_crc[good.size() - 8] ^= 1;
    expect_error(bad_crc, "checksum");
    auto bad_size = good;
    bad_size[good.size() - 1] ^= 1;
    expect_error(bad_size, "size");
    expect_error(good + std::string(1, '\0'), "trailing");
    expect_error(good + good, "concatenated");
    expect_error(std::string{"\x1f\x8c"} + good.substr(2), "not a gzip");
    auto method = good;
    method[2] = 7;
    expect_error(method, "method");
    auto reserved = good;
    reserved[3] = static_cast<char>(0x20);
    expect_error(reserved, "reserved");
    expect_error("", "truncated");
    expect_error(good.substr(0, 5), "truncated");
    auto hcrc = read_file(dir / "header-all.gz");
    hcrc[hcrc.size() > 40 ? 30 : 12] ^= 0;
    auto flipped = hcrc;
    flipped[10] ^= 0x7f;
    auto o = inflate(flipped);
    CHECK(!o.result.ok);
    CHECK(inflate(read_file(dir / "header-all.gz")).result.ok);
}

TEST_CASE("every truncation of a stream is an error")
{
    auto dir = env_path("DCC_TEST_DPM_GZIP");
    if (dir.empty())
        return;
    for (char const* name : {"dynamic.gz", "stored.gz", "fixed.gz", "header-all.gz", "long-runs.gz"})
    {
        auto good = read_file(dir / name);
        for (std::size_t len = 0; len < good.size(); ++len)
        {
            auto o = inflate(std::string_view{good}.substr(0, len));
            if (o.result.ok)
            {
                CHECK(false);
                std::println(std::cerr, "      {} accepted at {} of {}", name, len, good.size());
                break;
            }
            CHECK(!o.result.error.empty());
        }
    }
}

TEST_CASE("malformed deflate data is rejected")
{
    auto expect_error = [](BitWriter const& w, std::string_view fragment)
    {
        auto o = inflate(gzip_wrap(w.bytes, "aaaa"));
        CHECK(!o.result.ok);
        CHECK(o.result.error.find(fragment) != std::string::npos);
        if (o.result.error.find(fragment) == std::string::npos)
            std::println(std::cerr, "      wanted '{}', got: {}", fragment, o.result.error);
    };
    {
        BitWriter w;
        w.value(1, 1);
        w.value(3, 2);
        expect_error(w, "invalid block type");
    }
    {
        BitWriter w;
        w.value(1, 1);
        w.value(0, 2);
        while (w.used != 0)
            w.bit(0);
        w.bytes += std::string{"\x05\x00\x00\x00", 4};
        expect_error(w, "length check");
    }
    {
        BitWriter w;
        w.value(1, 1);
        w.value(1, 2);
        fixed_literal(w, 'a');
        w.code(1, 7);
        w.code(1, 5);
        expect_error(w, "before the start");
    }
    {
        BitWriter w;
        w.value(1, 1);
        w.value(1, 2);
        fixed_literal(w, 'a');
        w.code(1, 7);
        w.code(30, 5);
        expect_error(w, "Huffman code");
    }
    {
        BitWriter w;
        w.value(1, 1);
        w.value(1, 2);
        w.code(0xc0 + 6, 8);
        expect_error(w, "invalid length code");
    }
    {
        BitWriter w;
        w.value(1, 1);
        w.value(2, 2);
        w.value(0, 5);
        w.value(0, 5);
        w.value(0, 4);
        w.value(1, 3);
        w.value(1, 3);
        w.value(1, 3);
        w.value(0, 3);
        expect_error(w, "code length code");
    }
    {
        BitWriter w;
        w.value(1, 1);
        w.value(2, 2);
        w.value(0, 5);
        w.value(0, 5);
        w.value(0, 4);
        w.value(1, 3);
        w.value(0, 3);
        w.value(0, 3);
        w.value(0, 3);
        expect_error(w, "code length code");
    }
    {
        BitWriter w;
        w.value(1, 1);
        w.value(2, 2);
        w.value(31, 5);
        w.value(0, 5);
        w.value(0, 4);
        expect_error(w, "too many");
    }
    {
        BitWriter w;
        w.value(1, 1);
        w.value(1, 2);
        fixed_literal(w, 'a');
        w.code(0, 7);
        auto ok_stream = gzip_wrap(w.bytes, "a");
        auto o = inflate(ok_stream);
        CHECK(o.result.ok);
        CHECK_EQ(o.data, std::string{"a"});
    }
}

TEST_CASE("back references reach the maximum length and distance")
{
    BitWriter w;
    w.value(1, 1);
    w.value(1, 2);
    for (int i = 0; i < 5; ++i)
        fixed_literal(w, static_cast<unsigned>('a' + i));
    w.code(0xc0 + 5, 8);
    w.code(1, 5);
    w.code(0, 7);
    std::string expected = "abcde";
    for (int i = 0; i < 258; ++i)
        expected.push_back(expected[expected.size() - 2]);
    auto o = inflate(gzip_wrap(w.bytes, expected));
    CHECK(o.result.ok);
    CHECK(o.data == expected);
}

TEST_CASE("mutated streams never crash and succeed only with a verified checksum")
{
    auto dir = env_path("DCC_TEST_DPM_GZIP");
    if (dir.empty())
        return;
    std::vector<std::string> seeds;
    for (char const* name : {"dynamic.gz", "stored.gz", "fixed.gz", "header-all.gz", "long-runs.gz", "multi-block.gz", "level1.gz", "empty.gz"})
        seeds.push_back(read_file(dir / name));
    std::mt19937 rng{777};
    dpm::inflate::Limits limits;
    limits.max_output = 16u << 20;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (int round = 0; round < 6000; ++round)
    {
        auto data = seeds[rng() % seeds.size()];
        switch (rng() % 6)
        {
            case 0:
                for (unsigned i = 0, n = 1 + rng() % 4; i < n && !data.empty(); ++i)
                    data[rng() % data.size()] ^= static_cast<char>(1u << (rng() % 8));
                break;
            case 1:
                data.resize(rng() % (data.size() + 1));
                break;
            case 2:
            {
                auto other = seeds[rng() % seeds.size()];
                data = data.substr(0, rng() % (data.size() + 1)) + other.substr(rng() % (other.size() + 1));
                break;
            }
            case 3:
                if (!data.empty())
                    data.erase(rng() % data.size(), 1 + rng() % 3);
                break;
            case 4:
                data.insert(data.empty() ? 0 : rng() % data.size(), 1 + rng() % 3, static_cast<char>(rng()));
                break;
            default:
                if (!data.empty())
                    data[rng() % data.size()] = static_cast<char>(rng());
                break;
        }
        auto o = inflate(data, limits);
        if (o.result.ok)
        {
            ++accepted;
            REQUIRE(data.size() >= 18);
            std::uint32_t stored_crc = 0;
            std::uint32_t stored_size = 0;
            for (int i = 0; i < 4; ++i)
            {
                stored_crc |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[data.size() - 8 + static_cast<std::size_t>(i)])) << (8 * i);
                stored_size |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[data.size() - 4 + static_cast<std::size_t>(i)])) << (8 * i);
            }
            CHECK_EQ(crc32(o.data), stored_crc);
            CHECK_EQ(static_cast<std::uint32_t>(o.data.size()), stored_size);
        }
        else
        {
            ++rejected;
            CHECK(!o.result.error.empty());
        }
    }
    CHECK(rejected > 3000);
    CHECK(accepted > 0);
}
