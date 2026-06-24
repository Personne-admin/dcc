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

            inject_decls(*root);

            load_transitively(m_importer, *root);

            collect_all(m_graph.all(), m_diag, m_types, m_alloc);
            resolve_usings(m_graph.all(), m_diag, m_alloc);
            resolve_signature_types(m_graph.all(), m_diag, m_types, m_alloc);
            validate_attributes(m_graph.all(), m_diag, m_alloc);
            validate_public_signatures(m_graph.all(), m_diag);
            analyze_bodies(m_graph.all(), m_diag, m_ast_ctx, m_types, m_alloc, m_spec_registry);

            complete_all_templated_tagged_enums(m_types, m_alloc);

            return root;
        }

    private:
        void inject_decls(ModuleInfo& root)
        {
            if (m_opts.injected_decls.empty() || !root.tu)
                return;

            std::vector<ast::Decl*> prepend;
            for (auto const& snippet : m_opts.injected_decls)
            {
                auto fid = m_sm.add_synthetic("<command-line>", snippet + "\n");
                auto* tu = m_importer.parse_source(fid);
                if (!tu)
                    continue;

                for (auto* imp : tu->imports)
                    root.tu->imports.push_back(imp);
                for (auto* d : tu->decls)
                    prepend.push_back(d);
            }

            root.tu->decls.insert(root.tu->decls.begin(), prepend.begin(), prepend.end());
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
    };

} // namespace dcc::sema
