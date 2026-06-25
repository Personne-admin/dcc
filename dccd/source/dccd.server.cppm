export module dccd.server;

import std;
import dcc.sm;
import dcc.diag;
import dcc.session;
import dccd.protocol;
import dccd.semantic_tokens;
import dccd.completion;
import dccd.inlay_hints;
import dccd.workspace_symbols;
import dccd.format;
import dcc.query;
import dcc.ast;
import dcc.sema;
import dcc.sema.type_helpers;
import dcc.vfs;

export namespace dccd
{
    class LanguageServer
    {
    public:
        LanguageServer()
        {
            dcc::session::SessionOptions sopts;
            sopts.silent_diagnostics = true;
            sopts.diagnostic_stream = &m_log;
            m_session.emplace(sopts);
        }

        LanguageServer(LanguageServer const&) = delete;
        LanguageServer& operator=(LanguageServer const&) = delete;
        LanguageServer(LanguageServer&&) = delete;
        LanguageServer& operator=(LanguageServer&&) = delete;

        [[nodiscard]] std::optional<protocol::JsonValue> handle_message(protocol::RpcInfo const& rpc)
        {
            if (rpc.is_request())
            {
                std::string method = rpc.method.value();

                if (method == "initialize")
                    return handle_initialize(rpc);
                if (method == "initialized")
                    return handle_initialized(rpc);
                if (method == "shutdown")
                    return handle_shutdown(rpc);
                if (method == "textDocument/didOpen")
                {
                    handle_did_open(rpc);
                    return std::nullopt;
                }
                if (method == "textDocument/didChange")
                {
                    handle_did_change(rpc);
                    return std::nullopt;
                }
                if (method == "textDocument/didClose")
                {
                    handle_did_close(rpc);
                    return std::nullopt;
                }
                if (method == "textDocument/definition")
                    return handle_definition(rpc);
                if (method == "textDocument/hover")
                    return handle_hover(rpc);
                if (method == "textDocument/semanticTokens/full")
                    return handle_semantic_tokens_full(rpc);
                if (method == "textDocument/completion")
                    return handle_completion(rpc);
                if (method == "textDocument/signatureHelp")
                    return handle_signature_help(rpc);
                if (method == "textDocument/references")
                    return handle_references(rpc);
                if (method == "textDocument/documentHighlight")
                    return handle_document_highlight(rpc);
                if (method == "textDocument/rename")
                    return handle_rename(rpc);
                if (method == "textDocument/codeAction")
                    return handle_code_action(rpc);
                if (method == "textDocument/inlayHint")
                    return handle_inlay_hint(rpc);
                if (method == "textDocument/formatting")
                    return handle_formatting(rpc);
                if (method == "workspace/symbol")
                    return handle_workspace_symbol(rpc);
                if (method == "dccd/virtualDocument")
                    return handle_virtual_document(rpc);

                return protocol::build_error_response(rpc.id.value(), -32601, std::format("Method not found: {}", method));
            }

            if (rpc.is_notification())
            {
                std::string method = rpc.method.value();

                if (method == "initialized")
                {
                    std::ignore = handle_initialized(rpc);
                    return std::nullopt;
                }
                if (method == "exit")
                {
                    m_should_exit = true;
                    return std::nullopt;
                }
                if (method == "textDocument/didOpen")
                {
                    handle_did_open(rpc);
                    return std::nullopt;
                }
                if (method == "textDocument/didChange")
                {
                    handle_did_change(rpc);
                    return std::nullopt;
                }
                if (method == "textDocument/didClose")
                {
                    handle_did_close(rpc);
                    return std::nullopt;
                }
                if (method == "workspace/didChangeConfiguration")
                {
                    handle_workspace_did_change_configuration(rpc);
                    return std::nullopt;
                }
                if (method == "workspace/didChangeWatchedFiles")
                {
                    handle_workspace_did_change_watched_files(rpc);
                    return std::nullopt;
                }

                return std::nullopt;
            }

            return std::nullopt;
        }

        [[nodiscard]] bool should_exit() const noexcept { return m_should_exit; }

    private:
        std::optional<dcc::session::CompilerSession> m_session;
        std::ostream& m_log{std::cerr};
        bool m_should_exit{false};
        std::vector<std::filesystem::path> m_workspace_roots;
        std::vector<std::filesystem::path> m_lsp_include_paths;
        std::vector<std::filesystem::path> m_project_include_paths;
        std::vector<std::filesystem::path> m_global_include_paths;

        struct CachedDiagnostic
        {
            protocol::LspDiagnostic lsp_diag;
            dcc::diag::Diagnostic compiler_diag;
        };

        std::map<std::string, std::vector<CachedDiagnostic>, std::less<>> m_diagnostic_cache;
        std::unordered_set<std::string> m_stale_uris;

