#pragma once

namespace compdb
{
    struct Entry
    {
        std::string directory;
        std::string file;
        std::vector<std::string> arguments;
        std::string output;
    };

    std::string quote(std::string_view value)
    {
        std::string result = "\"";
        for (char byte : value)
        {
            auto c = static_cast<unsigned char>(byte);
            if (c == '"' || c == '\\')
            {
                result += '\\';
                result += static_cast<char>(c);
            }
            else if (c < 32)
                result += std::format("\\u{:04x}", c);
            else
                result += static_cast<char>(c);
        }
        return result + '"';
    }

    std::string serialize(Entry const& entry)
    {
        std::string result = "{\"directory\":" + quote(entry.directory) + ",\"file\":" + quote(entry.file) + ",\"arguments\":[";
        bool first = true;
        for (auto const& arg : entry.arguments)
        {
            if (!first)
                result += ',';

            first = false;
            result += quote(arg);
        }
        return result + "],\"output\":" + quote(entry.output) + "}";
    }

    class Parser
    {
        std::string_view text;
        std::size_t pos = 0;

        bool take(char c)
        {
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\r' || text[pos] == '\t'))
                ++pos;

            if (pos == text.size() || text[pos] != c)
                return false;

            ++pos;
            return true;
        }

        std::optional<unsigned> hex4()
        {
            unsigned value = 0;
            for (int i = 0; i < 4; ++i)
            {
                if (pos == text.size())
                    return {};

                char c = text[pos++];
                unsigned digit;
                if (c >= '0' && c <= '9')
                    digit = static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    digit = static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    digit = static_cast<unsigned>(c - 'A' + 10);
                else
                    return {};
                value = value * 16 + digit;
            }

            return value;
        }

        std::optional<std::string> string()
        {
            if (!take('"'))
                return {};

            std::string result;
            while (pos < text.size())
            {
                auto c = static_cast<unsigned char>(text[pos++]);
                if (c == '"')
                    return result;
                if (c < 32)
                    return {};
                if (c != '\\')
                {
                    result += static_cast<char>(c);
                    if (c >= 0x80)
                    {
                        unsigned count = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
                        if (count == 0 || pos + count > text.size())
                            return {};
                        auto next = static_cast<unsigned char>(text[pos]);
                        if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) || (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90))
                            return {};
                        while (count--)
                        {
                            auto continuation = static_cast<unsigned char>(text[pos++]);
                            if (continuation < 0x80 || continuation > 0xbf)
                                return {};
                            result += static_cast<char>(continuation);
                        }
                    }
                    continue;
                }
                if (pos == text.size())
                    return {};
                switch (text[pos++])
                {
                    case '"':
                        result += '"';
                        break;
                    case '\\':
                        result += '\\';
                        break;
                    case '/':
                        result += '/';
                        break;
                    case 'b':
                        result += '\b';
                        break;
                    case 'f':
                        result += '\f';
                        break;
                    case 'n':
                        result += '\n';
                        break;
                    case 'r':
                        result += '\r';
                        break;
                    case 't':
                        result += '\t';
                        break;
                    case 'u': {
                        auto cp = hex4();
                        if (!cp)
                            return {};
                        if (*cp >= 0xd800 && *cp <= 0xdbff)
                        {
                            if (pos + 2 > text.size() || text.substr(pos, 2) != "\\u")
                                return {};
                            pos += 2;
                            auto low = hex4();
                            if (!low || *low < 0xdc00 || *low > 0xdfff)
                                return {};
                            *cp = 0x10000 + ((*cp - 0xd800) << 10) + *low - 0xdc00;
                        }
                        else if (*cp >= 0xdc00 && *cp <= 0xdfff)
                            return {};
                        if (*cp < 0x80)
                            result += static_cast<char>(*cp);
                        else
                        {
                            unsigned count = *cp < 0x800 ? 2 : *cp < 0x10000 ? 3 : 4;
                            result += static_cast<char>((0xffu << (8 - count)) | (*cp >> (6 * (count - 1))));
                            for (unsigned i = count - 1; i > 0; --i)
                                result += static_cast<char>(0x80 | ((*cp >> (6 * (i - 1))) & 0x3f));
                        }
                        break;
                    }
                    default:
                        return {};
                }
            }
            return {};
        }

    public:
        explicit Parser(std::string_view input) : text(input) {}

        std::optional<Entry> parse()
        {
            Entry entry;
            std::set<std::string> fields;
            if (!take('{'))
                return {};

            do
            {
                auto key = string();
                if (!key || !fields.insert(*key).second || !take(':'))
                    return {};

                if (*key == "arguments")
                {
                    if (!take('['))
                        return {};

                    if (!take(']'))
                    {
                        do
                        {
                            auto arg = string();
                            if (!arg)
                                return {};

                            entry.arguments.push_back(std::move(*arg));
                        } while (take(','));
                        if (!take(']'))
                            return {};
                    }
                }
                else
                {
                    auto value = string();
                    if (!value || value->empty() || value->find('\0') != std::string::npos)
                        return {};

                    if (*key == "directory")
                        entry.directory = std::move(*value);
                    else if (*key == "file")
                        entry.file = std::move(*value);
                    else if (*key == "output")
                        entry.output = std::move(*value);
                    else
                        return {};
                }
            } while (take(','));

            if (!take('}') || take('\0') || pos != text.size() || fields.size() != 4 || entry.arguments.empty())
                return {};

            return entry;
        }
    };

    std::string absolute(std::filesystem::path path, std::filesystem::path const& base = std::filesystem::current_path())
    {
        if (path.is_relative())
            path = base / path;

        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(path, ec);
        return (ec ? path.lexically_normal() : canonical).string();
    }

} // namespace compdb
