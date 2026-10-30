export module dpm.manifest;

import std;
import dcc.sm;
import dcc.si;
import dcc.diag;
import dcc.ast;
import dcc.lex;
import dcc.utf8;
import dcc.sema;
import dcc.session;
import dcc.types;
import dcc.comptime;
import dcc.ctfe;
import dpm.semver;

export namespace dpm::manifest
{
    inline constexpr std::size_t k_max_file_size = 1U << 20;
    inline constexpr std::string_view k_virtual_path = "/dpm-virtual/manifest.dcon";
    inline constexpr std::string_view k_split_path = "/dpm-virtual/split.dcon";

    enum class FileKind : std::uint8_t
    {
        Project,
        Lock,
        RegistryAlias,
        RegistryLocation,
        RegistryConfig,
        UserConfig,
    };

    [[nodiscard]] constexpr std::string_view type_name(FileKind kind) noexcept
    {
        switch (kind)
        {
            case FileKind::Project:
                return "Project";
            case FileKind::Lock:
                return "Lock";
            case FileKind::RegistryAlias:
                return "RegistryAlias";
            case FileKind::RegistryLocation:
                return "RegistryLocation";
            case FileKind::RegistryConfig:
                return "RegistryConfig";
            case FileKind::UserConfig:
                return "UserConfig";
        }
        return "Project";
    }

    struct Pos
    {
        std::uint32_t line{1};
        std::uint32_t column{1};

        [[nodiscard]] bool operator==(Pos const&) const noexcept = default;
    };

    struct Range
    {
        Pos begin;
        Pos end;
        std::uint32_t begin_offset{};
        std::uint32_t end_offset{};

        [[nodiscard]] bool operator==(Range const& other) const noexcept { return begin == other.begin && end == other.end; }
    };

    struct Diagnostic
    {
        std::string file;
        Range range;
        std::string message;

        [[nodiscard]] std::string format() const { return std::format("{}:{}:{}: {}", file, range.begin.line, range.begin.column, message); }
    };

    struct Node
    {
        enum class Kind : std::uint8_t
        {
            Struct,
            List,
            String,
            Int,
            Bool,
            Enum,
        };

        Kind kind{Kind::Struct};
        Range range;
        bool from_default{false};
        std::string text;
        std::int64_t integer{};
        bool boolean{};
        std::vector<std::pair<std::string, Node>> fields;
        std::vector<Node> items;

        [[nodiscard]] Node const* find(std::string_view name) const
        {
            for (auto const& [field, node] : fields)
                if (field == name)
                    return &node;
            return nullptr;
        }

        [[nodiscard]] std::string dump(bool include_defaults = false) const
        {
            std::string out;
            dump_into(out, 0, include_defaults);
            out += "\n";
            return out;
        }

    private:
        static std::string quote(std::string_view s)
        {
            std::string r = "\"";
            for (char c : s)
            {
                if (c == '"' || c == '\\')
                    r += '\\';
                r += c;
            }
            r += '"';
            return r;
        }

        void dump_into(std::string& out, int depth, bool include_defaults) const
        {
            auto pad = [](int d) { return std::string(static_cast<std::size_t>(d) * 2, ' '); };
            switch (kind)
            {
                case Kind::String:
                    out += quote(text);
                    return;
                case Kind::Int:
                    out += std::to_string(integer);
                    return;
                case Kind::Bool:
                    out += boolean ? "true" : "false";
                    return;
                case Kind::Enum:
                    out += text;
                    return;
                case Kind::List: {
                    out += "[";
                    for (auto const& item : items)
                    {
                        out += "\n" + pad(depth + 1);
                        item.dump_into(out, depth + 1, include_defaults);
                        out += ",";
                    }
                    if (!items.empty())
                        out += "\n" + pad(depth);
                    out += "]";
                    return;
                }
                case Kind::Struct: {
                    out += "{";
                    for (auto const& [name, node] : fields)
                    {
                        if (node.from_default && !include_defaults)
                            continue;
                        out += "\n" + pad(depth + 1) + name + " = ";
                        node.dump_into(out, depth + 1, include_defaults);
                        out += ",";
                    }
                    out += "\n" + pad(depth) + "}";
                    return;
                }
            }
        }
    };

    enum class Tri : std::uint8_t
    {
        Default,
        On,
        Off,
    };
    enum class Target : std::uint8_t
    {
        Host,
        X86_64Elf,
        X86_64Coff,
        X86Elf,
        X86Coff,
        I8086Binary,
        I8086Mz,
    };
    enum class Backend : std::uint8_t
    {
        Default,
        Llvm,
        Custom,
    };
    enum class Model : std::uint8_t
    {
        Default,
        Small,
        Kernel,
        Medium,
        Large,
        Unreal,
        Unreal32,
    };
    enum class Optimize : std::uint8_t
    {
        Default,
        O0,
        O1,
        O2,
        Size,
    };
    enum class Libdcext : std::uint8_t
    {
        Default,
        Off,
        Linux,
        Windows,
        Freestanding,
    };
    enum class Kind : std::uint8_t
    {
        Executable,
        SharedLibrary,
    };

    struct Settings
    {
        Target target{Target::Host};
        Backend backend{Backend::Default};
        std::string arch;
        Model model{Model::Default};
        Optimize optimize{Optimize::Default};
        Tri whole_program{Tri::Default};
        Libdcext libdcext{Libdcext::Default};
        Tri debug_info{Tri::Default};
        Tri bounds_check{Tri::Default};
        Tri restricted_check{Tri::Default};
        Tri far_check{Tri::Default};
        Tri pic{Tri::Default};
        std::string base;
        std::string stack_reserve;
        std::vector<std::string> extra_flags;
    };

    struct Dependency
    {
        std::string location;
        std::string version;
        Range range;
    };

    struct Product
    {
        std::string name;
        Kind kind{Kind::Executable};
        std::string entry;
        Settings settings;
        std::vector<std::string> link;
        Range range;
    };

    enum class ProfileBase : std::uint8_t
    {
        Debug,
        Release,
    };

    struct Profile
    {
        std::string name;
        ProfileBase inherits{ProfileBase::Debug};
        Settings settings;
        Range range;
    };

    struct Project
    {
        std::string name;
        std::string version;
        std::string description;
        std::string license;
        std::string alias;
        std::string requires_dcc;
        std::string source;
        std::vector<Dependency> dependencies;
        std::vector<Dependency> dev_dependencies;
        std::vector<Product> products;
        std::string test_dir;
        Settings settings;
        std::vector<Profile> profiles;
        std::string registry;
    };

    struct LockedPackage
    {
        std::string location;
        std::string version;
        std::string source;
        std::string commit;
        std::string hash;
        std::vector<std::string> dependencies;
        bool dev{false};
        Range range;
    };

    struct Lock
    {
        std::string manifest;
        std::string dcc;
        std::vector<LockedPackage> packages;
    };

    struct RegistryVersion
    {
        std::string version;
        std::string source;
        std::string commit;
        std::string hash;
        std::vector<Dependency> dependencies;
        std::string requires_dcc;
        bool yanked{false};
        Range range;
    };

