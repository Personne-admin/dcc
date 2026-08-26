export module dccd.workspace_index;

import std;
import dcc.sm;
import dcc.session;
import dcc.sema;
import dcc.query;
import dcc.ast;
import dccd.protocol;

export namespace dccd::workspace_index
{
    struct SyncStats
    {
        std::uint64_t modules_re_extracted{0};
        std::uint64_t modules_retained{0};
        std::uint64_t modules_dropped{0};
        std::uint64_t unlinked_parsed{0};
        std::uint64_t unlinked_reused{0};
        std::uint64_t workspace_queries_served_without_parse{0};
        std::uint64_t sync_count{0};
    };

    class WorkspaceIndex
    {
    public:
        struct ModuleRecord
        {
            std::string canonical_path;
            std::string file_path;
            dcc::sm::FileId file_id{dcc::sm::FileId::Invalid};
            std::uint64_t content_revision{0};
            std::vector<std::string> imports;
            std::vector<dcc::query::IndexedSymbolRecord> symbols;
            std::vector<dcc::query::ReferenceOccurrence> occurrences;
        };

        struct UnlinkedProjection
        {
            std::string path_key;
            dcc::sm::DiskSignature signature;
            std::uint64_t content_revision{0};
            std::vector<dcc::query::IndexedSymbolRecord> symbols;
        };

        WorkspaceIndex() = default;

        WorkspaceIndex(WorkspaceIndex const&) = delete;
        WorkspaceIndex& operator=(WorkspaceIndex const&) = delete;
        WorkspaceIndex(WorkspaceIndex&&) = delete;
        WorkspaceIndex& operator=(WorkspaceIndex&&) = delete;

        void sync(dcc::session::CompilerSession const& session);

        void ensure_synced(dcc::session::CompilerSession const& session);

        [[nodiscard]] std::vector<protocol::SymbolInformation> search_symbols(dcc::session::CompilerSession& session,
                                                                              std::span<std::filesystem::path const> workspace_roots, std::string_view query);

        [[nodiscard]] std::vector<dcc::sm::SourceRange> occurrences_for(dcc::query::SymbolId const& id) const;

        [[nodiscard]] std::optional<dcc::sm::SourceRange> declaration_for(dcc::query::SymbolId const& id) const;

        [[nodiscard]] bool module_fresh(dcc::sm::FileId file, std::uint64_t current_revision) const noexcept;

        [[nodiscard]] bool has_module_for_file(dcc::sm::FileId file) const noexcept;

        void invalidate_unlinked(std::string_view path_key);

        void invalidate_module(std::string_view canonical_path);

        void clear() noexcept;

        [[nodiscard]] SyncStats const& stats() const noexcept { return m_stats; }
        [[nodiscard]] std::size_t module_count() const noexcept { return m_modules.size(); }
        [[nodiscard]] std::size_t unlinked_count() const noexcept { return m_unlinked.size(); }

        [[nodiscard]] ModuleRecord const* module_record(std::string_view canonical_path) const noexcept;
        [[nodiscard]] std::optional<dcc::query::SymbolId> symbol_id_for(std::string_view canonical_path, std::string_view name) const;

    private:
        struct GraphSnapshot
        {
            std::uint64_t revision{0};
            std::vector<std::string> imports;
        };

        void build_unlinked_projections(dcc::session::CompilerSession& session, std::span<std::filesystem::path const> workspace_roots, bool& parsed_any);

        [[nodiscard]] std::optional<protocol::SymbolInformation> to_symbol_information(dcc::query::IndexedSymbolRecord const& rec,
                                                                                       dcc::sm::SourceManager const& sm) const;

        [[nodiscard]] static bool match_query(std::string_view name, std::string_view query) noexcept;

        std::unordered_map<std::string, ModuleRecord> m_modules;
        std::vector<std::string> m_module_order;
        std::unordered_map<std::string, UnlinkedProjection> m_unlinked;
        SyncStats m_stats;
        bool m_synced{false};
    };

} // namespace dccd::workspace_index

module :private;

namespace dccd::workspace_index
{
    namespace
    {
        constexpr std::size_t kMaxResults = 250;

        [[nodiscard]] bool same_imports(std::vector<std::string> const& a, std::vector<std::string> const& b)
        {
            return a == b;
        }

