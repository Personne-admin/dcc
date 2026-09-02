export module dcc.sema;

export import dcc.types;
export import dcc.sema.scope;
export import dcc.sema.importer;
export import dcc.sema.collect;
export import dcc.sema.using_resolver;
export import dcc.sema.type_resolver;
export import dcc.sema.public_validator;
export import dcc.sema.attribute_validator;
export import dcc.sema.body_analyzer;
export import dcc.sema.type_dumper;
export import dcc.sema.instantiator;

import std;
import dcc.ast;
import dcc.sm;
import dcc.si;
import dcc.diag;
import dcc.target;

export namespace dcc::sema
{
    struct SemaOptions
    {
        std::vector<std::filesystem::path> import_roots;
        std::size_t arena_initial_size{256 * 1024};
        dcc::si::string_interner* interner{nullptr};
        dcc::target::TargetConfig target{dcc::target::TargetConfig::host_default()};
        std::vector<std::string> injected_decls;
    };

    class SemaContext
    {
    public:
        SemaContext(sm::SourceManager& sm, diag::DiagnosticEngine& diag, ast::AstContext& ast_ctx, ParseFn parse, SemaOptions opts)
            : m_sm{sm}, m_diag{diag}, m_ast_ctx{ast_ctx}, m_opts{std::move(opts)}, m_buffer{m_opts.arena_initial_size}, m_alloc{&m_buffer},
              m_types{256 * 1024, &m_opts.target}, m_graph{}, m_importer{m_graph, m_sm, diag, m_ast_ctx, std::move(parse), m_opts.interner}
        {
            for (auto const& root : m_opts.import_roots)
                m_graph.add_root(root);
        }

        SemaContext(SemaContext const&) = delete;
        SemaContext& operator=(SemaContext const&) = delete;

        [[nodiscard]] ModuleGraph& graph() noexcept { return m_graph; }
        [[nodiscard]] types::TypeContext& types() noexcept { return m_types; }
        [[nodiscard]] SpecializationRegistry& spec_registry() noexcept { return m_spec_registry; }
        [[nodiscard]] std::pmr::polymorphic_allocator<> allocator() noexcept { return m_alloc; }

        ModuleInfo* analyze_entry(std::filesystem::path const& entry_file)
        {
            auto* root = m_importer.load_entry(entry_file);
            if (!root)
                return nullptr;

            prepare_injected_sources();
            load_transitively(
                m_importer, *root, [this](ModuleInfo& module) { inject_decls(module); },
                [this](ast::ImportDecl const& decl) { return m_injected_imports.contains(&decl); });

            std::vector<sm::SourceRange> parser_recovery_ranges;
            for (auto const& module : m_graph.all())
                if (module->tu)
                    parser_recovery_ranges.insert(parser_recovery_ranges.end(), module->tu->parser_recovery_ranges.begin(),
                                                  module->tu->parser_recovery_ranges.end());
            m_diag.set_parser_recovery_ranges(parser_recovery_ranges);

            collect_all(m_graph.all(), m_diag, m_types, m_alloc);
            resolve_usings(m_graph.all(), m_diag, m_alloc);
            m_diag.set_parser_recovery_suppression(true);
            resolve_signature_types(m_graph.all(), m_diag, m_types, m_alloc);
            validate_attributes(m_graph.all(), m_diag, m_alloc);
            validate_public_signatures(m_graph.all(), m_diag);
            analyze_bodies(m_graph.all(), m_diag, m_ast_ctx, m_types, m_alloc, m_spec_registry, &m_opts.target);
            m_diag.set_parser_recovery_suppression(false);

            complete_all_templated_tagged_enums(m_types, m_alloc);

            return root;
        }

    private:
        void prepare_injected_sources()
        {
            if (m_injected_sources_prepared)
                return;
            m_injected_sources_prepared = true;

            int idx = 1;
            for (auto const& snippet : m_opts.injected_decls)
            {
                auto src_name = std::format("<command-line -J #{}>", idx++);
                m_injected_sources.push_back(m_sm.add_synthetic(std::move(src_name), snippet + "\n"));
            }
        }

        void inject_decls(ModuleInfo& module)
        {
            if (m_injected_sources.empty() || !module.tu || !m_injected_modules.insert(&module).second)
                return;

            std::vector<ast::Decl*> prepend;
            for (auto fid : m_injected_sources)
            {
                auto* tu = m_importer.parse_source(fid);
                if (!tu)
                    continue;

                for (auto* imp : tu->imports)
                {
                    module.tu->imports.push_back(imp);
                    if (auto const* import_decl = ast::node_cast<ast::ImportDecl>(imp))
                        m_injected_imports.insert(import_decl);
                }
                for (auto* d : tu->decls)
                    prepend.push_back(d);
                module.tu->parser_recovery_ranges.insert(module.tu->parser_recovery_ranges.end(), tu->parser_recovery_ranges.begin(),
                                                         tu->parser_recovery_ranges.end());
            }

            module.tu->decls.insert(module.tu->decls.begin(), prepend.begin(), prepend.end());
        }

        sm::SourceManager& m_sm;
        diag::DiagnosticEngine& m_diag;
        ast::AstContext& m_ast_ctx;
        SemaOptions m_opts;

        std::pmr::monotonic_buffer_resource m_buffer;
        std::pmr::polymorphic_allocator<> m_alloc;

        types::TypeContext m_types;
        ModuleGraph m_graph;
        Importer m_importer;
        SpecializationRegistry m_spec_registry;
        std::vector<sm::FileId> m_injected_sources;
        bool m_injected_sources_prepared{};
        std::unordered_set<ModuleInfo const*> m_injected_modules;
        std::unordered_set<ast::ImportDecl const*> m_injected_imports;
    };

} // namespace dcc::sema