    struct RegistryAlias
    {
        std::string alias;
        std::string location;
        std::string description;
        std::vector<RegistryVersion> versions;
    };

    struct RegistryLocation
    {
        std::string alias;
    };

    struct RegistryConfig
    {
        std::int64_t format{1};
        std::string name;
        std::string description;
    };

    struct UserConfig
    {
        std::vector<std::string> registries;
        std::string cache_dir;
        bool offline{false};
    };

    struct LoadOptions
    {
        std::string file_name{"project.dcon"};
        std::filesystem::path schema_root;
        std::filesystem::path project_root;
        FileKind kind{FileKind::Project};
        bool allow_file_urls{false};
    };

    struct LoadResult
    {
        bool ok{false};
        std::vector<Diagnostic> diagnostics;
        Node tree;
        std::optional<Project> project;
        std::optional<Lock> lock;
        std::optional<RegistryAlias> registry_alias;
        std::optional<RegistryLocation> registry_location;
        std::optional<RegistryConfig> registry_config;
        std::optional<UserConfig> user_config;
    };

    namespace detail
    {
        using TokenKind = dcc::lex::TokenKind;

        struct Tok
        {
            TokenKind kind;
            std::uint32_t begin;
            std::uint32_t end;
        };

        struct Source
        {
            dcc::session::CompilerSession session{{.silent_diagnostics = true}};
            dcc::sm::SourceFile const* file{};
            std::string text;
        };

        inline Pos pos_at(dcc::sm::SourceFile const& file, std::uint32_t offset)
        {
            auto lc = file.line_col(std::min<std::uint32_t>(offset, static_cast<std::uint32_t>(file.size())));
            if (!lc)
                return {};
            return {lc->line, lc->column};
        }

        inline Range range_at(dcc::sm::SourceFile const& file, std::uint32_t begin, std::uint32_t end)
        {
            return {pos_at(file, begin), pos_at(file, std::max(begin, end)), begin, std::max(begin, end)};
        }

        inline std::vector<Tok> lex_tokens(dcc::session::CompilerSession& session, dcc::sm::SourceFile const& file)
        {
            std::vector<Tok> tokens;
            dcc::lex::Lexer lexer{file, session.interner(), false};
            for (;;)
            {
                auto tok = lexer.next();
                if (tok.kind == TokenKind::Eof)
                    break;
                tokens.push_back({tok.kind, tok.range.begin.offset, tok.range.end.offset});
                if (tok.kind == TokenKind::Invalid)
                    break;
            }
            return tokens;
        }

        inline bool is_open(TokenKind k)
        {
            return k == TokenKind::LBrace || k == TokenKind::LParen || k == TokenKind::LBracket;
        }

        inline bool is_close(TokenKind k)
        {
            return k == TokenKind::RBrace || k == TokenKind::RParen || k == TokenKind::RBracket;
        }

        struct Split
        {
            std::uint32_t init_begin{};
            std::string type_path;
            bool typed{false};
            std::uint32_t path_begin{};
            std::uint32_t path_end{};
            int schema_version{1};
        };

        struct Reporter
        {
            std::string file_name;
            std::vector<Diagnostic>* out{};

            void add(Range range, std::string message) const { out->push_back({file_name, range, std::move(message)}); }
        };

        inline bool parse_version_segment(std::string_view seg, int& version)
        {
            if (seg.size() < 2 || seg[0] != 'v')
                return false;
            int value = 0;
            for (char c : seg.substr(1))
            {
                if (c < '0' || c > '9' || value > 100000)
                    return false;
                value = value * 10 + (c - '0');
            }
            version = value;
            return value > 0;
        }

        inline bool split_manifest(Source& src, FileKind kind, Reporter const& report, Split& split)
        {
            auto const& file = *src.file;
            auto tokens = lex_tokens(src.session, file);
            auto size = static_cast<std::uint32_t>(src.text.size());
            if (!tokens.empty() && tokens.back().kind == TokenKind::Invalid)
            {
                report.add(range_at(file, tokens.back().begin, tokens.back().end), "invalid token");
                return false;
            }
            int depth = 0;
            std::size_t stmt_start = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                auto k = tokens[i].kind;
                if (is_open(k))
                    ++depth;
                else if (is_close(k))
                    --depth;
                else if (k == TokenKind::Semicolon && depth == 0)
                {
                    if (tokens[stmt_start].kind != TokenKind::KwConst)
                    {
                        report.add(range_at(file, tokens[stmt_start].begin, tokens[stmt_start].end),
                                   "only 'const' declarations are allowed before the initializer");
                        return false;
                    }
                    stmt_start = i + 1;
                }
            }
            if (stmt_start >= tokens.size())
            {
                report.add(range_at(file, size, size), "expected an initializer");
                return false;
            }
            std::size_t i = stmt_start;
            split.init_begin = tokens[i].begin;
            std::size_t lbrace = i;
            std::vector<std::string_view> segments;
            if (tokens[i].kind == TokenKind::Identifier)
            {
                for (;;)
                {
                    if (i >= tokens.size() || tokens[i].kind != TokenKind::Identifier)
                    {
                        report.add(range_at(file, tokens[stmt_start].begin, tokens[std::min(i, tokens.size() - 1)].end), "expected a struct literal");
                        return false;
                    }
                    segments.push_back(std::string_view{src.text}.substr(tokens[i].begin, tokens[i].end - tokens[i].begin));
                    ++i;
                    if (i < tokens.size() && tokens[i].kind == TokenKind::ColonColon)
                    {
                        ++i;
                        continue;
                    }
                    break;
                }
                split.typed = true;
                split.path_begin = tokens[stmt_start].begin;
                split.path_end = tokens[i - 1].end;
                split.type_path = src.text.substr(split.path_begin, split.path_end - split.path_begin);
                lbrace = i;
            }
            if (lbrace >= tokens.size() || tokens[lbrace].kind != TokenKind::LBrace)
            {
                report.add(range_at(file, tokens[stmt_start].begin, tokens[std::min(lbrace, tokens.size() - 1)].end), "expected a struct literal");
                return false;
            }
            if (split.typed)
            {
                auto path_range = range_at(file, split.path_begin, split.path_end);
                int version = 0;
                if (segments.size() != 3 || segments[0] != "dpm" || !parse_version_segment(segments[1], version) || segments[2] != type_name(kind))
                {
                    report.add(path_range, std::format("expected 'dpm::vN::{}'", type_name(kind)));
                    return false;
                }
                split.schema_version = version;
            }
            depth = 0;
            std::size_t j = lbrace;
            for (; j < tokens.size(); ++j)
            {
                if (is_open(tokens[j].kind))
                    ++depth;
                else if (is_close(tokens[j].kind) && --depth == 0)
                    break;
            }
            if (j >= tokens.size())
            {
                report.add(range_at(file, tokens[lbrace].begin, tokens[lbrace].end), "unclosed '{'");
                return false;
            }
            if (j + 1 < tokens.size())
            {
                report.add(range_at(file, tokens[j + 1].begin, tokens.back().end), "unexpected tokens after the initializer");
                return false;
            }
            return true;
        }