        [[nodiscard]] protocol::SymbolKind to_protocol_kind(dcc::query::IndexedSymbolRecord const& rec)
        {
            switch (rec.kind)
            {
                case dcc::query::SymbolKind::Field:
                    return protocol::SymbolKind::Field;
                case dcc::query::SymbolKind::EnumVariant:
                    return protocol::SymbolKind::EnumMember;
                case dcc::query::SymbolKind::UsingAlias:
                    return protocol::SymbolKind::Namespace;
                case dcc::query::SymbolKind::Declaration:
                case dcc::query::SymbolKind::FuncParam:
                case dcc::query::SymbolKind::TemplateParam:
                case dcc::query::SymbolKind::ImportAlias:
                case dcc::query::SymbolKind::Module:
                case dcc::query::SymbolKind::Unknown:
                    break;
            }

            switch (static_cast<dcc::ast::DeclKind>(rec.decl_kind))
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
                default:
                    return protocol::SymbolKind::Variable;
            }
        }

        [[nodiscard]] std::vector<std::filesystem::path> collect_workspace_files(std::span<std::filesystem::path const> workspace_roots)
        {
            std::vector<std::filesystem::path> files;
            for (auto const& root : workspace_roots)
            {
                std::error_code ec;
                if (!std::filesystem::is_directory(root, ec) || ec)
                    continue;

                for (auto const& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec))
                {
                    if (ec)
                        break;

                    if (!entry.is_regular_file())
                        continue;

                    if (entry.path().extension() != ".dc")
                        continue;

                    files.push_back(entry.path());
                }
            }

