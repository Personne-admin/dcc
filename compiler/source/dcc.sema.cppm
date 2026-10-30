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
import dcc.sema.type_helpers;

export namespace dcc::sema
{
    struct SemaOptions
    {
        std::vector<std::filesystem::path> import_roots;
        std::vector<std::pair<std::string, std::filesystem::path>> library_sources;
        std::size_t arena_initial_size{256 * 1024};
        dcc::si::string_interner* interner{nullptr};
        dcc::target::TargetConfig target{dcc::target::TargetConfig::host_default()};
        std::vector<std::string> injected_decls;
        bool enable_doc_comments{false};
        bool restricted_check{false};
        std::string default_allocator;
    };

    class SemaContext
    {
    public:
        SemaContext(sm::SourceManager& sm, diag::DiagnosticEngine& diag, ast::AstContext& ast_ctx, ParseFn parse, SemaOptions opts)
            : m_sm{sm}, m_diag{diag}, m_ast_ctx{ast_ctx}, m_opts{std::move(opts)}, m_buffer{m_opts.arena_initial_size}, m_alloc{&m_buffer},
              m_types{256 * 1024, &m_opts.target}, m_graph{},
              m_importer{m_graph, m_sm, diag, m_ast_ctx, std::move(parse), m_opts.interner, &m_opts.target, m_opts.default_allocator}
        {
            for (auto const& root : m_opts.import_roots)
                m_graph.add_root(root);
        }

        SemaContext(SemaContext const&) = delete;
        SemaContext& operator=(SemaContext const&) = delete;

        [[nodiscard]] ModuleGraph& graph() noexcept { return m_graph; }
        [[nodiscard]] dcc::target::TargetConfig const& target() const noexcept { return m_opts.target; }
        [[nodiscard]] types::TypeContext& types() noexcept { return m_types; }
        [[nodiscard]] SpecializationRegistry& spec_registry() noexcept { return m_spec_registry; }
        [[nodiscard]] std::pmr::polymorphic_allocator<> allocator() noexcept { return m_alloc; }

        ModuleInfo* analyze_entry(std::filesystem::path const& entry_file)
        {
            auto* root = m_importer.load_entry(entry_file);
            if (!root)
                return nullptr;

            std::vector<ModuleInfo*> candidates;
            for (auto const& [name, path] : m_opts.library_sources)
                if (auto* candidate = m_importer.load_entry(path))
                {
                    if (candidate->canonical_path.str() != name ||
                        std::filesystem::weakly_canonical(candidate->file_path) != std::filesystem::weakly_canonical(path))
                        m_diag.error("library source '{}' declares module '{}', expected '{}'", path.string(), candidate->canonical_path.str(), name);
                    candidates.push_back(candidate);
                }
            prepare_injected_sources();
            load_transitively(
                m_importer, *root, [this](ModuleInfo& module) { inject_decls(module); },
                [this](ast::ImportDecl const& decl) { return m_injected_imports.contains(&decl); });
            for (auto* candidate : candidates)
                load_transitively(
                    m_importer, *candidate, [this](ModuleInfo& module) { inject_decls(module); },
                    [this](ast::ImportDecl const& decl) { return m_injected_imports.contains(&decl); });

            std::vector<sm::SourceRange> parser_recovery_ranges;
            for (auto const& module : m_graph.all())
                if (module->tu)
                    parser_recovery_ranges.insert(parser_recovery_ranges.end(), module->tu->parser_recovery_ranges.begin(),
                                                  module->tu->parser_recovery_ranges.end());
            m_diag.set_parser_recovery_ranges(parser_recovery_ranges);

            if (!prepare_allocator_config())
                return root;

            collect_all(m_graph.all(), m_diag, m_types, m_alloc);
            resolve_usings(m_graph.all(), m_diag, m_alloc);
            if (!validate_allocator_selection())
                return root;
            m_diag.set_parser_recovery_suppression(true);
            resolve_signature_types(m_graph.all(), m_diag, m_types, m_alloc, m_ast_ctx, m_spec_registry);
            validate_attributes(m_graph.all(), m_diag, m_alloc);
            validate_public_signatures(m_graph.all(), m_diag);
            analyze_bodies(m_graph.all(), m_diag, m_ast_ctx, m_types, m_alloc, m_spec_registry, &m_opts.target, m_opts.restricted_check);
            m_diag.set_parser_recovery_suppression(false);

            complete_all_templated_tagged_enums(m_types, m_alloc);
            complete_all_templated_structs(m_types, m_alloc);
            check_allocator_layout();

            if (!m_diag.has_errors() && std::ranges::any_of(m_graph.all(), [](auto const& mod) { return mod->needs_i8086_arithmetic; }))
                analyze_i8086_arithmetic(*root);

            return root;
        }