        inline std::vector<std::string> schema_public_types(std::string_view schema_text)
        {
            dcc::session::CompilerSession session{{.silent_diagnostics = true}};
            auto id = session.open_in_memory("file:///dpm-virtual/schema.dc", std::string{schema_text});
            auto const* file = session.source_manager().get(id);
            std::vector<std::string> names;
            if (!file)
                return names;
            auto tokens = lex_tokens(session, *file);
            for (std::size_t i = 0; i + 2 < tokens.size(); ++i)
            {
                if (tokens[i].kind != TokenKind::KwPublic)
                    continue;
                if (tokens[i + 1].kind != TokenKind::KwEnum && tokens[i + 1].kind != TokenKind::KwStruct)
                    continue;
                if (tokens[i + 2].kind != TokenKind::Identifier)
                    continue;
                names.emplace_back(schema_text.substr(tokens[i + 2].begin, tokens[i + 2].end - tokens[i + 2].begin));
            }
            return names;
        }

        inline std::optional<std::string> read_file(std::filesystem::path const& path)
        {
            std::ifstream in{path, std::ios::binary};
            if (!in)
                return std::nullopt;
            return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        }

        struct OffsetMap
        {
            std::uint32_t header{};
            std::uint32_t init_begin{};
            std::uint32_t insert{};
            std::uint32_t user_size{};

            [[nodiscard]] std::uint32_t to_user(std::uint32_t offset) const
            {
                if (offset < header)
                    return 0;
                auto o = offset - header;
                if (o < init_begin)
                    return o;
                if (o < init_begin + insert)
                    return init_begin;
                return std::min(o - insert, user_size);
            }
        };

        inline char const* describe(dcc::ast::ExprKind kind)
        {
            using K = dcc::ast::ExprKind;
            switch (kind)
            {
                case K::Call:
                    return "function calls";
                case K::Lambda:
                    return "lambdas";
                case K::Index:
                    return "indexing";
                case K::Cast:
                    return "casts";
                case K::Sizeof:
                case K::Alignof:
                case K::Offsetof:
                    return "sizeof, alignof and offsetof";
                case K::Block:
                case K::If:
                case K::Match:
                    return "blocks, if and match expressions";
                case K::Postfix:
                    return "postfix operators";
                case K::Asm:
                    return "inline assembly";
                case K::NullLiteral:
                    return "null";
                default:
                    return "this expression";
            }
        }

        struct Purity
        {
            std::function<Range(dcc::sm::SourceRange)> map;
            Reporter const* report{};
            bool ok{true};

            void fail(dcc::ast::Expr const& expr)
            {
                ok = false;
                report->add(map(expr.range), std::format("{} are not allowed in a manifest", describe(expr.kind)));
            }

            void check(dcc::ast::Expr const* expr)
            {
                using K = dcc::ast::ExprKind;
                if (!expr)
                    return;
                switch (expr->kind)
                {
                    case K::IntLiteral:
                    case K::FloatLiteral:
                    case K::StringLiteral:
                    case K::CharLiteral:
                    case K::BoolLiteral:
                        return;
                    case K::Ident:
                    case K::PathExpr: {
                        auto const* decl = expr->sema.resolved_decl;
                        if (decl && decl->kind == dcc::ast::DeclKind::Func)
                            fail(*expr);
                        return;
                    }
                    case K::Unary: {
                        auto const& u = static_cast<dcc::ast::UnaryExpr const&>(*expr);
                        if (u.op != TokenKind::Minus && u.op != TokenKind::Plus && u.op != TokenKind::Bang && u.op != TokenKind::Tilde)
                        {
                            ok = false;
                            report->add(map(expr->range), "this operator is not allowed in a manifest");
                            return;
                        }
                        check(u.operand);
                        return;
                    }
                    case K::Binary: {
                        auto const& b = static_cast<dcc::ast::BinaryExpr const&>(*expr);
                        switch (b.op)
                        {
                            case TokenKind::Plus:
                            case TokenKind::Minus:
                            case TokenKind::Star:
                            case TokenKind::Slash:
                            case TokenKind::Percent:
                            case TokenKind::Amp:
                            case TokenKind::Pipe:
                            case TokenKind::Caret:
                            case TokenKind::AmpAmp:
                            case TokenKind::PipePipe:
                            case TokenKind::EqEq:
                            case TokenKind::BangEq:
                            case TokenKind::Lt:
                            case TokenKind::Gt:
                            case TokenKind::LtEq:
                            case TokenKind::GtEq:
                            case TokenKind::LtLt:
                            case TokenKind::GtGt:
                                break;
                            default:
                                ok = false;
                                report->add(map(expr->range), "this operator is not allowed in a manifest");
                                return;
                        }
                        check(b.lhs);
                        check(b.rhs);
                        return;
                    }
                    case K::FieldAccess:
                        check(static_cast<dcc::ast::FieldAccessExpr const&>(*expr).object);
                        return;
                    case K::StructLiteral: {
                        auto const& lit = static_cast<dcc::ast::StructLiteralExpr const&>(*expr);
                        for (auto const& field : lit.fields)
                        {
                            if (field.default_owner || (!field.name_range.valid() && !field.name.empty()))
                                continue;
                            check(field.value);
                        }
                        return;
                    }
                    case K::Block: {
                        auto const& block = static_cast<dcc::ast::BlockExpr const&>(*expr);
                        if (!block.body.stmts.empty())
                        {
                            fail(*expr);
                            return;
                        }
                        check(block.body.tail);
                        return;
                    }
                    default:
                        fail(*expr);
                        return;
                }
            }
        };

        struct TreeBuilder
        {
            std::function<std::optional<Range>(dcc::sm::SourceRange)> map;

            static dcc::types::Type const* canonical_of(dcc::ast::TypeExpr const* type)
            {
                return type ? reinterpret_cast<dcc::types::Type const*>(type->sema.canonical) : nullptr;
            }

