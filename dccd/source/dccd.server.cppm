export module dccd.server;

import std;
import dcc.sm;
import dcc.diag;
import dcc.session;
import dccd.protocol;
import dccd.transport;
import dccd.semantic_tokens;
import dccd.completion;
import dccd.inlay_hints;
import dccd.workspace_index;
import dccd.format;
import dccd.compilation_database;
import dcc.query;
import dcc.lex;
import dcc.ast;
import dcc.sema;
import dcc.sema.type_helpers;
import dcc.vfs;
import dcc.target;

export namespace dccd
{
    class LanguageServer
    {
    public:
        explicit LanguageServer(std::ostream* output_sink = nullptr) : LanguageServer(output_sink, std::make_shared<transport::CancellationRegistry>()) {}

        LanguageServer(std::ostream* output_sink, std::shared_ptr<transport::CancellationRegistry> cancellation)
            : m_output{output_sink}, m_cancellation{std::move(cancellation)}
        {
            if (!m_cancellation)
                m_cancellation = std::make_shared<transport::CancellationRegistry>();

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

                auto request_id = protocol::RequestId::from_json(rpc.id.value());
                if (!request_id.valid())
                    return protocol::build_error_response(rpc.id.value(), -32600, "Invalid Request: id must be a number or a string");

                std::ignore = m_cancellation->register_pending(request_id);
                CurrentRequestScope scope{*this, request_id};

                if (m_cancellation->is_cancelled(request_id))
                    return protocol::build_error_response(rpc.id.value(), protocol::kErrorRequestCancelled, "Request cancelled");

                try
                {
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
                    if (method == "textDocument/prepareRename")
                        return handle_prepare_rename(rpc);
                    if (method == "textDocument/codeAction")
                        return handle_code_action(rpc);
                    if (method == "textDocument/inlayHint")
                        return handle_inlay_hint(rpc);
                    if (method == "textDocument/formatting")
                        return handle_formatting(rpc);
                    if (method == "textDocument/rangeFormatting")
                        return handle_range_formatting(rpc);
                    if (method == "textDocument/onTypeFormatting")
                        return handle_on_type_formatting(rpc);
                    if (method == "workspace/symbol")
                        return handle_workspace_symbol(rpc);
                    if (method == "dccd/virtualDocument")
                        return handle_virtual_document(rpc);

                    return protocol::build_error_response(rpc.id.value(), -32601, std::format("Method not found: {}", method));
                }
                catch (RequestCancelledError const&)
                {
                    return protocol::build_error_response(rpc.id.value(), protocol::kErrorRequestCancelled, "Request cancelled");
                }
            }

