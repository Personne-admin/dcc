export module dpm.analyze;

import std;
import dcc.lex;
import dcc.session;
import dpm.cache;
import dpm.lock;
import dpm.manifest;
import dpm.schema;
import dpm.semver;

export namespace dpm::analyze
{
    inline constexpr std::size_t k_max_text = manifest::k_max_file_size;

    enum class Severity : std::uint8_t
    {
        Error,
        Warning,
        Information,
        Hint,
    };

    struct Finding
    {
        Severity severity{Severity::Error};
        std::uint32_t begin{};
        std::uint32_t end{};
        std::string message;
    };

    struct Options
    {
        std::filesystem::path schema_root;
        std::filesystem::path file;
        std::string dcc_version;
        std::filesystem::path cache_root;
        bool allow_file_urls{};
    };

    struct Result
    {
        manifest::FileKind kind{manifest::FileKind::Project};
        bool loaded{};
        std::vector<Finding> findings;
    };

    namespace detail
    {
        using TokenKind = dcc::lex::TokenKind;
        using manifest::detail::Tok;

        inline std::string file_label(Options const& o)
        {
            auto name = o.file.filename().string();
            return name.empty() ? std::string{"project.dcon"} : name;
        }

        inline std::optional<std::string> read_small(std::filesystem::path const& path)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
                return std::nullopt;
            auto size = std::filesystem::file_size(path, ec);
            if (ec || size > k_max_text)
                return std::nullopt;
            std::ifstream in{path, std::ios::binary};
            if (!in)
                return std::nullopt;
            return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        }

        inline manifest::LoadOptions load_options(Options const& o, manifest::FileKind kind, std::string name, std::filesystem::path const& project_root)
        {
            manifest::LoadOptions options;
            options.file_name = std::move(name);
            options.schema_root = o.schema_root;
            options.project_root = project_root;
            options.kind = kind;
            options.allow_file_urls = o.allow_file_urls;
            return options;
        }

        inline std::filesystem::path directory_of(Options const& o)
        {
            auto dir = o.file.parent_path();
            std::error_code ec;
            if (!dir.empty() && std::filesystem::is_directory(dir, ec))
                return dir;
            return {};
        }

        inline bool is_alias(std::string_view loc)
        {
            return loc.find('/') == std::string_view::npos && loc.find('.') == std::string_view::npos && loc.find(':') == std::string_view::npos;
        }

        inline std::string normalize_path_location(std::string_view location)
        {
            if (!location.starts_with("path:"))
                return std::string{location};
            auto text = (std::filesystem::path{std::string{location.substr(5)}}).lexically_normal().generic_string();
            while (text.size() > 1 && text.ends_with('/'))
                text.pop_back();
            return "path:" + (text.empty() ? std::string{"."} : text);
        }

        inline void add_finding(std::vector<Finding>& out, Severity severity, manifest::Range const& range, std::string message)
        {
            out.push_back({severity, range.begin_offset, range.end_offset, std::move(message)});
        }

        inline manifest::Range const* field_range(manifest::Node const& root, std::string_view name, manifest::Range& storage)
        {
            if (auto const* n = root.find(name))
            {
                storage = n->range;
                return &storage;
            }
            return nullptr;
        }

        inline std::filesystem::path cache_root_of(Options const& o)
        {
            return o.cache_root.empty() ? cache::default_root() : o.cache_root;
        }

        inline void template_findings(manifest::LoadResult const& loaded, Options const& o, std::vector<Finding>& out)
        {
            auto const& tmpl = *loaded.project_template;
            if (o.dcc_version.empty())
                return;
            auto have = semver::parse(o.dcc_version);
            if (!have)
                return;
            manifest::Range scratch;
            for (auto const& [field, value, tool] : {std::tuple{"requires_dpm", &tmpl.requires_dpm, "dpm"}, std::tuple{"requires_dcc", &tmpl.requires_dcc, "dcc"}})
            {
                if (value->empty())
                    continue;
                auto need = semver::parse(*value);
                if (need && semver::compare(*have, *need) < 0)
                    if (auto const* r = field_range(loaded.tree, field, scratch))
                        add_finding(out, Severity::Warning, *r, std::format("requires {} {}, but this is {} {}", tool, *value, tool, o.dcc_version));
            }
        }