            Node build(dcc::comptime::Value const& value, dcc::types::Type const* declared, dcc::ast::Expr const* expr, Range parent, bool defaulted) const
            {
                Node node;
                node.from_default = defaulted;
                node.range = parent;
                if (!defaulted && expr)
                    if (auto mapped = map(expr->range))
                        node.range = *mapped;
                auto const* type = declared ? declared : value.type;
                using VK = dcc::comptime::Value::Kind;
                switch (value.kind())
                {
                    case VK::Int: {
                        if (auto const* et = dcc::types::type_cast<dcc::types::EnumType>(type))
                        {
                            auto const* decl = reinterpret_cast<dcc::ast::EnumDecl const*>(et->decl);
                            for (auto const& variant : decl->variants)
                                if (variant.discriminant == value.get_int())
                                {
                                    node.kind = Node::Kind::Enum;
                                    node.text = std::string{decl->name} + "::" + std::string{variant.name};
                                    return node;
                                }
                        }
                        node.kind = Node::Kind::Int;
                        node.integer = value.get_int();
                        return node;
                    }
                    case VK::Bool:
                        node.kind = Node::Kind::Bool;
                        node.boolean = value.get_bool();
                        return node;
                    case VK::String:
                        node.kind = Node::Kind::String;
                        node.text = value.get_string();
                        return node;
                    default:
                        break;
                }
                auto const* literal = expr && expr->kind == dcc::ast::ExprKind::StructLiteral ? static_cast<dcc::ast::StructLiteralExpr const*>(expr) : nullptr;
                if (auto const* st = dcc::types::type_cast<dcc::types::StructType>(type); st && value.kind() == VK::Aggregate)
                {
                    node.kind = Node::Kind::Struct;
                    auto const* decl = reinterpret_cast<dcc::ast::StructDecl const*>(st->decl);
                    std::size_t slot = 0;
                    for (auto const& field : decl->fields)
                    {
                        if (field.name.empty())
                            continue;
                        if (slot >= value.size())
                            break;
                        dcc::ast::StructLiteralField const* lf = nullptr;
                        if (literal)
                            for (auto const& candidate : literal->fields)
                                if (candidate.name == field.name)
                                    lf = &candidate;
                        bool const child_default = defaulted || (literal && (!lf || lf->default_owner || !lf->name_range.valid()));
                        node.fields.emplace_back(std::string{field.name},
                                                 build(value.at(slot), canonical_of(field.type), lf ? lf->value : nullptr, node.range, child_default));
                        ++slot;
                    }
                    return node;
                }
                node.kind = Node::Kind::List;
                dcc::types::Type const* element = nullptr;
                if (auto const* slice = dcc::types::type_cast<dcc::types::SliceType>(type))
                    element = slice->element;
                dcc::ast::Expr const* single = nullptr;
                if (expr && expr->kind == dcc::ast::ExprKind::Block)
                {
                    auto const& block = static_cast<dcc::ast::BlockExpr const&>(*expr);
                    if (block.body.stmts.empty())
                        single = block.body.tail;
                }
                bool const aligned = literal && literal->fields.size() == value.size();
                for (std::size_t i = 0; i < value.size(); ++i)
                {
                    dcc::ast::Expr const* child = nullptr;
                    if (aligned)
                        child = literal->fields[i].value;
                    else if (single && value.size() == 1)
                        child = single;
                    node.items.push_back(build(value.at(i), element, child, node.range, defaulted));
                }
                return node;
            }
        };

