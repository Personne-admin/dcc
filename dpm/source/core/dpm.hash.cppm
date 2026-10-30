export module dpm.hash;

import std;

export namespace dpm::hash
{
    class Sha256
    {
    public:
        Sha256() { reset(); }

        void reset()
        {
            state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
            length_ = 0;
            used_ = 0;
        }

        void update(std::string_view data) { update(reinterpret_cast<unsigned char const*>(data.data()), data.size()); }

        void update(unsigned char const* data, std::size_t size)
        {
            length_ += size;
            while (size > 0)
            {
                auto take = std::min(size, block_.size() - used_);
                std::copy_n(data, take, block_.begin() + static_cast<std::ptrdiff_t>(used_));
                used_ += take;
                data += take;
                size -= take;
                if (used_ == block_.size())
                {
                    compress(block_.data());
                    used_ = 0;
                }
            }
        }

        [[nodiscard]] std::array<unsigned char, 32> finish()
        {
            std::uint64_t const bits = length_ * 8;
            unsigned char const pad = 0x80;
            update(&pad, 1);
            unsigned char const zero = 0;
            while (used_ != 56)
                update(&zero, 1);
            std::array<unsigned char, 8> tail{};
            for (int i = 0; i < 8; ++i)
                tail[static_cast<std::size_t>(i)] = static_cast<unsigned char>(bits >> (56 - 8 * i));
            update(tail.data(), tail.size());
            std::array<unsigned char, 32> out{};
            for (std::size_t i = 0; i < 8; ++i)
                for (std::size_t j = 0; j < 4; ++j)
                    out[i * 4 + j] = static_cast<unsigned char>(state_[i] >> (24 - 8 * j));
            reset();
            return out;
        }

        [[nodiscard]] std::string finish_hex()
        {
            auto digest = finish();
            std::string out;
            out.reserve(64);
            for (auto byte : digest)
                out += std::format("{:02x}", byte);
            return out;
        }

    private:
        static constexpr std::array<std::uint32_t, 64> k_round = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu,
            0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau,
            0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u,
            0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
            0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        static constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

        void compress(unsigned char const* chunk)
        {
            std::array<std::uint32_t, 64> w{};
            for (std::size_t i = 0; i < 16; ++i)
                w[i] = (std::uint32_t{chunk[i * 4]} << 24) | (std::uint32_t{chunk[i * 4 + 1]} << 16) | (std::uint32_t{chunk[i * 4 + 2]} << 8) |
                       std::uint32_t{chunk[i * 4 + 3]};
            for (std::size_t i = 16; i < 64; ++i)
            {
                auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            auto v = state_;
            for (std::size_t i = 0; i < 64; ++i)
            {
                auto s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
                auto ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
                auto t1 = v[7] + s1 + ch + k_round[i] + w[i];
                auto s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
                auto maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
                auto t2 = s0 + maj;
                v[7] = v[6];
                v[6] = v[5];
                v[5] = v[4];
                v[4] = v[3] + t1;
                v[3] = v[2];
                v[2] = v[1];
                v[1] = v[0];
                v[0] = t1 + t2;
            }
            for (std::size_t i = 0; i < 8; ++i)
                state_[i] += v[i];
        }

        std::array<std::uint32_t, 8> state_{};
        std::array<unsigned char, 64> block_{};
        std::size_t used_{};
        std::uint64_t length_{};
    };

    [[nodiscard]] inline std::string sha256_hex(std::string_view data)
    {
        Sha256 h;
        h.update(data);
        return h.finish_hex();
    }

    [[nodiscard]] inline bool is_hash_string(std::string_view s)
    {
        if (!s.starts_with("sha256:") || s.size() != 7 + 64)
            return false;
        return std::ranges::all_of(s.substr(7), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    struct FileHash
    {
        bool ok{};
        std::string hex;
        std::uint64_t size{};
        std::string error;
    };

    [[nodiscard]] inline FileHash hash_file(std::filesystem::path const& path)
    {
        std::ifstream in{path, std::ios::binary};
        if (!in)
            return {false, {}, 0, "cannot read " + path.string()};
        Sha256 h;
        std::array<char, 65536> buffer{};
        std::uint64_t total = 0;
        while (in)
        {
            in.read(buffer.data(), buffer.size());
            auto got = static_cast<std::size_t>(in.gcount());
            h.update(reinterpret_cast<unsigned char const*>(buffer.data()), got);
            total += got;
        }
        if (!in.eof())
            return {false, {}, 0, "cannot read " + path.string()};
        return {true, h.finish_hex(), total, {}};
    }

    struct TreeHash
    {
        bool ok{};
        std::string hash;
        std::size_t files{};
        std::uint64_t bytes{};
        std::string error;
    };

    [[nodiscard]] inline TreeHash tree_hash(std::filesystem::path const& root)
    {
        TreeHash result;
        std::error_code ec;
        auto root_status = std::filesystem::symlink_status(root, ec);
        if (ec || !std::filesystem::is_directory(root_status))
        {
            result.error = "not a directory: " + root.string();
            return result;
        }
        struct Entry
        {
            std::string rel;
            std::filesystem::path full;
            bool exec{};
        };
        std::vector<Entry> entries;
        std::vector<std::filesystem::path> stack{root};
        while (!stack.empty())
        {
            auto dir = std::move(stack.back());
            stack.pop_back();
            std::filesystem::directory_iterator it{dir, ec};
            if (ec)
            {
                result.error = "cannot list " + dir.string();
                return result;
            }
            for (auto const& item : it)
            {
                auto rel = item.path().lexically_relative(root).generic_string();
                if (rel == ".git")
                    continue;
                auto status = item.symlink_status(ec);
                if (ec)
                {
                    result.error = "cannot stat " + rel;
                    return result;
                }
                if (std::filesystem::is_symlink(status))
                {
                    result.error = "symbolic link not allowed: " + rel;
                    return result;
                }
                if (std::filesystem::is_directory(status))
                    stack.push_back(item.path());
                else if (std::filesystem::is_regular_file(status))
                {
                    bool exec = (status.permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
                    entries.push_back({std::move(rel), item.path(), exec});
                }
                else
                {
                    result.error = "special file not allowed: " + rel;
                    return result;
                }
            }
        }
        std::ranges::sort(entries, [](Entry const& a, Entry const& b) { return a.rel < b.rel; });
        Sha256 outer;
        for (auto const& e : entries)
        {
            auto file = hash_file(e.full);
            if (!file.ok)
            {
                result.error = file.error;
                return result;
            }
            outer.update(e.rel);
            outer.update(std::string_view{"\0", 1});
            outer.update(e.exec ? "1" : "0");
            outer.update(std::string_view{"\0", 1});
            outer.update(file.hex);
            outer.update("\n");
            result.bytes += file.size;
        }
        result.files = entries.size();
        result.hash = "sha256:" + outer.finish_hex();
        result.ok = true;
        return result;
    }
} // namespace dpm::hash