        inline void project_findings(manifest::LoadResult const& loaded, Options const& o, std::vector<Finding>& out)
        {
            auto const& project = *loaded.project;
            manifest::Range scratch;
            if (!project.requires_dcc.empty() && !o.dcc_version.empty())
            {
                auto need = semver::parse(project.requires_dcc);
                auto have = semver::parse(o.dcc_version);
                if (need && have && semver::compare(*have, *need) < 0)
                    if (auto const* r = field_range(loaded.tree, "requires_dcc", scratch))
                        add_finding(out, Severity::Warning, *r, std::format("requires dcc {}, but this is dcc {}", project.requires_dcc, o.dcc_version));
            }
            auto dir = directory_of(o);
            if (dir.empty())
                return;
            auto lock_text = read_small(dir / lock::k_lock_file);
            if (!lock_text)
                return;
            auto lock_options = load_options(o, manifest::FileKind::Lock, std::string{lock::k_lock_file}, dir);
            auto lock_loaded = manifest::load(*lock_text, lock_options);
            manifest::Range anchor;
            bool has_anchor = false;
            for (auto name : {"dependencies", "dev_dependencies"})
                if (auto const* r = field_range(loaded.tree, name, scratch))
                {
                    anchor = *r;
                    has_anchor = true;
                    break;
                }
            if (!lock_loaded.ok || !lock_loaded.lock)
            {
                if (has_anchor)
                    add_finding(out, Severity::Warning, anchor, "project.lock.dcon has errors; run 'dpm lock'");
                return;
            }
            auto const& lock = *lock_loaded.lock;
            if (lock.manifest != dpm::lock::fingerprint(project) && has_anchor)
                add_finding(out, Severity::Warning, anchor, "the manifest fingerprint differs from project.lock.dcon: run 'dpm lock'");
            auto cache = cache_root_of(o);
            for (auto const* list : {&project.dependencies, &project.dev_dependencies})
                for (auto const& d : *list)
                {
                    if (is_alias(d.location))
                        continue;
                    auto wanted = normalize_path_location(d.location);
                    manifest::LockedPackage const* found = nullptr;
                    for (auto const& p : lock.packages)
                        if (p.location == wanted)
                            found = &p;
                    if (!found)
                    {
                        add_finding(out, Severity::Warning, d.range, std::format("dependency {} has no lock entry: run 'dpm lock'", d.location));
                        continue;
                    }
                    auto locked = semver::parse(found->version);
                    auto minimum = semver::parse(d.version);
                    if (locked && minimum && semver::compare(*locked, *minimum) < 0)
                        add_finding(out, Severity::Warning, d.range,
                                    std::format("locked version {} is lower than the required minimum {}: run 'dpm lock'", found->version, d.version));
                    if (!found->hash.empty() && !found->location.starts_with("path:") && found->hash.size() > 7)
                    {
                        std::error_code ec;
                        if (!std::filesystem::exists(cache / "src" / found->hash.substr(7), ec))
                            add_finding(out, Severity::Information, d.range, std::format("{}@{} is not in the dpm cache: run 'dpm fetch'", d.location, found->version));
                    }
                }
        }

