export module dcc.session;

import std;
import dcc.sm;
import dcc.si;
import dcc.lex;
import dcc.parser;
import dcc.diag;
import dcc.ast;
import dcc.sema;
import dcc.target;

export namespace dcc::session
{
    struct SessionOptions
    {
        std::ostream* diagnostic_stream{&std::cerr};
        bool silent_diagnostics{false};
        std::size_t ast_arena_initial_size{64 * 1024};
    };

    struct CompileOptions
    {
        std::vector<std::filesystem::path> import_roots;
        bool inject_libdcext_prelude{false};
        std::size_t arena_initial_size{256 * 1024};
        dcc::target::TargetConfig target{dcc::target::TargetConfig::host_default()};
        std::vector<std::string> injected_decls;
    };

    struct CompileResult
    {
        sema::ModuleInfo* module{nullptr};
        bool has_errors{false};
    };

    class CompilerSession
    {
    public:
        explicit CompilerSession(SessionOptions opts = {})
            : m_interner{}, m_diag{m_sm, opts.diagnostic_stream ? *opts.diagnostic_stream : std::cerr},
              m_ast_ctx{std::make_unique<ast::AstContext>(opts.ast_arena_initial_size)}
        {
            if (opts.silent_diagnostics)
                m_diag.set_silent(true);
        }

        CompilerSession(CompilerSession const&) = delete;
        CompilerSession& operator=(CompilerSession const&) = delete;
        CompilerSession(CompilerSession&&) = delete;
        CompilerSession& operator=(CompilerSession&&) = delete;

        [[nodiscard]] sm::SourceManager& source_manager() noexcept { return m_sm; }
        [[nodiscard]] sm::SourceManager const& source_manager() const noexcept { return m_sm; }

        [[nodiscard]] diag::DiagnosticEngine& diagnostics() noexcept { return m_diag; }
        [[nodiscard]] diag::DiagnosticEngine const& diagnostics() const noexcept { return m_diag; }

        void clear_diagnostics() { m_diag.clear_diagnostics(); }

        [[nodiscard]] std::expected<sm::FileId, sm::Error> load_file(std::filesystem::path const& path) { return m_sm.load(path); }
        [[nodiscard]] std::expected<sm::FileId, sm::Error> load_uri(std::string_view uri) { return m_sm.load_uri(uri); }

        [[nodiscard]] sm::FileId open_in_memory(std::string uri, std::string content, std::optional<std::int64_t> version = std::nullopt)
        {
            return m_sm.open_in_memory(std::move(uri), std::move(content), version);
        }

        [[nodiscard]] std::expected<void, sm::Error> update_in_memory(std::string_view uri, std::string new_content,
                                                                      std::optional<std::int64_t> new_version = std::nullopt)
        {
            return m_sm.update_in_memory(uri, std::move(new_content), new_version);
        }

        [[nodiscard]] std::expected<void, sm::Error> close_in_memory(std::string_view uri) { return m_sm.close_in_memory(uri); }

        [[nodiscard]] ast::TranslationUnit* parse_file(sm::FileId fid) { return parse_file(fid, *m_ast_ctx, m_diag); }

        [[nodiscard]] ast::AstContext& ast_context() noexcept { return *m_ast_ctx; }
        [[nodiscard]] ast::AstContext const& ast_context() const noexcept { return *m_ast_ctx; }

        [[nodiscard]] si::string_interner& interner() noexcept { return m_interner; }
        [[nodiscard]] si::string_interner const& interner() const noexcept { return m_interner; }

        [[nodiscard]] sema::SemaContext* sema_context() noexcept { return m_sema.get(); }
        [[nodiscard]] sema::SemaContext const* sema_context() const noexcept { return m_sema.get(); }

        ast::TranslationUnit* parse_file(sm::FileId fid, ast::AstContext& ast, diag::DiagnosticEngine& d)
        {
            auto const* file = m_sm.get(fid);
            if (!file)
            {
                d.error("internal error: missing source file for id {}", static_cast<std::uint32_t>(fid));
                return nullptr;
            }

            lex::Lexer lexer{*file, m_interner};
            auto mode =
                (file->kind() == sm::FileKind::InMemory || file->kind() == sm::FileKind::Synthetic) ? parser::ParseMode::Interactive : parser::ParseMode::Batch;

            parser::Parser parser{lexer, ast, d, mode};
            auto* tu = parser.parse();

            if (m_prelude_enabled && tu && file->path() == m_entry_path)
            {
                auto alloc = ast.allocator();
                ast::Path prelude_path{alloc};
                prelude_path.segments.push_back({m_interner.intern("std"), sm::SourceRange{}});
                prelude_path.segments.push_back({m_interner.intern("prelude"), sm::SourceRange{}});
                auto* import_decl = ast.make<ast::ImportDecl>(sm::SourceRange{}, std::move(prelude_path), sm::SourceRange{}, alloc);
                tu->imports.push_back(import_decl);
            }

            return tu;
        }

        CompileResult analyze_entry(std::filesystem::path const& entry_path, CompileOptions const& opts)
        {
            m_entry_path = entry_path;
            m_prelude_enabled = opts.inject_libdcext_prelude;

            m_sema.reset();
            m_ast_ctx = std::make_unique<ast::AstContext>(opts.arena_initial_size);

            sema::SemaOptions sopts;
            sopts.arena_initial_size = opts.arena_initial_size;
            sopts.import_roots = opts.import_roots;
            sopts.interner = &m_interner;
            sopts.target = opts.target;
            sopts.injected_decls = opts.injected_decls;

            auto parse = [this](sm::FileId fid, ast::AstContext& ast, diag::DiagnosticEngine& d) -> ast::TranslationUnit* { return parse_file(fid, ast, d); };

            m_sema = std::make_unique<sema::SemaContext>(m_sm, m_diag, *m_ast_ctx, std::move(parse), std::move(sopts));

            auto* module = m_sema->analyze_entry(entry_path);

            return CompileResult{
                .module = module,
                .has_errors = m_diag.has_errors(),
            };
        }

    private:
        sm::SourceManager m_sm;
        si::string_interner m_interner;
        diag::DiagnosticEngine m_diag;
        std::unique_ptr<ast::AstContext> m_ast_ctx;
        std::unique_ptr<sema::SemaContext> m_sema;

        std::filesystem::path m_entry_path;
        bool m_prelude_enabled{false};
    };

} // namespace dcc::session
