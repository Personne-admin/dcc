export module dcc.backend.object.inspect;

import std;

export namespace dcc::backend::object
{
    [[nodiscard]] bool object_matches_target(std::filesystem::path const& path, bool coff)
    {
        std::ifstream input(path, std::ios::binary);
        std::array<unsigned char, 20> header{};
        input.read(reinterpret_cast<char*>(header.data()), header.size());
        if (!input)
            return false;
        if (coff)
            return header[0] == 0x64 && header[1] == 0x86;
        return header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' && header[3] == 'F' && header[4] == 2 && header[5] == 1 && header[16] == 1 &&
               header[17] == 0 && header[18] == 62 && header[19] == 0;
    }

    [[nodiscard]] bool object_defines(std::filesystem::path const& path, std::string_view symbol)
    {
        std::ifstream input(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(input)), {});
        auto read = [&](std::size_t offset, std::size_t count) -> std::uint64_t {
            if (offset > bytes.size() || count > bytes.size() - offset)
                throw std::runtime_error(std::format("truncated symbol table in '{}'", path.string()));

            std::uint64_t value{};
            for (std::size_t i = 0; i < count; ++i)
                value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);

            return value;
        };

        auto name = [&](std::size_t offset) {
            if (offset >= bytes.size())
                throw std::runtime_error(std::format("invalid symbol name in '{}'", path.string()));

            auto end = bytes.find('\0', offset);
            return std::string_view(bytes).substr(offset, end == std::string::npos ? bytes.size() - offset : end - offset);
        };

        if (bytes.starts_with("\177ELF"))
        {
            auto table = read(40, 8);
            auto stride = read(58, 2);
            auto count = read(60, 2);
            for (std::size_t i = 0; i < count; ++i)
            {
                auto section = table + (i * stride);
                if (read(section + 4, 4) != 2)
                    continue;
                auto start = read(section + 24, 8);
                auto size = read(section + 32, 8);
                auto step = read(section + 56, 8);
                auto strings = read(table + (read(section + 40, 4) * stride) + 24, 8);
                if (step < 24 || size > bytes.size())
                    throw std::runtime_error(std::format("invalid symbol table in '{}'", path.string()));

                for (std::size_t j = 0; j < size / step; ++j)
                {
                    auto entry = start + (j * step);
                    if (read(entry + 6, 2) != 0 && (read(entry + 4, 1) >> 4) != 0 && name(strings + read(entry, 4)) == symbol)
                        return true;
                }
            }
        }
        else if (read(0, 2) == 0x8664)
        {
            auto table = read(8, 4);
            auto count = read(12, 4);
            for (std::size_t i = 0; i < count;)
            {
                auto entry = table + (i * 18);
                auto section = read(entry + 12, 2);
                auto storage = read(entry + 16, 1);
                auto symbol_name = read(entry, 4) == 0 ? name(table + (count * 18) + read(entry + 4, 4)) : name(entry).substr(0, 8);
                if (section > 0 && section < 0x8000 && storage == 2 && symbol_name == symbol)
                    return true;

                i += 1 + read(entry + 17, 1);
            }
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> object_sections(std::filesystem::path const& path, std::string_view wanted)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error(std::format("cannot read object '{}'", path.string()));

        std::string bytes((std::istreambuf_iterator<char>(input)), {});
        auto read = [&](std::size_t offset, std::size_t count) -> std::uint64_t {
            if (offset > bytes.size() || count > bytes.size() - offset)
                throw std::runtime_error(std::format("truncated object '{}'", path.string()));

            std::uint64_t value{};
            for (std::size_t i = 0; i < count; ++i)
                value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8);

            return value;
        };

        auto view = [&](std::size_t offset, std::size_t size) {
            if (offset > bytes.size() || size > bytes.size() - offset)
                throw std::runtime_error(std::format("invalid object section in '{}'", path.string()));

            return std::string_view(bytes).substr(offset, size);
        };

        auto name = [&](std::size_t offset, std::size_t size) {
            auto text = view(offset, size);
            return text.substr(0, text.find('\0'));
        };

        std::vector<std::string> result;
        if (bytes.starts_with("\177ELF"))
        {
            if (read(4, 1) != 2 || read(5, 1) != 1 || read(16, 2) != 1)
                throw std::runtime_error(std::format("expected a little-endian ELF64 object: '{}'", path.string()));

            auto table = read(40, 8);
            auto stride = read(58, 2);
            auto count = read(60, 2);
            auto strings = table + (read(62, 2) * stride);
            auto string_offset = read(strings + 24, 8);
            auto string_size = read(strings + 32, 8);
            if (stride < 64 || count > bytes.size() / stride)
                throw std::runtime_error(std::format("invalid ELF section table in '{}'", path.string()));

            for (std::size_t i = 0; i < count; ++i)
            {
                auto section = table + (i * stride);
                auto index = read(section, 4);
                if (index >= string_size)
                    throw std::runtime_error(std::format("invalid ELF section name in '{}'", path.string()));

                if (name(string_offset + index, string_size - index) == wanted)
                    result.emplace_back(view(read(section + 24, 8), read(section + 32, 8)));
            }
        }
        else if (read(0, 2) == 0x8664)
        {
            auto count = read(2, 2);
            auto table = 20 + read(16, 2);
            for (std::size_t i = 0; i < count; ++i)
            {
                auto section = table + (i * 40);
                if (name(section, 8) == wanted)
                    result.emplace_back(view(read(section + 20, 4), read(section + 16, 4)));
            }
        }
        else
        {
            throw std::runtime_error(std::format("unsupported object format in '{}'", path.string()));
        }

        return result;
    }

}