        inline void lock_findings(manifest::LoadResult const& loaded, Options const& o, std::vector<Finding>& out)
        {
            auto dir = directory_of(o);
            if (dir.empty())
                return;
            auto project_text = read_small(dir / "project.dcon");
            if (!project_text)
                return;
            auto project_loaded = manifest::load(*project_text, load_options(o, manifest::FileKind::Project, "project.dcon", dir));
            if (!project_loaded.ok || !project_loaded.project)
                return;
            auto const& project = *project_loaded.project;
            auto const& lock = *loaded.lock;
            manifest::Range scratch;
            if (lock.manifest != dpm::lock::fingerprint(project))
                if (auto const* r = field_range(loaded.tree, "manifest", scratch))
                    add_finding(out, Severity::Warning, *r, "the lock was made for different dependencies: run 'dpm lock'");
            std::set<std::string> wanted;
            bool has_alias = false;
            for (auto const* list : {&project.dependencies, &project.dev_dependencies})
                for (auto const& d : *list)
                {
                    if (is_alias(d.location))
                        has_alias = true;
                    else
                        wanted.insert(normalize_path_location(d.location));
                }
            if (has_alias)
                return;
            std::set<std::string> depended_on;
            for (auto const& p : lock.packages)
                for (auto const& d : p.dependencies)
                    depended_on.insert(d);
            for (auto const& p : lock.packages)
                if (!depended_on.contains(p.location) && !wanted.contains(p.location))
                    add_finding(out, Severity::Hint, p.range, std::format("lock entry {} is no longer required: run 'dpm lock'", p.location));
        }
    } // namespace detail

    [[nodiscard]] inline Result analyze(std::string_view text, Options const& options)
    {
        Result result;
        auto name = detail::file_label(options);
        result.kind = manifest::detect_kind(text, name);
        auto loaded = manifest::load(text, detail::load_options(options, result.kind, name, detail::directory_of(options)));
        for (auto const& d : loaded.diagnostics)
        {
            if (d.file == name)
                detail::add_finding(result.findings, Severity::Error, d.range, d.message);
            else
                result.findings.push_back({Severity::Error, 0, 0, std::format("{}: {}", d.file, d.message)});
        }
        result.loaded = loaded.ok;
        if (loaded.ok && result.kind == manifest::FileKind::Project && loaded.project)
            detail::project_findings(loaded, options, result.findings);
        else if (loaded.ok && result.kind == manifest::FileKind::Lock && loaded.lock)
            detail::lock_findings(loaded, options, result.findings);
        else if (loaded.ok && result.kind == manifest::FileKind::Template && loaded.project_template)
            detail::template_findings(loaded, options, result.findings);
        std::ranges::stable_sort(result.findings, [](Finding const& a, Finding const& b) { return std::tie(a.begin, a.end) < std::tie(b.begin, b.end); });
        return result;
    }

    enum class Where : std::uint8_t
    {
        None,
        FieldName,
        Value,
    };

    struct Cursor
    {
        Where where{Where::None};
        std::string struct_type;
        std::string field;
        std::string value_type;
        std::vector<std::string> present;
        std::uint32_t token_begin{};
        std::uint32_t token_end{};
        std::string token_text;
        std::string prefix;
        std::string qualifier;
        std::string qualified_variant_type;
        bool in_string{};
        bool on_field_name{};
        std::vector<std::string> constants;
    };

    namespace detail
    {
        struct Ctx
        {
            enum class Kind : std::uint8_t
            {
                Struct,
                List,
                Opaque,
            };
            Kind kind{Kind::Opaque};
            std::string type;
            std::string current_field;
            bool in_value{};
            std::vector<std::string> present;
        };

        inline std::string_view slice(std::string_view text, Tok const& t)
        {
            return text.substr(t.begin, t.end - t.begin);
        }

        inline std::string root_type_name(std::string_view text, std::vector<Tok> const& tokens, std::size_t start, manifest::FileKind kind, std::size_t& open)
        {
            std::size_t i = start;
            std::string last;
            while (i < tokens.size() && tokens[i].kind == TokenKind::Identifier)
            {
                last = std::string{slice(text, tokens[i])};
                ++i;
                if (i < tokens.size() && tokens[i].kind == TokenKind::ColonColon)
                    ++i;
                else
                    break;
            }
            open = i;
            if (!last.empty() && i < tokens.size() && tokens[i].kind == TokenKind::LBrace)
                return last;
            open = start;
            while (open < tokens.size() && tokens[open].kind != TokenKind::LBrace)
                ++open;
            return std::string{manifest::type_name(kind)};
        }

        inline void enter_brace_into(Ctx const& parent, schema::Schema const& schema, Ctx& next)
        {
            if (parent.kind == Ctx::Kind::Struct && parent.in_value)
            {
                if (auto const* type = schema.find(parent.type))
                    if (auto const* field = type->field(parent.current_field))
                    {
                        auto ref = schema::parse_type(field->type);
                        if (ref.list_depth > 0)
                        {
                            next.kind = Ctx::Kind::List;
                            next.type = ref.base;
                            next.current_field = std::to_string(ref.list_depth - 1);
                        }
                        else if (auto const* inner = schema.find(ref.base); inner && !inner->is_enum)
                        {
                            next.kind = Ctx::Kind::Struct;
                            next.type = ref.base;
                        }
                    }
            }
            else if (parent.kind == Ctx::Kind::List)
            {
                int depth = std::atoi(parent.current_field.c_str());
                if (depth > 0)
                {
                    next.kind = Ctx::Kind::List;
                    next.type = parent.type;
                    next.current_field = std::to_string(depth - 1);
                }
                else if (auto const* inner = schema.find(parent.type); inner && !inner->is_enum)
                {
                    next.kind = Ctx::Kind::Struct;
                    next.type = parent.type;
                }
            }
        }
    } // namespace detail

    [[nodiscard]] inline std::optional<schema::Schema> load_schema(std::filesystem::path const& schema_root)
    {
        auto text = detail::read_small(schema_root / "dpm" / "v1.dc");
        if (!text)
            return std::nullopt;
        return schema::parse(*text);
    }

    [[nodiscard]] inline Cursor locate(std::string_view text, manifest::FileKind kind, schema::Schema const& schema, std::uint32_t offset)
    {
        using detail::Ctx;
        using detail::Tok;
        using detail::TokenKind;
        Cursor cursor;
        if (text.size() > k_max_text || offset > text.size())
            return cursor;
        manifest::detail::Source src;
        src.text = std::string{text};
        auto id = src.session.open_in_memory(std::string{"file://"} + std::string{manifest::k_split_path}, src.text);
        src.file = src.session.source_manager().get(id);
        if (!src.file)
            return cursor;
        auto tokens = manifest::detail::lex_tokens(src.session, *src.file);
        std::string_view view{src.text};

        int depth = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            if (manifest::detail::is_open(tokens[i].kind))
                ++depth;
            else if (manifest::detail::is_close(tokens[i].kind))
                --depth;
            else if (tokens[i].kind == TokenKind::Semicolon && depth == 0)
                start = i + 1;
            else if (depth == 0 && i + 1 < tokens.size() && tokens[i + 1].kind == TokenKind::Identifier && detail::slice(view, tokens[i]) == "const")
                cursor.constants.emplace_back(detail::slice(view, tokens[i + 1]));
        }
        std::size_t open = start;
        auto root_type = detail::root_type_name(view, tokens, start, kind, open);
        if (open >= tokens.size() || tokens[open].kind != TokenKind::LBrace)
            return cursor;

        std::optional<std::size_t> under;
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            auto const& t = tokens[i];
            bool word = t.kind == TokenKind::Identifier;
            bool str = t.kind == TokenKind::StringLiteral;
            if ((word && t.begin <= offset && offset <= t.end) || (str && t.begin < offset && offset < t.end))
            {
                under = i;
                break;
            }
            if (t.begin >= offset)
                break;
        }

        struct Frame
        {
            Ctx ctx;
            std::size_t open_index{};
        };
        std::vector<Frame> stack;
        for (std::size_t i = open; i < tokens.size(); ++i)
        {
            auto const& t = tokens[i];
            if (under ? i >= *under : t.begin >= offset)
                break;
            if (t.end > offset)
                break;
            switch (t.kind)
            {
                case TokenKind::LBrace:
                    if (stack.empty())
                    {
                        Frame f;
                        f.ctx.kind = Ctx::Kind::Struct;
                        f.ctx.type = root_type;
                        f.open_index = i;
                        stack.push_back(std::move(f));
                    }
                    else
                    {
                        Frame f;
                        detail::enter_brace_into(stack.back().ctx, schema, f.ctx);
                        f.open_index = i;
                        stack.push_back(std::move(f));
                    }
                    break;
                case TokenKind::LParen:
                case TokenKind::LBracket:
                    stack.push_back({Ctx{}, i});
                    break;
                case TokenKind::RBrace:
                case TokenKind::RParen:
                case TokenKind::RBracket:
                    if (!stack.empty())
                        stack.pop_back();
                    break;
                case TokenKind::Comma:
                    if (!stack.empty() && stack.back().ctx.kind == Ctx::Kind::Struct)
                    {
                        stack.back().ctx.in_value = false;
                        stack.back().ctx.current_field.clear();
                    }
                    break;
                case TokenKind::Identifier:
                    if (!stack.empty() && stack.back().ctx.kind == Ctx::Kind::Struct && !stack.back().ctx.in_value && i + 1 < tokens.size() &&
                        tokens[i + 1].kind == TokenKind::Eq && tokens[i + 1].begin < offset)
                        stack.back().ctx.current_field = std::string{detail::slice(view, t)};
                    break;
                case TokenKind::Eq:
                    if (!stack.empty() && stack.back().ctx.kind == Ctx::Kind::Struct && !stack.back().ctx.current_field.empty())
                        stack.back().ctx.in_value = true;
                    break;
                default:
                    break;
            }
        }
        if (under)
        {
            auto const& t = tokens[*under];
            cursor.token_begin = t.begin;
            cursor.token_end = t.end;
            cursor.token_text = std::string{detail::slice(view, t)};
            cursor.in_string = t.kind == TokenKind::StringLiteral;
            cursor.prefix = std::string{view.substr(t.begin, offset - t.begin)};
            if (*under >= 2 && tokens[*under - 1].kind == TokenKind::ColonColon && tokens[*under - 2].kind == TokenKind::Identifier)
                cursor.qualified_variant_type = std::string{detail::slice(view, tokens[*under - 2])};
        }
        else
        {
            cursor.token_begin = cursor.token_end = offset;
            std::size_t before = 0;
            while (before < tokens.size() && tokens[before].end <= offset)
                ++before;
            if (before >= 2 && tokens[before - 1].kind == TokenKind::ColonColon && tokens[before - 2].kind == TokenKind::Identifier)
                cursor.qualified_variant_type = std::string{detail::slice(view, tokens[before - 2])};
        }
        if (stack.empty())
            return cursor;
        auto const& top = stack.back();
        if (top.ctx.kind == Ctx::Kind::Struct)
        {
            cursor.struct_type = top.ctx.type;
            if (top.ctx.in_value)
            {
                cursor.where = Where::Value;
                cursor.field = top.ctx.current_field;
                if (auto const* type = schema.find(top.ctx.type))
                    if (auto const* field = type->field(top.ctx.current_field))
                        cursor.value_type = field->type;
            }
            else
            {
                cursor.where = Where::FieldName;
                if (under && !cursor.in_string && *under + 1 < tokens.size() && tokens[*under + 1].kind == TokenKind::Eq)
                {
                    cursor.on_field_name = true;
                    cursor.field = cursor.token_text;
                }
            }
            int nest = 0;
            for (std::size_t i = top.open_index + 1; i < tokens.size(); ++i)
            {
                if (manifest::detail::is_open(tokens[i].kind))
                    ++nest;
                else if (manifest::detail::is_close(tokens[i].kind))
                {
                    if (nest == 0)
                        break;
                    --nest;
                }
                else if (nest == 0 && tokens[i].kind == TokenKind::Identifier && i + 1 < tokens.size() && tokens[i + 1].kind == TokenKind::Eq &&
                         (tokens[i - 1].kind == TokenKind::LBrace || tokens[i - 1].kind == TokenKind::Comma))
                    cursor.present.emplace_back(detail::slice(view, tokens[i]));
            }
        }
        else if (top.ctx.kind == Ctx::Kind::List)
        {
            cursor.where = Where::Value;
            cursor.value_type = top.ctx.type;
        }
        if (cursor.on_field_name)
            std::erase(cursor.present, cursor.field);
        return cursor;
    }

    struct Hover
    {
        std::string markdown;
        std::uint32_t begin{};
        std::uint32_t end{};
    };

    namespace detail
    {
        inline std::string field_markdown(schema::Type const& type, schema::Field const& f)
        {
            std::string out = "```dc\n" + f.type + " " + f.name;
            if (f.has_default)
                out += " = " + f.default_text;
            out += "\n```\n";
            out += std::format("\nField of `{}`.", type.name);
            if (!f.doc.empty())
                out += "\n\n" + f.doc;
            return out;
        }

        inline std::string type_markdown(schema::Type const& type)
        {
            std::string out = std::format("```dc\n{} {}\n```\n", type.is_enum ? "enum" : "struct", type.name);
            if (!type.doc.empty())
                out += "\n" + type.doc;
            return out;
        }

        inline std::string variant_markdown(schema::Type const& type, schema::Variant const& v)
        {
            std::string out = std::format("```dc\n{}::{}\n```\n", type.name, v.name);
            if (!v.doc.empty())
                out += "\n" + v.doc;
            return out;
        }

        inline std::optional<std::string> cached_alias_description(std::filesystem::path const& cache_root, std::string_view alias, Options const& o)
        {
            if (!is_alias(alias) || alias.size() < 2)
                return std::nullopt;
            std::error_code ec;
            auto registry = cache_root / "registry";
            if (!std::filesystem::is_directory(registry, ec))
                return std::nullopt;
            for (auto const& entry : std::filesystem::directory_iterator{registry, ec})
            {
                auto file = entry.path() / "aliases" / std::string{alias.substr(0, 2)} / (std::string{alias} + ".dcon");
                auto text = read_small(file);
                if (!text)
                    continue;
                auto loaded = manifest::load(*text, load_options(o, manifest::FileKind::RegistryAlias, file.filename().string(), {}));
                if (loaded.ok && loaded.registry_alias)
                    return std::format("{} ({} released versions)", loaded.registry_alias->description.empty() ? loaded.registry_alias->location : loaded.registry_alias->description,
                                       loaded.registry_alias->versions.size());
            }
            return std::nullopt;
        }
    } // namespace detail

    [[nodiscard]] inline std::optional<Hover> hover(std::string_view text, Options const& options, schema::Schema const& schema, std::uint32_t offset)
    {
        auto kind = manifest::detect_kind(text, detail::file_label(options));
        auto cursor = locate(text, kind, schema, offset);
        if (cursor.token_text.empty())
            return std::nullopt;
        Hover h;
        h.begin = cursor.token_begin;
        h.end = cursor.token_end;
        if (cursor.on_field_name)
        {
            if (auto const* type = schema.find(cursor.struct_type))
                if (auto const* f = type->field(cursor.field))
                {
                    h.markdown = detail::field_markdown(*type, *f);
                    return h;
                }
            return std::nullopt;
        }
        if (!cursor.qualified_variant_type.empty())
            if (auto const* type = schema.find(cursor.qualified_variant_type); type && type->is_enum)
                if (auto const* v = type->variant(cursor.token_text))
                {
                    h.markdown = detail::variant_markdown(*type, *v);
                    return h;
                }
        if (!cursor.in_string)
            if (auto const* type = schema.find(cursor.token_text))
            {
                h.markdown = detail::type_markdown(*type);
                return h;
            }
        if (cursor.in_string && cursor.where == Where::Value && cursor.struct_type == "Dependency" && cursor.field == "location")
        {
            auto value = cursor.token_text.substr(1, cursor.token_text.size() >= 2 ? cursor.token_text.size() - 2 : 0);
            std::string body;
            auto dir = detail::directory_of(options);
            if (!dir.empty())
                if (auto lock_text = detail::read_small(dir / lock::k_lock_file))
                {
                    auto loaded = manifest::load(*lock_text, detail::load_options(options, manifest::FileKind::Lock, std::string{lock::k_lock_file}, dir));
                    if (loaded.ok && loaded.lock)
                        for (auto const& p : loaded.lock->packages)
                            if (p.location == detail::normalize_path_location(value))
                            {
                                body += std::format("Locked: version {}", p.version);
                                if (!p.commit.empty())
                                    body += std::format(", commit {}", p.commit);
                                body += "\n";
                            }
                }
            if (auto description = detail::cached_alias_description(detail::cache_root_of(options), value, options))
                body += "Registry: " + *description + "\n";
            if (!body.empty())
            {
                h.markdown = std::move(body);
                return h;
            }
        }
        return std::nullopt;
    }

    struct Completion
    {
        enum class Kind : std::uint8_t
        {
            Field,
            Variant,
            Constant,
            Alias,
        };
        Kind kind{Kind::Field};
        std::string label;
        std::string detail;
        std::string documentation;
        std::string insert;
        bool snippet{};
    };

    struct CompletionList
    {
        std::vector<Completion> items;
        std::uint32_t begin{};
        std::uint32_t end{};
    };

    [[nodiscard]] inline CompletionList complete(std::string_view text, Options const& options, schema::Schema const& schema, std::uint32_t offset)
    {
        CompletionList list;
        auto kind = manifest::detect_kind(text, detail::file_label(options));
        auto cursor = locate(text, kind, schema, offset);
        list.begin = cursor.token_begin;
        list.end = cursor.token_end;
        if (cursor.where == Where::FieldName)
        {
            if (cursor.in_string)
                return list;
            auto const* type = schema.find(cursor.struct_type);
            if (!type || type->is_enum)
                return list;
            for (auto const& f : type->fields)
            {
                if (std::ranges::find(cursor.present, f.name) != cursor.present.end())
                    continue;
                Completion c;
                c.kind = Completion::Kind::Field;
                c.label = f.name;
                c.detail = f.type + (f.has_default ? " = " + f.default_text : std::string{});
                c.documentation = f.doc;
                c.insert = f.name + " = $0,";
                c.snippet = true;
                list.items.push_back(std::move(c));
            }
            return list;
        }
        if (cursor.where != Where::Value)
            return list;
        if (cursor.in_string)
        {
            if (cursor.struct_type == "Dependency" && cursor.field == "location")
            {
                std::error_code ec;
                auto registry = detail::cache_root_of(options) / "registry";
                std::set<std::string> names;
                if (std::filesystem::is_directory(registry, ec))
                    for (auto const& entry : std::filesystem::directory_iterator{registry, ec})
                    {
                        auto aliases = entry.path() / "aliases";
                        if (!std::filesystem::is_directory(aliases, ec))
                            continue;
                        for (auto const& shard : std::filesystem::directory_iterator{aliases, ec})
                            for (auto const& file : std::filesystem::directory_iterator{shard.path(), ec})
                                if (file.path().extension() == ".dcon" && file.is_regular_file(ec))
                                    names.insert(file.path().stem().string());
                    }
                for (auto const& name : names)
                {
                    Completion c;
                    c.kind = Completion::Kind::Alias;
                    c.label = name;
                    c.detail = "registry alias (cached)";
                    c.insert = name;
                    list.items.push_back(std::move(c));
                }
            }
            return list;
        }
        auto ref = schema::parse_type(cursor.value_type);
        if (ref.list_depth == 0)
            if (auto const* type = schema.find(ref.base); type && type->is_enum)
            {
                bool qualified = !cursor.qualified_variant_type.empty();
                if (!qualified || cursor.qualified_variant_type == type->name)
                    for (auto const& v : type->variants)
                    {
                        Completion c;
                        c.kind = Completion::Kind::Variant;
                        c.label = qualified ? v.name : type->name + "::" + v.name;
                        c.detail = type->name;
                        c.documentation = v.doc;
                        c.insert = c.label;
                        list.items.push_back(std::move(c));
                    }
            }
        for (auto const& name : cursor.constants)
        {
            Completion c;
            c.kind = Completion::Kind::Constant;
            c.label = name;
            c.detail = "const";
            c.insert = name;
            list.items.push_back(std::move(c));
        }
        return list;
    }

    struct Definition
    {
        std::filesystem::path file;
        std::uint32_t line{};
        std::string name;
    };

    [[nodiscard]] inline std::optional<Definition> definition(std::string_view text, Options const& options, schema::Schema const& schema, std::uint32_t offset)
    {
        auto kind = manifest::detect_kind(text, detail::file_label(options));
        auto cursor = locate(text, kind, schema, offset);
        if (cursor.token_text.empty() || cursor.in_string)
            return std::nullopt;
        Definition d;
        d.file = options.schema_root / "dpm" / "v1.dc";
        if (cursor.on_field_name)
        {
            if (auto const* type = schema.find(cursor.struct_type))
                if (auto const* f = type->field(cursor.field))
                {
                    d.line = f->line;
                    d.name = f->name;
                    return d;
                }
            return std::nullopt;
        }
        if (!cursor.qualified_variant_type.empty())
            if (auto const* type = schema.find(cursor.qualified_variant_type); type && type->is_enum)
                if (auto const* v = type->variant(cursor.token_text))
                {
                    d.line = v->line;
                    d.name = v->name;
                    return d;
                }
        if (auto const* type = schema.find(cursor.token_text))
        {
            d.line = type->line;
            d.name = type->name;
            return d;
        }
        return std::nullopt;
    }
} // namespace dpm::analyze
