export module dccd.workspace_symbols;

import std;
import dcc.sm;
import dcc.ast;
import dcc.sema;
import dcc.sema.scope;
import dcc.session;
import dcc.query;
import dcc.parser;
import dcc.lex;
import dcc.diag;
import dccd.protocol;

export namespace dccd::workspace_symbols
{
    [[nodiscard]] std::vector<protocol::SymbolInformation>
    search_workspace_symbols(dcc::session::CompilerSession& session, std::span<std::filesystem::path const> workspace_roots, std::string_view query);

} // namespace dccd::workspace_symbols

module :private;

namespace dccd::workspace_symbols
{
    namespace
    {
        constexpr std::size_t kMaxResults = 250;

        struct SymbolKey
        {
            std::string_view name;
            dcc::sm::SourceRange range;

            bool operator==(SymbolKey const& o) const noexcept
            {
                return name == o.name && range.begin.fileId == o.range.begin.fileId && range.begin.offset == o.range.begin.offset &&
                       range.end.offset == o.range.end.offset;
            }
        };

        struct SymbolKeyHash
        {
            std::size_t operator()(SymbolKey const& k) const noexcept
            {
                auto h1 = std::hash<std::string_view>{}(k.name);
                auto h2 = static_cast<std::size_t>(static_cast<std::uint32_t>(k.range.begin.fileId));
                auto h3 = static_cast<std::size_t>(k.range.begin.offset);
                auto h4 = static_cast<std::size_t>(k.range.end.offset);
                return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
            }
        };

        [[nodiscard]] bool match_query(std::string_view name, std::string_view query)
        {
            if (query.empty())
                return true;

            if (name.empty())
                return false;

            if (query.size() > name.size())
                return false;

            auto icase_char = [](char c) -> char {
                if (c >= 'A' && c <= 'Z')
                    return static_cast<char>(c + ('a' - 'A'));

                return c;
            };

            for (std::size_t i = 0; i + query.size() <= name.size(); ++i)
            {
                bool matches = true;
                for (std::size_t j = 0; j < query.size(); ++j)
                    if (icase_char(name[i + j]) != icase_char(query[j]))
                    {
                        matches = false;
                        break;
                    }

                if (matches)
                    return true;
            }

            return false;
        }

        [[nodiscard]] std::optional<protocol::LspLocation> range_to_lsp_location(dcc::sm::SourceManager& sm, dcc::sm::SourceRange const& r)
        {
            if (!r.valid())
                return std::nullopt;

            auto const* sf = sm.get(r.begin.fileId);
            if (!sf)
                return std::nullopt;

            auto start_pos = sm.location_to_lsp_position(r.begin);
            auto end_pos = sm.location_to_lsp_position(r.end);
            if (!start_pos || !end_pos)
                return std::nullopt;

            protocol::LspLocation loc;
            loc.uri = sf->uri();
            loc.range.start.line = start_pos->line;
            loc.range.start.character = start_pos->character;
            loc.range.end.line = end_pos->line;
            loc.range.end.character = end_pos->character;
            return loc;
        }

        [[nodiscard]] protocol::SymbolKind decl_kind_to_symbol_kind(dcc::ast::Decl const* d)
        {
            switch (d->kind)
            {
                case dcc::ast::DeclKind::Struct:
                    return protocol::SymbolKind::Struct;
                case dcc::ast::DeclKind::Union:
                    return protocol::SymbolKind::Struct;
                case dcc::ast::DeclKind::Enum:
                    return protocol::SymbolKind::Enum;
                case dcc::ast::DeclKind::Func:
                    return protocol::SymbolKind::Function;
                case dcc::ast::DeclKind::Var:
                    return protocol::SymbolKind::Variable;
                case dcc::ast::DeclKind::Using:
                    return protocol::SymbolKind::Namespace;
                case dcc::ast::DeclKind::Module:
                    return protocol::SymbolKind::Module;
                case dcc::ast::DeclKind::Import:
                    return protocol::SymbolKind::Module;
                case dcc::ast::DeclKind::StaticIfGroup:
                    return protocol::SymbolKind::Namespace;
            }
            return protocol::SymbolKind::Variable;
        }

