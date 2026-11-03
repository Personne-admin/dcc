export module dpm.expand;

import std;

export namespace dpm::expand
{
    enum class Kind : std::uint8_t
    {
        Text,
        Root,
        Profile,
        Dcc,
        Out,
        Target,
        Asset,
        TaskDir,
    };

    struct Piece
    {
        Kind kind{Kind::Text};
        std::string text;
    };

    struct Scanned
    {
        std::vector<Piece> pieces;
        std::string error;

        [[nodiscard]] bool ok() const { return error.empty(); }
    };

    [[nodiscard]] inline std::string_view token_name(Kind kind)
    {
        switch (kind)
        {
        case Kind::Root:
            return "root";
        case Kind::Profile:
            return "profile";
        case Kind::Dcc:
            return "dcc";
        case Kind::Out:
            return "out";
        case Kind::Target:
            return "target";
        case Kind::Asset:
            return "asset";
        case Kind::TaskDir:
            return "task_dir";
        case Kind::Text:
            break;
        }
        return "";
    }

    [[nodiscard]] inline bool is_path_token(Kind kind)
    {
        return kind == Kind::Root || kind == Kind::Dcc || kind == Kind::Out || kind == Kind::Asset || kind == Kind::TaskDir;
    }

    [[nodiscard]] inline Scanned scan(std::string_view text)
    {
        Scanned out;
        auto add_text = [&](std::string_view s)
        {
            if (s.empty())
                return;
            if (out.pieces.empty() || out.pieces.back().kind != Kind::Text)
                out.pieces.push_back({Kind::Text, {}});
            out.pieces.back().text += s;
        };
        std::size_t i = 0;
        while (i < text.size())
        {
            auto dollar = text.find('$', i);
            if (dollar == std::string_view::npos)
            {
                add_text(text.substr(i));
                break;
            }
            add_text(text.substr(i, dollar - i));
            if (dollar + 1 < text.size() && text[dollar + 1] == '$')
            {
                add_text("$");
                i = dollar + 2;
                continue;
            }
            if (dollar + 1 >= text.size() || text[dollar + 1] != '{')
            {
                out.error = "a '$' must start a ${...} token; write '$$' for a literal '$'";
                return out;
            }
            auto close = text.find('}', dollar + 2);
            if (close == std::string_view::npos)
            {
                out.error = "unterminated '${'";
                return out;
            }
            auto body = text.substr(dollar + 2, close - dollar - 2);
            auto colon = body.find(':');
            auto name = body.substr(0, colon);
            auto arg = colon == std::string_view::npos ? std::string_view{} : body.substr(colon + 1);
            bool const has_arg = colon != std::string_view::npos;
            Piece piece;
            if (name == "root" && !has_arg)
                piece.kind = Kind::Root;
            else if (name == "profile" && !has_arg)
                piece.kind = Kind::Profile;
            else if (name == "dcc" && !has_arg)
                piece.kind = Kind::Dcc;
            else if (name == "task_dir")
                piece.kind = Kind::TaskDir;
            else if (name == "out" && has_arg)
                piece.kind = Kind::Out;
            else if (name == "target" && has_arg)
                piece.kind = Kind::Target;
            else if (name == "asset" && has_arg)
                piece.kind = Kind::Asset;
            else
            {
                bool const known = name == "root" || name == "profile" || name == "dcc" || name == "out" || name == "target" || name == "asset";
                if (known)
                    out.error = has_arg ? std::format("'${{{}}}' takes no argument", name) : std::format("'${{{}}}' needs a name: ${{{}:NAME}}", name, name);
                else
                    out.error = std::format("unknown token '${{{}}}'", body);
                return out;
            }
            if (has_arg)
            {
                bool valid = !arg.empty() && std::ranges::all_of(arg, [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
                if (!valid)
                {
                    out.error = std::format("'${{{}}}' needs a name of letters, digits, '_' and '-'", body);
                    return out;
                }
            }
            piece.text = std::string{arg};
            out.pieces.push_back(std::move(piece));
            i = close + 1;
        }
        return out;
    }

    [[nodiscard]] inline std::optional<std::string> path_problem(Scanned const& scanned, bool allow_asset)
    {
        auto const& pieces = scanned.pieces;
        if (pieces.empty())
            return "the path is empty";
        std::size_t first = 0;
        if (pieces[0].kind != Kind::Text)
        {
            auto kind = pieces[0].kind;
            if (kind == Kind::Dcc)
                return "'${dcc}' is not a directory";
            if (kind == Kind::Asset && !allow_asset)
                return "'${asset:...}' is read-only and cannot be written or used as a working directory";
            if (kind == Kind::Profile || kind == Kind::Target)
                first = 0;
            else
                first = 1;
        }
        std::string rest;
        for (std::size_t i = first; i < pieces.size(); ++i)
        {
            if (is_path_token(pieces[i].kind))
                return std::format("'${{{}}}' can only start a path", token_name(pieces[i].kind));
            rest += pieces[i].kind == Kind::Text ? pieces[i].text : std::string{"x"};
        }
        if (first == 1)
        {
            if (!rest.empty() && rest.front() != '/')
                return std::format("'${{{}}}' must be followed by '/' or end the path", token_name(pieces[0].kind));
            if (!rest.empty())
                rest.erase(0, 1);
            if (rest.empty())
                return std::nullopt;
        }
        else if (rest.empty() || rest.front() == '/')
            return "the path must be relative to the project root, or start with ${root}, ${task_dir}, ${out:...} or ${asset:...}";
        if (rest.find('\\') != std::string::npos)
            return "the path must use '/' as separator";
        std::size_t pos = 0;
        while (pos <= rest.size())
        {
            auto slash = rest.find('/', pos);
            auto part = std::string_view{rest}.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            if (part == "..")
                return "the path must not contain '..'";
            if (slash == std::string::npos)
                break;
            pos = slash + 1;
        }
        return std::nullopt;
    }
} // namespace dpm::expand
