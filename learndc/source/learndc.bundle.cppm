export module learndc.bundle;

import std;
import learndc.text;
import learndc.catalog;

extern "C" const unsigned char learndc_bundle_data[];
extern "C" const unsigned long long learndc_bundle_size;

export namespace learndc::bundle
{
    constexpr std::string_view magic = "LDCB1\n";

    [[nodiscard]] inline std::string_view raw()
    {
        return {reinterpret_cast<char const*>(learndc_bundle_data), static_cast<std::size_t>(learndc_bundle_size)};
    }

    [[nodiscard]] inline std::expected<catalog::ContentSource, text::Error> decode(std::string_view archive, std::string label = "embedded")
    {
        if (!archive.starts_with(magic))
            return std::unexpected(text::Error{"content archive has an unknown format (bad magic)"});
        catalog::ContentSource source;
        source.label = std::move(label);
        std::size_t pos = magic.size();
        auto const read_u32 = [&](std::uint32_t& out) -> bool
        {
            if (archive.size() - pos < 4)
                return false;
            out = 0;
            for (std::size_t i = 0; i < 4; ++i)
                out |= static_cast<std::uint32_t>(static_cast<unsigned char>(archive[pos + i])) << (8 * i);
            pos += 4;
            return true;
        };
        std::size_t index = 0;
        while (pos < archive.size())
        {
            std::uint32_t path_length = 0;
            if (!read_u32(path_length))
                return std::unexpected(text::Error{std::format("content archive is truncated (entry {} header)", index)});
            if (path_length == 0 || archive.size() - pos < path_length)
                return std::unexpected(text::Error{std::format("content archive is corrupt (entry {} path length)", index)});
            std::string path(archive.substr(pos, path_length));
            pos += path_length;
            std::uint32_t data_length = 0;
            if (!read_u32(data_length))
                return std::unexpected(text::Error{std::format("content archive is truncated (entry {} length)", index)});
            if (archive.size() - pos < data_length)
                return std::unexpected(text::Error{std::format("content archive is truncated (entry '{}' data)", path)});
            if (auto problem = catalog::path_problem(path))
                return std::unexpected(text::Error{std::format("content archive entry '{}' is invalid: {}", path, *problem)});
            if (source.files.contains(path))
                return std::unexpected(text::Error{std::format("content archive lists '{}' twice", path)});
            source.files.emplace(std::move(path), std::string(archive.substr(pos, data_length)));
            pos += data_length;
            ++index;
        }
        return source;
    }

    [[nodiscard]] inline std::expected<catalog::ContentSource, text::Error> embedded()
    {
        return decode(raw());
    }
}
