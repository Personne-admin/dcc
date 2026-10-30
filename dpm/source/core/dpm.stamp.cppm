module;

#include <unistd.h>

export module dpm.stamp;

import std;
import dpm.hash;

export namespace dpm::stamp
{
    inline constexpr std::chrono::seconds k_racy_window{2};
    inline constexpr std::size_t k_max_inputs = 100000;
    inline constexpr std::uintmax_t k_max_stamp_size = 64ull << 20;

    struct Input
    {
        std::filesystem::path path;
        std::uint64_t size{};
        std::int64_t mtime_ns{};
        bool racy{};
        std::string hash;
    };

    struct Stamp
    {
        std::string key;
        std::uint64_t output_size{};
        std::int64_t output_mtime_ns{};
        std::vector<Input> inputs;
    };

    enum class State : std::uint8_t
    {
        Fresh,
        KeyChanged,
        OutputChanged,
        InputChanged,
        InputMissing,
    };

    struct Verdict
    {
        State state{State::KeyChanged};
        std::string detail;
        bool refreshed{};
    };

    [[nodiscard]] inline std::int64_t to_ns(std::filesystem::file_time_type t)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    }

    [[nodiscard]] inline std::string make_key(std::span<std::string const> argv, std::string_view compiler_identity, std::string_view fingerprint)
    {
        hash::Sha256 h;
        auto add = [&](std::string_view part)
        {
            h.update(std::to_string(part.size()));
            h.update(":");
            h.update(part);
            h.update("\n");
        };
        add("dpm-stamp-key 1");
        add(std::to_string(argv.size()));
        for (auto const& a : argv)
            add(a);
        add(compiler_identity);
        add(fingerprint);
        return "sha256:" + h.finish_hex();
    }

    [[nodiscard]] inline std::vector<std::filesystem::path> parse_depfile(std::string_view text)
    {
        std::vector<std::string> words;
        std::string word;
        bool in_word = false;
        auto flush = [&]
        {
            if (in_word)
                words.push_back(std::move(word));
            word.clear();
            in_word = false;
        };
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size())
            {
                char next = text[i + 1];
                if (next == '\n')
                {
                    ++i;
                    flush();
                    continue;
                }
                if (next == ' ' || next == '#' || next == ':' || next == '%' || next == '\t' || next == '\\')
                {
                    word += next;
                    in_word = true;
                    ++i;
                    continue;
                }
            }
            if (c == '$' && i + 1 < text.size() && text[i + 1] == '$')
            {
                word += '$';
                in_word = true;
                ++i;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                flush();
                continue;
            }
            word += c;
            in_word = true;
        }
        flush();
        std::vector<std::filesystem::path> paths;
        bool seen_target = false;
        for (auto& w : words)
        {
            if (!seen_target)
            {
                if (w.ends_with(':'))
                    seen_target = true;
                continue;
            }
            paths.emplace_back(std::move(w));
        }
        return paths;
    }

    [[nodiscard]] inline std::optional<Input> observe(std::filesystem::path const& path, std::filesystem::file_time_type built_at)
    {
        std::error_code ec;
        auto status = std::filesystem::status(path, ec);
        if (ec || !std::filesystem::is_regular_file(status))
            return std::nullopt;
        Input in;
        in.path = path;
        in.size = std::filesystem::file_size(path, ec);
        if (ec)
            return std::nullopt;
        auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec)
            return std::nullopt;
        in.mtime_ns = to_ns(mtime);
        in.racy = mtime + k_racy_window >= built_at;
        auto h = hash::hash_file(path);
        if (!h.ok || h.size != in.size)
            return std::nullopt;
        in.hash = h.hex;
        return in;
    }

    [[nodiscard]] inline std::optional<Stamp> record(std::string key, std::filesystem::path const& output, std::span<std::filesystem::path const> inputs,
                                                     std::filesystem::file_time_type built_at)
    {
        Stamp stamp;
        stamp.key = std::move(key);
        std::error_code ec;
        stamp.output_size = std::filesystem::file_size(output, ec);
        if (ec)
            return std::nullopt;
        auto mtime = std::filesystem::last_write_time(output, ec);
        if (ec)
            return std::nullopt;
        stamp.output_mtime_ns = to_ns(mtime);
        if (inputs.size() > k_max_inputs)
            return std::nullopt;
        std::set<std::filesystem::path> seen;
        for (auto const& path : inputs)
        {
            if (path.string().find('\n') != std::string::npos || !seen.insert(path).second)
                continue;
            auto in = observe(path, built_at);
            if (!in)
                return std::nullopt;
            stamp.inputs.push_back(std::move(*in));
        }
        return stamp;
    }

    [[nodiscard]] inline std::string print(Stamp const& s)
    {
        std::string out = "dpm-stamp 1\n";
        out += std::format("key {}\n", s.key);
        out += std::format("output {} {}\n", s.output_size, s.output_mtime_ns);
        for (auto const& in : s.inputs)
            out += std::format("input {} {} {} {} {}\n", in.size, in.mtime_ns, in.racy ? 1 : 0, in.hash, in.path.string());
        return out;
    }

    [[nodiscard]] inline std::optional<Stamp> parse(std::string_view text)
    {
        auto next_line = [&]() -> std::optional<std::string_view>
        {
            if (text.empty())
                return std::nullopt;
            auto end = text.find('\n');
            if (end == std::string_view::npos)
                return std::nullopt;
            auto line = text.substr(0, end);
            text.remove_prefix(end + 1);
            return line;
        };
        auto number = [](std::string_view s, auto& out)
        {
            auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
            return ec == std::errc{} && ptr == s.data() + s.size();
        };
        auto take = [](std::string_view& s) -> std::string_view
        {
            auto space = s.find(' ');
            auto word = s.substr(0, space);
            s = space == std::string_view::npos ? std::string_view{} : s.substr(space + 1);
            return word;
        };
        auto header = next_line();
        if (!header || *header != "dpm-stamp 1")
            return std::nullopt;
        Stamp stamp;
        auto key_line = next_line();
        if (!key_line || !key_line->starts_with("key ") || !hash::is_hash_string(key_line->substr(4)))
            return std::nullopt;
        stamp.key = std::string{key_line->substr(4)};
        auto output_line = next_line();
        if (!output_line || !output_line->starts_with("output "))
            return std::nullopt;
        auto rest = output_line->substr(7);
        if (!number(take(rest), stamp.output_size) || !number(rest, stamp.output_mtime_ns))
            return std::nullopt;
        while (auto line = next_line())
        {
            if (!line->starts_with("input ") || stamp.inputs.size() >= k_max_inputs)
                return std::nullopt;
            auto body = line->substr(6);
            Input in;
            int racy = 0;
            if (!number(take(body), in.size) || !number(take(body), in.mtime_ns) || !number(take(body), racy) || (racy != 0 && racy != 1))
                return std::nullopt;
            in.racy = racy == 1;
            in.hash = std::string{take(body)};
            if (in.hash.size() != 64 || body.empty())
                return std::nullopt;
            in.path = std::filesystem::path{std::string{body}};
            stamp.inputs.push_back(std::move(in));
        }
        if (!text.empty())
            return std::nullopt;
        return stamp;
    }

    [[nodiscard]] inline std::optional<Stamp> read(std::filesystem::path const& path)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
            return std::nullopt;
        auto size = std::filesystem::file_size(path, ec);
        if (ec || size > k_max_stamp_size)
            return std::nullopt;
        std::ifstream in{path, std::ios::binary};
        if (!in)
            return std::nullopt;
        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        return parse(text);
    }

    [[nodiscard]] inline bool write(std::filesystem::path const& path, Stamp const& stamp)
    {
        auto temp = path;
        temp += std::format(".tmp{}", ::getpid());
        {
            std::ofstream out{temp, std::ios::binary | std::ios::trunc};
            if (!out)
                return false;
            auto text = print(stamp);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!out)
                return false;
        }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec)
        {
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    }

    inline void remove(std::filesystem::path const& path)
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    [[nodiscard]] inline Verdict verify(Stamp& stamp, std::string_view key, std::filesystem::path const& output, std::filesystem::file_time_type now)
    {
        Verdict verdict;
        if (stamp.key != key)
        {
            verdict.detail = "the command line, the compiler or the dependencies changed";
            return verdict;
        }
        std::error_code ec;
        auto size = std::filesystem::file_size(output, ec);
        auto mtime = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(output, ec);
        if (ec || size != stamp.output_size || to_ns(mtime) != stamp.output_mtime_ns)
        {
            verdict.state = State::OutputChanged;
            verdict.detail = std::format("{} is missing or was modified", output.string());
            return verdict;
        }
        for (auto& in : stamp.inputs)
        {
            auto status = std::filesystem::status(in.path, ec);
            if (ec || !std::filesystem::is_regular_file(status))
            {
                verdict.state = State::InputMissing;
                verdict.detail = std::format("{} is gone", in.path.string());
                return verdict;
            }
            auto in_size = std::filesystem::file_size(in.path, ec);
            auto in_mtime = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(in.path, ec);
            if (ec)
            {
                verdict.state = State::InputMissing;
                verdict.detail = std::format("{} cannot be read", in.path.string());
                return verdict;
            }
            bool const unchanged = in_size == in.size && to_ns(in_mtime) == in.mtime_ns;
            if (unchanged && !in.racy)
                continue;
            if (in_size != in.size)
            {
                verdict.state = State::InputChanged;
                verdict.detail = std::format("{} changed size", in.path.string());
                return verdict;
            }
            auto h = hash::hash_file(in.path);
            if (!h.ok || h.hex != in.hash)
            {
                verdict.state = State::InputChanged;
                verdict.detail = std::format("{} changed", in.path.string());
                return verdict;
            }
            bool const racy = in_mtime + k_racy_window >= now;
            if (in.mtime_ns != to_ns(in_mtime) || in.racy != racy)
                verdict.refreshed = true;
            in.mtime_ns = to_ns(in_mtime);
            in.racy = racy;
        }
        verdict.state = State::Fresh;
        return verdict;
    }
} // namespace dpm::stamp
