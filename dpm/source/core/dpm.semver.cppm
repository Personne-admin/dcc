export module dpm.semver;

import std;

export namespace dpm::semver
{
    struct Version
    {
        std::uint64_t major{};
        std::uint64_t minor{};
        std::uint64_t patch{};
        std::vector<std::string> pre;
        std::string build;

        [[nodiscard]] bool is_prerelease() const noexcept { return !pre.empty(); }
    };

    struct Series
    {
        std::uint64_t major{};
        std::optional<std::uint64_t> minor;

        [[nodiscard]] bool operator==(Series const&) const noexcept = default;

        [[nodiscard]] std::string to_string() const
        {
            if (minor)
                return std::format("{}.{}.x", major, *minor);
            return std::format("{}.x", major);
        }
    };

    namespace detail
    {
        inline bool all_digits(std::string_view s)
        {
            return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
        }

        inline bool identifier_chars(std::string_view s)
        {
            return !s.empty() && std::ranges::all_of(s, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; });
        }

        inline std::optional<std::uint64_t> number(std::string_view s)
        {
            if (!all_digits(s) || (s.size() > 1 && s[0] == '0'))
                return std::nullopt;
            std::uint64_t value = 0;
            auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
            if (ec != std::errc{} || ptr != s.data() + s.size())
                return std::nullopt;
            return value;
        }

        inline std::vector<std::string_view> split(std::string_view s, char sep)
        {
            std::vector<std::string_view> parts;
            std::size_t start = 0;
            while (true)
            {
                auto pos = s.find(sep, start);
                parts.push_back(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
                if (pos == std::string_view::npos)
                    break;
                start = pos + 1;
            }
            return parts;
        }
    } // namespace detail

    [[nodiscard]] inline std::optional<Version> parse(std::string_view text)
    {
        Version v;
        if (auto plus = text.find('+'); plus != std::string_view::npos)
        {
            v.build = std::string{text.substr(plus + 1)};
            text = text.substr(0, plus);
            for (auto part : detail::split(v.build, '.'))
                if (!detail::identifier_chars(part))
                    return std::nullopt;
        }
        if (auto dash = text.find('-'); dash != std::string_view::npos)
        {
            auto pre = text.substr(dash + 1);
            text = text.substr(0, dash);
            for (auto part : detail::split(pre, '.'))
            {
                if (!detail::identifier_chars(part))
                    return std::nullopt;
                if (detail::all_digits(part) && !detail::number(part))
                    return std::nullopt;
                v.pre.emplace_back(part);
            }
        }
        auto core = detail::split(text, '.');
        if (core.size() != 3)
            return std::nullopt;
        auto major = detail::number(core[0]);
        auto minor = detail::number(core[1]);
        auto patch = detail::number(core[2]);
        if (!major || !minor || !patch)
            return std::nullopt;
        v.major = *major;
        v.minor = *minor;
        v.patch = *patch;
        return v;
    }

    [[nodiscard]] inline std::optional<Version> parse_tag(std::string_view tag, std::string_view subdir = {})
    {
        if (!subdir.empty())
        {
            if (tag.starts_with(subdir) && tag.size() > subdir.size() && tag[subdir.size()] == '/')
                tag.remove_prefix(subdir.size() + 1);
            else
                return std::nullopt;
        }
        if (tag.starts_with('v'))
            tag.remove_prefix(1);
        return parse(tag);
    }

    [[nodiscard]] inline std::string to_string(Version const& v)
    {
        std::string out = std::format("{}.{}.{}", v.major, v.minor, v.patch);
        for (std::size_t i = 0; i < v.pre.size(); ++i)
            out += (i == 0 ? "-" : ".") + v.pre[i];
        if (!v.build.empty())
            out += "+" + v.build;
        return out;
    }

    [[nodiscard]] inline std::strong_ordering compare(Version const& a, Version const& b)
    {
        if (auto c = a.major <=> b.major; c != 0)
            return c;
        if (auto c = a.minor <=> b.minor; c != 0)
            return c;
        if (auto c = a.patch <=> b.patch; c != 0)
            return c;
        if (a.pre.empty() != b.pre.empty())
            return a.pre.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
        for (std::size_t i = 0; i < std::min(a.pre.size(), b.pre.size()); ++i)
        {
            bool const an = detail::all_digits(a.pre[i]);
            bool const bn = detail::all_digits(b.pre[i]);
            if (an && bn)
            {
                auto x = *detail::number(a.pre[i]);
                auto y = *detail::number(b.pre[i]);
                if (auto c = x <=> y; c != 0)
                    return c;
            }
            else if (an != bn)
                return an ? std::strong_ordering::less : std::strong_ordering::greater;
            else if (auto c = a.pre[i].compare(b.pre[i]); c != 0)
                return c < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        return a.pre.size() <=> b.pre.size();
    }

    [[nodiscard]] inline Series series_of(Version const& v)
    {
        if (v.major == 0)
            return {0, v.minor};
        return {v.major, std::nullopt};
    }

    [[nodiscard]] inline bool same_series(Version const& a, Version const& b)
    {
        return series_of(a) == series_of(b);
    }

    [[nodiscard]] inline bool satisfies_minimum(Version const& candidate, Version const& minimum)
    {
        if (!same_series(candidate, minimum))
            return false;
        if (candidate.is_prerelease() && !minimum.is_prerelease())
            return false;
        return compare(candidate, minimum) != std::strong_ordering::less;
    }

    [[nodiscard]] inline bool is_valid(std::string_view text)
    {
        return parse(text).has_value();
    }
} // namespace dpm::semver