        [[nodiscard]] std::string_view decl_name(dcc::ast::Decl const* d)
        {
            switch (d->kind)
            {
                case dcc::ast::DeclKind::Struct:
                    return static_cast<dcc::ast::StructDecl const*>(d)->name;
                case dcc::ast::DeclKind::Union:
                    return static_cast<dcc::ast::UnionDecl const*>(d)->name;
                case dcc::ast::DeclKind::Enum:
                    return static_cast<dcc::ast::EnumDecl const*>(d)->name;
                case dcc::ast::DeclKind::Func:
                    return static_cast<dcc::ast::FuncDecl const*>(d)->name;
                case dcc::ast::DeclKind::Var:
                    return static_cast<dcc::ast::VarDecl const*>(d)->name;
                case dcc::ast::DeclKind::Using: {
                    auto const* ud = static_cast<dcc::ast::UsingDecl const*>(d);
                    return ud->alias_path.is_empty() ? std::string_view{} : ud->alias_path.tail_name();
                }
                case dcc::ast::DeclKind::Module: {
                    auto const* md = static_cast<dcc::ast::ModuleDecl const*>(d);
                    return md->module_path.tail_name();
                }
                default:
                    return {};
            }
        }

        [[nodiscard]] dcc::sm::SourceRange decl_name_range(dcc::ast::Decl const* d)
        {
            return dcc::query::decl_name_range(d);
        }

        class SymbolCollector
        {
        public:
            SymbolCollector(std::string_view query) : m_query{query} { m_seen.reserve(512); }

            void add(std::string_view name, protocol::SymbolKind kind, dcc::sm::SourceRange const& range, std::optional<std::string> container = std::nullopt)
            {
                if (m_results.size() >= kMaxResults)
                    return;

                if (!match_query(name, m_query))
                    return;

                SymbolKey key{name, range};
                if (m_seen.contains(key))
                    return;

                protocol::SymbolInformation info;
                info.name = std::string{name};
                info.kind = kind;
                info.containerName = std::move(container);

                m_pending.emplace_back(std::move(info), key);
            }

            void resolve_locations(dcc::sm::SourceManager& sm)
            {
                for (auto& [info, key] : m_pending)
                {
                    auto loc = range_to_lsp_location(sm, key.range);
                    if (!loc)
                        continue;

                    info.location = std::move(*loc);
                    m_results.push_back(std::move(info));
                    m_seen.insert(key);

                    if (m_results.size() >= kMaxResults)
                        break;
                }
                m_pending.clear();
            }

            [[nodiscard]] std::vector<protocol::SymbolInformation>& results() { return m_results; }
            [[nodiscard]] std::vector<protocol::SymbolInformation> const& results() const { return m_results; }

        private:
            std::string_view m_query;
            std::vector<protocol::SymbolInformation> m_results;
            std::vector<std::pair<protocol::SymbolInformation, SymbolKey>> m_pending;
            std::unordered_set<SymbolKey, SymbolKeyHash> m_seen;
        };

        [[nodiscard]] std::string path_to_string(dcc::ast::Path const& p)
        {
            std::string s;
            for (std::size_t i = 0; i < p.segments.size(); ++i)
            {
                if (i > 0)
                    s += "::";
                s += p.segments[i].name;
            }
            return s;
        }

        [[nodiscard]] std::string module_name_from_info(dcc::sema::ModuleInfo const& mod)
        {
            if (mod.tu && mod.tu->module_decl)
            {
                auto const* md = static_cast<dcc::ast::ModuleDecl const*>(mod.tu->module_decl);
                auto s = path_to_string(md->module_path);
                if (!s.empty())
                    return s;
            }
            return mod.canonical_path.str();
        }