        inline bool is_identifier(std::string_view text)
        {
            if (text.empty())
                return false;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                char c = text[i];
                bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
                bool digit = c >= '0' && c <= '9';
                if (!(alpha || (i > 0 && digit)))
                    return false;
            }
            return true;
        }

        inline bool is_reserved_word(std::string_view text)
        {
            dcc::session::CompilerSession session{{.silent_diagnostics = true}};
            auto id = session.open_in_memory("file:///dpm-virtual/word.dc", std::string{text});
            auto const* file = session.source_manager().get(id);
            if (!file)
                return true;
            auto tokens = lex_tokens(session, *file);
            return tokens.size() != 1 || tokens[0].kind != TokenKind::Identifier;
        }

        inline std::vector<std::string_view> split_path(std::string_view s, char sep)
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

        inline std::optional<std::string> location_error(std::string_view loc, bool allow_file = false)
        {
            if (loc.empty())
                return "location is empty";
            if (loc.starts_with("path:"))
                return loc.size() == 5 ? std::optional<std::string>{"path location has no directory"} : std::nullopt;
            if (allow_file && loc.starts_with("git+file://"))
                return loc.size() == 11 ? std::optional<std::string>{"git location needs a path"} : std::nullopt;
            if (allow_file && loc.starts_with("file://"))
            {
                if (loc.ends_with(".tar.gz") || loc.ends_with(".tgz"))
                    return std::nullopt;
                if (loc.ends_with(".tar.xz") || loc.ends_with(".txz"))
                    return "only .tar.gz and .tgz tarballs are supported";
                return "tarball location must end in .tar.gz or .tgz";
            }
            if (loc.starts_with("git+https://"))
                return loc.size() == 12 || loc.find('/', 12) == std::string_view::npos ? std::optional<std::string>{"git location needs a host and a path"}
                                                                                       : std::nullopt;
            if (loc.starts_with("https://"))
            {
                if (loc.ends_with(".tar.gz") || loc.ends_with(".tgz"))
                    return std::nullopt;
                if (loc.ends_with(".tar.xz") || loc.ends_with(".txz"))
                    return "only .tar.gz and .tgz tarballs are supported";
                return "tarball location must end in .tar.gz or .tgz";
            }
            if (loc.find(':') != std::string_view::npos)
                return "unrecognized location scheme";
            if (loc.find('/') == std::string_view::npos && loc.find('.') == std::string_view::npos)
            {
                if (!std::ranges::all_of(loc, [](char c) {
                        return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '-';
                    }))
                    return "registry alias may contain only lowercase letters, digits, '_' and '-'";
                return std::nullopt;
            }
            auto parts = split_path(loc, '/');
            static constexpr std::array<std::string_view, 4> hosts{"github.com", "gitlab.com", "codeberg.org", "bitbucket.org"};
            bool const known = std::ranges::find(hosts, parts[0]) != hosts.end() || parts[0] == "sr.ht";
            if (!known)
                return "unknown git host; use git+https://host/path.git for other servers";
            if (parts.size() < 3)
                return "location needs host/owner/repo";
            for (auto part : parts)
                if (part.empty() || part == "." || part == "..")
                    return "location has an empty or relative path segment";
            if (parts[0] == "sr.ht" && !parts[1].starts_with('~'))
                return "sr.ht owners start with '~'";
            return std::nullopt;
        }

        inline std::optional<std::string> alias_error(std::string_view name)
        {
            bool shape = name.size() >= 2 && name.size() <= 32 && name[0] >= 'a' && name[0] <= 'z' &&
                         std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
            if (!shape)
                return std::format("'{}' is not a valid alias; use 2 to 32 characters from [a-z][a-z0-9_]", name);
            static constexpr std::array<std::string_view, 5> reserved{"std", "core", "assert", "dcext", "dpm"};
            if (std::ranges::find(reserved, name) != reserved.end() || is_reserved_word(name))
                return std::format("'{}' is a reserved name", name);
            return std::nullopt;
        }

        inline bool is_relative_subpath(std::string_view p)
        {
            if (p.empty() || p.front() == '/' || p.find('\\') != std::string_view::npos || (p.size() > 1 && p[1] == ':'))
                return false;
            for (auto part : split_path(p, '/'))
                if (part == "..")
                    return false;
            return true;
        }

        struct Decoder
        {
            Reporter const* report{};
            std::filesystem::path project_root;
            bool allow_file{false};
            bool ok{true};

            void error(Node const& at, std::string message)
            {
                ok = false;
                report->add(at.range, std::move(message));
            }

            void internal(Node const& at, std::string_view field)
            {
                error(at, std::format("internal error: schema field '{}' does not match this dpm", field));
            }

            Node const* field(Node const& node, std::string_view name)
            {
                auto const* f = node.find(name);
                if (!f)
                    internal(node, name);
                return f;
            }

            std::string str(Node const& node, std::string_view name)
            {
                auto const* f = field(node, name);
                if (!f)
                    return {};
                if (f->kind != Node::Kind::String)
                {
                    internal(*f, name);
                    return {};
                }
                return f->text;
            }

            std::vector<std::string> strs(Node const& node, std::string_view name)
            {
                std::vector<std::string> out;
                auto const* f = field(node, name);
                if (!f)
                    return out;
                for (auto const& item : f->items)
                {
                    if (item.kind != Node::Kind::String)
                    {
                        internal(item, name);
                        continue;
                    }
                    out.push_back(item.text);
                }
                return out;
            }

            template <class E, std::size_t N> E enumerator(Node const& node, std::string_view name, std::array<std::string_view, N> const& names)
            {
                auto const* f = field(node, name);
                if (!f || f->kind != Node::Kind::Enum)
                {
                    if (f)
                        internal(*f, name);
                    return E{};
                }
                auto sep = f->text.rfind("::");
                auto variant = std::string_view{f->text}.substr(sep == std::string::npos ? 0 : sep + 2);
                for (std::size_t i = 0; i < N; ++i)
                    if (names[i] == variant)
                        return static_cast<E>(i);
                internal(*f, name);
                return E{};
            }

            Tri tri(Node const& n, std::string_view name)
            {
                static constexpr std::array<std::string_view, 3> names{"Default", "On", "Off"};
                return enumerator<Tri>(n, name, names);
            }

            Settings settings(Node const& n)
            {
                Settings s;
                static constexpr std::array<std::string_view, 7> targets{"Host", "X86_64Elf", "X86_64Coff", "X86Elf", "X86Coff", "I8086Binary", "I8086Mz"};
                static constexpr std::array<std::string_view, 3> backends{"Default", "Llvm", "Custom"};
                static constexpr std::array<std::string_view, 7> models{"Default", "Small", "Kernel", "Medium", "Large", "Unreal", "Unreal32"};
                static constexpr std::array<std::string_view, 5> optimizes{"Default", "O0", "O1", "O2", "Size"};
                static constexpr std::array<std::string_view, 5> libs{"Default", "Off", "Linux", "Windows", "Freestanding"};
                s.target = enumerator<Target>(n, "target", targets);
                s.backend = enumerator<Backend>(n, "backend", backends);
                s.arch = str(n, "arch");
                s.model = enumerator<Model>(n, "model", models);
                s.optimize = enumerator<Optimize>(n, "optimize", optimizes);
                s.whole_program = tri(n, "whole_program");
                s.libdcext = enumerator<Libdcext>(n, "libdcext", libs);
                s.debug_info = tri(n, "debug_info");
                s.bounds_check = tri(n, "bounds_check");
                s.restricted_check = tri(n, "restricted_check");
                s.far_check = tri(n, "far_check");
                s.pic = tri(n, "pic");
                s.base = str(n, "base");
                s.stack_reserve = str(n, "stack_reserve");
                s.extra_flags = strs(n, "extra_flags");
                if (auto const* flags = n.find("extra_flags"))
                    check_flags(*flags, s);
                return s;
            }

            void check_flags(Node const& flags, Settings const& s)
            {
                for (auto const& item : flags.items)
                {
                    auto const& flag = item.text;
                    bool owned = flag == "-o" || flag == "-c" || flag == "-S" || flag == "--depfile" || flag.starts_with("-I") || flag.starts_with("-l") ||
                                 flag.starts_with("-o") || flag.starts_with("--depfile=") || flag == "--compdb-entry" || flag.starts_with("--compdb-entry=");
                    if (s.target != Target::Host && (flag == "-target" || flag.starts_with("-target=") || flag == "--target"))
                        owned = true;
                    if (owned)
                        error(item, std::format("'{}' is set by dpm and cannot be passed in extra_flags", flag));
                }
            }

            void check_entry_path(Node const& at, std::string const& value, std::string_view what)
            {
                if (!is_relative_subpath(value))
                    error(at, std::format("{} must be a relative path inside the package", what));
            }

            std::vector<Dependency> dependencies(Node const& node, std::string_view name)
            {
                std::vector<Dependency> out;
                auto const* list = field(node, name);
                if (!list)
                    return out;
                for (auto const& item : list->items)
                {
                    Dependency d;
                    d.range = item.range;
                    d.location = str(item, "location");
                    d.version = str(item, "version");
                    auto const* loc = item.find("location");
                    auto const* ver = item.find("version");
                    if (auto message = location_error(d.location, allow_file))
                        error(loc ? *loc : item, *message);
                    else if (d.location.starts_with("path:"))
                    {
                        if (!d.version.empty())
                            error(ver ? *ver : item, "path dependencies take no version");
                    }
                    else if (d.version.empty())
                        error(item, "dependency needs a minimum version");
                    if (!d.version.empty() && !dpm::semver::is_valid(d.version))
                        error(ver ? *ver : item, std::format("'{}' is not a valid version", d.version));
                    out.push_back(std::move(d));
                }
                return out;
            }

            static bool is_hex40(std::string_view s)
            {
                return s.size() == 40 && std::ranges::all_of(s, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
            }

            Lock lock(Node const& root)
            {
                Lock l;
                l.manifest = str(root, "manifest");
                l.dcc = str(root, "dcc");
                if (auto const* n = root.find("manifest"); n && !is_sha256(l.manifest))
                    error(*n, "manifest fingerprint must be 'sha256:' followed by 64 lowercase hex digits");
                if (auto const* n = root.find("dcc"); n && !l.dcc.empty() && !dpm::semver::is_valid(l.dcc))
                    error(*n, std::format("'{}' is not a valid version", l.dcc));
                auto const* list = field(root, "packages");
                if (!list)
                    return l;
                std::set<std::string> seen;
                for (auto const& item : list->items)
                    if (auto const* loc = item.find("location"); loc && loc->kind == Node::Kind::String)
                        seen.insert(loc->text);
                std::string previous;
                bool have_previous = false;
                for (auto const& item : list->items)
                {
                    LockedPackage pk;
                    pk.range = item.range;
                    pk.location = str(item, "location");
                    pk.version = str(item, "version");
                    pk.source = str(item, "source");
                    pk.commit = str(item, "commit");
                    pk.hash = str(item, "hash");
                    pk.dependencies = strs(item, "dependencies");
                    static constexpr std::array<std::string_view, 3> names{"Default", "On", "Off"};
                    auto dev = enumerator<Tri>(item, "dev", names);
                    pk.dev = dev == Tri::On;
                    auto const* loc = item.find("location");
                    auto const* ver = item.find("version");
                    auto const* src = item.find("source");
                    auto const* com = item.find("commit");
                    auto const* hsh = item.find("hash");
                    if (loc)
                    {
                        if (auto message = location_error(pk.location, allow_file))
                            error(*loc, *message);
                        else if (is_alias(pk.location))
                            error(*loc, "lock locations are canonical; a registry alias is not allowed");
                        else if (have_previous && pk.location <= previous)
                            error(*loc, pk.location == previous ? std::format("duplicate package '{}'", pk.location)
                                                                : std::format("packages must be sorted by location; '{}' comes after '{}'", pk.location, previous));
                        previous = pk.location;
                        have_previous = true;
                    }
                    if (ver && (!dpm::semver::is_valid(pk.version)))
                        error(*ver, std::format("'{}' is not a valid version", pk.version));
                    bool const is_path = pk.location.starts_with("path:");
                    if (src)
                    {
                        bool const known = pk.source.starts_with("git+https://") || pk.source.starts_with("https://") || pk.source.starts_with("path:") ||
                                           pk.source.starts_with("registry:") || (allow_file && (pk.source.starts_with("git+file://") || pk.source.starts_with("file://")));
                        if (!known)
                            error(*src, "source must start with git+https://, https://, path: or registry:");
                        else if (pk.source.ends_with(".tar.xz") || pk.source.ends_with(".txz"))
                            error(*src, "only .tar.gz and .tgz tarballs are supported");
                        else if (is_path && pk.source != pk.location)
                            error(*src, "a path package's source must equal its location");
                        else if (!is_path && pk.source.starts_with("path:"))
                            error(*src, "only path packages use a path: source");
                    }
                    if (com)
                    {
                        if (!pk.commit.empty() && !is_hex40(pk.commit))
                            error(*com, "commit must be 40 lowercase hex digits");
                        else if (pk.commit.empty() && (pk.source.starts_with("git+https://") || pk.source.starts_with("git+file://")))
                            error(*com, "a git source needs a full commit");
                        else if (!pk.commit.empty() && (pk.source.starts_with("https://") || pk.source.starts_with("file://") || is_path))
                            error(*com, "only git sources have a commit");
                    }
                    if (hsh)
                    {
                        if (is_path)
                        {
                            if (!pk.hash.empty())
                                error(*hsh, "a path package has no hash");
                        }
                        else if (!is_sha256(pk.hash))
                            error(*hsh, "hash must be 'sha256:' followed by 64 lowercase hex digits");
                    }
                    if (dev == Tri::Off)
                        if (auto const* n = item.find("dev"))
                            error(*n, "dev must be Tri::On or omitted");
                    if (auto const* deps = item.find("dependencies"))
                    {
                        std::string last;
                        bool first = true;
                        for (auto const& d : deps->items)
                        {
                            if (!first && d.text <= last)
                                error(d, d.text == last ? std::format("duplicate dependency '{}'", d.text) : "dependencies must be sorted");
                            else if (!seen.contains(d.text))
                                error(d, std::format("dependency '{}' has no lock entry", d.text));
                            last = d.text;
                            first = false;
                        }
                    }
                    l.packages.push_back(std::move(pk));
                }
                return l;
            }

            std::optional<std::string> alias_name_error(std::string const& name) { return alias_error(name); }

            RegistryVersion registry_version(Node const& item)
            {
                RegistryVersion v;
                v.range = item.range;
                v.version = str(item, "version");
                v.source = str(item, "source");
                v.commit = str(item, "commit");
                v.hash = str(item, "hash");
                v.requires_dcc = str(item, "requires_dcc");
                v.dependencies = dependencies(item, "dependencies");
                if (auto const* y = field(item, "yanked"); y && y->kind == Node::Kind::Bool)
                    v.yanked = y->boolean;
                auto const* ver = item.find("version");
                auto const* src = item.find("source");
                auto const* com = item.find("commit");
                auto const* hsh = item.find("hash");
                if (ver && !dpm::semver::is_valid(v.version))
                    error(*ver, std::format("'{}' is not a valid version", v.version));
                if (auto const* n = item.find("requires_dcc"); n && !v.requires_dcc.empty() && !dpm::semver::is_valid(v.requires_dcc))
                    error(*n, std::format("'{}' is not a valid version", v.requires_dcc));
                bool git = false;
                bool tarball = false;
                if (src)
                {
                    git = v.source.starts_with("git+https://") || (allow_file && v.source.starts_with("git+file://"));
                    tarball = v.source.starts_with("https://") || (allow_file && v.source.starts_with("file://"));
                    if (!git && !tarball)
                        error(*src, "source must start with git+https:// or https://");
                    else if (auto message = location_error(v.source, allow_file))
                        error(*src, *message);
                }
                if (com)
                {
                    if (git && !is_hex40(v.commit))
                        error(*com, "a git source needs a full commit of 40 lowercase hex digits");
                    else if (tarball && !v.commit.empty())
                        error(*com, "only git sources have a commit");
                }
                if (hsh && !is_sha256(v.hash))
                    error(*hsh, "hash must be 'sha256:' followed by 64 lowercase hex digits");
                if (auto const* deps = item.find("dependencies"))
                    for (std::size_t i = 0; i < v.dependencies.size() && i < deps->items.size(); ++i)
                        if (v.dependencies[i].location.starts_with("path:"))
                            error(deps->items[i], "a registry version cannot depend on a path: package");
                return v;
            }

            RegistryAlias registry_alias(Node const& root)
            {
                RegistryAlias a;
                a.alias = str(root, "alias");
                a.location = str(root, "location");
                a.description = str(root, "description");
                if (auto const* n = root.find("alias"))
                    if (auto message = alias_name_error(a.alias))
                        error(*n, *message);
                if (auto const* n = root.find("location"))
                {
                    if (auto message = location_error(a.location, allow_file))
                        error(*n, *message);
                    else if (is_alias(a.location) || a.location.starts_with("path:"))
                        error(*n, "the location of an alias must be a canonical location");
                }
                if (auto const* list = field(root, "versions"))
                {
                    std::optional<dpm::semver::Version> previous;
                    for (auto const& item : list->items)
                    {
                        auto v = registry_version(item);
                        auto parsed = dpm::semver::parse(v.version);
                        if (parsed && previous && dpm::semver::compare(*parsed, *previous) <= 0)
                            if (auto const* n = item.find("version"))
                                error(*n, "versions must be unique and ascending");
                        if (parsed)
                            previous = parsed;
                        a.versions.push_back(std::move(v));
                    }
                }
                return a;
            }

            RegistryLocation registry_location(Node const& root)
            {
                RegistryLocation l;
                l.alias = str(root, "alias");
                if (auto const* n = root.find("alias"))
                    if (auto message = alias_name_error(l.alias))
                        error(*n, *message);
                return l;
            }

            RegistryConfig registry_config(Node const& root)
            {
                RegistryConfig c;
                c.name = str(root, "name");
                c.description = str(root, "description");
                if (auto const* f = field(root, "format"); f && f->kind == Node::Kind::Int)
                {
                    c.format = f->integer;
                    if (c.format != 1)
                        error(*f, std::format("unsupported registry format {}", c.format));
                }
                if (auto const* n = root.find("name"); n && c.name.empty())
                    error(*n, "registry name is empty");
                return c;
            }

            UserConfig user_config(Node const& root)
            {
                UserConfig c;
                c.registries = strs(root, "registries");
                c.cache_dir = str(root, "cache_dir");
                if (auto const* f = field(root, "offline"); f && f->kind == Node::Kind::Bool)
                    c.offline = f->boolean;
                if (auto const* list = root.find("registries"))
                    for (std::size_t i = 0; i < c.registries.size() && i < list->items.size(); ++i)
                    {
                        auto const& url = c.registries[i];
                        if (!url.starts_with("https://") && !(allow_file && url.starts_with("file://")))
                            error(list->items[i], allow_file ? "a registry must be an https:// or file:// URL" : "a registry must be an https:// URL");
                    }
                return c;
            }

            static bool is_sha256(std::string_view s)
            {
                return s.size() == 71 && s.starts_with("sha256:") &&
                       std::ranges::all_of(s.substr(7), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
            }

            static bool is_alias(std::string_view loc)
            {
                return loc.find('/') == std::string_view::npos && loc.find('.') == std::string_view::npos && loc.find(':') == std::string_view::npos;
            }

            Project project(Node const& root)
            {
                Project p;
                p.name = str(root, "name");
                p.version = str(root, "version");
                p.description = str(root, "description");
                p.license = str(root, "license");
                p.alias = str(root, "alias");
                p.requires_dcc = str(root, "requires_dcc");
                p.source = str(root, "source");
                p.test_dir = str(root, "test_dir");
                p.registry = str(root, "registry");
                if (auto const* n = root.find("name"); n && (!is_identifier(p.name) || is_reserved_word(p.name)))
                    error(*n, std::format("'{}' is not a valid package name; use a single identifier", p.name));
                if (auto const* n = root.find("version"); n && !dpm::semver::is_valid(p.version))
                    error(*n, std::format("'{}' is not a valid version", p.version));
                if (auto const* n = root.find("requires_dcc"); n && !p.requires_dcc.empty() && !dpm::semver::is_valid(p.requires_dcc))
                    error(*n, std::format("'{}' is not a valid version", p.requires_dcc));
                if (auto const* n = root.find("alias"); n && !p.alias.empty())
                    if (auto message = alias_error(p.alias))
                        error(*n, *message);
                if (auto const* n = root.find("source"))
                    check_entry_path(*n, p.source, "source");
                if (auto const* n = root.find("test_dir"))
                    check_entry_path(*n, p.test_dir, "test_dir");
                if (auto const* n = root.find("registry");
                    n && !p.registry.empty() && !p.registry.starts_with("https://") && !(allow_file && p.registry.starts_with("file://")))
                    error(*n, allow_file ? "registry must be an https:// or file:// URL" : "registry must be an https:// URL");
                p.dependencies = dependencies(root, "dependencies");
                p.dev_dependencies = dependencies(root, "dev_dependencies");
                if (auto const* s = field(root, "settings"))
                    p.settings = settings(*s);
                static constexpr std::array<std::string_view, 2> kinds{"Executable", "SharedLibrary"};
                std::set<std::string> product_names;
                if (auto const* list = field(root, "products"))
                    for (auto const& item : list->items)
                    {
                        Product pr;
                        pr.range = item.range;
                        pr.name = str(item, "name");
                        pr.kind = enumerator<Kind>(item, "kind", kinds);
                        pr.entry = str(item, "entry");
                        pr.link = strs(item, "link");
                        if (auto const* s = field(item, "settings"))
                            pr.settings = settings(*s);
                        if (auto const* n = item.find("name"))
                        {
                            if (!is_identifier(pr.name))
                                error(*n, std::format("'{}' is not a valid product name", pr.name));
                            else if (!product_names.insert(pr.name).second)
                                error(*n, std::format("duplicate product '{}'", pr.name));
                        }
                        if (auto const* n = item.find("entry"))
                        {
                            check_entry_path(*n, pr.entry, "entry");
                            if (!project_root.empty() && is_relative_subpath(pr.entry))
                            {
                                std::error_code ec;
                                if (!std::filesystem::is_regular_file(project_root / pr.entry, ec))
                                    error(*n, std::format("entry file '{}' does not exist", pr.entry));
                            }
                        }
                        p.products.push_back(std::move(pr));
                    }
                std::set<std::string> profile_names;
                if (auto const* list = field(root, "profiles"))
                    for (auto const& item : list->items)
                    {
                        Profile pf;
                        pf.range = item.range;
                        pf.name = str(item, "name");
                        static constexpr std::array<std::string_view, 2> bases{"Debug", "Release"};
                        pf.inherits = enumerator<ProfileBase>(item, "inherits", bases);
                        if (auto const* s = field(item, "settings"))
                            pf.settings = settings(*s);
                        if (auto const* n = item.find("name"))
                        {
                            if (!is_identifier(pf.name))
                                error(*n, std::format("'{}' is not a valid profile name", pf.name));
                            else if (pf.name == "debug" || pf.name == "release")
                                error(*n, std::format("profile '{}' is built in and cannot be declared; use another name and set inherits", pf.name));
                            else if (!profile_names.insert(pf.name).second)
                                error(*n, std::format("duplicate profile '{}'", pf.name));
                        }
                        p.profiles.push_back(std::move(pf));
                    }
                return p;
            }
        };
    } // namespace detail

    [[nodiscard]] inline FileKind kind_from_file_name(std::string_view file_name)
    {
        auto slash = file_name.find_last_of("/\\");
        auto leaf = slash == std::string_view::npos ? file_name : file_name.substr(slash + 1);
        if (leaf == "project.lock.dcon")
            return FileKind::Lock;
        if (leaf == "registry.dcon")
            return FileKind::RegistryConfig;
        if (leaf == "config.dcon")
            return FileKind::UserConfig;
        return FileKind::Project;
    }

    [[nodiscard]] inline FileKind detect_kind(std::string_view text, std::string_view file_name)
    {
        auto fallback = kind_from_file_name(file_name);
        if (text.size() > k_max_file_size || text.find('\0') != std::string_view::npos)
            return fallback;
        detail::Source src;
        src.text = std::string{text};
        auto id = src.session.open_in_memory(std::string{"file://"} + std::string{k_split_path}, src.text);
        src.file = src.session.source_manager().get(id);
        if (!src.file)
            return fallback;
        auto tokens = detail::lex_tokens(src.session, *src.file);
        int depth = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            if (detail::is_open(tokens[i].kind))
                ++depth;
            else if (detail::is_close(tokens[i].kind))
                --depth;
            else if (tokens[i].kind == detail::TokenKind::Semicolon && depth == 0)
                start = i + 1;
        }
        std::vector<std::string_view> segments;
        std::size_t i = start;
        while (i < tokens.size() && tokens[i].kind == detail::TokenKind::Identifier)
        {
            segments.push_back(std::string_view{src.text}.substr(tokens[i].begin, tokens[i].end - tokens[i].begin));
            ++i;
            if (i < tokens.size() && tokens[i].kind == detail::TokenKind::ColonColon)
                ++i;
            else
                break;
        }
        if (segments.size() != 3 || segments[0] != "dpm" || i >= tokens.size() || tokens[i].kind != detail::TokenKind::LBrace)
            return fallback;
        for (auto kind : {FileKind::Project, FileKind::Lock, FileKind::RegistryAlias, FileKind::RegistryLocation, FileKind::RegistryConfig, FileKind::UserConfig})
            if (segments[2] == type_name(kind))
                return kind;
        return fallback;
    }

    inline LoadResult load(std::string_view text, LoadOptions const& options)
    {
        LoadResult result;
        detail::Reporter report{options.file_name, &result.diagnostics};
        auto fail_at_start = [&](std::string message) {
            report.add({}, std::move(message));
            return std::move(result);
        };

        if (text.size() > k_max_file_size)
            return fail_at_start("file is larger than 1 MiB");
        if (text.find('\0') != std::string_view::npos)
            return fail_at_start("file contains a NUL byte");
        for (auto cp : dcc::utf8::codepoints(text))
            if (!cp)
                return fail_at_start("file is not valid UTF-8");

        detail::Source src;
        src.text = std::string{text};
        auto split_id = src.session.open_in_memory(std::string{"file://"} + std::string{k_split_path}, src.text);
        src.file = src.session.source_manager().get(split_id);
        if (!src.file)
            return fail_at_start("internal error: cannot open the manifest buffer");

        detail::Split split;
        if (!detail::split_manifest(src, options.kind, report, split))
            return result;

        auto schema_name = std::format("v{}", split.schema_version);
        auto schema_path = options.schema_root / "dpm" / (schema_name + ".dc");
        auto schema_text = detail::read_file(schema_path);
        if (!schema_text)
        {
            auto where = split.typed ? detail::range_at(*src.file, split.path_begin, split.path_end) : Range{};
            report.add(where, std::format("schema 'dpm::{}' is not available", schema_name));
            return result;
        }

        std::string header = std::format("module dpm_manifest; public import dpm::{}; using dpm::{}::{{", schema_name, schema_name);
        auto names = detail::schema_public_types(*schema_text);
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            if (i != 0)
                header += ", ";
            header += names[i];
        }
        header += "}; ";
        std::string declared = split.typed ? split.type_path : std::format("dpm::{}::{}", schema_name, type_name(options.kind));
        std::string insert = std::format("public const {} __manifest = ", declared);

        std::string wrapper;
        wrapper.append(header);
        wrapper.append(src.text, 0, split.init_begin);
        wrapper.append(insert);
        wrapper.append(src.text, split.init_begin, std::string::npos);
        wrapper.append("\n;\n");

        detail::OffsetMap offsets{static_cast<std::uint32_t>(header.size()), split.init_begin, static_cast<std::uint32_t>(insert.size()),
                                  static_cast<std::uint32_t>(src.text.size())};

        dcc::session::CompilerSession session{{.silent_diagnostics = true, .enable_doc_comments = true}};
        auto wrapper_id = session.open_in_memory(std::string{"file://"} + std::string{k_virtual_path}, std::move(wrapper));

        auto map_range = [&](dcc::sm::SourceRange r) -> std::optional<Range> {
            if (!r.valid() || r.begin.fileId != wrapper_id)
                return std::nullopt;
            return detail::range_at(*src.file, offsets.to_user(r.begin.offset), offsets.to_user(r.end.offset));
        };
        auto map_or_start = [&](dcc::sm::SourceRange r) { return map_range(r).value_or(Range{}); };

        dcc::session::CompileOptions compile_options;
        compile_options.import_roots.push_back(options.schema_root);
        auto compiled = session.analyze_entry(std::filesystem::path{k_virtual_path}, compile_options);

        auto const& sm = session.source_manager();
        bool had_error = false;
        for (auto const& diagnostic : session.diagnostics().diagnostics())
        {
            if (diagnostic.severity() != dcc::diag::Severity::Error)
                continue;
            had_error = true;
            Range range;
            std::string file = options.file_name;
            if (!diagnostic.labels().empty())
            {
                auto const& label = diagnostic.labels().front().range;
                if (auto mapped = map_range(label))
                    range = *mapped;
                else if (label.valid())
                {
                    if (auto const* other = sm.get(label.begin.fileId))
                    {
                        file = other->path().string();
                        range = detail::range_at(*other, label.begin.offset, label.end.offset);
                    }
                }
            }
            result.diagnostics.push_back({file, range, diagnostic.message()});
        }
        if (had_error || compiled.has_errors || !compiled.module || !compiled.module->tu)
        {
            if (result.diagnostics.empty())
                report.add({}, "manifest analysis failed");
            return result;
        }

        dcc::ast::VarDecl const* var = nullptr;
        for (auto* decl : compiled.module->tu->decls)
            if (auto* v = dcc::ast::node_cast<dcc::ast::VarDecl>(decl); v && v->name == "__manifest")
                var = v;
        if (!var || !var->init)
        {
            report.add({}, "manifest declaration not found");
            return result;
        }

        detail::Purity purity;
        purity.map = map_or_start;
        purity.report = &report;
        for (auto* decl : compiled.module->tu->decls)
            if (auto* v = dcc::ast::node_cast<dcc::ast::VarDecl>(decl))
                purity.check(v->init);
        if (!purity.ok)
            return result;

        dcc::ctfe::Context context;
        context.call_site = var->init->range;
        context.types = &session.sema_context()->types();
        context.source_manager = &session.source_manager();
        context.step_limit = 10000;
        context.memory_limit = 1 << 20;
        auto evaluated = dcc::ctfe::Evaluator(std::move(context), dcc::ctfe::Mode::Required).evaluate(*var->init);
        if (evaluated.failed() || !evaluated.value)
        {
            report.add(map_or_start(var->init->range), "evaluation failed: " + evaluated.message);
            return result;
        }

        detail::TreeBuilder builder{map_range};
        auto const* declared_type = detail::TreeBuilder::canonical_of(var->type);
        result.tree = builder.build(*evaluated.value, declared_type, var->init, map_or_start(var->init->range), false);

        if (options.kind == FileKind::Project)
        {
            detail::Decoder decoder;
            decoder.report = &report;
            decoder.project_root = options.project_root;
            decoder.allow_file = options.allow_file_urls;
            auto project = decoder.project(result.tree);
            if (!decoder.ok)
                return result;
            result.project = std::move(project);
        }
        else if (options.kind == FileKind::Lock)
        {
            detail::Decoder decoder;
            decoder.report = &report;
            decoder.project_root = options.project_root;
            decoder.allow_file = options.allow_file_urls;
            auto lock = decoder.lock(result.tree);
            if (!decoder.ok)
                return result;
            result.lock = std::move(lock);
        }
        else
        {
            detail::Decoder decoder;
            decoder.report = &report;
            decoder.project_root = options.project_root;
            decoder.allow_file = options.allow_file_urls;
            switch (options.kind)
            {
                case FileKind::RegistryAlias:
                    result.registry_alias = decoder.registry_alias(result.tree);
                    break;
                case FileKind::RegistryLocation:
                    result.registry_location = decoder.registry_location(result.tree);
                    break;
                case FileKind::RegistryConfig:
                    result.registry_config = decoder.registry_config(result.tree);
                    break;
                case FileKind::UserConfig:
                    result.user_config = decoder.user_config(result.tree);
                    break;
                default:
                    break;
            }
            if (!decoder.ok)
                return result;
        }
        result.ok = true;
        return result;
    }
} // namespace dpm::manifest