        std::vector<ModuleInfo*> analyze_resolve_only_files(std::span<std::filesystem::path const> entry_files)
        {
            std::vector<ModuleInfo*> entries;
            entries.reserve(entry_files.size());
            for (auto const& entry_file : entry_files)
            {
                auto* root = m_importer.load_entry(entry_file);
                if (!root)
                    continue;

                entries.push_back(root);
            }

            prepare_injected_sources();
            for (auto* root : entries)
            {
                load_transitively(
                    m_importer, *root, [this](ModuleInfo& module) { inject_decls(module); },
                    [this](ast::ImportDecl const& decl) { return m_injected_imports.contains(&decl); });
            }

            std::vector<sm::SourceRange> parser_recovery_ranges;
            for (auto const& module : m_graph.all())
                if (module->tu)
                    parser_recovery_ranges.insert(parser_recovery_ranges.end(), module->tu->parser_recovery_ranges.begin(),
                                                  module->tu->parser_recovery_ranges.end());
            m_diag.set_parser_recovery_ranges(parser_recovery_ranges);

            collect_all(m_graph.all(), m_diag, m_types, m_alloc);
            resolve_usings(m_graph.all(), m_diag, m_alloc);
            m_diag.set_parser_recovery_suppression(true);
            resolve_signature_types(m_graph.all(), m_diag, m_types, m_alloc, m_ast_ctx, m_spec_registry);
            validate_attributes(m_graph.all(), m_diag, m_alloc);
            validate_public_signatures(m_graph.all(), m_diag);
            m_diag.set_parser_recovery_suppression(false);

            return entries;
        }

    private:
        void analyze_i8086_arithmetic(ModuleInfo const& root)
        {
            auto const first = m_graph.all().size();
            ast::Path path{m_ast_ctx.allocator()};
            for (auto name : {"core", "i8086_arith"})
                path.segments.push_back({m_opts.interner ? m_opts.interner->intern(name) : std::string_view{name}, sm::SourceRange{}});
            auto* helper = m_importer.resolve_import(path, root);
            if (!helper || helper->state == ModuleState::BodiesAnalyzed)
                return;

            load_transitively(m_importer, *helper);
            auto modules = std::span{m_graph.all()}.subspan(first);
            collect_all(modules, m_diag, m_types, m_alloc);
            resolve_usings(modules, m_diag, m_alloc);
            resolve_signature_types(modules, m_diag, m_types, m_alloc, m_ast_ctx, m_spec_registry);
            validate_attributes(modules, m_diag, m_alloc);
            validate_public_signatures(modules, m_diag);
            analyze_bodies(modules, m_diag, m_ast_ctx, m_types, m_alloc, m_spec_registry, &m_opts.target, m_opts.restricted_check);
        }

        struct AllocatorSpelling
        {
            std::string_view module_path;
            std::string_view type_name;
        };

        [[nodiscard]] static AllocatorSpelling split_allocator_spelling(std::string_view spelling)
        {
            auto separator = spelling.rfind("::");
            if (separator == std::string_view::npos)
                return {};
            return {spelling.substr(0, separator), spelling.substr(separator + 2)};
        }

        [[nodiscard]] ModuleInfo* module_with_path(std::string_view path) const
        {
            for (auto const& module : m_graph.all())
                if (module->canonical_path.str() == path)
                    return module.get();
            return nullptr;
        }

        [[nodiscard]] ModuleInfo* allocator_config_module() const { return module_with_path("core::allocator_config"); }

        void record_unselected_allocator_decl(ModuleInfo& config)
        {
            if (!config.tu)
                return;

            for (auto* decl : config.tu->decls)
                if (auto* struct_decl = ast::node_cast<ast::StructDecl>(decl); struct_decl && struct_decl->name == "NoDefaultAllocatorSelected")
                {
                    m_types.unselected_default_allocator_decl = static_cast<void const*>(struct_decl);
                    return;
                }
        }

        [[nodiscard]] bool report_allocator_cycle(ModuleInfo const& start, ModuleInfo const& target)
        {
            std::vector<ModuleInfo const*> queue{&start};
            std::unordered_map<ModuleInfo const*, ModuleInfo const*> parent;
            parent.emplace(&start, nullptr);
            bool found = false;
            for (std::size_t head = 0; head < queue.size() && !found; ++head)
            {
                auto const* current = queue[head];
                for (auto const& import : current->imports)
                {
                    auto const* next = import.target;
                    if (!next || parent.contains(next))
                        continue;
                    parent.emplace(next, current);
                    if (next == &target)
                    {
                        found = true;
                        break;
                    }
                    queue.push_back(next);
                }
            }

            if (!found)
                return false;

            std::vector<ModuleInfo const*> chain;
            for (auto const* node = &target; node; node = parent[node])
                chain.push_back(node);
            std::ranges::reverse(chain);

            std::string message = "default allocator configuration cycle: `core::allocator_config` imports `";
            message += start.canonical_path.str();
            message += "`";
            for (std::size_t i = 1; i + 1 < chain.size(); ++i)
                message += std::format(", which imports `{}`", chain[i]->canonical_path.str());
            message += ", which imports `core::allocator_config`";
            m_diag.error("{}", message);
            return true;
        }

