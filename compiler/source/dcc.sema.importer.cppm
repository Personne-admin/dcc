export module dcc.sema.importer;

import std;
import dcc.ast;
import dcc.sm;
import dcc.si;
import dcc.diag;
import dcc.sema.scope;
import dcc.vfs;

export namespace dcc::sema
{
    using ParseFn = std::function<ast::TranslationUnit*(sm::FileId, ast::AstContext&, diag::DiagnosticEngine&)>;

    class ModuleGraph
    {
    public:
        void add_root(std::filesystem::path dir) { m_roots.push_back(std::move(dir)); }

        [[nodiscard]] std::span<std::filesystem::path const> roots() const noexcept { return m_roots; }

        [[nodiscard]] ModuleInfo* find(ModulePath const& path)
        {
            auto it = m_by_path.find(path);
            return it == m_by_path.end() ? nullptr : it->second;
        }

        [[nodiscard]] ModuleInfo const* find(ModulePath const& path) const
        {
            auto it = m_by_path.find(path);
            return it == m_by_path.end() ? nullptr : it->second;
        }

        [[nodiscard]] ast::TranslationUnit const* find_tu_for_file(sm::FileId fid) const noexcept
        {
            for (auto const& m : m_owned)
                if (m->file_id == fid)
                    return m->tu;

            return nullptr;
        }

        ModuleInfo* insert(std::unique_ptr<ModuleInfo> info)
        {
            auto* raw = info.get();
            m_by_path.emplace(raw->canonical_path, raw);
            m_owned.push_back(std::move(info));
            return raw;
        }

        [[nodiscard]] auto const& all() const noexcept { return m_owned; }

    private:
        std::vector<std::filesystem::path> m_roots;
        std::vector<std::unique_ptr<ModuleInfo>> m_owned;
        std::unordered_map<ModulePath, ModuleInfo*, ModulePathHash> m_by_path;
    };

    class Importer
    {
    public:
        Importer(ModuleGraph& graph, sm::SourceManager& sm, diag::DiagnosticEngine& diag, ast::AstContext& ast_ctx, ParseFn parse,
                 si::string_interner* interner = nullptr)
            : m_graph{graph}, m_sm{sm}, m_diag{diag}, m_ast_ctx{ast_ctx}, m_parse{std::move(parse)}, m_interner{interner}
        {
        }

        [[nodiscard]] diag::DiagnosticEngine& diag() noexcept { return m_diag; }

        // Parse a standalone source file (e.g. a `-J`/`--inject` snippet) using the configured parse callback.
        [[nodiscard]] ast::TranslationUnit* parse_source(sm::FileId fid) { return m_parse(fid, m_ast_ctx, m_diag); }

        ModuleInfo* load_entry(std::filesystem::path const& file)
        {
            auto canonical = std::filesystem::weakly_canonical(file);
            if (canonical.has_parent_path())
                m_graph.add_root(canonical.parent_path());

            return load_file(canonical, std::nullopt);
        }

        ModuleInfo* resolve_import(ast::Path const& import_path, ModuleInfo const& from)
        {
            auto written = ModulePath::from_ast(import_path);
            if (auto* hit = try_resolve(written))
                return hit;

            auto parent = from.canonical_path.parent();
            if (!parent.empty())
            {
                std::vector<std::string_view> combined;
                combined.reserve(parent.segments().size() + import_path.segments.size());
                for (auto const& s : parent.segments())
                    combined.push_back(s);
                for (auto const& s : import_path.segments)
                {
                    if (m_interner)
                        combined.push_back(m_interner->intern(s.name));
                    else
                        combined.push_back(s.name);
                }

                ModulePath shorthand{std::move(combined)};
                if (auto* hit = try_resolve(shorthand))
                    return hit;
            }

            m_diag.error(import_path.range, "could not resolve module `{}`", written.str());
            return nullptr;
        }

        [[nodiscard]] static ModulePath alias_prefix_for(ModulePath const& target, ModulePath const& importer) { return target.strip_common_prefix(importer); }

    private:
        ModuleGraph& m_graph;
        sm::SourceManager& m_sm;
        diag::DiagnosticEngine& m_diag;
        ast::AstContext& m_ast_ctx;
        ParseFn m_parse;
        si::string_interner* m_interner{nullptr};

        std::unordered_set<std::filesystem::path::string_type> in_flight_;

        static std::filesystem::path module_to_relative(ModulePath const& p)
        {
            std::filesystem::path rel;
            auto segs = p.segments();
            for (std::size_t i = 0; i + 1 < segs.size(); ++i)
                rel /= std::string{segs[i]};

            if (!segs.empty())
                rel /= std::string{segs.back()} + ".dc";

            return rel;
        }

