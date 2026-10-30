export module dpm.schema;

import std;

export namespace dpm::schema
{
    struct Field
    {
        std::string name;
        std::string type;
        std::string doc;
        std::string default_text;
        bool has_default{};
        std::uint32_t line{};
    };

    struct Variant
    {
        std::string name;
        std::string doc;
        std::uint32_t line{};
    };

    struct Type
    {
        std::string name;
        std::string doc;
        bool is_enum{};
        std::vector<Field> fields;
        std::vector<Variant> variants;
        std::uint32_t line{};

        [[nodiscard]] Field const* field(std::string_view field_name) const
        {
            for (auto const& f : fields)
                if (f.name == field_name)
                    return &f;
            return nullptr;
        }

        [[nodiscard]] Variant const* variant(std::string_view variant_name) const
        {
            for (auto const& v : variants)
                if (v.name == variant_name)
                    return &v;
            return nullptr;
        }
    };

    struct Schema
    {
        std::map<std::string, Type, std::less<>> types;

        [[nodiscard]] Type const* find(std::string_view name) const
        {
            auto it = types.find(name);
            return it == types.end() ? nullptr : &it->second;
        }
    };

    struct TypeRef
    {
        std::string base;
        int list_depth{};
    };

    [[nodiscard]] inline TypeRef parse_type(std::string_view text)
    {
        TypeRef ref;
        auto trim = [](std::string_view s)
        {
            while (!s.empty() && s.front() == ' ')
                s.remove_prefix(1);
            while (!s.empty() && s.back() == ' ')
                s.remove_suffix(1);
            return s;
        };
        text = trim(text);
        while (text.starts_with("[]"))
        {
            text.remove_prefix(2);
            text = trim(text);
            if (text.starts_with("const "))
                text = trim(text.substr(6));
            ++ref.list_depth;
        }
        ref.base = std::string{text};
        if (ref.base == "char" && ref.list_depth > 0)
        {
            --ref.list_depth;
            ref.base = "string";
        }
        return ref;
    }

    namespace detail
    {
        inline std::string_view trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        }

        inline bool is_ident_char(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }
    } // namespace detail

    [[nodiscard]] inline std::optional<Schema> parse(std::string_view text)
    {
        Schema schema;
        std::string doc;
        Type current;
        bool in_type = false;
        bool opened = false;
        std::uint32_t line_no = 0;
        std::size_t pos = 0;
        while (pos <= text.size())
        {
            auto end = text.find('\n', pos);
            auto raw = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
            pos = end == std::string_view::npos ? text.size() + 1 : end + 1;
            auto line = detail::trim(raw);
            auto this_line = line_no++;
            if (line.starts_with("///"))
            {
                auto body = line.substr(3);
                if (body.starts_with(' '))
                    body.remove_prefix(1);
                if (!doc.empty())
                    doc += '\n';
                doc += std::string{body};
                continue;
            }
            if (line.empty())
            {
                if (!in_type)
                    doc.clear();
                continue;
            }
            if (!in_type)
            {
                bool is_enum = line.starts_with("public enum ");
                bool is_struct = line.starts_with("public struct ");
                if (!is_enum && !is_struct)
                {
                    doc.clear();
                    continue;
                }
                auto rest = line.substr(is_enum ? 12 : 14);
                std::size_t n = 0;
                while (n < rest.size() && detail::is_ident_char(rest[n]))
                    ++n;
                if (n == 0)
                    return std::nullopt;
                current = Type{};
                current.name = std::string{rest.substr(0, n)};
                current.is_enum = is_enum;
                current.doc = std::move(doc);
                current.line = this_line;
                doc.clear();
                in_type = true;
                opened = false;
                continue;
            }
            if (!opened)
            {
                if (line != "{")
                    return std::nullopt;
                opened = true;
                continue;
            }
            if (line == "}")
            {
                auto name = current.name;
                schema.types.emplace(std::move(name), std::move(current));
                current = Type{};
                in_type = false;
                doc.clear();
                continue;
            }
            if (current.is_enum)
            {
                std::size_t n = 0;
                while (n < line.size() && detail::is_ident_char(line[n]))
                    ++n;
                if (n == 0)
                    return std::nullopt;
                Variant v;
                v.name = std::string{line.substr(0, n)};
                v.doc = std::move(doc);
                v.line = this_line;
                current.variants.push_back(std::move(v));
                doc.clear();
                continue;
            }
            if (!line.ends_with(';'))
                return std::nullopt;
            auto body = detail::trim(line.substr(0, line.size() - 1));
            auto eq = body.find(" = ");
            Field f;
            std::string_view head = eq == std::string_view::npos ? body : body.substr(0, eq);
            if (eq != std::string_view::npos)
            {
                f.has_default = true;
                f.default_text = std::string{detail::trim(body.substr(eq + 3))};
            }
            head = detail::trim(head);
            auto space = head.rfind(' ');
            if (space == std::string_view::npos)
                return std::nullopt;
            f.name = std::string{head.substr(space + 1)};
            f.type = std::string{detail::trim(head.substr(0, space))};
            f.doc = std::move(doc);
            f.line = this_line;
            current.fields.push_back(std::move(f));
            doc.clear();
        }
        if (in_type)
            return std::nullopt;
        return schema;
    }
} // namespace dpm::schema