        [[nodiscard]] bool prepare_allocator_config()
        {
            auto* config = allocator_config_module();
            if (!config)
                return true;

            record_unselected_allocator_decl(*config);

            if (m_opts.default_allocator.empty())
                return true;

            auto spelling = split_allocator_spelling(m_opts.default_allocator);
            if (spelling.module_path == "core::allocator_config")
            {
                m_diag.error("default allocator `{}` must not be declared in `core::allocator_config`", m_opts.default_allocator);
                return false;
            }

            auto* selected_module = module_with_path(spelling.module_path);
            if (!selected_module)
                return false;

            return !report_allocator_cycle(*selected_module, *config);
        }

        [[nodiscard]] Symbol const* find_scope_type(Scope const& scope, std::string_view name) const
        {
            if (m_opts.interner)
                if (auto const* binding = scope.find_binding_local(m_opts.interner->intern(name)); binding && binding->has_type)
                    return &binding->type_sym;

            for (auto const& [binding_name, binding] : scope.bindings())
                if (binding_name == name && binding.has_type)
                    return &binding.type_sym;

            return nullptr;
        }

        [[nodiscard]] bool validate_allocator_selection()
        {
            if (m_opts.default_allocator.empty() || !allocator_config_module())
                return true;

            auto spelling = split_allocator_spelling(m_opts.default_allocator);
            auto* selected_module = module_with_path(spelling.module_path);
            if (!selected_module)
                return true;

            Symbol const* selected_symbol = nullptr;
            bool is_public = false;
            if (selected_module->export_scope)
                if (auto const* symbol = find_scope_type(*selected_module->export_scope, spelling.type_name))
                {
                    selected_symbol = symbol;
                    is_public = true;
                }
            if (!selected_symbol && selected_module->own_scope)
                selected_symbol = find_scope_type(*selected_module->own_scope, spelling.type_name);

            if (!selected_symbol)
            {
                m_diag.error("module `{}` has no type `{}`; -fdefault-allocator requires a public type", spelling.module_path, spelling.type_name);
                return false;
            }

            if (!is_public)
            {
                m_diag.error("type `{}` is not public in module `{}`; -fdefault-allocator requires a public type", spelling.type_name,
                             spelling.module_path);
                return false;
            }

            auto const* selected_decl = selected_symbol->decl;
            std::size_t template_params = 0;
            switch (selected_decl->kind)
            {
                case ast::DeclKind::Struct:
                    template_params = static_cast<ast::StructDecl const*>(selected_decl)->template_params.size();
                    break;
                case ast::DeclKind::Enum:
                    template_params = static_cast<ast::EnumDecl const*>(selected_decl)->template_params.size();
                    break;
                case ast::DeclKind::Using:
                    template_params = static_cast<ast::UsingDecl const*>(selected_decl)->template_params.size();
                    break;
                default:
                    break;
            }

            if (template_params != 0)
            {
                m_diag.error("type `{}::{}` is generic; -fdefault-allocator requires a concrete type", spelling.module_path, spelling.type_name);
                return false;
            }

            return true;
        }

        void check_allocator_layout()
        {
            if (m_opts.default_allocator.empty())
                return;

            auto* config = allocator_config_module();
            if (!config || !config->tu)
                return;

            ast::UsingDecl const* alias = nullptr;
            for (auto* decl : config->tu->decls)
                if (auto* using_decl = ast::node_cast<ast::UsingDecl>(decl);
                    using_decl && using_decl->using_kind == ast::UsingKind::Alias && using_decl->alias_path.segments.size() == 1 &&
                    using_decl->alias_path.segments.front().name == "DefaultAllocator")
                {
                    alias = using_decl;
                    break;
                }

            if (!alias || !alias->target_type)
                return;

            auto* selected = get_canonical(alias->target_type->sema);
            if (!selected || selected->kind == types::TypeKind::Error)
                return;

            if (!selected->is_complete)
            {
                m_diag.error("default allocator `{}` is not a complete type", m_opts.default_allocator);
                return;
            }

            if (selected->byte_size != 0)
                m_diag.error("default allocator `{}` is not zero-size: it occupies {} bytes", m_opts.default_allocator, selected->byte_size);
        }

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