            std::ranges::sort(files);
            return files;
        }

        [[nodiscard]] std::string canonical_path_key(std::filesystem::path const& p)
        {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(p, ec);
            if (ec)
                return p.string();
            return canonical.string();
        }

    } // anonymous namespace

    void WorkspaceIndex::sync(dcc::session::CompilerSession const& session)
    {
        ++m_stats.sync_count;

        auto* sema_ctx = const_cast<dcc::sema::SemaContext*>(session.sema_context());
        if (!sema_ctx)
        {
            clear();
            return;
        }

        auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
        auto const& sm = session.source_manager();

        std::unordered_map<std::string, GraphSnapshot> snapshot;
        std::unordered_map<std::string, std::vector<std::string>> reverse_edges;
        for (auto const& mod : graph.all())
        {
            GraphSnapshot snap;
            snap.revision = sm.content_revision(mod->file_id);
            for (auto const& binding : mod->imports)
            {
                if (!binding.target)
                    continue;

                auto target = binding.target->canonical_path.str();
                snap.imports.push_back(target);
                reverse_edges[target].push_back(mod->canonical_path.str());
            }
            snapshot.emplace(mod->canonical_path.str(), std::move(snap));
        }

        std::unordered_set<std::string> changed;
        for (auto const& [path, snap] : snapshot)
        {
            auto it = m_modules.find(path);
            bool differs = it == m_modules.end() || it->second.content_revision != snap.revision || !same_imports(it->second.imports, snap.imports);
            if (differs)
                changed.insert(path);
        }

        std::unordered_set<std::string> must_extract = changed;
        std::vector<std::string> queue(changed.begin(), changed.end());
        while (!queue.empty())
        {
            auto path = std::move(queue.back());
            queue.pop_back();

            auto it = reverse_edges.find(path);
            if (it == reverse_edges.end())
                continue;

            for (auto const& importer : it->second)
                if (must_extract.insert(importer).second)
                    queue.push_back(importer);
        }

        std::unordered_map<std::string, std::uint64_t> skip;
        for (auto const& [path, snap] : snapshot)
        {
            if (must_extract.contains(path))
                continue;

            auto it = m_modules.find(path);
            if (it != m_modules.end() && it->second.content_revision == snap.revision && same_imports(it->second.imports, snap.imports))
                skip.emplace(path, snap.revision);
        }

        auto extraction = dcc::query::extract_workspace(session, skip);

        std::unordered_set<std::string> seen;
        for (auto& em : extraction.modules)
        {
            seen.insert(em.canonical_path);

            auto it = m_modules.find(em.canonical_path);
            if (em.skipped)
            {
                if (it != m_modules.end())
                {
                    it->second.file_id = em.file_id;
                    it->second.file_path = em.file_path;
                    it->second.content_revision = em.content_revision;
                    it->second.imports = std::move(em.imports);
                    ++m_stats.modules_retained;
                    continue;
                }
            }

            ModuleRecord rec;
            rec.canonical_path = std::move(em.canonical_path);
            rec.file_path = std::move(em.file_path);
            rec.file_id = em.file_id;
            rec.content_revision = em.content_revision;
            rec.imports = std::move(em.imports);
            rec.symbols = std::move(em.symbols);
            rec.occurrences = std::move(em.references);

            if (it == m_modules.end())
                m_module_order.push_back(rec.canonical_path);
            m_modules[rec.canonical_path] = std::move(rec);
            ++m_stats.modules_re_extracted;
        }

        for (auto it = m_modules.begin(); it != m_modules.end();)
        {
            if (!seen.contains(it->first))
            {
                ++m_stats.modules_dropped;
                it = m_modules.erase(it);
            }
            else
                ++it;
        }
        std::erase_if(m_module_order, [&](std::string const& path) { return !m_modules.contains(path); });

        for (auto const& path : m_module_order)
            m_unlinked.erase(m_modules.at(path).file_path);

        m_synced = true;
    }

    void WorkspaceIndex::ensure_synced(dcc::session::CompilerSession const& session)
    {
        if (m_synced)
            return;
        sync(session);
    }

    bool WorkspaceIndex::module_fresh(dcc::sm::FileId file, std::uint64_t current_revision) const noexcept
    {
        for (auto const& path : m_module_order)
        {
            auto const& rec = m_modules.at(path);
            if (rec.file_id == file)
                return rec.content_revision == current_revision;
        }
        return false;
    }

    bool WorkspaceIndex::has_module_for_file(dcc::sm::FileId file) const noexcept
    {
        for (auto const& path : m_module_order)
            if (m_modules.at(path).file_id == file)
                return true;
        return false;
    }

    std::vector<dcc::sm::SourceRange> WorkspaceIndex::occurrences_for(dcc::query::SymbolId const& id) const
    {
        std::vector<dcc::sm::SourceRange> out;
        for (auto const& path : m_module_order)
        {
            auto const& rec = m_modules.at(path);
            for (auto const& occ : rec.occurrences)
                if (occ.target == id)
                    out.push_back(occ.range);
        }

        std::ranges::sort(out, [](dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) {
            auto fa = static_cast<std::uint32_t>(a.begin.fileId);
            auto fb = static_cast<std::uint32_t>(b.begin.fileId);
            if (fa != fb)
                return fa < fb;

            if (a.begin.offset != b.begin.offset)
                return a.begin.offset < b.begin.offset;

            return a.end.offset < b.end.offset;
        });
        auto [first, last] = std::ranges::unique(out, [](dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) {
            return a.begin.fileId == b.begin.fileId && a.begin.offset == b.begin.offset && a.end.offset == b.end.offset;
        });

        out.erase(first, last);
        return out;
    }

    void WorkspaceIndex::invalidate_unlinked(std::string_view path_key)
    {
        m_unlinked.erase(std::string{path_key});
    }

    std::optional<dcc::sm::SourceRange> WorkspaceIndex::declaration_for(dcc::query::SymbolId const& id) const
    {
        if (!id.valid())
            return std::nullopt;

        for (auto const& path : m_module_order)
        {
            auto const& rec = m_modules.at(path);
            for (auto const& sym : rec.symbols)
                if (sym.id == id)
                    return sym.definition_range;
        }

        return std::nullopt;
    }

    void WorkspaceIndex::invalidate_module(std::string_view canonical_path)
    {
        auto it = m_modules.find(std::string{canonical_path});
        if (it == m_modules.end())
            return;

        m_modules.erase(it);
        std::erase_if(m_module_order, [&](std::string const& p) { return p == canonical_path; });

        m_synced = false;
    }

    void WorkspaceIndex::clear() noexcept
    {
        m_modules.clear();
        m_module_order.clear();
        m_unlinked.clear();
        m_synced = false;
    }

    WorkspaceIndex::ModuleRecord const* WorkspaceIndex::module_record(std::string_view canonical_path) const noexcept
    {
        auto it = m_modules.find(std::string{canonical_path});
        return it == m_modules.end() ? nullptr : &it->second;
    }

    std::optional<dcc::query::SymbolId> WorkspaceIndex::symbol_id_for(std::string_view canonical_path, std::string_view name) const
    {
        auto const* rec = module_record(canonical_path);
        if (!rec)
            return std::nullopt;

        for (auto const& sym : rec->symbols)
            if (sym.name == name && sym.kind == dcc::query::SymbolKind::Declaration)
                return sym.id;

        return std::nullopt;
    }

    std::optional<protocol::SymbolInformation> WorkspaceIndex::to_symbol_information(dcc::query::IndexedSymbolRecord const& rec,
                                                                                     dcc::sm::SourceManager const& sm) const
    {
        protocol::SymbolInformation info;
        info.name = rec.name;
        info.kind = to_protocol_kind(rec);
        if (rec.container.empty())
            info.containerName = std::nullopt;
        else
            info.containerName = rec.container;

        auto const* sf = sm.get(rec.definition_range.begin.fileId);
        if (!sf)
            return std::nullopt;

        auto start_pos = sm.location_to_lsp_position(rec.definition_range.begin);
        auto end_pos = sm.location_to_lsp_position(rec.definition_range.end);
        if (!start_pos || !end_pos)
            return std::nullopt;

        info.location.uri = sf->uri();
        info.location.range.start.line = start_pos->line;
        info.location.range.start.character = start_pos->character;
        info.location.range.end.line = end_pos->line;
        info.location.range.end.character = end_pos->character;
        return info;
    }

    bool WorkspaceIndex::match_query(std::string_view name, std::string_view query) noexcept
    {
        if (query.empty())
            return true;
        if (name.empty() || query.size() > name.size())
            return false;

        auto icase = [](char c) -> char {
            if (c >= 'A' && c <= 'Z')
                return static_cast<char>(c + ('a' - 'A'));
            return c;
        };

        for (std::size_t i = 0; i + query.size() <= name.size(); ++i)
        {
            bool matches = true;
            for (std::size_t j = 0; j < query.size(); ++j)
                if (icase(name[i + j]) != icase(query[j]))
                {
                    matches = false;
                    break;
                }
            if (matches)
                return true;
        }
        return false;
    }

    void WorkspaceIndex::build_unlinked_projections(dcc::session::CompilerSession& session, std::span<std::filesystem::path const> workspace_roots,
                                                    bool& parsed_any)
    {
        auto& sm = session.source_manager();

        std::unordered_set<std::string> graph_paths;
        for (auto const& path : m_module_order)
            graph_paths.insert(m_modules.at(path).file_path);

        auto files = collect_workspace_files(workspace_roots);
        for (auto const& file : files)
        {
            auto key = canonical_path_key(file);
            if (graph_paths.contains(key))
                continue;

            auto signature = dcc::sm::SourceManager::stat_disk_signature(file);
            if (!signature)
                continue;

            auto it = m_unlinked.find(key);
            if (it != m_unlinked.end() && it->second.signature == signature)
            {
                ++m_stats.unlinked_reused;
                continue;
            }

            if (auto existing_fid = sm.find_by_path(file))
                std::ignore = sm.refresh_disk_file(*existing_fid);

            auto fid = sm.load(file);
            if (!fid)
                continue;

            auto records = dcc::query::extract_file_symbols(session, *fid);
            UnlinkedProjection proj;
            proj.path_key = key;
            proj.signature = *signature;
            proj.content_revision = sm.content_revision(*fid);
            proj.symbols = std::move(records);
            m_unlinked[key] = std::move(proj);
            ++m_stats.unlinked_parsed;
            parsed_any = true;
        }
    }

    std::vector<protocol::SymbolInformation> WorkspaceIndex::search_symbols(dcc::session::CompilerSession& session,
                                                                            std::span<std::filesystem::path const> workspace_roots, std::string_view query)
    {
        ensure_synced(session);

        bool parsed_any = false;
        if (!workspace_roots.empty())
            build_unlinked_projections(session, workspace_roots, parsed_any);

        auto const& sm = session.source_manager();

        struct Key
        {
            std::string_view name;
            dcc::sm::SourceRange range;
            bool operator==(Key const& o) const noexcept
            {
                return name == o.name && range.begin.fileId == o.range.begin.fileId && range.begin.offset == o.range.begin.offset &&
                       range.end.offset == o.range.end.offset;
            }
        };
        struct KeyHash
        {
            std::size_t operator()(Key const& k) const noexcept
            {
                auto h1 = std::hash<std::string_view>{}(k.name);
                auto h2 = static_cast<std::size_t>(static_cast<std::uint32_t>(k.range.begin.fileId));
                auto h3 = static_cast<std::size_t>(k.range.begin.offset);
                auto h4 = static_cast<std::size_t>(k.range.end.offset);
                return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
            }
        };
        std::unordered_set<Key, KeyHash> seen;

        std::vector<protocol::SymbolInformation> results;
        results.reserve(64);

        auto add_symbol = [&](dcc::query::IndexedSymbolRecord const& rec) {
            if (results.size() >= kMaxResults)
                return;
            if (!match_query(rec.name, query))
                return;

            Key key{rec.name, rec.definition_range};
            if (!seen.insert(key).second)
                return;

            auto info = to_symbol_information(rec, sm);
            if (!info)
                return;
            results.push_back(std::move(*info));
        };

        for (auto const& path : m_module_order)
        {
            auto const& rec = m_modules.at(path);
            for (auto const& sym : rec.symbols)
                add_symbol(sym);
        }

        std::vector<std::string> unlinked_keys;
        unlinked_keys.reserve(m_unlinked.size());
        for (auto const& [key, _] : m_unlinked)
            unlinked_keys.push_back(key);

        std::ranges::sort(unlinked_keys);
        for (auto const& key : unlinked_keys)
        {
            auto const& proj = m_unlinked.at(key);
            for (auto const& sym : proj.symbols)
                add_symbol(sym);
        }

        if (!parsed_any)
            ++m_stats.workspace_queries_served_without_parse;

        return results;
    }

} // namespace dccd::workspace_index