        [[nodiscard]] std::optional<protocol::JsonValue> handle_initialize(protocol::RpcInfo const& rpc)
        {
            m_workspace_roots.clear();

            if (rpc.params.has_value())
            {
                auto init_params = protocol::InitializeParams::from_json(rpc.params.value());

                if (init_params.workspaceFolders.has_value())
                {
                    for (auto const& wf : *init_params.workspaceFolders)
                    {
                        auto path = dcc::sm::SourceManager::parse_file_uri(wf.uri);
                        if (path)
                            m_workspace_roots.push_back(std::move(*path));
                    }
                }
                else if (init_params.rootUri.has_value())
                {
                    auto path = dcc::sm::SourceManager::parse_file_uri(*init_params.rootUri);
                    if (path)
                        m_workspace_roots.push_back(std::move(*path));
                }
            }

            if (!m_workspace_roots.empty())
            {
                std::ranges::sort(m_workspace_roots);
                auto [first, last] = std::ranges::unique(m_workspace_roots);
                m_workspace_roots.erase(first, last);
            }

            std::println(m_log, "[dccd] initialize: {} workspace root(s)", m_workspace_roots.size());

            read_global_config();
            read_project_configs();

            if (rpc.params.has_value())
            {
                if (auto const* init_opts = rpc.params.value().find_member("initializationOptions"))
                {
                    protocol::DidChangeConfigurationParams cfg;
                    cfg.settings = *init_opts;
                    parse_lsp_include_paths(cfg);
                }
            }

            return protocol::build_response(rpc.id.value(), protocol::make_initialize_result());
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_initialized(protocol::RpcInfo const&) { return std::nullopt; }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_shutdown(protocol::RpcInfo const& rpc)
        {
            return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
        }

        void handle_did_open(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DidOpenTextDocumentParams::from_json(rpc.params.value());

            if (dcc::vfs::is_dcc_core_uri(params.textDocument.uri))
            {
                std::println(m_log, "[dccd] didOpen: rejecting read-only dcc-core: document {}", params.textDocument.uri);
                return;
            }

            auto fid = m_session->open_in_memory(params.textDocument.uri, params.textDocument.text, params.textDocument.version);

            clear_stale_marker(params.textDocument.uri);

            auto const* sf = m_session->source_manager().get(fid);
            if (sf)
                std::println(m_log, "[dccd] didOpen: uri={} fid={} path=\"{}\" kind={}", params.textDocument.uri, static_cast<std::uint32_t>(fid),
                             sf->path().string(), static_cast<int>(sf->kind()));
            else
                std::println(m_log, "[dccd] didOpen: uri={} fid={} (file lookup failed)", params.textDocument.uri, static_cast<std::uint32_t>(fid));

            recompile_document(params.textDocument.uri);
            publish_diagnostics(params.textDocument.uri);
        }

        void handle_did_change(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DidChangeTextDocumentParams::from_json(rpc.params.value());

            if (dcc::vfs::is_dcc_core_uri(params.textDocument.uri))
            {
                std::println(m_log, "[dccd] didChange: rejecting read-only dcc-core: document {}", params.textDocument.uri);
                return;
            }

            if (params.contentChanges.empty())
            {
                std::println(m_log, "[dccd] didChange with no contentChanges for {}", params.textDocument.uri);
                return;
            }

            auto const& last_change = params.contentChanges.back();

            auto result = m_session->update_in_memory(params.textDocument.uri, last_change.text, params.textDocument.version);
            if (!result)
            {
                std::println(m_log, "[dccd] update_in_memory failed for {}: {}", params.textDocument.uri, dcc::sm::to_string(result.error()));
                m_stale_uris.insert(params.textDocument.uri);
                m_diagnostic_cache.erase(params.textDocument.uri);
                return;
            }

            clear_stale_marker(params.textDocument.uri);

            auto fid = m_session->source_manager().find_by_uri(params.textDocument.uri);
            if (fid)
            {
                auto const* sf = m_session->source_manager().get(*fid);
                if (sf)
                    std::println(m_log, "[dccd] didChange: uri={} fid={} path=\"{}\" kind={} version={}", params.textDocument.uri,
                                 static_cast<std::uint32_t>(*fid), sf->path().string(), static_cast<int>(sf->kind()), params.textDocument.version);
            }

            recompile_document(params.textDocument.uri);
            publish_diagnostics(params.textDocument.uri);
        }

        void handle_did_close(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DidCloseTextDocumentParams::from_json(rpc.params.value());

            auto result = m_session->close_in_memory(params.uri);
            if (!result)
                std::println(m_log, "[dccd] close_in_memory failed for {}: {}", params.uri, dcc::sm::to_string(result.error()));

            m_diagnostic_cache.erase(params.uri);

            protocol::PublishDiagnosticsParams empty_params;
            empty_params.uri = params.uri;
            publish_lsp_diagnostics(empty_params);
        }

        [[nodiscard]] static dcc::sm::Position protocol_position_to_sm_position(protocol::LspPosition const& pos) noexcept
        {
            return dcc::sm::Position{pos.line, pos.character};
        }

        [[nodiscard]] std::optional<dcc::sm::FileId> file_id_from_uri(std::string const& uri)
        {
            auto fid = m_session->source_manager().find_by_uri(uri);
            if (fid)
                return *fid;

            if (dcc::vfs::is_dcc_core_uri(uri))
            {
                auto const* entry = dcc::vfs::lookup_by_uri(uri);
                if (entry)
                {
                    auto materialized = dcc::vfs::materialize(*entry, m_session->source_manager());
                    return materialized;
                }

                std::println(m_log, "[dccd] file_id_from_uri: dcc-core: URI has no matching entry: {}", uri);
                return std::nullopt;
            }

            if (uri.starts_with("file://"))
            {
                auto result = m_session->load_uri(uri);
                if (result)
                    return *result;
            }

            std::println(m_log, "[dccd] file_id_from_uri: cannot find or load {}", uri);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<dcc::query::NodeAtLocation> query_at_params(std::string const& uri, dcc::sm::Position sm_pos)
        {
            auto fid_opt = file_id_from_uri(uri);
            if (!fid_opt)
            {
                std::println(m_log, "[dccd] query_at_params: no file for uri {}", uri);
                return std::nullopt;
            }

            if (!dcc::query::file_in_module_graph(*m_session, *fid_opt) && !dcc::vfs::is_dcc_core_uri(uri))
            {
                std::println(m_log, "[dccd] query_at_params: {} not in module graph; recompiling as entry", uri);
                recompile_document(uri);

                fid_opt = file_id_from_uri(uri);
                if (!fid_opt)
                {
                    std::println(m_log, "[dccd] query_at_params: no file for uri {} after recompile", uri);
                    return std::nullopt;
                }
            }

            auto node = dcc::query::find_node_at(*m_session, *fid_opt, sm_pos);
            if (!node)
            {
                std::println(m_log, "[dccd] query_at_params: find_node_at returned nullopt for {}", uri);
                return std::nullopt;
            }

            if (!node->has_ast_node())
            {
                std::println(m_log, "[dccd] query_at_params: no AST node at position in {}", uri);
                return std::nullopt;
            }

            return node;
        }

        [[nodiscard]] std::optional<protocol::LspLocation> source_range_to_lsp_location(dcc::sm::SourceRange const& range)
        {
            auto& sm = m_session->source_manager();
            auto const* file = sm.get(range.begin.fileId);
            if (!file)
                return std::nullopt;

            auto start_pos = sm.location_to_lsp_position(range.begin);
            auto end_pos = sm.location_to_lsp_position(range.end);
            if (!start_pos || !end_pos)
                return std::nullopt;

            protocol::LspLocation loc;
            loc.uri = file->uri();
            loc.range.start.line = start_pos->line;
            loc.range.start.character = start_pos->character;
            loc.range.end.line = end_pos->line;
            loc.range.end.character = end_pos->character;
            return loc;
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_definition(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DefinitionParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);
            auto node = query_at_params(params.textDocument.uri, sm_pos);
            if (!node)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (node->resolved_field)
            {
                auto loc = source_range_to_lsp_location(node->resolved_field->range);
                if (loc)
                    return protocol::build_response(rpc.id.value(), loc->to_json());

                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            if (node->resolved_definition_range.valid())
            {
                auto loc = source_range_to_lsp_location(node->resolved_definition_range);
                if (loc)
                    return protocol::build_response(rpc.id.value(), loc->to_json());

                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            dcc::ast::Decl const* target = nullptr;
            if (node->resolved_specialization)
                target = node->resolved_specialization;
            else if (node->ufcs_callee)
                target = node->ufcs_callee;
            else if (node->resolved_decl)
                target = node->resolved_decl;
            else if (node->hovered_decl)
                target = node->hovered_decl;

            if (!target)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto target_range = dcc::query::decl_name_range(target);
            if (!target_range.valid())
                target_range = target->range;

            if (!target_range.valid())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto loc = source_range_to_lsp_location(target_range);
            if (!loc)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (node->resolved_via_using)
            {
                auto using_range = dcc::query::decl_name_range(node->resolved_via_using);
                if (!using_range.valid())
                    using_range = node->resolved_via_using->range;

                auto using_loc = using_range.valid() ? source_range_to_lsp_location(using_range) : std::nullopt;
                if (using_loc && !(using_loc->uri == loc->uri && using_loc->range.start.line == loc->range.start.line &&
                                   using_loc->range.start.character == loc->range.start.character))
                {
                    auto arr = protocol::JsonValue::empty_array();
                    arr.push_back(using_loc->to_json());
                    arr.push_back(loc->to_json());
                    return protocol::build_response(rpc.id.value(), std::move(arr));
                }
            }

            return protocol::build_response(rpc.id.value(), loc->to_json());
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_hover(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::HoverParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);
            std::println(m_log, "[dccd] hover: uri={} line={} char={}", params.textDocument.uri, sm_pos.line, sm_pos.character);
            auto node = query_at_params(params.textDocument.uri, sm_pos);
            if (!node)
            {
                std::println(m_log, "[dccd] hover: query_at_params returned null");
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            std::string markdown;

            if (node->resolved_field)
            {
                auto const* f = node->resolved_field;
                std::string type_str = "<unknown>";
                if (f->type && f->type->sema.canonical)
                    type_str = format_dcc_type(dcc::sema::get_canonical(f->type->sema));

                markdown = std::format("```dc\n{} {}\n```", type_str, f->name);
            }
            else if (node->resolved_param)
            {
                auto const& p = *node->resolved_param;
                std::string type_str;
                if (node->resolved_type)
                    type_str = format_dcc_type(node->resolved_type);
                else if (p.type && p.type->sema.canonical)
                    type_str = format_dcc_type(dcc::sema::get_canonical(p.type->sema));
                else
                    type_str = "<template-dependent>";

                if (p.name.empty())
                    markdown = std::format("```dc\n{}\n```", type_str);
                else
                    markdown = std::format("```dc\n{} {}\n```", type_str, p.name);
            }
            else
            {
                dcc::ast::Decl const* target = nullptr;
                if (node->resolved_specialization)
                    target = node->resolved_specialization;
                else if (node->ufcs_callee)
                    target = node->ufcs_callee;
                else if (node->resolved_decl)
                    target = node->resolved_decl;
                else if (node->hovered_decl)
                    target = node->hovered_decl;

                if (!target && node->resolved_type)
                    markdown = std::format("```dc\n{}\n```", format_dcc_type(node->resolved_type));
                else if (target)
                {
                    switch (target->kind)
                    {
                        case dcc::ast::DeclKind::Func: {
                            auto const* fd = static_cast<dcc::ast::FuncDecl const*>(target);
                            std::string ret_str = "void";
                            if (fd->return_type && fd->return_type->sema.canonical)
                                ret_str = format_dcc_type(dcc::sema::get_canonical(fd->return_type->sema));
                            std::string sig = std::format("{} {}(", ret_str, fd->name);
                            for (std::size_t i = 0; i < fd->params.size(); ++i)
                            {
                                if (i > 0)
                                    sig += ", ";
                                if (fd->params[i].type && fd->params[i].type->sema.canonical)
                                {
                                    auto ty = dcc::sema::get_canonical(fd->params[i].type->sema);
                                    sig += std::format("{} {}", format_dcc_type(ty), fd->params[i].name);
                                }
                                else
                                {
                                    sig += fd->params[i].name;
                                }
                            }
                            sig += ")";
                            markdown = std::format("```dc\n{}\n```", sig);
                            break;
                        }
                        case dcc::ast::DeclKind::Var: {
                            auto const* vd = static_cast<dcc::ast::VarDecl const*>(target);
                            std::string type_str;

                            if (node->resolved_type)
                                type_str = format_dcc_type(node->resolved_type);
                            else if (vd->type && vd->type->sema.canonical)
                                type_str = format_dcc_type(dcc::sema::get_canonical(vd->type->sema));
                            if (type_str.empty())
                                markdown = std::format("```dc\n{}\n```", vd->name);
                            else
                                markdown = std::format("```dc\n{} {}\n```", type_str, vd->name);
                            break;
                        }
                        case dcc::ast::DeclKind::Using: {
                            auto const* ud = static_cast<dcc::ast::UsingDecl const*>(target);
                            std::string name_str;
                            if (!ud->alias_path.is_empty())
                                name_str = std::string{ud->alias_path.tail_name()};
                            else
                                name_str = "<unnamed>";

                            std::string target_str;
                            if (ud->target_type && ud->target_type->sema.canonical)
                                target_str = format_dcc_type(dcc::sema::get_canonical(ud->target_type->sema));
                            else if (!ud->target_path.is_empty())
                            {
                                for (std::size_t i = 0; i < ud->target_path.segments.size(); ++i)
                                {
                                    if (i > 0)
                                        target_str += "::";
                                    target_str += ud->target_path.segments[i].name;
                                }
                            }
                            else if (ud->target_expr)
                                target_str = "<expr>";

                            if (target_str.empty())
                                target_str = "<unknown>";

                            markdown = std::format("```dc\nusing {} = {}\n```", name_str, target_str);
                            break;
                        }
                        default: {
                            std::string_view kind_str;
                            std::string_view name;
                            switch (target->kind)
                            {
                                case dcc::ast::DeclKind::Struct:
                                    kind_str = "struct";
                                    name = static_cast<dcc::ast::StructDecl const*>(target)->name;
                                    break;
                                case dcc::ast::DeclKind::Union:
                                    kind_str = "union";
                                    name = static_cast<dcc::ast::UnionDecl const*>(target)->name;
                                    break;
                                case dcc::ast::DeclKind::Enum:
                                    kind_str = "enum";
                                    name = static_cast<dcc::ast::EnumDecl const*>(target)->name;
                                    break;
                                case dcc::ast::DeclKind::Module:
                                    kind_str = "module";
                                    break;
                                case dcc::ast::DeclKind::Import:
                                    kind_str = "import";
                                    break;
                                default:
                                    kind_str = "decl";
                                    break;
                            }
                            if (name.empty())
                                markdown = std::format("```dc\n{}\n```", kind_str);
                            else
                                markdown = std::format("```dc\n{} {}\n```", kind_str, name);
                            break;
                        }
                    }
                }
            }

            if (markdown.empty())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            protocol::Hover hover;
            hover.contents.kind = "markdown";
            hover.contents.value = std::move(markdown);
            return protocol::build_response(rpc.id.value(), hover.to_json());
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_semantic_tokens_full(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::SemanticTokensParams::from_json(rpc.params.value());

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
            {
                std::println(m_log, "[dccd] semanticTokens/full: cannot find file for {}", params.textDocument.uri);
                protocol::SemanticTokens empty;
                return protocol::build_response(rpc.id.value(), empty.to_json());
            }

            auto const requested_fid = *fid_opt;
            dcc::ast::TranslationUnit const* tu = nullptr;

            if (m_stale_uris.contains(params.textDocument.uri))
            {
                std::println(m_log, "[dccd] semanticTokens/full: URI marked stale (previous update failed) for {}; returning empty", params.textDocument.uri);
                protocol::SemanticTokens empty;
                return protocol::build_response(rpc.id.value(), empty.to_json());
            }

            {
                auto* sema_ctx = m_session->sema_context();
                if (sema_ctx)
                {
                    auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();

                    dcc::sema::ModuleInfo const* module = nullptr;
                    for (auto const& mod : graph.all())
                    {
                        if (mod->file_id == requested_fid)
                        {
                            module = mod.get();
                            break;
                        }
                    }

                    if (module && module->tu)
                        tu = module->tu;

                    if (!tu)
                        tu = graph.find_tu_for_file(requested_fid);
                }
            }

            if (!tu)
            {
                std::println(m_log, "[dccd] semanticTokens/full: no resolved TU via sema for {}", params.textDocument.uri);
                tu = m_session->parse_file(requested_fid);
            }

            if (!tu)
            {
                std::println(m_log, "[dccd] semanticTokens/full: no parseable TU for {}", params.textDocument.uri);
                protocol::SemanticTokens empty;
                return protocol::build_response(rpc.id.value(), empty.to_json());
            }

            auto data = dccd::semantic_tokens::collect_tokens(m_session->source_manager(), tu, requested_fid);

            protocol::SemanticTokens result;
            result.data = std::move(data);
            return protocol::build_response(rpc.id.value(), result.to_json());
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_completion(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::CompletionParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);

            auto completion_list = dccd::completion::compute_completions(*m_session, params.textDocument.uri, sm_pos);
            return protocol::build_response(rpc.id.value(), completion_list.to_json());
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_signature_help(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::SignatureHelpParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);
            auto node = query_at_params(params.textDocument.uri, sm_pos);
            if (!node)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (!node->enclosing_call)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto const* call = node->enclosing_call;

            dcc::ast::FuncDecl const* target = nullptr;
            if (call->sema.ufcs_callee && call->sema.ufcs_callee->kind == dcc::ast::DeclKind::Func)
                target = static_cast<dcc::ast::FuncDecl const*>(call->sema.ufcs_callee);
            else if (call->sema.resolved_specialization)
                target = call->sema.resolved_specialization;
            else if (call->sema.resolved_decl && call->sema.resolved_decl->kind == dcc::ast::DeclKind::Func)
                target = static_cast<dcc::ast::FuncDecl const*>(call->sema.resolved_decl);

            if (!target && call->callee)
            {
                if (call->callee->sema.resolved_specialization)
                    target = call->callee->sema.resolved_specialization;
                else if (call->callee->sema.resolved_decl && call->callee->sema.resolved_decl->kind == dcc::ast::DeclKind::Func)
                    target = static_cast<dcc::ast::FuncDecl const*>(call->callee->sema.resolved_decl);
                else if (call->callee->sema.ufcs_callee && call->callee->sema.ufcs_callee->kind == dcc::ast::DeclKind::Func)
                    target = static_cast<dcc::ast::FuncDecl const*>(call->callee->sema.ufcs_callee);
            }

            if (!target && call->callee && node->scope)
            {
                auto const* callee_expr = call->callee;
                while (callee_expr && callee_expr->kind == dcc::ast::ExprKind::TemplateInst)
                    callee_expr = static_cast<dcc::ast::TemplateInstExpr const*>(callee_expr)->callee;

                if (callee_expr)
                {
                    std::span<dcc::sema::Symbol const> syms;
                    if (callee_expr->kind == dcc::ast::ExprKind::Ident)
                    {
                        auto const* id = static_cast<dcc::ast::IdentExpr const*>(callee_expr);
                        syms = node->scope->lookup_values(id->name);
                    }
                    else if (callee_expr->kind == dcc::ast::ExprKind::PathExpr)
                    {
                        auto const* path_expr = static_cast<dcc::ast::PathExpr const*>(callee_expr);
                        syms = dcc::sema::resolve_value_overloads(*node->scope, path_expr->path);
                    }

                    if (!syms.empty())
                    {
                        dcc::ast::FuncDecl const* best_fd = nullptr;
                        std::size_t arg_count = call->args.size();
                        std::size_t best_excess = std::numeric_limits<std::size_t>::max();

                        for (auto const& sym : syms)
                        {
                            if (!sym.decl || sym.decl->kind != dcc::ast::DeclKind::Func)
                                continue;

                            auto const* fd = static_cast<dcc::ast::FuncDecl const*>(sym.decl);
                            if (fd->params.size() >= arg_count)
                            {
                                std::size_t excess = fd->params.size() - arg_count;
                                if (!best_fd || excess < best_excess)
                                {
                                    best_fd = fd;
                                    best_excess = excess;
                                }
                            }
                            else if (!best_fd)
                                best_fd = fd;
                        }

                        if (best_fd)
                            target = best_fd;
                    }
                }
            }

            if (!target)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            std::string ret_str = "void";
            if (target->return_type && target->return_type->sema.canonical)
                ret_str = format_dcc_type(dcc::sema::get_canonical(target->return_type->sema));

            protocol::SignatureInformation sig_info;
            sig_info.label = std::format("{} {}(", ret_str, target->name);

            for (std::size_t i = 0; i < target->params.size(); ++i)
            {
                protocol::ParameterInformation param;
                auto const& fp = target->params[i];
                if (fp.type && fp.type->sema.canonical)
                {
                    auto ty = dcc::sema::get_canonical(fp.type->sema);
                    std::string type_str = format_dcc_type(ty);
                    if (fp.name.empty())
                        param.label = type_str;
                    else
                        param.label = std::format("{} {}", type_str, fp.name);
                }
                else
                {
                    if (fp.name.empty())
                        param.label = "<unknown>";
                    else
                        param.label = fp.name;
                }

                if (i > 0)
                    sig_info.label += ", ";
                sig_info.label += param.label;

                sig_info.parameters.push_back(std::move(param));
            }
            sig_info.label += ")";

            std::uint32_t active_param = node->active_argument_index;
            if (active_param > target->params.size())
                active_param = static_cast<std::uint32_t>(target->params.size());
            sig_info.activeParameter = active_param;

            protocol::SignatureHelp help;
            help.signatures.push_back(std::move(sig_info));
            help.activeSignature = 0;
            help.activeParameter = active_param;

            return protocol::build_response(rpc.id.value(), help.to_json());
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_references(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::ReferenceParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);
            auto node = query_at_params(params.textDocument.uri, sm_pos);
            if (!node)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            dcc::ast::Decl const* target = nullptr;
            if (node->ufcs_callee)
                target = node->ufcs_callee;
            else if (node->resolved_field)
                ;
            else if (node->resolved_decl)
                target = node->resolved_decl;
            else if (node->hovered_decl)
                target = node->hovered_decl;

            std::vector<dcc::sm::SourceRange> field_ranges;
            if (node->resolved_field && !target)
            {
                auto const* field_decl = node->resolved_field;
                auto const* parent_decl = node->resolved_field_parent;
                if (parent_decl)
                {
                    auto* sema_ctx = m_session->sema_context();
                    if (sema_ctx)
                    {
                        auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
                        for (auto const& mod : graph.all())
                        {
                            if (!mod || !mod->tu)
                                continue;

                            collect_field_references(mod->tu, field_ranges, field_decl->name, parent_decl);
                        }
                    }
                }
            }

            if (!target && field_ranges.empty())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            std::vector<dcc::sm::SourceRange> ref_ranges;
            if (target)
                ref_ranges = dcc::query::find_references(*m_session, target);

            if (!field_ranges.empty())
                ref_ranges.insert(ref_ranges.end(), std::make_move_iterator(field_ranges.begin()), std::make_move_iterator(field_ranges.end()));

            if (params.context.includeDeclaration)
            {
                if (target)
                {
                    auto decl_range = dcc::query::decl_name_range(target);
                    if (decl_range.valid())
                        ref_ranges.push_back(decl_range);
                }
                else if (node->resolved_field)
                {
                    auto field_decl_range = dcc::query::field_name_range(node->resolved_field);
                    if (field_decl_range.valid())
                        ref_ranges.push_back(field_decl_range);
                }
            }

            std::ranges::sort(ref_ranges, [](dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) {
                auto fid_a = static_cast<std::uint32_t>(a.begin.fileId);
                auto fid_b = static_cast<std::uint32_t>(b.begin.fileId);
                if (fid_a != fid_b)
                    return fid_a < fid_b;
                if (a.begin.offset != b.begin.offset)
                    return a.begin.offset < b.begin.offset;
                return a.end.offset < b.end.offset;
            });

            auto [first2, last2] = std::ranges::unique(ref_ranges, [](dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) {
                return a.begin.fileId == b.begin.fileId && a.begin.offset == b.begin.offset && a.end.offset == b.end.offset;
            });

            ref_ranges.erase(first2, last2);

            auto arr = protocol::JsonValue::empty_array();
            for (auto const& range : ref_ranges)
            {
                auto loc = source_range_to_lsp_location(range);
                if (loc)
                    arr.push_back(loc->to_json());
            }

            std::println(m_log, "[dccd] textDocument/references: {} reference(s) found", arr.array_size());

            return protocol::build_response(rpc.id.value(), std::move(arr));
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_document_highlight(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DocumentHighlightParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);
            auto node = query_at_params(params.textDocument.uri, sm_pos);
            if (!node)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            dcc::ast::Decl const* target = nullptr;
            if (node->ufcs_callee)
                target = node->ufcs_callee;
            else if (node->resolved_field)
                ;
            else if (node->resolved_decl)
                target = node->resolved_decl;
            else if (node->hovered_decl)
                target = node->hovered_decl;

            std::vector<dcc::sm::SourceRange> field_ranges;
            if (node->resolved_field && !target)
            {
                auto const* field_decl = node->resolved_field;
                auto const* parent_decl = node->resolved_field_parent;
                if (parent_decl)
                {
                    auto* sema_ctx = m_session->sema_context();
                    if (sema_ctx)
                    {
                        auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
                        for (auto const& mod : graph.all())
                        {
                            if (!mod || !mod->tu)
                                continue;

                            collect_field_references(mod->tu, field_ranges, field_decl->name, parent_decl);
                        }
                    }
                }
            }

            if (!target && field_ranges.empty())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            std::vector<dcc::sm::SourceRange> ref_ranges;
            if (target)
                ref_ranges = dcc::query::find_references(*m_session, target);

            if (!field_ranges.empty())
                ref_ranges.insert(ref_ranges.end(), std::make_move_iterator(field_ranges.begin()), std::make_move_iterator(field_ranges.end()));

            if (target)
            {
                auto decl_range = dcc::query::decl_name_range(target);
                if (decl_range.valid())
                    ref_ranges.push_back(decl_range);
            }
            else if (node->resolved_field)
            {
                auto field_decl_range = dcc::query::field_name_range(node->resolved_field);
                if (field_decl_range.valid())
                    ref_ranges.push_back(field_decl_range);
            }

            std::ranges::sort(ref_ranges, [](dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) {
                auto fid_a = static_cast<std::uint32_t>(a.begin.fileId);
                auto fid_b = static_cast<std::uint32_t>(b.begin.fileId);
                if (fid_a != fid_b)
                    return fid_a < fid_b;
                if (a.begin.offset != b.begin.offset)
                    return a.begin.offset < b.begin.offset;
                return a.end.offset < b.end.offset;
            });

            auto [first2, last2] = std::ranges::unique(ref_ranges, [](dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) {
                return a.begin.fileId == b.begin.fileId && a.begin.offset == b.begin.offset && a.end.offset == b.end.offset;
            });

            ref_ranges.erase(first2, last2);

            auto active_fid = node->file;

            auto& sm = m_session->source_manager();

            auto arr = protocol::JsonValue::empty_array();
            for (auto const& range : ref_ranges)
            {
                if (range.begin.fileId != active_fid || range.end.fileId != active_fid)
                    continue;

                auto start_pos = sm.location_to_lsp_position(range.begin);
                auto end_pos = sm.location_to_lsp_position(range.end);
                if (!start_pos || !end_pos)
                    continue;

                protocol::DocumentHighlight highlight;
                highlight.range.start.line = start_pos->line;
                highlight.range.start.character = start_pos->character;
                highlight.range.end.line = end_pos->line;
                highlight.range.end.character = end_pos->character;
                highlight.kind = protocol::DocumentHighlightKind::Read;

                arr.push_back(highlight.to_json());
            }

            std::println(m_log, "[dccd] textDocument/documentHighlight: {} highlight(s) found", arr.array_size());

            return protocol::build_response(rpc.id.value(), std::move(arr));
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_rename(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::RenameParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);
            auto node = query_at_params(params.textDocument.uri, sm_pos);
            if (!node)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            dcc::ast::Decl const* target = nullptr;
            if (node->ufcs_callee)
                target = node->ufcs_callee;
            else if (node->resolved_field)
                ;
            else if (node->resolved_decl)
                target = node->resolved_decl;
            else if (node->hovered_decl)
                target = node->hovered_decl;

            std::vector<dcc::sm::SourceRange> field_ranges;
            if (node->resolved_field && !target)
            {
                auto const* field_decl = node->resolved_field;
                auto const* parent_decl = node->resolved_field_parent;
                if (parent_decl)
                {
                    auto* sema_ctx = m_session->sema_context();
                    if (sema_ctx)
                    {
                        auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
                        for (auto const& mod : graph.all())
                        {
                            if (!mod || !mod->tu)
                                continue;

                            collect_field_references(mod->tu, field_ranges, field_decl->name, parent_decl);
                        }
                    }
                }
            }

            if (!target && field_ranges.empty())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (target)
            {
                auto decl_range = dcc::query::decl_name_range(target);
                if (!decl_range.valid())
                    return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            std::vector<dcc::sm::SourceRange> ref_ranges;
            if (target)
                ref_ranges = dcc::query::find_references(*m_session, target);

            if (!field_ranges.empty())
            {
                ref_ranges.insert(ref_ranges.end(), std::make_move_iterator(field_ranges.begin()), std::make_move_iterator(field_ranges.end()));
            }

            if (target)
            {
                auto decl_range = dcc::query::decl_name_range(target);
                if (decl_range.valid())
                    ref_ranges.push_back(decl_range);
            }
            else if (node->resolved_field)
            {
                auto field_decl_range = dcc::query::field_name_range(node->resolved_field);
                if (field_decl_range.valid())
                    ref_ranges.push_back(field_decl_range);
            }

            std::ranges::sort(ref_ranges, [](dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) {
                auto fid_a = static_cast<std::uint32_t>(a.begin.fileId);
                auto fid_b = static_cast<std::uint32_t>(b.begin.fileId);
                if (fid_a != fid_b)
                    return fid_a < fid_b;
                if (a.begin.offset != b.begin.offset)
                    return a.begin.offset < b.begin.offset;
                return a.end.offset < b.end.offset;
            });

            auto [first3, last3] = std::ranges::unique(ref_ranges, [](dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) {
                return a.begin.fileId == b.begin.fileId && a.begin.offset == b.begin.offset && a.end.offset == b.end.offset;
            });

            ref_ranges.erase(first3, last3);

            protocol::WorkspaceEdit we;
            for (auto const& range : ref_ranges)
            {
                auto& sm = m_session->source_manager();
                auto const* file = sm.get(range.begin.fileId);
                if (!file)
                    continue;

                auto start_pos = sm.location_to_lsp_position(range.begin);
                auto end_pos = sm.location_to_lsp_position(range.end);
                if (!start_pos || !end_pos)
                    continue;

                protocol::TextEdit edit;
                edit.range.start.line = start_pos->line;
                edit.range.start.character = start_pos->character;
                edit.range.end.line = end_pos->line;
                edit.range.end.character = end_pos->character;
                edit.newText = params.newName;

                we.changes[file->uri()].push_back(std::move(edit));
            }

            std::size_t total_edits = 0;
            for (auto const& [uri, edits] : we.changes)
                total_edits += edits.size();

            std::println(m_log, "[dccd] textDocument/rename: {} reference(s), {} edit(s)", ref_ranges.size(), total_edits);

            return protocol::build_response(rpc.id.value(), we.to_json());
        }

        [[nodiscard]] CachedDiagnostic const* match_cached_diagnostic(std::vector<CachedDiagnostic> const& cached,
                                                                      protocol::LspDiagnostic const& incoming) const noexcept
        {
            for (auto const& c : cached)
            {
                auto const& r = c.lsp_diag.range;
                auto const& ir = incoming.range;

                if (r.start.line != ir.start.line || r.start.character != ir.start.character || r.end.line != ir.end.line ||
                    r.end.character != ir.end.character)
                    continue;

                if (c.lsp_diag.message != incoming.message)
                    continue;

                if (c.lsp_diag.severity.has_value() && incoming.severity.has_value() && c.lsp_diag.severity != incoming.severity)
                    continue;

                return &c;
            }
            return nullptr;
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_code_action(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::CodeActionParams::from_json(rpc.params.value());

            std::println(m_log, "[dccd] codeAction: uri={} context_diagnostics={}", params.textDocument.uri, params.context.diagnostics.size());

            auto cache_it = m_diagnostic_cache.find(params.textDocument.uri);
            if (cache_it == m_diagnostic_cache.end())
            {
                std::println(m_log, "[dccd] codeAction: no cached diagnostics for {}", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            auto& cached = cache_it->second;

            auto arr = protocol::JsonValue::empty_array();
            for (auto const& ctx_diag : params.context.diagnostics)
            {
                auto const* cached_diag = match_cached_diagnostic(cached, ctx_diag);
                if (!cached_diag)
                {
                    std::println(m_log, "[dccd] codeAction: no cached match for diag at {},{}-{},{}  msg=\"{}\"", ctx_diag.range.start.line,
                                 ctx_diag.range.start.character, ctx_diag.range.end.line, ctx_diag.range.end.character, ctx_diag.message);
                    continue;
                }

                for (auto const& fix : cached_diag->compiler_diag.fixes())
                {
                    auto& sm = m_session->source_manager();
                    auto const* sf = sm.get(fix.range.begin.fileId);
                    if (!sf)
                        continue;

                    auto start_pos = sm.location_to_lsp_position(fix.range.begin);
                    auto end_pos = sm.location_to_lsp_position(fix.range.end);
                    if (!start_pos || !end_pos)
                        continue;

                    protocol::CodeAction action;
                    action.title = fix.message.empty() ? std::string{"Apply fix"} : std::string{fix.message};
                    action.kind = protocol::kCodeActionQuickFix;
                    action.diagnostics.push_back(ctx_diag);

                    protocol::TextEdit edit;
                    edit.range.start.line = start_pos->line;
                    edit.range.start.character = start_pos->character;
                    edit.range.end.line = end_pos->line;
                    edit.range.end.character = end_pos->character;
                    edit.newText = fix.replacement;

                    action.edit.changes[sf->uri()].push_back(std::move(edit));

                    arr.push_back(action.to_json());
                }
            }

            std::println(m_log, "[dccd] codeAction: returning {} code action(s)", arr.array_size());
            return protocol::build_response(rpc.id.value(), std::move(arr));
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_inlay_hint(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::InlayHintParams::from_json(rpc.params.value());

            auto* sema_ctx = m_session->sema_context();
            if (!sema_ctx)
            {
                std::println(m_log, "[dccd] inlayHint: no sema context for {}", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
            {
                std::println(m_log, "[dccd] inlayHint: cannot find file for {}", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();

            dcc::sema::ModuleInfo const* module = nullptr;
            for (auto const& mod : graph.all())
            {
                auto const* sf = m_session->source_manager().get(mod->file_id);
                if (sf && sf->uri() == params.textDocument.uri)
                {
                    module = mod.get();
                    break;
                }
            }

            if (!module)
            {
                for (auto const& mod : graph.all())
                {
                    if (mod->file_id == *fid_opt)
                    {
                        module = mod.get();
                        break;
                    }
                }
            }

            if (!module || !module->tu)
            {
                std::println(m_log, "[dccd] inlayHint: no module/TU for {}", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            auto& sm = m_session->source_manager();

            auto lsp_to_sm = [&](protocol::LspPosition pos) -> std::optional<dcc::sm::Location> {
                auto loc = sm.lsp_position_to_location(*fid_opt, dcc::sm::Position{pos.line, pos.character});
                if (!loc)
                    return std::nullopt;
                return *loc;
            };

            auto start_loc = lsp_to_sm(params.range.start);
            auto end_loc = lsp_to_sm(params.range.end);
            if (!start_loc || !end_loc)
            {
                std::println(m_log, "[dccd] inlayHint: cannot convert range for {}", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            dcc::sm::SourceRange request_range{*start_loc, *end_loc};

            auto formatter = [&](dcc::types::Type const* ty) -> std::string { return LanguageServer::format_dcc_type(ty); };

            auto hints = dccd::inlay_hints::collect_inlay_hints(sm, module->tu, request_range, formatter);

            auto arr = protocol::JsonValue::empty_array();
            for (auto const& h : hints)
                arr.push_back(h.to_json());

            std::println(m_log, "[dccd] inlayHint: {} hint(s) for {}", arr.array_size(), params.textDocument.uri);

            return protocol::build_response(rpc.id.value(), std::move(arr));
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_formatting(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DocumentFormattingParams::from_json(rpc.params.value());

            std::println(m_log, "[dccd] formatting: uri={} tabSize={} insertSpaces={}", params.textDocument.uri, params.options.tabSize,
                         params.options.insertSpaces);

            if (!m_session.has_value())
            {
                std::println(m_log, "[dccd] formatting: no session");
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
            {
                std::println(m_log, "[dccd] formatting: cannot find file for {}", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            auto const* sf = m_session->source_manager().get(*fid_opt);
            if (!sf)
            {
                std::println(m_log, "[dccd] formatting: null source file for {}", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            auto edit = dccd::format::format_document(*sf, m_session->interner(), params.options);
            if (!edit)
            {
                std::println(m_log, "[dccd] formatting: format_document returned null (lex errors or malformed input)");
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            auto arr = protocol::JsonValue::empty_array();
            arr.push_back(edit->to_json());
            return protocol::build_response(rpc.id.value(), std::move(arr));
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_workspace_symbol(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::WorkspaceSymbolParams::from_json(rpc.params.value());

            std::println(m_log, "[dccd] workspace/symbol: query=\"{}\"", params.query);

            if (!m_session.has_value())
            {
                std::println(m_log, "[dccd] workspace/symbol: no session");
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            auto symbols = dccd::workspace_symbols::search_workspace_symbols(*m_session, m_workspace_roots, params.query);

            auto arr = protocol::JsonValue::empty_array();
            for (auto const& sym : symbols)
                arr.push_back(sym.to_json());

            std::println(m_log, "[dccd] workspace/symbol: {} result(s)", arr.array_size());

            return protocol::build_response(rpc.id.value(), std::move(arr));
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_virtual_document(protocol::RpcInfo const& rpc)
        {
            std::string uri;
            if (rpc.params.has_value())
                if (auto const* uri_val = rpc.params->find_member("uri"))
                    if (uri_val->is_string())
                        uri = uri_val->as_string();

            std::println(m_log, "[dccd] dccd/virtualDocument: uri=\"{}\"", uri);

            if (!dcc::vfs::is_dcc_core_uri(uri))
                return protocol::build_error_response(rpc.id.value(), -32602, std::format("not a virtual URI: {}", uri));

            auto text = dcc::vfs::source_text_for_uri(uri);
            if (text.empty())
                return protocol::build_error_response(rpc.id.value(), -32602, std::format("unknown virtual URI: {}", uri));

            auto result = protocol::JsonValue::empty_object();
            result.set("text", protocol::JsonValue::string_val(std::string{text}));

            return protocol::build_response(rpc.id.value(), std::move(result));
        }

        void collect_field_references_in_expr(dcc::ast::Expr const* expr, std::vector<dcc::sm::SourceRange>& out, std::string_view field_name,
                                              dcc::ast::Decl const* parent_decl)
        {
            if (!expr)
                return;

            switch (expr->kind)
            {
                case dcc::ast::ExprKind::FieldAccess: {
                    auto* e = static_cast<dcc::ast::FieldAccessExpr const*>(expr);
                    if (e->field == field_name)
                    {
                        if (e->sema.resolved_decl == parent_decl || e->object)
                        {
                            if (e->sema.resolved_decl == parent_decl)
                                out.push_back(e->field_range);
                            else if (e->object && e->object->sema.resolved_type)
                            {
                                auto* obj_type = dcc::sema::get_resolved_type(e->object->sema);
                                if (obj_type)
                                {
                                    dcc::ast::Decl const* dd = nullptr;
                                    if (obj_type->kind == dcc::types::TypeKind::Struct || obj_type->kind == dcc::types::TypeKind::Union ||
                                        obj_type->kind == dcc::types::TypeKind::Enum)
                                    {
                                        dd = reinterpret_cast<dcc::ast::Decl const*>(static_cast<dcc::types::UserType const*>(obj_type)->decl);
                                    }

                                    if (!dd && obj_type->kind == dcc::types::TypeKind::Pointer)
                                    {
                                        auto const* pt = static_cast<dcc::types::PointerType const*>(obj_type);
                                        if (pt->pointee &&
                                            (pt->pointee->kind == dcc::types::TypeKind::Struct || pt->pointee->kind == dcc::types::TypeKind::Union ||
                                             pt->pointee->kind == dcc::types::TypeKind::Enum))
                                        {
                                            dd = reinterpret_cast<dcc::ast::Decl const*>(static_cast<dcc::types::UserType const*>(pt->pointee)->decl);
                                        }
                                    }
                                    if (dd == parent_decl)
                                        out.push_back(e->field_range);
                                }
                            }
                        }
                    }

                    collect_field_references_in_expr(e->object, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::Call: {
                    auto* e = static_cast<dcc::ast::CallExpr const*>(expr);
                    collect_field_references_in_expr(e->callee, out, field_name, parent_decl);
                    for (auto* a : e->args)
                        collect_field_references_in_expr(a, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::TemplateInst: {
                    auto* e = static_cast<dcc::ast::TemplateInstExpr const*>(expr);
                    collect_field_references_in_expr(e->callee, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::Unary: {
                    auto* e = static_cast<dcc::ast::UnaryExpr const*>(expr);
                    collect_field_references_in_expr(e->operand, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::Postfix: {
                    auto* e = static_cast<dcc::ast::PostfixExpr const*>(expr);
                    collect_field_references_in_expr(e->operand, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::Binary: {
                    auto* e = static_cast<dcc::ast::BinaryExpr const*>(expr);
                    collect_field_references_in_expr(e->lhs, out, field_name, parent_decl);
                    collect_field_references_in_expr(e->rhs, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::Index: {
                    auto* e = static_cast<dcc::ast::IndexExpr const*>(expr);
                    collect_field_references_in_expr(e->object, out, field_name, parent_decl);
                    collect_field_references_in_expr(e->index, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::Cast: {
                    auto* e = static_cast<dcc::ast::CastExpr const*>(expr);
                    collect_field_references_in_expr(e->operand, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::Block: {
                    auto* e = static_cast<dcc::ast::BlockExpr const*>(expr);
                    for (auto* s : e->body.stmts)
                        collect_field_references_in_stmt(s, out, field_name, parent_decl);
                    collect_field_references_in_expr(e->body.tail, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::If: {
                    auto* e = static_cast<dcc::ast::IfExpr const*>(expr);
                    collect_field_references_in_expr(e->condition, out, field_name, parent_decl);
                    for (auto* s : e->then_block.stmts)
                        collect_field_references_in_stmt(s, out, field_name, parent_decl);
                    collect_field_references_in_expr(e->then_block.tail, out, field_name, parent_decl);
                    collect_field_references_in_expr(e->else_branch, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::Match: {
                    auto* e = static_cast<dcc::ast::MatchExpr const*>(expr);
                    collect_field_references_in_expr(e->operand, out, field_name, parent_decl);
                    for (auto const& arm : e->arms)
                        collect_field_references_in_expr(arm.body, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::StructLiteral: {
                    auto* e = static_cast<dcc::ast::StructLiteralExpr const*>(expr);
                    for (auto const& f : e->fields)
                        collect_field_references_in_expr(f.value, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::Range: {
                    auto* e = static_cast<dcc::ast::RangeExpr const*>(expr);
                    collect_field_references_in_expr(e->start, out, field_name, parent_decl);
                    collect_field_references_in_expr(e->end, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::ExprKind::TypeAST: {
                    break;
                }
                default:
                    break;
            }
        }

        void collect_field_references_in_stmt(dcc::ast::Stmt const* stmt, std::vector<dcc::sm::SourceRange>& out, std::string_view field_name,
                                              dcc::ast::Decl const* parent_decl)
        {
            if (!stmt)
                return;

            switch (stmt->kind)
            {
                case dcc::ast::StmtKind::Expr: {
                    auto* s = static_cast<dcc::ast::ExprStmt const*>(stmt);
                    collect_field_references_in_expr(s->expr, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::StmtKind::Return: {
                    auto* s = static_cast<dcc::ast::ReturnStmt const*>(stmt);
                    collect_field_references_in_expr(s->value, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::StmtKind::While: {
                    auto* s = static_cast<dcc::ast::WhileStmt const*>(stmt);
                    collect_field_references_in_expr(s->condition, out, field_name, parent_decl);
                    for (auto* bs : s->body.stmts)
                        collect_field_references_in_stmt(bs, out, field_name, parent_decl);
                    collect_field_references_in_expr(s->body.tail, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::StmtKind::DoWhile: {
                    auto* s = static_cast<dcc::ast::DoWhileStmt const*>(stmt);
                    for (auto* bs : s->body.stmts)
                        collect_field_references_in_stmt(bs, out, field_name, parent_decl);
                    collect_field_references_in_expr(s->body.tail, out, field_name, parent_decl);
                    collect_field_references_in_expr(s->condition, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::StmtKind::For: {
                    auto* s = static_cast<dcc::ast::ForStmt const*>(stmt);
                    collect_field_references_in_stmt(s->init, out, field_name, parent_decl);
                    collect_field_references_in_expr(s->cond, out, field_name, parent_decl);
                    collect_field_references_in_expr(s->update, out, field_name, parent_decl);
                    for (auto* bs : s->body.stmts)
                        collect_field_references_in_stmt(bs, out, field_name, parent_decl);
                    collect_field_references_in_expr(s->body.tail, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::StmtKind::ForIn: {
                    auto* s = static_cast<dcc::ast::ForInStmt const*>(stmt);
                    collect_field_references_in_expr(s->iterable, out, field_name, parent_decl);
                    for (auto* bs : s->body.stmts)
                        collect_field_references_in_stmt(bs, out, field_name, parent_decl);
                    collect_field_references_in_expr(s->body.tail, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::StmtKind::Defer: {
                    auto* s = static_cast<dcc::ast::DeferStmt const*>(stmt);
                    collect_field_references_in_stmt(s->body, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::StmtKind::StaticIf: {
                    auto* s = static_cast<dcc::ast::StaticIfStmt const*>(stmt);
                    collect_field_references_in_expr(s->condition, out, field_name, parent_decl);
                    for (auto* bs : s->then_block.stmts)
                        collect_field_references_in_stmt(bs, out, field_name, parent_decl);
                    collect_field_references_in_expr(s->then_block.tail, out, field_name, parent_decl);
                    collect_field_references_in_stmt(s->else_branch, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::StmtKind::StaticMatch: {
                    auto* s = static_cast<dcc::ast::StaticMatchStmt const*>(stmt);
                    collect_field_references_in_expr(s->operand, out, field_name, parent_decl);
                    for (auto const& arm : s->arms)
                        collect_field_references_in_expr(arm.body, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::StmtKind::Ambiguous: {
                    auto* s = static_cast<dcc::ast::AmbiguousStmt const*>(stmt);
                    collect_field_references_in_expr(s->as_expr, out, field_name, parent_decl);
                    break;
                }
                default:
                    break;
            }
        }

        void collect_field_references_in_decl(dcc::ast::Decl const* decl, std::vector<dcc::sm::SourceRange>& out, std::string_view field_name,
                                              dcc::ast::Decl const* parent_decl)
        {
            if (!decl)
                return;

            switch (decl->kind)
            {
                case dcc::ast::DeclKind::Func: {
                    auto* d = static_cast<dcc::ast::FuncDecl const*>(decl);
                    collect_field_references_in_expr(d->constraint, out, field_name, parent_decl);
                    if (d->body.has_value())
                    {
                        for (auto* s : d->body->stmts)
                            collect_field_references_in_stmt(s, out, field_name, parent_decl);
                        collect_field_references_in_expr(d->body->tail, out, field_name, parent_decl);
                    }
                    break;
                }
                case dcc::ast::DeclKind::Var: {
                    auto* d = static_cast<dcc::ast::VarDecl const*>(decl);
                    collect_field_references_in_expr(d->init, out, field_name, parent_decl);
                    break;
                }
                case dcc::ast::DeclKind::Using: {
                    auto* d = static_cast<dcc::ast::UsingDecl const*>(decl);
                    collect_field_references_in_expr(d->target_expr, out, field_name, parent_decl);
                    break;
                }
                default:
                    break;
            }
        }

        void collect_field_references(dcc::ast::TranslationUnit const* tu, std::vector<dcc::sm::SourceRange>& out, std::string_view field_name,
                                      dcc::ast::Decl const* parent_decl)
        {
            if (!tu)
                return;

            for (auto* d : tu->imports)
                collect_field_references_in_decl(d, out, field_name, parent_decl);
            for (auto* d : tu->decls)
                collect_field_references_in_decl(d, out, field_name, parent_decl);
        }

        void recompile_document(std::string const& uri)
        {
            std::println(m_log, "[dccd] recompile_document: incoming URI=\"{}\"", uri);

            if (dcc::vfs::is_dcc_core_uri(uri))
            {
                std::println(m_log, "[dccd] recompile_document: skipping read-only dcc-core: URI {}", uri);
                return;
            }

            auto path = dcc::sm::SourceManager::parse_file_uri(uri);
            if (!path)
            {
                std::println(m_log, "[dccd] recompile_document: cannot resolve non-file URI to local path: {}", uri);
                return;
            }
            std::println(m_log, "[dccd] recompile_document: resolved path=\"{}\"", path->string());

            auto fid_opt = m_session->source_manager().find_by_uri(uri);
            if (fid_opt)
            {
                auto const* sf = m_session->source_manager().get(*fid_opt);
                if (sf)
                    std::println(m_log, "[dccd] recompile_document: SM maps uri -> fid={} path=\"{}\" kind={}", static_cast<std::uint32_t>(*fid_opt),
                                 sf->path().string(), static_cast<int>(sf->kind()));
                else
                    std::println(m_log, "[dccd] recompile_document: SM maps uri -> fid={} (null file)", static_cast<std::uint32_t>(*fid_opt));
            }
            else
                std::println(m_log, "[dccd] recompile_document: SM find_by_uri returned nullopt for \"{}\"", uri);

            m_session->clear_diagnostics();

            dcc::session::CompileOptions opts;
            opts.arena_initial_size = 256 * 1024;

            auto roots = compute_import_roots();

            if (path->has_parent_path())
                roots.push_back(path->parent_path());

            for (auto const& root : m_workspace_roots)
                roots.push_back(root);

            std::vector<std::filesystem::path> deduped;
            for (auto& r : roots)
            {
                bool found = false;
                for (auto const& existing : deduped)
                {
                    if (existing == r)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    deduped.push_back(std::move(r));
            }

            opts.import_roots = std::move(deduped);

            std::println(m_log, "[dccd] recompile_document: {} import root(s)", opts.import_roots.size());
            for (auto const& r : opts.import_roots)
                std::println(m_log, "[dccd]   import root: \"{}\"", r.string());

            auto result = m_session->analyze_entry(*path, opts);

            if (result.module)
            {
                auto const* sf = m_session->source_manager().get(result.module->file_id);
                if (sf)
                    std::println(m_log, "[dccd] recompile_document: module file_id={} path=\"{}\" kind={}", static_cast<std::uint32_t>(result.module->file_id),
                                 sf->path().string(), static_cast<int>(sf->kind()));
                else
                    std::println(m_log, "[dccd] recompile_document: module file_id={} (null file)", static_cast<std::uint32_t>(result.module->file_id));

                if (fid_opt && result.module->file_id != *fid_opt)
                    std::println(m_log, "[dccd] recompile_document: WARNING module file_id={} differs from open_in_memory file_id={}",
                                 static_cast<std::uint32_t>(result.module->file_id), static_cast<std::uint32_t>(*fid_opt));
            }
            else
                std::println(m_log, "[dccd] recompile_document: analyze_entry returned null module");

            std::println(m_log, "[dccd] recompile_document: has_errors={} success={}", result.has_errors, (result.module != nullptr));

            std::size_t diag_count = 0;
            auto const& sm = m_session->source_manager();
            for (auto const& diag : m_session->diagnostics().diagnostics())
            {
                auto labels = diag.labels();
                bool belongs = false;
                for (auto const& label : labels)
                {
                    auto const* sf = sm.get(label.range.begin.fileId);
                    if (sf && sf->uri() == uri)
                    {
                        belongs = true;
                        break;
                    }
                }
                if (belongs)
                    ++diag_count;
            }
            std::println(m_log, "[dccd] recompile_document: {} diagnostics for URI \"{}\"", diag_count, uri);
        }

        [[nodiscard]] static std::filesystem::path global_config_dir()
        {
            if (char const* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0')
                return std::filesystem::path{xdg} / "dcc";

            if (char const* home = std::getenv("HOME"); home && home[0] != '\0')
                return std::filesystem::path{home} / ".config" / "dcc";

            return {};
        }

        [[nodiscard]] static std::filesystem::path global_config_path() { return global_config_dir() / "dcc.json"; }

        [[nodiscard]] static std::filesystem::path project_config_path(std::filesystem::path const& workspace_root) { return workspace_root / "dcc.json"; }

        [[nodiscard]] static std::vector<std::string> parse_include_paths_from_json(protocol::JsonValue const& config_json)
        {
            std::vector<std::string> paths;
            auto const* arr = config_json.get_array("includePaths");
            if (!arr)
                arr = config_json.get_array("importPaths");

            if (!arr)
                return paths;

            for (auto const& elem : arr->as_array())
                if (elem.is_string())
                    paths.push_back(elem.as_string());

            return paths;
        }

        void load_config_file(std::filesystem::path const& config_path, std::filesystem::path const& base_dir, std::vector<std::filesystem::path>& out_paths)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(config_path, ec) || ec)
                return;

            std::ifstream in(config_path);
            if (!in.is_open())
                return;

            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto json = protocol::JsonValue::parse(content);
            if (!json || !json->is_object())
            {
                std::println(m_log, "[dccd] invalid JSON in config file: {}", config_path.string());
                return;
            }

            auto raw_paths = parse_include_paths_from_json(*json);
            for (auto& raw : raw_paths)
            {
                raw = expand_workspace_variables(std::move(raw), base_dir);

                std::filesystem::path p{std::move(raw)};
                if (!p.is_absolute())
                    p = base_dir / p;

                std::error_code ec2;
                p = std::filesystem::weakly_canonical(p, ec2);
                if (ec2)
                    p = p.lexically_normal();

                std::error_code ec3;
                if (!std::filesystem::is_directory(p, ec3) || ec3)
                {
                    std::println(m_log, "[dccd] config includes nonexistent directory, ignoring: {}", p.string());
                    continue;
                }

                bool duplicate = false;
                for (auto const& existing : out_paths)
                {
                    if (existing == p)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    out_paths.push_back(std::move(p));
            }
        }

        void read_global_config()
        {
            m_global_include_paths.clear();
            auto config_dir = global_config_dir();
            auto config_path = global_config_path();
            std::println(m_log, "[dccd] reading global config: {}", config_path.string());
            load_config_file(config_path, config_dir, m_global_include_paths);
            std::println(m_log, "[dccd] global config: {} include paths", m_global_include_paths.size());
        }

        void read_project_configs()
        {
            m_project_include_paths.clear();
            for (auto const& root : m_workspace_roots)
            {
                auto config_path = project_config_path(root);
                std::println(m_log, "[dccd] reading project config: {}", config_path.string());
                load_config_file(config_path, root, m_project_include_paths);
            }

            std::println(m_log, "[dccd] project configs: {} include paths", m_project_include_paths.size());
        }

        [[nodiscard]] static std::string expand_workspace_variables(std::string path, std::filesystem::path const& workspace_root)
        {
            constexpr std::string_view kWorkspaceFolder = "${workspaceFolder}";
            constexpr std::string_view kWorkspaceFolderBasename = "${workspaceFolderBasename}";

            auto replace = [](std::string& s, std::string_view var, std::string const& replacement) {
                std::size_t pos = 0;
                while ((pos = s.find(var, pos)) != std::string::npos)
                {
                    s.replace(pos, var.size(), replacement);
                    pos += replacement.size();
                }
            };

            if (workspace_root.empty())
                return path;

            replace(path, kWorkspaceFolder, workspace_root.string());

            auto stem = workspace_root.filename().string();
            if (!stem.empty())
                replace(path, kWorkspaceFolderBasename, stem);

            return path;
        }

        void parse_lsp_include_paths(protocol::DidChangeConfigurationParams const& params)
        {
            m_lsp_include_paths.clear();
            if (!params.settings.has_value())
                return;

            auto const* dcc_obj = params.settings->get_object("dcc");
            if (!dcc_obj)
                return;

            auto raw_paths = parse_include_paths_from_json(*dcc_obj);
            if (raw_paths.empty())
                return;

            std::filesystem::path base_dir;
            if (!m_workspace_roots.empty())
                base_dir = m_workspace_roots.front();
            else
                base_dir = std::filesystem::current_path();

            auto workspace_root = base_dir;

            for (auto& raw : raw_paths)
            {
                raw = expand_workspace_variables(std::move(raw), workspace_root);

                std::filesystem::path p{std::move(raw)};
                if (!p.is_absolute())
                    p = base_dir / p;

                std::error_code ec;
                p = std::filesystem::weakly_canonical(p, ec);
                if (ec)
                    p = p.lexically_normal();

                std::error_code ec2;
                if (!std::filesystem::is_directory(p, ec2) || ec2)
                {
                    std::println(m_log, "[dccd] LSP config includes nonexistent directory, ignoring: {}", p.string());
                    continue;
                }

                bool duplicate = false;
                for (auto const& existing : m_lsp_include_paths)
                {
                    if (existing == p)
                    {
                        duplicate = true;
                        break;
                    }
                }

                if (!duplicate)
                    m_lsp_include_paths.push_back(std::move(p));
            }

            std::println(m_log, "[dccd] LSP config: {} include paths", m_lsp_include_paths.size());
        }

        [[nodiscard]] std::vector<std::filesystem::path> compute_import_roots() const
        {
            std::vector<std::filesystem::path> roots;

            auto add_unique = [&](std::filesystem::path const& p) {
                for (auto const& existing : roots)
                    if (existing == p)
                        return;
                roots.push_back(p);
            };

            for (auto const& p : m_lsp_include_paths)
                add_unique(p);

            for (auto const& p : m_project_include_paths)
                add_unique(p);

            for (auto const& p : m_global_include_paths)
                add_unique(p);

            return roots;
        }

        void reconfigure_and_recompile()
        {
            read_global_config();
            read_project_configs();

            std::vector<std::string> uris;
            m_session->source_manager().for_each_file([&](dcc::sm::SourceFile const& sf) {
                if (sf.kind() == dcc::sm::FileKind::InMemory && !sf.is_closed() && !sf.uri().empty())
                    uris.push_back(sf.uri());
            });

            for (auto const& [uri, _] : m_diagnostic_cache)
            {
                bool found = false;
                for (auto const& u : uris)
                    if (u == uri)
                    {
                        found = true;
                        break;
                    }

                if (!found)
                    uris.push_back(uri);
            }

            for (auto const& uri : uris)
            {
                recompile_document(uri);
                publish_diagnostics(uri);
            }
        }

        void handle_workspace_did_change_configuration(protocol::RpcInfo const& rpc)
        {
            std::println(m_log, "[dccd] workspace/didChangeConfiguration received");
            auto params = protocol::DidChangeConfigurationParams::from_json(rpc.params.value());
            parse_lsp_include_paths(params);
            reconfigure_and_recompile();
        }

        void handle_workspace_did_change_watched_files(protocol::RpcInfo const& rpc)
        {
            std::println(m_log, "[dccd] workspace/didChangeWatchedFiles received");
            auto params = protocol::DidChangeWatchedFilesParams::from_json(rpc.params.value());

            bool reload = false;
            auto global_cfg = global_config_path();
            for (auto const& change : params.changes)
            {
                auto path = dcc::sm::SourceManager::parse_file_uri(change.uri);
                if (!path)
                    continue;

                std::error_code ec;
                auto canonical = std::filesystem::weakly_canonical(*path, ec);

                auto global_canonical = std::filesystem::weakly_canonical(global_cfg, ec);

                if (canonical == global_canonical)
                {
                    std::println(m_log, "[dccd] global config changed: {}", canonical.string());
                    reload = true;
                    break;
                }

                for (auto const& root : m_workspace_roots)
                {
                    auto project_cfg = std::filesystem::weakly_canonical(project_config_path(root), ec);
                    if (canonical == project_cfg)
                    {
                        std::println(m_log, "[dccd] project config changed: {}", canonical.string());
                        reload = true;
                        break;
                    }
                }
                if (reload)
                    break;
            }

            if (reload)
                reconfigure_and_recompile();
        }

        void publish_diagnostics(std::string const& uri)
        {
            protocol::PublishDiagnosticsParams params;
            params.uri = uri;

            auto const& sm = m_session->source_manager();

            auto opt_fid = sm.find_by_uri(uri);
            dcc::sm::FileId target_fid = opt_fid.value_or(dcc::sm::FileId::Invalid);

            auto& cache = m_diagnostic_cache[uri];
            cache.clear();

            for (auto const& diag : m_session->diagnostics().diagnostics())
            {
                protocol::LspDiagnostic lsp_diag;
                lsp_diag.source = "dcc";
                lsp_diag.message = diag.message();

                switch (diag.severity())
                {
                    case dcc::diag::Severity::Error:
                        lsp_diag.severity = protocol::DiagnosticSeverity::Error;
                        break;
                    case dcc::diag::Severity::Warning:
                        lsp_diag.severity = protocol::DiagnosticSeverity::Warning;
                        break;
                    case dcc::diag::Severity::Note:
                        lsp_diag.severity = protocol::DiagnosticSeverity::Information;
                        break;
                    case dcc::diag::Severity::Help:
                        lsp_diag.severity = protocol::DiagnosticSeverity::Hint;
                        break;
                }

                bool has_range = false;
                auto labels = diag.labels();
                if (!labels.empty())
                {
                    for (auto const& label : labels)
                    {
                        if (label.style == dcc::diag::LabelStyle::Primary && label.range.begin.fileId == target_fid)
                        {
                            auto start_pos = sm.location_to_lsp_position(label.range.begin);
                            auto end_pos = sm.location_to_lsp_position(label.range.end);

                            if (start_pos && end_pos)
                            {
                                lsp_diag.range.start.line = start_pos->line;
                                lsp_diag.range.start.character = start_pos->character;
                                lsp_diag.range.end.line = end_pos->line;
                                lsp_diag.range.end.character = end_pos->character;
                                has_range = true;
                            }
                            break;
                        }
                    }
                }

                if (!has_range && !labels.empty())
                {
                    bool any_for_target = false;
                    for (auto const& label : labels)
                        if (label.range.begin.fileId == target_fid)
                        {
                            any_for_target = true;
                            break;
                        }

                    if (!any_for_target)
                        continue;
                }

                std::string extended_msg = diag.message();
                for (auto const& note : diag.notes())
                {
                    extended_msg += "\nnote: ";
                    extended_msg += note;
                }

                for (auto const& help : diag.helps())
                {
                    extended_msg += "\nhelp: ";
                    extended_msg += help;
                }

                lsp_diag.message = std::move(extended_msg);
                cache.push_back(CachedDiagnostic{lsp_diag, diag});

                params.diagnostics.push_back(std::move(lsp_diag));
            }

            publish_lsp_diagnostics(params);
        }

        void publish_lsp_diagnostics(protocol::PublishDiagnosticsParams const& params)
        {
            auto notification = protocol::build_notification("textDocument/publishDiagnostics", params.to_json());
            send_message(notification);
        }

        void clear_stale_marker(std::string const& uri) noexcept { m_stale_uris.erase(uri); }

        void send_message(protocol::JsonValue const& msg)
        {
            std::string payload = msg.serialize();
            std::print("Content-Length: {}\r\n\r\n{}", payload.size(), payload);
            std::cout.flush();
        }

        [[nodiscard]] static std::optional<std::filesystem::path> file_uri_to_path(std::string_view uri)
        {
            constexpr std::string_view kFilePrefix = "file://";

            if (!uri.starts_with(kFilePrefix))
                return std::nullopt;

            auto rest = uri.substr(kFilePrefix.size());

            if (!rest.empty() && rest[0] == '/')
            {
                auto decoded = uri_decode(rest);
                return std::filesystem::path{std::move(decoded)};
            }

            return std::nullopt;
        }

        [[nodiscard]] static std::string format_dcc_type(dcc::types::Type const* ty)
        {
            if (!ty)
                return "<unresolved>";

            switch (ty->kind)
            {
                case dcc::types::TypeKind::Void:
                    return "void";
                case dcc::types::TypeKind::Bool:
                    return "bool";
                case dcc::types::TypeKind::Char:
                    return "char";
                case dcc::types::TypeKind::NullT:
                    return "null_t";
                case dcc::types::TypeKind::Int: {
                    auto const* it = static_cast<dcc::types::IntType const*>(ty);
                    return std::format("{}{}", it->is_signed ? 'i' : 'u', static_cast<unsigned>(it->bits));
                }
                case dcc::types::TypeKind::Float: {
                    auto const* ft = static_cast<dcc::types::FloatType const*>(ty);
                    return std::format("f{}", static_cast<unsigned>(ft->bits));
                }
                case dcc::types::TypeKind::Pointer: {
                    auto const* p = static_cast<dcc::types::PointerType const*>(ty);
                    std::string quals;
                    if (dcc::types::has_qual(p->pointee_quals, dcc::types::Qual::Const))
                        quals += "const ";
                    if (dcc::types::has_qual(p->pointee_quals, dcc::types::Qual::Volatile))
                        quals += "volatile ";
                    if (dcc::types::has_qual(p->pointee_quals, dcc::types::Qual::Restrict))
                        quals += "restrict ";
                    return std::format("{}{}*", quals, format_dcc_type(p->pointee));
                }

                case dcc::types::TypeKind::Array: {
                    auto const* a = static_cast<dcc::types::ArrayType const*>(ty);
                    return std::format("{}[{}]", format_dcc_type(a->element), a->count);
                }
                case dcc::types::TypeKind::RuntimeArray: {
                    auto const* a = static_cast<dcc::types::RuntimeArrayType const*>(ty);
                    return std::format("{}[]", format_dcc_type(a->element));
                }
                case dcc::types::TypeKind::Slice: {
                    auto const* s = static_cast<dcc::types::SliceType const*>(ty);
                    std::string quals;
                    if (dcc::types::has_qual(s->element_quals, dcc::types::Qual::Const))
                        quals += "const ";
                    if (dcc::types::has_qual(s->element_quals, dcc::types::Qual::Volatile))
                        quals += "volatile ";
                    if (dcc::types::has_qual(s->element_quals, dcc::types::Qual::Restrict))
                        quals += "restrict ";
                    return std::format("[]{}{}", quals, format_dcc_type(s->element));
                }
                case dcc::types::TypeKind::Fam: {
                    auto const* f = static_cast<dcc::types::FamType const*>(ty);
                    return std::format("{}[]", format_dcc_type(f->element));
                }
                case dcc::types::TypeKind::FuncPtr: {
                    auto const* f = static_cast<dcc::types::FuncPtrType const*>(ty);
                    std::string params;
                    for (std::size_t i = 0; i < f->params.size(); ++i)
                    {
                        if (i > 0)
                            params += ", ";
                        params += format_dcc_type(f->params[i]);
                    }
                    return std::format("{}(*)({})", format_dcc_type(f->return_type), params);
                }
                case dcc::types::TypeKind::Struct:
                case dcc::types::TypeKind::Union:
                case dcc::types::TypeKind::Enum: {
                    auto const* ut = static_cast<dcc::types::UserType const*>(ty);
                    auto const* dd = reinterpret_cast<dcc::ast::Decl const*>(ut->decl);
                    std::string_view name = "<null>";
                    if (dd)
                    {
                        switch (dd->kind)
                        {
                            case dcc::ast::DeclKind::Struct:
                                name = static_cast<dcc::ast::StructDecl const*>(dd)->name;
                                break;
                            case dcc::ast::DeclKind::Union:
                                name = static_cast<dcc::ast::UnionDecl const*>(dd)->name;
                                break;
                            case dcc::ast::DeclKind::Enum:
                                name = static_cast<dcc::ast::EnumDecl const*>(dd)->name;
                                break;
                            case dcc::ast::DeclKind::Using:
                                name = static_cast<dcc::ast::UsingDecl const*>(dd)->alias_path.tail_name();
                                break;
                            default:
                                break;
                        }
                    }
                    if (!ut->template_args.empty())
                    {
                        std::string args;
                        for (std::size_t i = 0; i < ut->template_args.size(); ++i)
                        {
                            if (i > 0)
                                args += ", ";
                            args += format_dcc_type(ut->template_args[i]);
                        }
                        return std::format("{}({})", name, args);
                    }
                    return std::string{name};
                }
                case dcc::types::TypeKind::TemplateParam:
                    return std::string{static_cast<dcc::types::TemplateParamType const*>(ty)->name};
                case dcc::types::TypeKind::Range:
                    return std::format("range({})", format_dcc_type(static_cast<dcc::types::RangeType const*>(ty)->element));
                case dcc::types::TypeKind::RangeInclusive:
                    return std::format("range_inclusive({})", format_dcc_type(static_cast<dcc::types::RangeInclusiveType const*>(ty)->element));
                case dcc::types::TypeKind::TypePack:
                    return std::format("type_pack({}: {})", format_dcc_type(static_cast<dcc::types::TypePackType const*>(ty)->element),
                                       static_cast<dcc::types::TypePackType const*>(ty)->pack_index);
                case dcc::types::TypeKind::Nominal:
                    return format_dcc_type(static_cast<dcc::types::NominalType const*>(ty)->underlying);
                case dcc::types::TypeKind::Error:
                    return "<error>";
            }
            return dcc::sema::format_dcc_type(ty);
        }

        [[nodiscard]] static std::string uri_decode(std::string_view s)
        {
            std::string result;
            result.reserve(s.size());

            for (std::size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '%' && i + 2 < s.size())
                {
                    auto hi = s[i + 1];
                    auto lo = s[i + 2];

                    auto is_hex = [](char c) -> bool { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); };

                    auto hex_val = [](char c) -> int {
                        if (c >= '0' && c <= '9')
                            return static_cast<int>(c - '0');
                        if (c >= 'A' && c <= 'F')
                            return static_cast<int>(c - 'A') + 10;
                        if (c >= 'a' && c <= 'f')
                            return static_cast<int>(c - 'a') + 10;
                        return -1;
                    };

                    if (is_hex(hi) && is_hex(lo))
                    {
                        int value = (hex_val(hi) << 4) | hex_val(lo);
                        result += static_cast<char>(value);
                        i += 2;
                        continue;
                    }
                }

                result += s[i];
            }

            return result;
        }
    };

} // namespace dccd