            if (rpc.is_notification())
            {
                std::string method = rpc.method.value();

                if (method == protocol::kCancelRequestMethod)
                {
                    handle_cancel_request(rpc);
                    return std::nullopt;
                }
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

        [[nodiscard]] transport::CancellationRegistry& cancellation_registry() noexcept { return *m_cancellation; }
        [[nodiscard]] transport::CancellationRegistry const& cancellation_registry() const noexcept { return *m_cancellation; }

        [[nodiscard]] dccd::workspace_index::WorkspaceIndex& workspace_index() noexcept { return m_workspace_index; }
        [[nodiscard]] dccd::workspace_index::WorkspaceIndex const& workspace_index() const noexcept { return m_workspace_index; }

        [[nodiscard]] dcc::sm::SourceManager& source_manager() noexcept { return m_session->source_manager(); }
        [[nodiscard]] dcc::sm::SourceManager const& source_manager() const noexcept { return m_session->source_manager(); }

    private:
        std::optional<dcc::session::CompilerSession> m_session;
        std::ostream& m_log{std::cerr};
        std::ostream* m_output{nullptr};
        bool m_should_exit{false};
        std::shared_ptr<transport::CancellationRegistry> m_cancellation;
        std::optional<protocol::RequestId> m_current_request_id;
        std::vector<std::filesystem::path> m_workspace_roots;
        std::vector<std::filesystem::path> m_lsp_include_paths;
        std::vector<std::filesystem::path> m_project_include_paths;
        std::vector<std::filesystem::path> m_global_include_paths;
        std::optional<std::string> m_lsp_compilation_database;
        std::map<std::string, std::string, std::less<>> m_project_compilation_database;

        struct WorkspaceCompilationDatabase
        {
            std::filesystem::path workspace_root;
            std::filesystem::path database_path;
            dccd::CompilationDatabase database;
        };
        std::vector<WorkspaceCompilationDatabase> m_workspace_databases;

        std::string m_active_entry_uri;
        dccd::workspace_index::WorkspaceIndex m_workspace_index;
        bool m_did_change_watched_files_supported{false};
        bool m_watch_registration_sent{false};
        protocol::InlayHintOptions m_inlay_hint_options;

        struct CachedDiagnostic
        {
            protocol::LspDiagnostic lsp_diag;
            dcc::diag::Diagnostic compiler_diag;
        };

        struct CachedDiagnosticEntry
        {
            std::optional<std::int64_t> version;
            std::uint64_t content_revision{0};
            std::uint64_t graph_generation{0};
            std::vector<CachedDiagnostic> diagnostics;
        };

        std::map<std::string, CachedDiagnosticEntry, std::less<>> m_diagnostic_cache;
        std::unordered_set<std::string> m_published_uris;
        std::unordered_set<std::string> m_stale_uris;

        std::map<dcc::sm::FileId, std::uint64_t> m_graph_revisions;
        std::uint64_t m_graph_generation{0};
        bool m_recompiling{false};

        [[nodiscard]] std::string const& active_entry_uri() const noexcept { return m_active_entry_uri; }

        [[nodiscard]] bool graph_snapshot_fresh() const noexcept
        {
            if (m_graph_revisions.empty())
                return false;

            auto const& sm = m_session->source_manager();
            for (auto const& [fid, rev] : m_graph_revisions)
                if (sm.content_revision(fid) != rev)
                    return false;

            return true;
        }

        struct RecompilingGuard
        {
            bool& flag;
            explicit RecompilingGuard(bool& f) noexcept : flag{f} { flag = true; }
            ~RecompilingGuard() noexcept { flag = false; }
            RecompilingGuard(RecompilingGuard const&) = delete;
            RecompilingGuard& operator=(RecompilingGuard const&) = delete;
        };

        struct RequestCancelledError
        {
        };

        struct CurrentRequestScope
        {
            LanguageServer& server;
            protocol::RequestId id;
            std::optional<protocol::RequestId> previous;

            CurrentRequestScope(LanguageServer& s, protocol::RequestId request_id) : server{s}, id{std::move(request_id)}
            {
                previous = server.m_current_request_id;
                server.m_current_request_id = id;
            }

            ~CurrentRequestScope()
            {
                server.m_cancellation->finish(id);
                server.m_current_request_id = previous;
            }

            CurrentRequestScope(CurrentRequestScope const&) = delete;
            CurrentRequestScope& operator=(CurrentRequestScope const&) = delete;
        };

        [[nodiscard]] bool current_request_cancelled() const noexcept
        {
            if (!m_current_request_id)
                return false;

            return m_cancellation->is_cancelled(*m_current_request_id);
        }

        void checkpoint_cancelled()
        {
            if (current_request_cancelled())
                throw RequestCancelledError{};
        }

        void handle_cancel_request(protocol::RpcInfo const& rpc)
        {
            if (!rpc.params.has_value())
            {
                std::println(m_log, "[dccd] $/cancelRequest: missing params");
                return;
            }

            auto params = protocol::CancelParams::from_json(rpc.params.value());
            if (!params.id.valid())
            {
                std::println(m_log, "[dccd] $/cancelRequest: id must be a number or a string");
                return;
            }

            bool cancelled = m_cancellation->cancel(params.id);
            std::println(m_log, "[dccd] $/cancelRequest: id={} {}", params.id.to_json().serialize(), cancelled ? "cancelled" : "no-op (not pending)");
        }

        bool ensure_graph_fresh(std::string const& uri)
        {
            if (m_stale_uris.contains(uri))
            {
                std::println(m_log, "[dccd] ensure_graph_fresh: URI marked stale (previous update failed) for {}; refusing", uri);
                return false;
            }

            if (graph_snapshot_fresh())
                return true;

            if (m_recompiling)
            {
                std::println(m_log, "[dccd] ensure_graph_fresh: already recompiling; not recursing for {}", uri);
                return false;
            }

            RecompilingGuard guard{m_recompiling};
            if (!m_active_entry_uri.empty())
                recompile_document(m_active_entry_uri);
            else
                recompile_document(uri);

            return graph_snapshot_fresh();
        }

        [[nodiscard]] bool analyzed_file_current(std::string const& uri) const noexcept
        {
            auto opt_fid = m_session->source_manager().find_by_uri(uri);
            if (!opt_fid)
                return false;

            auto it = m_graph_revisions.find(*opt_fid);
            if (it == m_graph_revisions.end())
                return true;

            return it->second == m_session->source_manager().content_revision(*opt_fid);
        }

        [[nodiscard]] std::uint64_t content_revision_for_uri(std::string const& uri) const noexcept
        {
            auto opt_fid = m_session->source_manager().find_by_uri(uri);
            if (!opt_fid)
                return 0;

            return m_session->source_manager().content_revision(*opt_fid);
        }

        [[nodiscard]] bool diagnostic_cache_fresh(std::string const& uri) const
        {
            auto it = m_diagnostic_cache.find(uri);
            if (it == m_diagnostic_cache.end())
                return false;

            auto const& entry = it->second;

            if (entry.graph_generation != m_graph_generation)
                return false;

            auto opt_fid = m_session->source_manager().find_by_uri(uri);
            if (!opt_fid)
                return false;

            auto const* sf = m_session->source_manager().get(*opt_fid);
            if (!sf || sf->content_revision() != entry.content_revision)
                return false;

            if (entry.version.has_value())
            {
                auto current = version_for_uri(uri);
                if (!current || *current != *entry.version)
                    return false;
            }

            return true;
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_initialize(protocol::RpcInfo const& rpc)
        {
            m_workspace_roots.clear();

            protocol::InitializeParams init_params;
            if (rpc.params.has_value())
            {
                init_params = protocol::InitializeParams::from_json(rpc.params.value());

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

            m_did_change_watched_files_supported = init_params.didChangeWatchedFilesDynamicRegistration;
            m_watch_registration_sent = false;
            std::println(m_log, "[dccd] initialize: didChangeWatchedFiles dynamicRegistration = {}",
                         m_did_change_watched_files_supported ? "supported" : "not supported");

            auto const selected = select_position_encoding(init_params);
            m_session->source_manager().set_position_encoding(selected);
            std::println(m_log, "[dccd] initialize: position encoding = {}", dcc::sm::to_string(selected));

            if (!m_workspace_roots.empty())
            {
                std::ranges::sort(m_workspace_roots);
                auto [first, last] = std::ranges::unique(m_workspace_roots);
                m_workspace_roots.erase(first, last);

                for (auto& root : m_workspace_roots)
                    root = normalize_path(std::move(root));
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
                    parse_lsp_configuration(cfg);
                }
            }

            load_compilation_databases();

            return protocol::build_response(rpc.id.value(), protocol::make_initialize_result(dcc::sm::to_string(selected)));
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_initialized(protocol::RpcInfo const&)
        {
            maybe_register_watched_files();
            return std::nullopt;
        }

        void maybe_register_watched_files()
        {
            if (!m_did_change_watched_files_supported || m_watch_registration_sent)
                return;

            m_watch_registration_sent = true;
            std::println(m_log, "[dccd] initialized: registering watched files via {}", protocol::kClientRegisterCapabilityMethod);
            send_message(protocol::build_register_capability_request());
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_shutdown(protocol::RpcInfo const& rpc)
        {
            return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
        }

        [[nodiscard]] bool is_stale_version(std::string const& uri, std::optional<std::int64_t> incoming_version, bool reject_equal) const
        {
            if (!incoming_version)
                return false;

            auto opt_fid = m_session->source_manager().find_by_uri(uri);
            if (!opt_fid)
                return false;

            auto const* sf = m_session->source_manager().get(*opt_fid);
            if (!sf || !sf->is_in_memory() || sf->is_closed())
                return false;

            auto current = sf->version();
            if (!current)
                return false;

            if (reject_equal)
                return *incoming_version <= *current;
            return *incoming_version < *current;
        }

        void handle_did_open(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DidOpenTextDocumentParams::from_json(rpc.params.value());

            if (dcc::vfs::is_dcc_core_uri(params.textDocument.uri))
            {
                std::println(m_log, "[dccd] didOpen: rejecting read-only dcc-core: document {}", params.textDocument.uri);
                return;
            }

            if (is_stale_version(params.textDocument.uri, params.textDocument.version, false))
            {
                std::println(m_log, "[dccd] didOpen: rejecting stale/duplicate open version {} for already-open document {}", params.textDocument.version,
                             params.textDocument.uri);
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
            publish_all_diagnostics();
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

            if (is_stale_version(params.textDocument.uri, params.textDocument.version, true))
            {
                std::println(m_log, "[dccd] didChange: rejecting stale/duplicate version {} for {}", params.textDocument.version, params.textDocument.uri);
                return;
            }

            auto const& last_change = params.contentChanges.back();

            auto result = m_session->update_in_memory(params.textDocument.uri, last_change.text, params.textDocument.version);
            if (!result)
            {
                std::println(m_log, "[dccd] update_in_memory failed for {}: {}", params.textDocument.uri, dcc::sm::to_string(result.error()));
                m_stale_uris.insert(params.textDocument.uri);
                publish_empty_diagnostics(params.textDocument.uri, params.textDocument.version);
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
            publish_all_diagnostics();
        }

        void handle_did_close(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DidCloseTextDocumentParams::from_json(rpc.params.value());

            std::optional<std::int64_t> known_version;
            if (auto fid = m_session->source_manager().find_by_uri(params.uri))
                if (auto const* sf = m_session->source_manager().get(*fid))
                    known_version = sf->version();

            auto result = m_session->close_in_memory(params.uri);
            if (!result)
                std::println(m_log, "[dccd] close_in_memory failed for {}: {}", params.uri, dcc::sm::to_string(result.error()));

            clear_stale_marker(params.uri);

            publish_empty_diagnostics(params.uri, known_version);
            m_published_uris.erase(params.uri);
        }

        [[nodiscard]] static dcc::sm::Position protocol_position_to_sm_position(protocol::LspPosition const& pos) noexcept
        {
            return dcc::sm::Position{pos.line, pos.character};
        }

        [[nodiscard]] static dcc::sm::PositionEncoding select_position_encoding(protocol::InitializeParams const& params) noexcept
        {
            for (auto const& enc : params.positionEncodings)
            {
                if (enc == protocol::PositionEncoding::Utf8)
                    return dcc::sm::PositionEncoding::Utf8;
                if (enc == protocol::PositionEncoding::Utf16)
                    return dcc::sm::PositionEncoding::Utf16;
                if (enc == protocol::PositionEncoding::Utf32)
                    return dcc::sm::PositionEncoding::Utf32;
            }

            return dcc::sm::PositionEncoding::Utf16;
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

        static void sort_dedup_source_ranges(std::vector<dcc::sm::SourceRange>& ranges)
        {
            std::ranges::sort(ranges, [](dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) {
                auto fa = static_cast<std::uint32_t>(a.begin.fileId);
                auto fb = static_cast<std::uint32_t>(b.begin.fileId);
                if (fa != fb)
                    return fa < fb;
                if (a.begin.offset != b.begin.offset)
                    return a.begin.offset < b.begin.offset;
                return a.end.offset < b.end.offset;
            });
            auto [first, last] = std::ranges::unique(ranges, [](dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) {
                return a.begin.fileId == b.begin.fileId && a.begin.offset == b.begin.offset && a.end.offset == b.end.offset;
            });
            ranges.erase(first, last);
        }

        struct RankedLabel
        {
            dcc::diag::Label const* label;
            std::size_t length;
        };

        [[nodiscard]] static std::vector<RankedLabel> rank_labels(std::span<dcc::diag::Label const> labels, dcc::sm::FileId target_fid)
        {
            std::vector<RankedLabel> out;
            for (auto const& label : labels)
            {
                if (!label.range.valid())
                    continue;

                if (target_fid != dcc::sm::FileId::Invalid && (label.range.begin.fileId != target_fid || label.range.end.fileId != target_fid))
                    continue;

                out.push_back({&label, static_cast<std::size_t>(label.range.end.offset - label.range.begin.offset)});
            }

            std::ranges::stable_sort(out, [](RankedLabel const& a, RankedLabel const& b) {
                bool a_primary = a.label->style == dcc::diag::LabelStyle::Primary;
                bool b_primary = b.label->style == dcc::diag::LabelStyle::Primary;
                if (a_primary != b_primary)
                    return a_primary;

                if (a.length != b.length)
                    return a.length < b.length;

                return false;
            });

            return out;
        }

        [[nodiscard]] static std::optional<protocol::LspRange> pick_primary_range(dcc::sm::SourceManager const& sm, std::span<dcc::diag::Label const> labels,
                                                                                  dcc::sm::FileId target_fid)
        {
            auto ranked = rank_labels(labels, target_fid);
            for (auto const& cand : ranked)
            {
                auto const* label = cand.label;

                auto start_pos = sm.location_to_lsp_position(label->range.begin);
                auto end_pos = sm.location_to_lsp_position(label->range.end);
                if (!start_pos || !end_pos)
                    continue;

                protocol::LspRange range;
                range.start.line = start_pos->line;
                range.start.character = start_pos->character;
                range.end.line = end_pos->line;
                range.end.character = end_pos->character;
                return range;
            }

            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::int64_t> version_for_uri(std::string const& uri) const
        {
            auto opt_fid = m_session->source_manager().find_by_uri(uri);
            if (!opt_fid)
                return std::nullopt;

            auto const* sf = m_session->source_manager().get(*opt_fid);
            if (!sf || !sf->is_in_memory() || sf->is_closed())
                return std::nullopt;

            return sf->version();
        }

        [[nodiscard]] static protocol::LspLocation make_location_at_start(std::string uri)
        {
            protocol::LspLocation loc;
            loc.uri = std::move(uri);

            return loc;
        }

        [[nodiscard]] std::optional<std::string> resolve_import_uri(std::string_view module_path)
        {
            if (auto const* entry = dcc::vfs::lookup_by_module_path(module_path))
                return std::string{entry->uri};

            auto* sema_ctx = m_session->sema_context();
            if (!sema_ctx)
                return std::nullopt;

            auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
            auto const& sm = m_session->source_manager();

            for (auto const& mod : graph.all())
            {
                if (mod->canonical_path.str() == module_path)
                {
                    auto const* sf = sm.get(mod->file_id);
                    if (sf)
                        return sf->uri();

                    return std::nullopt;
                }
            }

            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string> resolve_import_decl_uri(dcc::ast::ImportDecl const& import_decl)
        {
            auto* sema_ctx = m_session->sema_context();
            if (sema_ctx)
            {
                auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
                auto const& sm = m_session->source_manager();

                for (auto const& mod : graph.all())
                {
                    for (auto const& binding : mod->imports)
                    {
                        if (binding.decl == &import_decl && binding.target)
                        {
                            auto const* sf = sm.get(binding.target->file_id);
                            if (sf)
                                return sf->uri();
                        }
                    }
                }
            }

            auto module_path_str = dcc::sema::ModulePath::from_ast(import_decl.module_path).str();
            return resolve_import_uri(module_path_str);
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_definition(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DefinitionParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (!ensure_graph_fresh(params.textDocument.uri))
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto symbol = dcc::query::resolve_symbol_at(*m_session, *fid_opt, sm_pos);
            if (!symbol || !symbol->has_target())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (symbol->kind == dcc::query::SymbolKind::ImportAlias || symbol->kind == dcc::query::SymbolKind::Module)
            {
                if (symbol->via_import)
                {
                    auto uri = resolve_import_decl_uri(*symbol->via_import);
                    if (uri)
                    {
                        auto loc = make_location_at_start(std::move(*uri));
                        return protocol::build_response(rpc.id.value(), loc.to_json());
                    }
                }

                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            if (!symbol->definition_range.valid())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto loc = source_range_to_lsp_location(symbol->definition_range);
            if (!loc)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (symbol->via_using)
            {
                auto using_range = dcc::query::decl_name_range(symbol->via_using);
                if (!using_range.valid())
                    using_range = symbol->via_using->range;

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

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (!ensure_graph_fresh(params.textDocument.uri))
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            {
                auto loc_result = m_session->source_manager().lsp_position_to_location(*fid_opt, sm_pos);
                if (loc_result)
                {
                    auto asm_hover = try_asm_hover(*fid_opt, *loc_result);
                    if (asm_hover)
                    {
                        std::println(m_log, "[dccd] hover: returning inline-asm hover info");
                        return protocol::build_response(rpc.id.value(), asm_hover->to_json());
                    }
                }
            }

            auto symbol = dcc::query::resolve_symbol_at(*m_session, *fid_opt, sm_pos);
            if (!symbol || !symbol->has_target())
            {
                auto node = query_at_params(params.textDocument.uri, sm_pos);
                if (node && node->resolved_type)
                {
                    protocol::Hover hover;
                    hover.contents.kind = "markdown";
                    hover.contents.value = std::format("```dc\n{}\n```", format_dcc_type(node->resolved_type));
                    return protocol::build_response(rpc.id.value(), hover.to_json());
                }

                std::println(m_log, "[dccd] hover: no resolved symbol");
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            auto markdown = hover_markdown(*symbol);
            if (markdown.empty())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            protocol::Hover hover;
            hover.contents.kind = "markdown";
            hover.contents.value = std::move(markdown);
            return protocol::build_response(rpc.id.value(), hover.to_json());
        }

        [[nodiscard]] std::string hover_markdown(dcc::query::ResolvedSymbol const& symbol)
        {
            switch (symbol.kind)
            {
                case dcc::query::SymbolKind::Field: {
                    auto const* f = field_of_symbol(symbol);
                    if (!f)
                        return {};
                    std::string type_str = "<unknown>";
                    if (f->type && f->type->sema.canonical)
                        type_str = format_dcc_type(dcc::sema::get_canonical(f->type->sema));
                    return std::format("```dc\n{} {}\n```", type_str, f->name);
                }
                case dcc::query::SymbolKind::FuncParam: {
                    auto const* fd = symbol.owner_decl && symbol.owner_decl->kind == dcc::ast::DeclKind::Func
                                         ? static_cast<dcc::ast::FuncDecl const*>(symbol.owner_decl)
                                         : nullptr;
                    if (fd)
                    {
                        if (symbol.sub_index >= fd->params.size())
                            return {};
                        auto const& p = fd->params[symbol.sub_index];
                        std::string type_str;
                        if (p.type && p.type->sema.canonical)
                            type_str = format_dcc_type(dcc::sema::get_canonical(p.type->sema));
                        else
                            type_str = "<template-dependent>";

                        if (p.name.empty())
                            return std::format("```dc\n{}\n```", type_str);
                        return std::format("```dc\n{} {}\n```", type_str, p.name);
                    }

                    std::string_view name = symbol.name;
                    std::string type_str = "<unknown>";
                    if (auto* sema_ctx = m_session->sema_context())
                    {
                        auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
                        for (auto const& mod : graph.all())
                        {
                            if (!mod)
                                continue;
                            for (auto* lf : mod->lambda_funcs)
                            {
                                if (!lf || !lf->lambda_source)
                                    continue;
                                auto const* l = lf->lambda_source;
                                if (!l->range.valid())
                                    continue;
                                if (l->range.begin.fileId != symbol.id.owner_file || l->range.begin.offset != symbol.id.owner_offset)
                                    continue;
                                if (symbol.sub_index >= lf->params.size())
                                    break;
                                auto const& lp = lf->params[symbol.sub_index];
                                if (name.empty())
                                    name = lp.name;
                                if (lp.type && lp.type->sema.canonical)
                                    type_str = format_dcc_type(dcc::sema::get_canonical(lp.type->sema));
                                break;
                            }
                        }
                    }
                    if (name.empty())
                        return std::format("```dc\n{}\n```", type_str);
                    return std::format("```dc\n{} {}\n```", type_str, name);
                }
                case dcc::query::SymbolKind::TemplateParam: {
                    std::string_view name = symbol.name.empty() ? "<unnamed>" : symbol.name;
                    return std::format("```dc\ntemplate parameter {}\n```", name);
                }
                case dcc::query::SymbolKind::EnumVariant: {
                    std::string_view enum_name = "<enum>";
                    if (symbol.owner_decl && symbol.owner_decl->kind == dcc::ast::DeclKind::Enum)
                        enum_name = static_cast<dcc::ast::EnumDecl const*>(symbol.owner_decl)->name;
                    return std::format("```dc\n{}::{}\n```", enum_name, symbol.name);
                }
                case dcc::query::SymbolKind::UsingAlias: {
                    auto const* ud = symbol.decl ? dcc::ast::node_cast<dcc::ast::UsingDecl>(symbol.decl) : nullptr;
                    if (!ud)
                        return {};
                    std::string name_str = symbol.name.empty() ? "<unnamed>" : std::string{symbol.name};

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

                    return std::format("```dc\nusing {} = {}\n```", name_str, target_str);
                }
                case dcc::query::SymbolKind::Module:
                case dcc::query::SymbolKind::ImportAlias:
                    return std::format("```dc\nmodule {}\n```", symbol.name);
                case dcc::query::SymbolKind::Declaration:
                    break;
                default:
                    return {};
            }

            auto const* target = symbol.decl;
            if (!target)
            {
                if (symbol.kind == dcc::query::SymbolKind::Declaration)
                    return std::format("```dc\n{}\n```", symbol.name);
                return {};
            }

            return std::format("```dc\n{}\n```", dcc::query::symbol_display_name(symbol));
        }

        [[nodiscard]] static dcc::ast::FieldDecl const* field_of_symbol(dcc::query::ResolvedSymbol const& symbol)
        {
            if (symbol.kind != dcc::query::SymbolKind::Field || !symbol.owner_decl)
                return nullptr;

            if (auto const* sd = dcc::ast::node_cast<dcc::ast::StructDecl>(symbol.owner_decl))
            {
                if (symbol.sub_index < sd->fields.size())
                    return &sd->fields[symbol.sub_index];
            }
            else if (auto const* ud = dcc::ast::node_cast<dcc::ast::UnionDecl>(symbol.owner_decl))
            {
                if (symbol.sub_index < ud->fields.size())
                    return &ud->fields[symbol.sub_index];
            }

            return nullptr;
        }

        [[nodiscard]] std::optional<protocol::Hover> try_asm_hover(dcc::sm::FileId fid, dcc::sm::Location cursor_loc)
        {
            dcc::ast::TranslationUnit const* tu = nullptr;
            {
                auto* sema_ctx = m_session->sema_context();
                if (sema_ctx)
                {
                    auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
                    for (auto const& mod : graph.all())
                    {
                        if (mod->file_id == fid && mod->tu)
                        {
                            tu = mod->tu;
                            break;
                        }
                    }
                    if (!tu)
                        tu = graph.find_tu_for_file(fid);
                }
            }
            if (!tu)
                tu = m_session->parse_file(fid);

            if (!tu)
                return std::nullopt;

            auto& sm = m_session->source_manager();

            auto check_asm_template = [&](dcc::sm::SourceRange const& template_range, auto const& placeholder_spans,
                                          auto const& operands) -> std::optional<protocol::Hover> {
                if (!template_range.valid())
                    return std::nullopt;
                if (cursor_loc.fileId != template_range.begin.fileId)
                    return std::nullopt;
                if (cursor_loc.offset < template_range.begin.offset || cursor_loc.offset > template_range.end.offset)
                    return std::nullopt;

                auto content_base = template_range.begin.offset + 1;

                for (auto const& span : placeholder_spans)
                {
                    dcc::sm::Offset span_start;
                    dcc::sm::Offset span_end;
                    if (span.raw_range.valid() && span.raw_range.begin.fileId == template_range.begin.fileId)
                    {
                        span_start = span.raw_range.begin.offset;
                        span_end = span.raw_range.end.offset;
                    }
                    else
                    {
                        span_start = content_base + static_cast<dcc::sm::Offset>(span.byte_offset);
                        span_end = span_start + static_cast<dcc::sm::Offset>(span.byte_length);
                    }

                    if (cursor_loc.offset >= span_start && cursor_loc.offset <= span_end)
                    {
                        std::string hover_text;

                        switch (span.kind)
                        {
                            case dcc::ast::AsmPlaceholderSpan::Kind::OperandRef: {
                                if (span.operand_index < operands.size())
                                {
                                    auto const& op = operands[span.operand_index];
                                    std::string dir_str;
                                    switch (op.direction)
                                    {
                                        case dcc::ast::AsmOperandDirection::Out:
                                            dir_str = "out";
                                            break;
                                        case dcc::ast::AsmOperandDirection::In:
                                            dir_str = "in";
                                            break;
                                        case dcc::ast::AsmOperandDirection::InOut:
                                            dir_str = "inout";
                                            break;
                                    }
                                    std::string place_str;
                                    switch (op.placement_kind)
                                    {
                                        case dcc::ast::AsmPlacementKind::Reg:
                                            place_str = op.reg_name.empty() ? "reg" : std::format("reg[{}]", op.reg_name);
                                            break;
                                        case dcc::ast::AsmPlacementKind::RegPair:
                                            place_str = std::format("pair[{},{}]", op.reg_name, op.reg_name2);
                                            break;
                                        case dcc::ast::AsmPlacementKind::Mem:
                                            place_str = "mem";
                                            break;
                                        case dcc::ast::AsmPlacementKind::Imm:
                                            place_str = "imm";
                                            break;
                                    }

                                    std::string type_str = "<unknown>";
                                    if (op.type_override && op.type_override->sema.canonical)
                                        type_str = format_dcc_type(dcc::sema::get_canonical(op.type_override->sema));
                                    else if (op.expr)
                                    {
                                        auto const* resolved = dcc::sema::get_resolved_type(op.expr->sema);
                                        if (resolved)
                                            type_str = format_dcc_type(resolved);
                                    }

                                    hover_text = std::format("operand `{}`: {} (direction: {}, placement: {})", span.name, type_str, dir_str, place_str);
                                }
                                else
                                    hover_text = std::format("operand `{}` (unresolved)", span.name);

                                break;
                            }
                            case dcc::ast::AsmPlaceholderSpan::Kind::RegLiteral: {
                                // TODO(asm): use actual arch from session's TargetConfig
                                auto arch = dcc::target::TargetConfig::host_default().arch;
                                auto const* reg = dcc::target::lookup_register(arch, span.name);
                                if (reg)
                                {
                                    std::string cls_str;
                                    switch (reg->cls)
                                    {
                                        case dcc::target::PhysRegClass::GPR:
                                            cls_str = "GPR";
                                            break;
                                        case dcc::target::PhysRegClass::XMM:
                                            cls_str = "XMM";
                                            break;
                                        case dcc::target::PhysRegClass::Seg:
                                            cls_str = "Seg";
                                            break;
                                        case dcc::target::PhysRegClass::Flags:
                                            cls_str = "Flags";
                                            break;
                                        case dcc::target::PhysRegClass::ST:
                                            cls_str = "ST";
                                            break;
                                    }
                                    hover_text = std::format("register `{}`: {}, {}-bit", reg->name, cls_str, reg->width);
                                }
                                else
                                {
                                    hover_text = std::format("register `{}` (unknown)", span.name);
                                }
                                break;
                            }
                            case dcc::ast::AsmPlaceholderSpan::Kind::Unresolved: {
                                hover_text = std::format("`{}` (unresolved placeholder)", span.name);
                                break;
                            }
                        }

                        if (!hover_text.empty())
                        {
                            protocol::Hover hover;
                            hover.contents.kind = "markdown";
                            hover.contents.value = std::format("```dc\n{}\n```", hover_text);

                            auto start_pos_opt = sm.location_to_lsp_position(dcc::sm::Location{fid, span_start});
                            auto end_pos_opt = sm.location_to_lsp_position(dcc::sm::Location{fid, span_end});
                            if (start_pos_opt && end_pos_opt)
                            {
                                auto const& sp = *start_pos_opt;
                                auto const& ep = *end_pos_opt;
                                hover.range = protocol::LspRange{};
                                hover.range->start.line = sp.line;
                                hover.range->start.character = sp.character;
                                hover.range->end.line = ep.line;
                                hover.range->end.character = ep.character;
                            }

                            return hover;
                        }
                    }
                }
                return std::nullopt;
            };

            std::function<void(dcc::ast::Decl const*)> walk_decls;
            std::function<void(dcc::ast::Stmt const*)> walk_stmts;
            std::function<void(dcc::ast::Expr const*)> walk_exprs;

            walk_decls = [&](dcc::ast::Decl const* decl) -> void {
                if (!decl)
                    return;

                if (decl->kind == dcc::ast::DeclKind::Func)
                {
                    auto const* fd = static_cast<dcc::ast::FuncDecl const*>(decl);
                    if (fd->body.has_value())
                    {
                        for (auto* s : fd->body->stmts)
                            walk_stmts(s);
                        walk_exprs(fd->body->tail);
                    }
                }
            };

            walk_stmts = [&](dcc::ast::Stmt const* stmt) -> void {
                if (!stmt)
                    return;

                if (stmt->kind == dcc::ast::StmtKind::Asm)
                {
                    auto const* s = static_cast<dcc::ast::AsmStmt const*>(stmt);
                    auto result = check_asm_template(s->template_range, s->placeholder_spans, s->operands);
                    if (result)
                        throw result;
                }
                if (stmt->kind == dcc::ast::StmtKind::Expr)
                {
                    auto const* es = static_cast<dcc::ast::ExprStmt const*>(stmt);
                    walk_exprs(es->expr);
                }
                else if (stmt->kind == dcc::ast::StmtKind::DeclStmt)
                {
                    auto const* ds = static_cast<dcc::ast::DeclStmt const*>(stmt);
                    walk_decls(ds->decl);
                }
            };

            walk_exprs = [&](dcc::ast::Expr const* expr) -> void {
                if (!expr)
                    return;

                if (expr->kind == dcc::ast::ExprKind::Asm)
                {
                    auto const* e = static_cast<dcc::ast::AsmExpr const*>(expr);
                    auto result = check_asm_template(e->template_range, e->placeholder_spans, e->operands);
                    if (result)
                        throw result;
                }
            };

            try
            {
                for (auto* d : tu->imports)
                    walk_decls(d);
                for (auto* d : tu->decls)
                    walk_decls(d);
            }
            catch (std::optional<protocol::Hover> result)
            {
                return result;
            }

            return std::nullopt;
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

            if (!ensure_graph_fresh(params.textDocument.uri))
            {
                std::println(m_log, "[dccd] semanticTokens/full: stale graph for {}; returning empty", params.textDocument.uri);
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

            auto data = dccd::semantic_tokens::collect_tokens(m_session->source_manager(), tu, requested_fid, [this] { return current_request_cancelled(); });

            checkpoint_cancelled();

            protocol::SemanticTokens result;
            result.data = std::move(data);
            return protocol::build_response(rpc.id.value(), result.to_json());
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_completion(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::CompletionParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);

            if (!ensure_graph_fresh(params.textDocument.uri))
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto completion_list = dccd::completion::compute_completions(*m_session, params.textDocument.uri, sm_pos);

            checkpoint_cancelled();

            return protocol::build_response(rpc.id.value(), completion_list.to_json());
        }

        [[nodiscard]] protocol::SignatureInformation build_signature_information(dcc::ast::FuncDecl const* target)
        {
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
            return sig_info;
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_signature_help(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::SignatureHelpParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (!ensure_graph_fresh(params.textDocument.uri))
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto loc_opt = m_session->source_manager().lsp_position_to_location(*fid_opt, sm_pos);
            if (!loc_opt)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto active = dcc::query::find_active_call(*m_session, *fid_opt, *loc_opt);
            if (!active || !active->in_call_arguments)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            dcc::sema::Scope const* scope = nullptr;
            dcc::sema::ModuleInfo const* module = nullptr;
            {
                auto node = query_at_params(params.textDocument.uri, sm_pos);
                if (node)
                {
                    scope = node->scope;
                    module = node->module;
                }
                else if (auto* sema_ctx = m_session->sema_context())
                {
                    auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
                    for (auto const& mod : graph.all())
                        if (mod->file_id == *fid_opt)
                        {
                            module = mod.get();
                            break;
                        }
                }
            }

            std::vector<dcc::ast::FuncDecl const*> candidates;
            std::unordered_set<dcc::ast::Decl const*> seen;
            dcc::ast::FuncDecl const* resolved_target = nullptr;

            auto add_candidate = [&](dcc::ast::FuncDecl const* fd, bool resolved) {
                if (!fd || !seen.insert(fd).second)
                    return;
                candidates.push_back(fd);
                if (resolved && !resolved_target)
                    resolved_target = fd;
            };

            auto add_syms = [&](std::span<dcc::sema::Symbol const> syms) {
                for (auto const& sym : syms)
                    if (sym.decl && sym.decl->kind == dcc::ast::DeclKind::Func)
                        add_candidate(static_cast<dcc::ast::FuncDecl const*>(sym.decl), false);
            };

            if (active->call)
            {
                auto const* call = active->call;
                if (call->sema.ufcs_callee && call->sema.ufcs_callee->kind == dcc::ast::DeclKind::Func)
                    add_candidate(static_cast<dcc::ast::FuncDecl const*>(call->sema.ufcs_callee), true);
                if (call->sema.resolved_specialization)
                    add_candidate(call->sema.resolved_specialization, true);
                if (call->sema.resolved_decl && call->sema.resolved_decl->kind == dcc::ast::DeclKind::Func)
                    add_candidate(static_cast<dcc::ast::FuncDecl const*>(call->sema.resolved_decl), true);
            }
            if (active->callee_expr)
            {
                if (active->callee_expr->sema.resolved_specialization)
                    add_candidate(active->callee_expr->sema.resolved_specialization, true);
                if (active->callee_expr->sema.resolved_decl && active->callee_expr->sema.resolved_decl->kind == dcc::ast::DeclKind::Func)
                    add_candidate(static_cast<dcc::ast::FuncDecl const*>(active->callee_expr->sema.resolved_decl), true);
            }

            if (!active->callee_name.empty())
            {
                auto callee_name = m_session->interner().intern(active->callee_name);
                if (scope)
                    add_syms(scope->lookup_values(callee_name));
                if (module)
                {
                    if (module->ufcs_scope)
                        add_syms(module->ufcs_scope->lookup_values(callee_name));
                    if (module->own_scope)
                        add_syms(module->own_scope->lookup_values(callee_name));
                    for (auto const& imp : module->imports)
                        if (imp.target && imp.target->export_scope)
                            add_syms(imp.target->export_scope->lookup_values(callee_name));
                }
            }

            if (candidates.empty())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            std::uint32_t explicit_args = active->explicit_argument_count;
            std::uint32_t min_params = active->ufcs ? explicit_args + 1 : explicit_args;

            std::vector<protocol::SignatureInformation> sigs;
            sigs.reserve(candidates.size());
            std::size_t active_sig = 0;
            std::size_t best_excess = std::numeric_limits<std::size_t>::max();

            for (std::size_t idx = 0; idx < candidates.size(); ++idx)
            {
                auto const* fd = candidates[idx];
                auto sig = build_signature_information(fd);

                std::uint32_t ap = active->active_parameter;
                if (!sig.parameters.empty())
                    ap = std::min(ap, static_cast<std::uint32_t>(sig.parameters.size() - 1));
                else
                    ap = 0;
                sig.activeParameter = ap;

                sigs.push_back(std::move(sig));

                if (fd == resolved_target)
                {
                    active_sig = idx;
                    best_excess = 0;
                }
                else if (best_excess != 0)
                {
                    std::size_t excess = (fd->params.size() >= min_params) ? fd->params.size() - min_params : std::numeric_limits<std::size_t>::max();
                    if (excess < best_excess)
                    {
                        best_excess = excess;
                        active_sig = idx;
                    }
                }
            }

            protocol::SignatureHelp help;
            help.signatures = std::move(sigs);
            help.activeSignature = static_cast<std::uint32_t>(active_sig);

            std::uint32_t active_param = active->active_parameter;
            if (!help.signatures[active_sig].parameters.empty())
                active_param = std::min(active_param, static_cast<std::uint32_t>(help.signatures[active_sig].parameters.size() - 1));
            else
                active_param = 0;
            help.activeParameter = active_param;

            checkpoint_cancelled();

            return protocol::build_response(rpc.id.value(), help.to_json());
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_references(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::ReferenceParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (!ensure_graph_fresh(params.textDocument.uri))
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto symbol = dcc::query::resolve_symbol_at(*m_session, *fid_opt, sm_pos);
            if (!symbol || !symbol->has_target())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto& sm = m_session->source_manager();

            std::vector<dcc::sm::SourceRange> ref_ranges;
            if (m_workspace_index.module_fresh(symbol->id.file, sm.content_revision(symbol->id.file)))
            {
                ref_ranges = m_workspace_index.occurrences_for(symbol->id);
                if (params.context.includeDeclaration && symbol->definition_range.valid())
                    ref_ranges.push_back(symbol->definition_range);
                sort_dedup_source_ranges(ref_ranges);
            }
            else
                ref_ranges = dcc::query::find_symbol_references(*m_session, *symbol, params.context.includeDeclaration);

            checkpoint_cancelled();

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

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (!ensure_graph_fresh(params.textDocument.uri))
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto symbol = dcc::query::resolve_symbol_at(*m_session, *fid_opt, sm_pos);
            if (!symbol || !symbol->has_target())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto& sm = m_session->source_manager();

            std::vector<dcc::sm::SourceRange> ref_ranges;
            if (m_workspace_index.module_fresh(symbol->id.file, sm.content_revision(symbol->id.file)))
            {
                ref_ranges = m_workspace_index.occurrences_for(symbol->id);
                if (symbol->definition_range.valid())
                    ref_ranges.push_back(symbol->definition_range);
                sort_dedup_source_ranges(ref_ranges);
            }
            else
            {
                ref_ranges = dcc::query::find_symbol_references(*m_session, *symbol, true);
            }

            checkpoint_cancelled();

            auto active_fid = *fid_opt;

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

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (!ensure_graph_fresh(params.textDocument.uri))
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto symbol = dcc::query::resolve_symbol_at(*m_session, *fid_opt, sm_pos);
            if (!symbol || !symbol->has_target())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (!dcc::query::can_rename_symbol(*symbol))
            {
                auto reason = std::string{"not renameable"};
                if (symbol->is_module)
                    reason = "module/import aliases cannot be renamed";
                else if (symbol->is_ambiguous)
                    reason = "ambiguous symbol; refusing to guess";
                else if (symbol->is_external_alias)
                    reason = "symbol reached through an import/using alias cannot be safely renamed";
                else
                    reason = "symbol has no renamable name range";

                std::println(m_log, "[dccd] textDocument/rename: refusing: {}", reason);
                return protocol::build_error_response(rpc.id.value(), -32602, std::format("Cannot rename: {}", reason));
            }

            if (!is_valid_identifier(params.newName))
            {
                std::println(m_log, "[dccd] textDocument/rename: invalid newName \"{}\"", params.newName);
                return protocol::build_error_response(rpc.id.value(), -32602, std::format("`{}` is not a valid identifier", params.newName));
            }

            auto& sm = m_session->source_manager();

            std::vector<dcc::sm::SourceRange> ref_ranges;
            if (m_workspace_index.module_fresh(symbol->id.file, sm.content_revision(symbol->id.file)))
            {
                ref_ranges = m_workspace_index.occurrences_for(symbol->id);
                if (symbol->definition_range.valid())
                    ref_ranges.push_back(symbol->definition_range);
                sort_dedup_source_ranges(ref_ranges);
            }
            else
            {
                ref_ranges = dcc::query::find_symbol_references(*m_session, *symbol, true);
            }

            checkpoint_cancelled();

            protocol::WorkspaceEdit we;
            for (auto const& range : ref_ranges)
            {
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

        [[nodiscard]] std::optional<protocol::JsonValue> handle_prepare_rename(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::PrepareRenameParams::from_json(rpc.params.value());
            auto sm_pos = protocol_position_to_sm_position(params.position);

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            if (!ensure_graph_fresh(params.textDocument.uri))
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto symbol = dcc::query::resolve_symbol_at(*m_session, *fid_opt, sm_pos);
            if (!symbol || !symbol->has_target() || !dcc::query::can_rename_symbol(*symbol))
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto name_range = dcc::query::symbol_name_range(*symbol);
            if (!name_range.valid())
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto& sm = m_session->source_manager();
            auto start_pos = sm.location_to_lsp_position(name_range.begin);
            auto end_pos = sm.location_to_lsp_position(name_range.end);
            if (!start_pos || !end_pos)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            protocol::PrepareRenameResult result;
            result.range.start.line = start_pos->line;
            result.range.start.character = start_pos->character;
            result.range.end.line = end_pos->line;
            result.range.end.character = end_pos->character;
            result.placeholder = symbol->name.empty() ? std::string{} : std::string{symbol->name};

            std::println(m_log, "[dccd] textDocument/prepareRename: range {},{}-{},{} placeholder=\"{}\"", result.range.start.line,
                         result.range.start.character, result.range.end.line, result.range.end.character, result.placeholder);

            return protocol::build_response(rpc.id.value(), result.to_json());
        }

        [[nodiscard]] static bool is_valid_identifier(std::string_view name)
        {
            if (name.empty())
                return false;

            auto is_ident_start = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
            auto is_ident_cont = [&](char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); };

            if (!is_ident_start(name.front()))
                return false;
            for (char c : name)
                if (!is_ident_cont(c))
                    return false;

            return dcc::lex::classify_identifier(name) == dcc::lex::TokenKind::Identifier;
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

            if (!ensure_graph_fresh(params.textDocument.uri))
            {
                std::println(m_log, "[dccd] codeAction: stale graph for {}; refusing cached fixes", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            auto cache_it = m_diagnostic_cache.find(params.textDocument.uri);
            if (cache_it == m_diagnostic_cache.end())
            {
                std::println(m_log, "[dccd] codeAction: no cached diagnostics for {}", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            if (!diagnostic_cache_fresh(params.textDocument.uri))
            {
                std::println(m_log, "[dccd] codeAction: stale diagnostic cache for {}; refusing cached fixes", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            auto& cached = cache_it->second.diagnostics;

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

            checkpoint_cancelled();

            std::println(m_log, "[dccd] codeAction: returning {} code action(s)", arr.array_size());
            return protocol::build_response(rpc.id.value(), std::move(arr));
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_inlay_hint(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::InlayHintParams::from_json(rpc.params.value());

            auto fid_opt = file_id_from_uri(params.textDocument.uri);
            if (!fid_opt)
            {
                std::println(m_log, "[dccd] inlayHint: cannot find file for {}", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            if (!ensure_graph_fresh(params.textDocument.uri))
            {
                std::println(m_log, "[dccd] inlayHint: stale graph for {}", params.textDocument.uri);
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::empty_array());
            }

            auto* sema_ctx = m_session->sema_context();
            if (!sema_ctx)
            {
                std::println(m_log, "[dccd] inlayHint: no sema context for {}", params.textDocument.uri);
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

            auto hints = dccd::inlay_hints::collect_inlay_hints(sm, module->tu, request_range, formatter, m_inlay_hint_options,
                                                                [this] { return current_request_cancelled(); });

            checkpoint_cancelled();

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

            auto const* sf = formatting_source_file(params.textDocument.uri);
            if (!sf)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto edit = dccd::format::format_document(*sf, m_session->interner(), params.options, m_session->source_manager().position_encoding());

            checkpoint_cancelled();

            if (!edit)
            {
                std::println(m_log, "[dccd] formatting: format_document returned null (lex errors or malformed input)");
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());
            }

            auto arr = protocol::JsonValue::empty_array();
            arr.push_back(edit->to_json());
            return protocol::build_response(rpc.id.value(), std::move(arr));
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_range_formatting(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DocumentRangeFormattingParams::from_json(rpc.params.value());

            std::println(m_log, "[dccd] rangeFormatting: uri={} range=({}:{})-({}:{}) tabSize={} insertSpaces={}", params.textDocument.uri,
                         params.range.start.line, params.range.start.character, params.range.end.line, params.range.end.character, params.options.tabSize,
                         params.options.insertSpaces);

            auto const* sf = formatting_source_file(params.textDocument.uri);
            if (!sf)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto edits = dccd::format::format_range(*sf, m_session->interner(), params.options, params.range, m_session->source_manager().position_encoding());

            checkpoint_cancelled();

            auto arr = protocol::JsonValue::empty_array();
            for (auto const& edit : edits)
                arr.push_back(edit.to_json());

            std::println(m_log, "[dccd] rangeFormatting: {} edit(s) for {}", arr.array_size(), params.textDocument.uri);

            return protocol::build_response(rpc.id.value(), std::move(arr));
        }

        [[nodiscard]] std::optional<protocol::JsonValue> handle_on_type_formatting(protocol::RpcInfo const& rpc)
        {
            auto params = protocol::DocumentOnTypeFormattingParams::from_json(rpc.params.value());

            std::println(m_log, "[dccd] onTypeFormatting: uri={} ch=\"{}\" position=({}:{}) tabSize={} insertSpaces={}", params.textDocument.uri, params.ch,
                         params.position.line, params.position.character, params.options.tabSize, params.options.insertSpaces);

            auto const* sf = formatting_source_file(params.textDocument.uri);
            if (!sf)
                return protocol::build_response(rpc.id.value(), protocol::JsonValue::null_val());

            auto edits = dccd::format::format_on_type(*sf, m_session->interner(), params.options, params.ch, params.position,
                                                      m_session->source_manager().position_encoding());

            checkpoint_cancelled();

            auto arr = protocol::JsonValue::empty_array();
            for (auto const& edit : edits)
                arr.push_back(edit.to_json());

            std::println(m_log, "[dccd] onTypeFormatting: {} edit(s) for {}", arr.array_size(), params.textDocument.uri);

            return protocol::build_response(rpc.id.value(), std::move(arr));
        }

        [[nodiscard]] dcc::sm::SourceFile const* formatting_source_file(std::string const& uri)
        {
            if (!m_session.has_value())
            {
                std::println(m_log, "[dccd] formatting: no session");
                return nullptr;
            }

            if (m_stale_uris.contains(uri))
            {
                std::println(m_log, "[dccd] formatting: refusing stale URI (previous didChange failed) for {}", uri);
                return nullptr;
            }

            auto fid_opt = file_id_from_uri(uri);
            if (!fid_opt)
            {
                std::println(m_log, "[dccd] formatting: cannot find file for {}", uri);
                return nullptr;
            }

            auto const* sf = m_session->source_manager().get(*fid_opt);
            if (!sf)
            {
                std::println(m_log, "[dccd] formatting: null source file for {}", uri);
                return nullptr;
            }

            return sf;
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

            auto symbols = m_workspace_index.search_symbols(*m_session, m_workspace_roots, params.query);

            checkpoint_cancelled();

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

        void recompile_document(std::string const& uri)
        {
            std::println(m_log, "[dccd] recompile_document: incoming URI=\"{}\"", uri);

            if (dcc::vfs::is_dcc_core_uri(uri))
            {
                std::println(m_log, "[dccd] recompile_document: skipping read-only dcc-core: URI {}", uri);
                return;
            }

            m_active_entry_uri = uri;

            auto path = dcc::sm::SourceManager::parse_file_uri(uri);
            if (!path)
            {
                auto fid = m_session->source_manager().find_by_uri(uri);
                auto const* sf = fid ? m_session->source_manager().get(*fid) : nullptr;
                if (sf && sf->kind() == dcc::sm::FileKind::InMemory && !sf->is_closed())
                {
                    path = sf->path();
                    std::println(m_log, "[dccd] recompile_document: resolved non-file URI to in-memory path \"{}\"", path->string());
                }
                else
                {
                    std::println(m_log, "[dccd] recompile_document: cannot resolve non-file URI to local path: {}", uri);
                    publish_empty_diagnostics(uri, version_for_uri(uri));
                    m_published_uris.erase(uri);
                    return;
                }
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

            std::vector<std::filesystem::path> roots;

            if (auto const* command = find_compile_command(*path))
            {
                std::println(m_log, "[dccd] compile command found for: {}", path->string());
                if (auto analysis = project_analysis_command(*command, m_log))
                {
                    if (analysis->target)
                        opts.target = *analysis->target;

                    opts.injected_decls = std::move(analysis->injected_decls);
                    opts.inject_libdcext_prelude = analysis->inject_libdcext_prelude;
                    roots = std::move(analysis->import_roots);

                    std::println(m_log, "[dccd] compile command: {} import roots, {} injected declarations, target={}", roots.size(),
                                 opts.injected_decls.size(), analysis->target ? analysis->target->triple : std::string{"(default)"});
                }
            }
            else
            {
                std::println(m_log, "[dccd] no compile command for \"{}\"; using fallback analysis configuration", path->string());
            }

            auto manual_roots = compute_import_roots();
            for (auto& r : manual_roots)
                roots.push_back(std::move(r));

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

            checkpoint_cancelled();

            m_workspace_index.sync(*m_session);

            m_graph_revisions.clear();
            if (auto* sema_ctx = m_session->sema_context())
            {
                auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
                auto const& sm = m_session->source_manager();
                for (auto const& mod : graph.all())
                    m_graph_revisions[mod->file_id] = sm.content_revision(mod->file_id);
            }
            ++m_graph_generation;

            checkpoint_cancelled();

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

        void load_config_file(std::filesystem::path const& config_path, std::filesystem::path const& base_dir, std::vector<std::filesystem::path>& out_paths,
                              std::optional<std::string>* out_compilation_database = nullptr)
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

            if (out_compilation_database)
                if (auto db = json->get_string("compilationDatabase"))
                    *out_compilation_database = std::move(*db);

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
            m_project_compilation_database.clear();
            for (auto const& root : m_workspace_roots)
            {
                auto config_path = project_config_path(root);
                std::println(m_log, "[dccd] reading project config: {}", config_path.string());
                std::optional<std::string> compilation_database;
                load_config_file(config_path, root, m_project_include_paths, &compilation_database);
                if (compilation_database)
                    m_project_compilation_database[root.string()] = std::move(*compilation_database);
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

        void parse_lsp_configuration(protocol::DidChangeConfigurationParams const& params)
        {
            m_lsp_include_paths.clear();
            m_lsp_compilation_database.reset();
            if (!params.settings.has_value())
                return;

            auto const* dcc_obj = params.settings->get_object("dcc");
            if (!dcc_obj)
                return;

            if (auto db = dcc_obj->get_string("compilationDatabase"))
            {
                if (!db->empty())
                    m_lsp_compilation_database = std::move(*db);
            }

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

        void parse_inlay_hint_options(protocol::DidChangeConfigurationParams const& params)
        {
            if (!params.settings.has_value())
                return;

            auto const* dcc_obj = params.settings->get_object("dcc");
            if (!dcc_obj)
                return;

            auto const* hints_obj = dcc_obj->get_object("inlayHints");
            if (!hints_obj)
                return;

            if (auto b = hints_obj->get_bool("typeHints"))
                m_inlay_hint_options.typeHints = *b;
            if (auto b = hints_obj->get_bool("parameterHints"))
                m_inlay_hint_options.parameterHints = *b;
            if (auto b = hints_obj->get_bool("suppressParameterNameMatches"))
                m_inlay_hint_options.suppressParameterNameMatches = *b;

            std::println(m_log, "[dccd] inlay hints: type={} parameter={} suppressNameMatch={}", m_inlay_hint_options.typeHints,
                         m_inlay_hint_options.parameterHints, m_inlay_hint_options.suppressParameterNameMatches);
        }

        [[nodiscard]] static std::filesystem::path normalize_path(std::filesystem::path p)
        {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(p, ec);
            if (ec)
                canonical = p.lexically_normal();
            return canonical;
        }

        [[nodiscard]] static std::filesystem::path resolve_database_path(std::string const& raw, std::filesystem::path const& workspace_root)
        {
            std::filesystem::path p{raw};
            if (!p.is_absolute())
                p = workspace_root / p;

            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(p, ec);
            if (ec)
                canonical = p.lexically_normal();
            return canonical;
        }

        void load_compilation_databases()
        {
            m_workspace_databases.clear();

            for (auto const& root : m_workspace_roots)
            {
                WorkspaceCompilationDatabase wcd;
                wcd.workspace_root = root;

                std::filesystem::path database_path;
                bool explicitly_configured = false;
                bool have_path = false;

                if (m_lsp_compilation_database && !m_lsp_compilation_database->empty())
                {
                    database_path = resolve_database_path(*m_lsp_compilation_database, root);
                    explicitly_configured = true;
                    have_path = true;
                }
                else if (auto it = m_project_compilation_database.find(root.string()); it != m_project_compilation_database.end() && !it->second.empty())
                {
                    database_path = resolve_database_path(it->second, root);
                    explicitly_configured = true;
                    have_path = true;
                }
                else
                {
                    database_path = root / "compile_commands.json";
                    have_path = true;
                }

                if (have_path)
                {
                    wcd.database_path = database_path;

                    std::error_code ec;
                    bool exists = std::filesystem::is_regular_file(database_path, ec) && !ec;
                    if (exists)
                    {
                        std::println(m_log, "[dccd] loading compilation database: {}", database_path.string());
                        std::ignore = wcd.database.load(database_path, m_log);
                    }
                    else if (explicitly_configured)
                    {
                        std::println(m_log, "[dccd] explicitly configured compilation database not found, falling back to no database: {}",
                                     database_path.string());
                    }
                }

                m_workspace_databases.push_back(std::move(wcd));
            }
        }

        [[nodiscard]] WorkspaceCompilationDatabase const* find_workspace_database_for(std::filesystem::path const& file) const
        {
            auto norm = normalize_path(file);
            WorkspaceCompilationDatabase const* best = nullptr;
            std::size_t best_len = 0;

            for (auto const& wcd : m_workspace_databases)
            {
                auto const& root = wcd.workspace_root;
                auto rel = norm.lexically_relative(root);

                bool under = true;
                if (!rel.empty())
                {
                    auto first = *rel.begin();
                    if (first == "..")
                        under = false;
                }

                if (!under)
                    continue;

                std::size_t len = root.string().size();
                if (len > best_len)
                {
                    best = &wcd;
                    best_len = len;
                }
            }

            return best;
        }

        [[nodiscard]] dccd::CompileCommand const* find_compile_command(std::filesystem::path const& file) const
        {
            auto const* wcd = find_workspace_database_for(file);
            if (!wcd || wcd->database.empty())
                return nullptr;
            return wcd->database.command_for(file);
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
            load_compilation_databases();

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
                recompile_document(uri);

            publish_all_diagnostics();
        }

        void handle_workspace_did_change_configuration(protocol::RpcInfo const& rpc)
        {
            std::println(m_log, "[dccd] workspace/didChangeConfiguration received");
            auto params = protocol::DidChangeConfigurationParams::from_json(rpc.params.value());

            auto old_lsp_include_paths = m_lsp_include_paths;
            auto old_lsp_compilation_database = m_lsp_compilation_database;
            parse_lsp_configuration(params);
            bool include_paths_changed = (old_lsp_include_paths != m_lsp_include_paths);
            bool compilation_database_changed = (old_lsp_compilation_database != m_lsp_compilation_database);

            parse_inlay_hint_options(params);

            if (include_paths_changed || compilation_database_changed)
                reconfigure_and_recompile();
        }

        void handle_workspace_did_change_watched_files(protocol::RpcInfo const& rpc)
        {
            std::println(m_log, "[dccd] workspace/didChangeWatchedFiles received");
            auto params = protocol::DidChangeWatchedFilesParams::from_json(rpc.params.value());

            bool reload = false;
            auto global_cfg = global_config_path();
            bool graph_file_changed = false;
            std::vector<std::string> changed_graph_uris;

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

                bool project_cfg = false;
                for (auto const& root : m_workspace_roots)
                {
                    auto project_cfg_path = std::filesystem::weakly_canonical(project_config_path(root), ec);
                    if (canonical == project_cfg_path)
                    {
                        std::println(m_log, "[dccd] project config changed: {}", canonical.string());
                        project_cfg = true;
                        break;
                    }
                }
                if (project_cfg)
                {
                    reload = true;
                    break;
                }

                for (auto const& wcd : m_workspace_databases)
                {
                    if (wcd.database_path.empty())
                        continue;

                    auto database_canonical = std::filesystem::weakly_canonical(wcd.database_path, ec);
                    if (canonical == database_canonical)
                    {
                        std::println(m_log, "[dccd] compilation database changed, reloading: {}", canonical.string());
                        reload = true;
                        break;
                    }
                }
                if (reload)
                    break;

                std::ignore = m_session->source_manager().refresh_disk_file(*path);
                m_workspace_index.invalidate_unlinked(canonical.string());

                auto fid = m_session->source_manager().find_by_path(*path);
                if (fid && dcc::query::file_in_module_graph(*m_session, *fid))
                {
                    graph_file_changed = true;
                    changed_graph_uris.push_back(change.uri);
                }
            }

            if (reload)
            {
                reconfigure_and_recompile();
                return;
            }

            if (graph_file_changed)
            {
                if (!m_active_entry_uri.empty())
                    recompile_document(m_active_entry_uri);
                else
                    for (auto const& uri : changed_graph_uris)
                        recompile_document(uri);

                publish_all_diagnostics();
            }
        }

        void publish_all_diagnostics()
        {
            auto const& sm = m_session->source_manager();

            std::map<std::string, std::vector<CachedDiagnostic>, std::less<>> grouped;

            for (auto const& diag : m_session->diagnostics().diagnostics())
            {
                auto labels = diag.labels();

                auto ranked = rank_labels(labels, dcc::sm::FileId::Invalid);
                if (ranked.empty())
                    continue;

                auto const* chosen = ranked.front().label;
                auto const* pub_file = sm.get(chosen->range.begin.fileId);
                if (!pub_file || pub_file->uri().empty())
                    continue;

                if (dcc::vfs::is_dcc_core_uri(pub_file->uri()))
                    continue;

                auto const& pub_uri = pub_file->uri();

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

                auto primary_range = pick_primary_range(sm, labels, chosen->range.begin.fileId);
                if (!primary_range)
                    continue;

                lsp_diag.range = *primary_range;

                std::vector<protocol::DiagnosticRelatedInformation> related;
                for (auto const& label : labels)
                {
                    if (label.style != dcc::diag::LabelStyle::Secondary)
                        continue;

                    auto loc = source_range_to_lsp_location(label.range);
                    if (!loc)
                        continue;

                    protocol::DiagnosticRelatedInformation ri;
                    ri.location = std::move(*loc);
                    ri.message = label.message;
                    related.push_back(std::move(ri));
                }
                if (!related.empty())
                    lsp_diag.relatedInformation = std::move(related);

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
                grouped[pub_uri].push_back(CachedDiagnostic{lsp_diag, diag});
            }

            std::unordered_set<std::string> published_this_round;
            for (auto& [uri, cached] : grouped)
                if (publish_diagnostics_group(uri, cached))
                    published_this_round.insert(uri);

            for (auto const& uri : m_published_uris)
            {
                if (published_this_round.contains(uri))
                    continue;

                publish_empty_diagnostics(uri, version_for_uri(uri));
            }

            m_published_uris = std::move(published_this_round);
        }

        [[nodiscard]] bool publish_diagnostics_group(std::string const& uri, std::vector<CachedDiagnostic>& cached)
        {
            if (!analyzed_file_current(uri))
            {
                std::println(m_log, "[dccd] publish_diagnostics_group: discarding stale diagnostics for {}", uri);
                return false;
            }

            protocol::PublishDiagnosticsParams params;
            params.uri = uri;
            params.version = version_for_uri(uri);

            CachedDiagnosticEntry entry;
            entry.version = params.version;
            entry.content_revision = content_revision_for_uri(uri);
            entry.graph_generation = m_graph_generation;
            entry.diagnostics = std::move(cached);

            m_diagnostic_cache[uri] = std::move(entry);

            for (auto const& c : m_diagnostic_cache[uri].diagnostics)
                params.diagnostics.push_back(c.lsp_diag);

            publish_lsp_diagnostics(params);
            return true;
        }

        void publish_empty_diagnostics(std::string const& uri, std::optional<std::int64_t> version)
        {
            m_diagnostic_cache.erase(uri);

            protocol::PublishDiagnosticsParams params;
            params.uri = uri;
            params.version = version;
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
            if (m_output)
            {
                *m_output << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
                m_output->flush();
            }
            else
            {
                std::print("Content-Length: {}\r\n\r\n{}", payload.size(), payload);
                std::cout.flush();
            }
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
                case dcc::types::TypeKind::Restricted: {
                    auto const* rt = static_cast<dcc::types::RestrictedType const*>(ty);
                    auto format_ordinal = [&](std::uint64_t ordinal) {
                        auto raw = dcc::int_domain::ordinal_to_raw_bits(ordinal, *rt->underlying);
                        if (rt->underlying->is_signed)
                            return std::to_string(raw);
                        return std::to_string(static_cast<std::uint64_t>(raw) & dcc::int_domain::mask_for_bits(rt->underlying->bits));
                    };
                    auto out = format_dcc_type(rt->underlying);
                    out += '{';
                    for (std::size_t i = 0; i < rt->intervals.size(); ++i)
                    {
                        if (i)
                            out += ", ";
                        out += format_ordinal(rt->intervals[i].lo);
                        if (rt->intervals[i].lo != rt->intervals[i].hi)
                            out += ".." + format_ordinal(rt->intervals[i].hi);
                    }
                    out += '}';
                    return out;
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
                case dcc::types::TypeKind::Lambda:
                    return "lambda";
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