        ModuleInfo* try_resolve_virtual(ModulePath const& path)
        {
            auto module_path_str = path.str();
            auto const* entry = dcc::vfs::lookup_by_module_path(module_path_str);
            if (!entry)
                return nullptr;

            if (auto* hit = m_graph.find(path))
                return hit;

            auto fid = dcc::vfs::materialize(*entry, m_sm);
            auto* tu = m_parse(fid, m_ast_ctx, m_diag);
            if (!tu)
                return nullptr;

            ModulePath canonical = ModulePath::from_ast(ast::node_cast<ast::ModuleDecl>(tu->module_decl)->module_path);
            if (canonical.empty())
                canonical = path;

            if (auto* hit = m_graph.find(canonical))
                return hit;

            auto info = std::make_unique<ModuleInfo>();
            info->canonical_path = canonical;
            info->file_path = std::string{entry->uri};
            info->file_id = fid;
            info->tu = tu;
            info->state = ModuleState::Parsed;

            return m_graph.insert(std::move(info));
        }

        ModuleInfo* try_resolve(ModulePath const& path)
        {
            if (auto* hit = m_graph.find(path))
                return hit;

            if (auto* virt = try_resolve_virtual(path))
                return virt;

            auto rel = module_to_relative(path);
            for (auto const& root : m_graph.roots())
            {
                auto candidate = root / rel;
                auto canonical = std::filesystem::weakly_canonical(candidate);

                if (m_sm.find_by_path(canonical) || m_sm.find_by_path(candidate))
                    return load_file(canonical, path);

                std::error_code ec;
                if (std::filesystem::exists(candidate, ec) && !ec)
                    return load_file(canonical, path);
            }

            return nullptr;
        }

        ModuleInfo* load_file(std::filesystem::path const& file, std::optional<ModulePath> expected)
        {
            auto key = file.native();
            if (auto [it, inserted] = in_flight_.emplace(std::move(key)); !inserted)
                return nullptr;

            auto guard = std::shared_ptr<void>(nullptr, [this, file](void*) { in_flight_.erase(file.native()); });

            auto file_id_opt = m_sm.load(file);
            if (!file_id_opt)
            {
                m_diag.error("failed to read source file `{}`", file.string());
                return nullptr;
            }

            auto* tu = m_parse(*file_id_opt, m_ast_ctx, m_diag);
            if (!tu)
                return nullptr;

            ModulePath canonical;
            if (tu->module_decl)
            {
                if (auto const* m = ast::node_cast<ast::ModuleDecl>(tu->module_decl))
                    canonical = ModulePath::from_ast(m->module_path);
            }

            if (canonical.empty())
            {
                m_diag.error(tu->range, "file `{}` is missing a `module` declaration", file.string());
                return nullptr;
            }

            if (expected && canonical != *expected)
            {
                auto const& cs = canonical.segments();
                auto const& es = expected->segments();
                bool ok = cs.size() <= es.size() && std::ranges::equal(cs, std::span{es}.last(cs.size()));
                if (!ok)
                {
                    m_diag.error(tu->module_decl->range, "module declaration `{}` does not match resolved path `{}`", canonical.str(), expected->str());
                    return nullptr;
                }

                canonical = *expected;
            }

            if (auto* hit = m_graph.find(canonical))
                return hit;

            auto info = std::make_unique<ModuleInfo>();
            info->canonical_path = canonical;
            info->file_path = file;
            info->file_id = *file_id_opt;
            info->tu = tu;
            info->state = ModuleState::Parsed;

            return m_graph.insert(std::move(info));
        }
    };

    void load_transitively(Importer& imp, ModuleInfo& root)
    {
        std::vector<ModuleInfo*> stack{&root};
        std::unordered_set<ModuleInfo const*> seen{&root};

        while (!stack.empty())
        {
            auto* m = stack.back();
            stack.pop_back();

            for (auto* import_decl : m->tu->imports)
            {
                auto const* imp_node = ast::node_cast<ast::ImportDecl>(import_decl);
                if (!imp_node)
                    continue;

                auto* target = imp.resolve_import(imp_node->module_path, *m);
                if (!target)
                {
                    m->has_errors = true;
                    continue;
                }

                if (target == m)
                {
                    m->has_errors = true;
                    imp.diag().error(imp_node->module_path.range, "module cannot import itself");
                    continue;
                }

                if (seen.insert(target).second)
                    stack.push_back(target);

                ModuleInfo::ImportBinding b{};
                b.decl = imp_node;
                b.target = target;
                b.alias_prefix = Importer::alias_prefix_for(target->canonical_path, m->canonical_path);
                m->imports.push_back(std::move(b));
            }
        }
    }

} // namespace dcc::sema