        void collect_from_tu(dcc::ast::TranslationUnit const* tu, SymbolCollector& collector, std::string const& container_name)
        {
            if (!tu)
                return;

            for (auto* d : tu->decls)
            {
                if (!d)
                    continue;

                auto name = decl_name(d);
                if (name.empty())
                    continue;

                auto kind = decl_kind_to_symbol_kind(d);
                auto nr = decl_name_range(d);
                if (!nr.valid())
                    nr = d->range;

                collector.add(name, kind, nr, container_name);

                switch (d->kind)
                {
                    case dcc::ast::DeclKind::Struct: {
                        auto const* sd = static_cast<dcc::ast::StructDecl const*>(d);
                        for (auto const& f : sd->fields)
                        {
                            if (!f.name.empty())
                            {
                                auto field_range = f.name_range.valid() ? f.name_range : f.range;
                                collector.add(f.name, protocol::SymbolKind::Field, field_range, std::string{name});
                            }
                        }
                        break;
                    }
                    case dcc::ast::DeclKind::Union: {
                        auto const* ud = static_cast<dcc::ast::UnionDecl const*>(d);
                        for (auto const& f : ud->fields)
                        {
                            if (!f.name.empty())
                            {
                                auto field_range = f.name_range.valid() ? f.name_range : f.range;
                                collector.add(f.name, protocol::SymbolKind::Field, field_range, std::string{name});
                            }
                        }
                        break;
                    }
                    case dcc::ast::DeclKind::Enum: {
                        auto const* ed = static_cast<dcc::ast::EnumDecl const*>(d);
                        for (auto const& v : ed->variants)
                        {
                            if (!v.name.empty())
                            {
                                auto variant_range = v.range.valid() ? v.range : d->range;
                                collector.add(v.name, protocol::SymbolKind::EnumMember, variant_range, std::string{name});
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        using LoadedPathSet = std::unordered_set<std::string>;

        void collect_from_graph(dcc::sema::SemaContext& sema, SymbolCollector& collector, LoadedPathSet& loaded_paths)
        {
            auto& graph = sema.graph();
            for (auto const& mod : graph.all())
            {
                if (!mod || !mod->tu)
                    continue;

                if (!mod->file_path.empty())
                {
                    std::error_code ec;
                    auto canonical = std::filesystem::weakly_canonical(mod->file_path, ec);
                    if (!ec)
                        loaded_paths.insert(canonical.string());
                    else
                        loaded_paths.insert(mod->file_path.string());
                }

                auto container = module_name_from_info(*mod);

                collect_from_tu(mod->tu, collector, container);
            }
        }

        void collect_from_unlinked_files(dcc::session::CompilerSession& session, std::span<std::filesystem::path const> workspace_roots,
                                         SymbolCollector& collector, LoadedPathSet const& loaded_paths)
        {
            for (auto const& root : workspace_roots)
            {
                std::error_code ec;
                if (!std::filesystem::is_directory(root, ec))
                    continue;

                for (auto const& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec))
                {
                    if (ec)
                        continue;

                    if (!entry.is_regular_file())
                        continue;

                    if (entry.path().extension() != ".dc")
                        continue;

                    std::error_code canonical_ec;
                    auto canonical = std::filesystem::weakly_canonical(entry.path(), canonical_ec);
                    std::string path_key;
                    if (!canonical_ec)
                        path_key = canonical.string();
                    else
                        path_key = entry.path().string();

                    if (loaded_paths.contains(path_key))
                        continue;

                    if (collector.results().size() >= kMaxResults)
                        return;

                    auto load_result = session.source_manager().load(entry.path());
                    if (!load_result)
                        continue;

                    auto fid = *load_result;

                    auto* tu = session.parse_file(fid);
                    if (!tu)
                        continue;

                    std::string container;
                    if (tu->module_decl)
                        container = path_to_string(static_cast<dcc::ast::ModuleDecl const*>(tu->module_decl)->module_path);
                    if (container.empty())
                    {
                        container = entry.path().filename().string();
                    }

                    collect_from_tu(tu, collector, container);
                }
            }
        }

    } // anonymous namespace

    std::vector<protocol::SymbolInformation> search_workspace_symbols(dcc::session::CompilerSession& session,
                                                                      std::span<std::filesystem::path const> workspace_roots, std::string_view query)
    {
        SymbolCollector collector{query};

        LoadedPathSet loaded_paths;

        auto* sema_ctx = session.sema_context();
        if (sema_ctx)
            collect_from_graph(*sema_ctx, collector, loaded_paths);

        if (collector.results().size() < kMaxResults && !workspace_roots.empty())
            collect_from_unlinked_files(session, workspace_roots, collector, loaded_paths);

        collector.resolve_locations(session.source_manager());

        return std::move(collector.results());
    }

} // namespace dccd::workspace_symbols
