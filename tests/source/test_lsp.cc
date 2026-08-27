import std;
import dcc.sm;
import dcc.session;
import dcc.ast;
import dcc.sema;
import dcc.sema.type_helpers;
import dccd.protocol;
import dccd.semantic_tokens;
import dccd.inlay_hints;
import dccd.server;
import dccd.workspace_index;

#include "harness.hh"

namespace
{
    using dccd::protocol::JsonValue;

    struct Sink
    {
        std::ostringstream stream;

        [[nodiscard]] std::string drain()
        {
            auto s = stream.str();
            stream.str(std::string{});
            stream.clear();
            return s;
        }
    };

    struct CapturedFrame
    {
        std::string method;
        JsonValue params;
    };

    [[nodiscard]] std::vector<JsonValue> parse_lsp_stream(std::string_view raw)
    {
        std::vector<JsonValue> out;
        std::size_t i = 0;
        while (i < raw.size())
        {
            auto hdr_end = raw.find("\r\n\r\n", i);
            if (hdr_end == std::string_view::npos)
                break;

            auto header = raw.substr(i, hdr_end - i);
            std::size_t content_length = 0;
            bool found = false;
            std::size_t pos = 0;
            while (pos < header.size())
            {
                auto nl = header.find("\r\n", pos);
                if (nl == std::string_view::npos)
                    nl = header.size();

                auto line = header.substr(pos, nl - pos);
                if (line.size() > 16 && line.substr(0, 16) == "Content-Length: ")
                {
                    auto val = line.substr(16);
                    auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), content_length);
                    found = (ec == std::errc{});
                }
                pos = (nl == header.size()) ? header.size() : nl + 2;
            }

            if (!found)
                break;

            auto body_start = hdr_end + 4;
            if (body_start + content_length > raw.size())
                break;

            auto body = raw.substr(body_start, content_length);
            if (auto parsed = JsonValue::parse(body))
                out.push_back(std::move(*parsed));

            i = body_start + content_length;
        }
        return out;
    }

    [[nodiscard]] std::vector<dccd::protocol::PublishDiagnosticsParams> send_and_collect_publishes(dccd::LanguageServer& server, Sink& sink, JsonValue rpc)
    {
        auto parsed = dccd::protocol::parse_rpc(rpc);
        if (!parsed)
            return {};

        std::ignore = server.handle_message(*parsed);

        auto frames = parse_lsp_stream(sink.drain());

        std::vector<dccd::protocol::PublishDiagnosticsParams> publishes;
        for (auto& frame : frames)
        {
            auto method = frame.get_string("method");
            CHECK(method.has_value());
            if (method)
                CHECK_EQ(*method, "textDocument/publishDiagnostics");
            else
                continue;

            auto params = frame.find_member("params");
            if (params)
                publishes.push_back(dccd::protocol::PublishDiagnosticsParams::from_json(*params));
        }
        return publishes;
    }

    void initialize_server(dccd::LanguageServer& server, Sink& sink, std::filesystem::path const& workspace_root = {},
                           std::vector<std::string_view> const& position_encodings = {}, bool watched_files_dynamic_registration = false)
    {
        auto params = dccd::protocol::JsonValue::empty_object();
        if (!workspace_root.empty())
        {
            auto folders = dccd::protocol::JsonValue::empty_array();
            auto folder = dccd::protocol::JsonValue::empty_object();
            folder.set("uri", dccd::protocol::JsonValue::string_val(dcc::sm::SourceManager::to_file_uri(workspace_root)));
            folder.set("name", dccd::protocol::JsonValue::string_val("test-workspace"));
            folders.push_back(std::move(folder));
            params.set("workspaceFolders", std::move(folders));
        }

        if (!position_encodings.empty() || watched_files_dynamic_registration)
        {
            auto caps = dccd::protocol::JsonValue::empty_object();

            if (!position_encodings.empty())
            {
                auto encs = dccd::protocol::JsonValue::empty_array();
                for (auto enc : position_encodings)
                    encs.push_back(dccd::protocol::JsonValue::string_val(std::string{enc}));

                auto general = dccd::protocol::JsonValue::empty_object();
                general.set("positionEncodings", std::move(encs));

                caps.set("general", std::move(general));
            }

            if (watched_files_dynamic_registration)
            {
                auto dwf = dccd::protocol::JsonValue::empty_object();
                dwf.set("dynamicRegistration", dccd::protocol::JsonValue::boolean(true));

                auto workspace = dccd::protocol::JsonValue::empty_object();
                workspace.set("didChangeWatchedFiles", std::move(dwf));

                caps.set("workspace", std::move(workspace));
            }

            params.set("capabilities", std::move(caps));
        }

        auto init_req = dccd::protocol::build_request(JsonValue::integer(1), "initialize", std::move(params));
        auto parsed = dccd::protocol::parse_rpc(init_req);
        if (!parsed)
            return;
        auto response = server.handle_message(*parsed);
        CHECK(response.has_value());
        CHECK(sink.drain().empty());
    }

    [[nodiscard]] std::optional<dccd::protocol::RpcInfo> parse_notification_rpc(std::string_view method, dccd::protocol::JsonValue params)
    {
        auto notif = dccd::protocol::build_notification(std::string{method}, std::move(params));
        return dccd::protocol::parse_rpc(notif);
    }

    [[nodiscard]] JsonValue text_document(std::string uri, std::int64_t version, std::string_view text)
    {
        auto td = JsonValue::empty_object();
        td.set("uri", JsonValue::string_val(std::move(uri)));
        td.set("languageId", JsonValue::string_val("dc"));
        td.set("version", JsonValue::integer(version));
        td.set("text", JsonValue::string_val(std::string{text}));
        return td;
    }

    [[nodiscard]] JsonValue make_did_open(std::string uri, std::int64_t version, std::string_view text)
    {
        auto params = JsonValue::empty_object();
        params.set("textDocument", text_document(std::move(uri), version, text));
        return dccd::protocol::build_notification("textDocument/didOpen", std::move(params));
    }

    [[nodiscard]] JsonValue make_did_change(std::string uri, std::int64_t version, std::string_view text)
    {
        auto td = JsonValue::empty_object();
        td.set("uri", JsonValue::string_val(std::move(uri)));
        td.set("version", JsonValue::integer(version));

        auto change = JsonValue::empty_object();
        change.set("text", JsonValue::string_val(std::string{text}));

        auto changes = JsonValue::empty_array();
        changes.push_back(std::move(change));

        auto params = JsonValue::empty_object();
        params.set("textDocument", std::move(td));
        params.set("contentChanges", std::move(changes));
        return dccd::protocol::build_notification("textDocument/didChange", std::move(params));
    }

    [[nodiscard]] JsonValue make_did_close(std::string uri)
    {
        auto td = JsonValue::empty_object();
        td.set("uri", JsonValue::string_val(std::move(uri)));

        auto params = JsonValue::empty_object();
        params.set("textDocument", std::move(td));
        return dccd::protocol::build_notification("textDocument/didClose", std::move(params));
    }

    struct TempDir
    {
        std::filesystem::path path;

        TempDir()
        {
            auto base = std::filesystem::temp_directory_path();
            auto tag = std::format("dcc-lsp-test-{}", std::chrono::steady_clock::now().time_since_epoch().count());
            path = base / tag;
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    [[nodiscard]] std::optional<dccd::protocol::JsonValue> send_request(dccd::LanguageServer& server, Sink& sink, dccd::protocol::JsonValue rpc)
    {
        auto parsed = dccd::protocol::parse_rpc(rpc);
        if (!parsed)
            return std::nullopt;
        auto response = server.handle_message(*parsed);
        CHECK(sink.drain().empty());
        return response;
    }

    [[nodiscard]] dccd::protocol::JsonValue make_position(std::uint32_t line, std::uint32_t character)
    {
        auto pos = dccd::protocol::JsonValue::empty_object();
        pos.set("line", dccd::protocol::JsonValue::integer(static_cast<std::int64_t>(line)));
        pos.set("character", dccd::protocol::JsonValue::integer(static_cast<std::int64_t>(character)));
        return pos;
    }

    [[nodiscard]] dccd::protocol::JsonValue make_completion_request(std::string const& uri, std::uint32_t line, std::uint32_t character)
    {
        auto td = dccd::protocol::JsonValue::empty_object();
        td.set("uri", dccd::protocol::JsonValue::string_val(uri));

        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", std::move(td));
        params.set("position", make_position(line, character));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::integer(1), "textDocument/completion", std::move(params));
    }

    [[nodiscard]] dccd::protocol::JsonValue make_signature_request(std::string const& uri, std::uint32_t line, std::uint32_t character)
    {
        auto td = dccd::protocol::JsonValue::empty_object();
        td.set("uri", dccd::protocol::JsonValue::string_val(uri));

        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", std::move(td));
        params.set("position", make_position(line, character));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::integer(1), "textDocument/signatureHelp", std::move(params));
    }

    [[nodiscard]] std::vector<dccd::protocol::CompletionItem> request_completion_items(dccd::LanguageServer& server, Sink& sink, std::string const& uri,
                                                                                       std::uint32_t line, std::uint32_t character)
    {
        auto resp = send_request(server, sink, make_completion_request(uri, line, character));
        if (!resp)
            return {};

        auto const* result_val = resp->find_member("result");
        if (!result_val)
            return {};

        auto const* items_arr = result_val->get_array("items");
        if (!items_arr)
            return {};

        std::vector<dccd::protocol::CompletionItem> items;
        for (auto const& item_json : items_arr->as_array())
            items.push_back(dccd::protocol::CompletionItem::from_json(item_json));
        return items;
    }

    [[nodiscard]] std::optional<dccd::protocol::SignatureHelp> request_signature_help(dccd::LanguageServer& server, Sink& sink, std::string const& uri,
                                                                                      std::uint32_t line, std::uint32_t character)
    {
        auto resp = send_request(server, sink, make_signature_request(uri, line, character));
        if (!resp)
            return std::nullopt;

        auto const* result_val = resp->find_member("result");
        if (!result_val || result_val->is_null())
            return std::nullopt;

        dccd::protocol::SignatureHelp help;
        if (auto n = result_val->get_integer("activeSignature"))
            help.activeSignature = static_cast<std::uint32_t>(*n);
        if (auto n = result_val->get_integer("activeParameter"))
            help.activeParameter = static_cast<std::uint32_t>(*n);
        auto const* sigs = result_val->get_array("signatures");
        if (sigs)
        {
            for (auto const& sig_json : sigs->as_array())
            {
                dccd::protocol::SignatureInformation sig;
                if (auto s = sig_json.get_string("label"))
                    sig.label = std::move(*s);
                if (auto n = sig_json.get_integer("activeParameter"))
                    sig.activeParameter = static_cast<std::uint32_t>(*n);
                auto const* params = sig_json.get_array("parameters");
                if (params)
                {
                    for (auto const& p_json : params->as_array())
                    {
                        dccd::protocol::ParameterInformation p;
                        if (auto s = p_json.get_string("label"))
                            p.label = std::move(*s);
                        sig.parameters.push_back(std::move(p));
                    }
                }
                help.signatures.push_back(std::move(sig));
            }
        }
        return help;
    }

    struct MarkedPosition
    {
        std::uint32_t line{};
        std::uint32_t character{};
    };

    [[nodiscard]] MarkedPosition position_of_marker(std::string_view text, char marker)
    {
        MarkedPosition pos;
        std::size_t i = 0;
        while (i < text.size())
        {
            char c = text[i];
            if (c == marker)
                return pos;
            if (c == '\n')
            {
                ++pos.line;
                pos.character = 0;
                ++i;
                continue;
            }

            auto b0 = static_cast<unsigned char>(c);
            int len = 1;
            if ((b0 & 0xE0u) == 0xC0u)
                len = 2;
            else if ((b0 & 0xF0u) == 0xE0u)
                len = 3;
            else if ((b0 & 0xF8u) == 0xF0u)
                len = 4;

            pos.character += (len == 4) ? 2u : 1u;
            i += static_cast<std::size_t>(len);
        }
        return pos;
    }

    [[nodiscard]] std::pair<std::string, MarkedPosition> strip_marker(std::string text, char marker)
    {
        auto pos = position_of_marker(text, marker);
        auto at = text.find(marker);
        if (at != std::string::npos)
            text.erase(at, 1);
        return {std::move(text), pos};
    }

    [[nodiscard]] dccd::protocol::JsonValue make_text_document(std::string const& uri)
    {
        auto td = dccd::protocol::JsonValue::empty_object();
        td.set("uri", dccd::protocol::JsonValue::string_val(uri));
        return td;
    }

    [[nodiscard]] dccd::protocol::JsonValue make_position_params_request(std::string const& uri, std::uint32_t line, std::uint32_t character,
                                                                         std::string_view method, std::string id = "1")
    {
        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", make_text_document(uri));
        params.set("position", make_position(line, character));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::string_val(id), std::string{method}, std::move(params));
    }

    [[nodiscard]] dccd::protocol::JsonValue make_references_request(std::string const& uri, std::uint32_t line, std::uint32_t character,
                                                                    bool include_declaration)
    {
        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", make_text_document(uri));
        params.set("position", make_position(line, character));
        auto ctx = dccd::protocol::JsonValue::empty_object();
        ctx.set("includeDeclaration", dccd::protocol::JsonValue::boolean(include_declaration));
        params.set("context", std::move(ctx));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::string_val("1"), "textDocument/references", std::move(params));
    }

    [[nodiscard]] dccd::protocol::JsonValue make_semantic_tokens_request(std::string const& uri, std::string id = "1")
    {
        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", make_text_document(uri));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::string_val(std::move(id)), "textDocument/semanticTokens/full", std::move(params));
    }

    [[nodiscard]] dccd::protocol::JsonValue make_rename_request(std::string const& uri, std::uint32_t line, std::uint32_t character, std::string new_name)
    {
        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", make_text_document(uri));
        params.set("position", make_position(line, character));
        params.set("newName", dccd::protocol::JsonValue::string_val(std::move(new_name)));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::string_val("1"), "textDocument/rename", std::move(params));
    }

    struct RpcError
    {
        std::int64_t code{};
        std::string message;
    };

    struct RpcResult
    {
        std::optional<dccd::protocol::JsonValue> value;
        std::optional<RpcError> error;
    };

    [[nodiscard]] RpcResult send_position_request(dccd::LanguageServer& server, Sink& sink, std::string const& uri, std::uint32_t line, std::uint32_t character,
                                                  std::string_view method)
    {
        auto resp = send_request(server, sink, make_position_params_request(uri, line, character, method));
        RpcResult out;
        if (!resp)
            return out;

        if (auto const* err = resp->find_member("error"))
        {
            RpcError e;
            if (auto c = err->get_integer("code"))
                e.code = *c;
            if (auto m = err->get_string("message"))
                e.message = std::move(*m);
            out.error = std::move(e);
            return out;
        }

        if (auto const* result = resp->find_member("result"))
            out.value = *result;
        return out;
    }

    [[nodiscard]] std::optional<dccd::protocol::LspLocation> request_definition(dccd::LanguageServer& server, Sink& sink, std::string const& uri,
                                                                                std::uint32_t line, std::uint32_t character)
    {
        auto res = send_position_request(server, sink, uri, line, character, "textDocument/definition");
        if (!res.value || res.value->is_null())
            return std::nullopt;

        if (res.value->is_array())
        {
            auto const& arr = res.value->as_array();
            if (arr.empty())
                return std::nullopt;
            return dccd::protocol::LspLocation::from_json(arr.back());
        }

        return dccd::protocol::LspLocation::from_json(*res.value);
    }

    [[nodiscard]] std::vector<dccd::protocol::LspLocation> request_references(dccd::LanguageServer& server, Sink& sink, std::string const& uri,
                                                                              std::uint32_t line, std::uint32_t character, bool include_declaration)
    {
        auto resp = send_request(server, sink, make_references_request(uri, line, character, include_declaration));
        std::vector<dccd::protocol::LspLocation> out;
        if (!resp)
            return out;

        auto const* result_val = resp->find_member("result");
        if (!result_val || !result_val->is_array())
            return out;

        for (auto const& loc_json : result_val->as_array())
            out.push_back(dccd::protocol::LspLocation::from_json(loc_json));
        return out;
    }

    [[nodiscard]] std::vector<dccd::protocol::LspRange> request_highlights(dccd::LanguageServer& server, Sink& sink, std::string const& uri, std::uint32_t line,
                                                                           std::uint32_t character)
    {
        auto resp = send_request(server, sink, make_position_params_request(uri, line, character, "textDocument/documentHighlight"));
        std::vector<dccd::protocol::LspRange> out;
        if (!resp)
            return out;

        auto const* result_val = resp->find_member("result");
        if (!result_val || !result_val->is_array())
            return out;

        for (auto const& h_json : result_val->as_array())
        {
            if (auto const* r = h_json.find_member("range"))
                out.push_back(dccd::protocol::LspRange::from_json(*r));
        }
        return out;
    }

    struct RenameOutcome
    {
        std::optional<dccd::protocol::WorkspaceEdit> edit;
        std::optional<RpcError> error;
    };

    [[nodiscard]] RenameOutcome request_rename(dccd::LanguageServer& server, Sink& sink, std::string const& uri, std::uint32_t line, std::uint32_t character,
                                               std::string new_name)
    {
        auto resp = send_request(server, sink, make_rename_request(uri, line, character, std::move(new_name)));
        RenameOutcome out;
        if (!resp)
            return out;

        if (auto const* err = resp->find_member("error"))
        {
            RpcError e;
            if (auto c = err->get_integer("code"))
                e.code = *c;
            if (auto m = err->get_string("message"))
                e.message = std::move(*m);
            out.error = std::move(e);
            return out;
        }

        if (auto const* result = resp->find_member("result"))
        {
            if (result->is_null())
                return out;

            dccd::protocol::WorkspaceEdit we;
            if (auto const* changes = result->get_object("changes"))
            {
                for (auto const& [edit_uri, edits_json] : changes->as_object())
                {
                    std::vector<dccd::protocol::TextEdit> edits;
                    for (auto const& e : edits_json.as_array())
                        edits.push_back(dccd::protocol::TextEdit::from_json(e));
                    we.changes[edit_uri] = std::move(edits);
                }
            }
            out.edit = std::move(we);
        }
        return out;
    }

    struct PrepareRenameOutcome
    {
        std::optional<dccd::protocol::LspRange> range;
        std::optional<std::string> placeholder;
        bool refused{false};
    };

    [[nodiscard]] PrepareRenameOutcome request_prepare_rename(dccd::LanguageServer& server, Sink& sink, std::string const& uri, std::uint32_t line,
                                                              std::uint32_t character)
    {
        auto res = send_position_request(server, sink, uri, line, character, "textDocument/prepareRename");
        PrepareRenameOutcome out;
        if (!res.value)
        {
            out.refused = true;
            return out;
        }

        if (res.value->is_null())
        {
            out.refused = true;
            return out;
        }

        if (auto const* r = res.value->find_member("range"))
            out.range = dccd::protocol::LspRange::from_json(*r);
        if (auto s = res.value->get_string("placeholder"))
            out.placeholder = std::move(*s);
        return out;
    }

    [[nodiscard]] std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> locations_to_ranges(std::vector<dccd::protocol::LspLocation> const& locs)
    {
        std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> out;
        for (auto const& loc : locs)
            out.emplace_back(loc.range.start.line, loc.range.start.character, loc.range.end.character);
        return out;
    }

    [[nodiscard]] std::string open_file(dccd::LanguageServer& server, Sink& sink, std::filesystem::path const& path, std::string_view text)
    {
        {
            std::ofstream out{path};
            out << text;
        }

        auto uri = dcc::sm::SourceManager::to_file_uri(path);
        auto publishes = send_and_collect_publishes(server, sink, make_did_open(uri, 1, text));
        std::ignore = publishes;
        return uri;
    }

    [[nodiscard]] std::string open_virtual_file(dccd::LanguageServer& server, Sink& sink, std::string uri, std::string_view text)
    {
        auto publishes = send_and_collect_publishes(server, sink, make_did_open(uri, 1, text));
        std::ignore = publishes;
        return uri;
    }

    [[nodiscard]] dccd::protocol::CompletionItem const* find_item(std::vector<dccd::protocol::CompletionItem> const& items, std::string_view label)
    {
        for (auto const& item : items)
            if (item.label == label)
                return &item;
        return nullptr;
    }

    [[nodiscard]] bool has_label(std::vector<dccd::protocol::CompletionItem> const& items, std::string_view label)
    {
        return find_item(items, label) != nullptr;
    }

    [[nodiscard]] std::optional<std::string> request_hover(dccd::LanguageServer& server, Sink& sink, std::string const& uri, std::uint32_t line,
                                                           std::uint32_t character)
    {
        auto res = send_position_request(server, sink, uri, line, character, "textDocument/hover");
        if (!res.value || res.value->is_null())
            return std::nullopt;

        if (auto const* contents = res.value->find_member("contents"))
            if (auto v = contents->get_string("value"))
                return v;
        return std::nullopt;
    }

    [[nodiscard]] bool check_edit_set(std::vector<dccd::protocol::TextEdit> const& edits, std::string_view new_text,
                                      std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> const& expected)
    {
        if (edits.size() != expected.size())
            return false;

        std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> actual;
        for (auto const& e : edits)
        {
            if (e.newText != new_text)
                return false;
            actual.emplace_back(e.range.start.line, e.range.start.character, e.range.end.character);
        }

        std::ranges::sort(actual);
        auto sorted_expected = expected;
        std::ranges::sort(sorted_expected);
        return actual == sorted_expected;
    }

    [[nodiscard]] bool has_range(std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> const& ranges, std::uint32_t line, std::uint32_t start,
                                 std::uint32_t end)
    {
        return std::ranges::find(ranges, std::tuple{line, start, end}) != ranges.end();
    }

    [[nodiscard]] dccd::protocol::JsonValue make_workspace_symbol_request(std::string_view query, std::string id = "1")
    {
        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("query", dccd::protocol::JsonValue::string_val(std::string{query}));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::string_val(id), "workspace/symbol", std::move(params));
    }

    [[nodiscard]] std::vector<dccd::protocol::SymbolInformation> request_workspace_symbols(dccd::LanguageServer& server, Sink& sink, std::string_view query)
    {
        auto resp = send_request(server, sink, make_workspace_symbol_request(query));
        std::vector<dccd::protocol::SymbolInformation> out;
        if (!resp)
            return out;

        auto const* result_val = resp->find_member("result");
        if (!result_val || !result_val->is_array())
            return out;

        for (auto const& sym_json : result_val->as_array())
            out.push_back(dccd::protocol::SymbolInformation::from_json(sym_json));
        return out;
    }

    [[nodiscard]] bool has_symbol_info(std::vector<dccd::protocol::SymbolInformation> const& infos, std::string_view name)
    {
        for (auto const& s : infos)
            if (s.name == name)
                return true;
        return false;
    }

    [[nodiscard]] dccd::protocol::JsonValue make_watched_files_change(std::string const& uri)
    {
        auto change = dccd::protocol::JsonValue::empty_object();
        change.set("uri", dccd::protocol::JsonValue::string_val(uri));
        change.set("type", dccd::protocol::JsonValue::integer(2));

        auto changes = dccd::protocol::JsonValue::empty_array();
        changes.push_back(std::move(change));

        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("changes", std::move(changes));
        return dccd::protocol::build_notification("workspace/didChangeWatchedFiles", std::move(params));
    }

    [[nodiscard]] dccd::protocol::JsonValue make_cancel_notification(dccd::protocol::RequestId const& id)
    {
        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("id", id.to_json());
        return dccd::protocol::build_notification(std::string{dccd::protocol::kCancelRequestMethod}, std::move(params));
    }

    [[nodiscard]] dccd::protocol::JsonValue make_code_action_request(std::string const& uri, std::vector<dccd::protocol::LspDiagnostic> const& ctx_diagnostics)
    {
        auto td = dccd::protocol::JsonValue::empty_object();
        td.set("uri", dccd::protocol::JsonValue::string_val(uri));

        auto diag_arr = dccd::protocol::JsonValue::empty_array();
        for (auto const& d : ctx_diagnostics)
            diag_arr.push_back(d.to_json());

        auto ctx = dccd::protocol::JsonValue::empty_object();
        ctx.set("diagnostics", std::move(diag_arr));

        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", std::move(td));
        params.set("context", std::move(ctx));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::integer(1), "textDocument/codeAction", std::move(params));
    }

    struct CodeActionResult
    {
        std::string title;
        std::string kind;
        std::vector<dccd::protocol::LspDiagnostic> diagnostics;
        std::optional<dccd::protocol::JsonValue> edit;
    };

    [[nodiscard]] std::vector<CodeActionResult> request_code_actions(dccd::LanguageServer& server, Sink& sink, std::string const& uri,
                                                                     std::vector<dccd::protocol::LspDiagnostic> const& ctx_diagnostics)
    {
        auto resp = send_request(server, sink, make_code_action_request(uri, ctx_diagnostics));
        std::vector<CodeActionResult> out;
        if (!resp)
            return out;

        auto const* result_val = resp->find_member("result");
        if (!result_val || !result_val->is_array())
            return out;

        for (auto const& action_json : result_val->as_array())
        {
            CodeActionResult act;
            if (auto s = action_json.get_string("title"))
                act.title = std::move(*s);
            if (auto s = action_json.get_string("kind"))
                act.kind = std::move(*s);
            if (auto const* diag_arr = action_json.get_array("diagnostics"))
                for (auto const& d : diag_arr->as_array())
                    act.diagnostics.push_back(dccd::protocol::LspDiagnostic::from_json(d));
            if (auto const* edit = action_json.find_member("edit"))
                act.edit = *edit;
            out.push_back(std::move(act));
        }
        return out;
    }

    [[nodiscard]] std::optional<dccd::protocol::TextEdit> extract_first_edit(CodeActionResult const& action)
    {
        if (!action.edit)
            return std::nullopt;

        auto const* changes = action.edit->get_object("changes");
        if (!changes)
            return std::nullopt;

        for (auto const& [edit_uri, edits_json] : changes->as_object())
        {
            std::ignore = edit_uri;
            if (!edits_json.is_array())
                continue;

            auto const& arr = edits_json.as_array();
            if (arr.empty())
                continue;

            return dccd::protocol::TextEdit::from_json(arr.front());
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t lsp_to_byte_offset(std::string_view text, std::uint32_t line, std::uint32_t character)
    {
        std::size_t i = 0;
        std::uint32_t cur_line = 0;
        std::uint32_t cur_char = 0;
        while (i < text.size())
        {
            if (cur_line == line && cur_char == character)
                return i;

            char c = text[i];
            if (c == '\n')
            {
                ++cur_line;
                cur_char = 0;
                ++i;
                continue;
            }

            auto b0 = static_cast<unsigned char>(c);
            int len = 1;
            if ((b0 & 0xE0u) == 0xC0u)
                len = 2;
            else if ((b0 & 0xF0u) == 0xE0u)
                len = 3;
            else if ((b0 & 0xF8u) == 0xF0u)
                len = 4;

            cur_char += (len == 4) ? 2u : 1u;
            i += static_cast<std::size_t>(len);
        }
        return i;
    }

    [[nodiscard]] std::string apply_text_edit(std::string text, dccd::protocol::TextEdit const& edit)
    {
        auto start = lsp_to_byte_offset(text, edit.range.start.line, edit.range.start.character);
        auto end = lsp_to_byte_offset(text, edit.range.end.line, edit.range.end.character);
        text.replace(start, end - start, edit.newText);
        return text;
    }

    [[nodiscard]] dccd::protocol::JsonValue make_formatting_request(std::string const& uri)
    {
        auto td = dccd::protocol::JsonValue::empty_object();
        td.set("uri", dccd::protocol::JsonValue::string_val(uri));

        auto opts = dccd::protocol::JsonValue::empty_object();
        opts.set("tabSize", dccd::protocol::JsonValue::integer(4));
        opts.set("insertSpaces", dccd::protocol::JsonValue::boolean(true));

        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", std::move(td));
        params.set("options", std::move(opts));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::integer(1), "textDocument/formatting", std::move(params));
    }

    [[nodiscard]] std::vector<dccd::protocol::TextEdit> request_formatting(dccd::LanguageServer& server, Sink& sink, std::string const& uri)
    {
        auto resp = send_request(server, sink, make_formatting_request(uri));
        std::vector<dccd::protocol::TextEdit> out;
        if (!resp)
            return out;

        auto const* result_val = resp->find_member("result");
        if (!result_val || !result_val->is_array())
            return out;

        for (auto const& edit_json : result_val->as_array())
            out.push_back(dccd::protocol::TextEdit::from_json(edit_json));
        return out;
    }

} // namespace

SECTION("lsp: diagnostic lifecycle");

TEST_CASE("didOpen publishes version 1 with duplicate-declaration relatedInformation")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto open = make_did_open("file:///tmp/dccd_lsp_test.dc", 1, "module m;\nvoid f() {\n    \"caf\u00e9 latte\"; i32 x = 0;\n    i32 x = 1;\n}\n");
    auto publishes = send_and_collect_publishes(server, sink, open);

    REQUIRE(publishes.size() == 1);
    auto const& publish = publishes[0];
    CHECK_EQ(publish.uri, "file:///tmp/dccd_lsp_test.dc");

    REQUIRE(publish.version.has_value());
    CHECK_EQ(*publish.version, 1);

    REQUIRE(publish.diagnostics.size() == 1);
    auto const& diag = publish.diagnostics[0];
    CHECK(diag.message.find("redefinition of `x`") != std::string::npos);

    CHECK_EQ(diag.range.start.line, 3u);
    CHECK_EQ(diag.range.start.character, 8u);
    CHECK_EQ(diag.range.end.line, 3u);
    CHECK_EQ(diag.range.end.character, 9u);

    REQUIRE(diag.relatedInformation.has_value());
    REQUIRE(diag.relatedInformation->size() == 1);
    auto const& ri = diag.relatedInformation->at(0);
    CHECK_EQ(ri.message, "previous declaration here");
    CHECK_EQ(ri.location.uri, "file:///tmp/dccd_lsp_test.dc");
    CHECK_EQ(ri.location.range.start.line, 2u);
    CHECK_EQ(ri.location.range.start.character, 22u);
    CHECK_EQ(ri.location.range.end.line, 2u);
    CHECK_EQ(ri.location.range.end.character, 23u);
}

TEST_CASE("successive versions replace and clear diagnostics")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    {
        auto publishes = send_and_collect_publishes(
            server, sink, make_did_open("file:///tmp/dccd_lsp_test.dc", 1, "module m;\nvoid f() {\n    i32 x = 0;\n    i32 x = 1;\n}\n"));
        REQUIRE(publishes.size() == 1);
        CHECK_EQ(*publishes[0].version, 1);
        REQUIRE(publishes[0].diagnostics.size() == 1);
    }

    {
        auto publishes =
            send_and_collect_publishes(server, sink, make_did_change("file:///tmp/dccd_lsp_test.dc", 2, "module m;\nvoid f() {\n    i32 x = 0;\n}\n"));
        REQUIRE(publishes.size() == 1);
        CHECK_EQ(*publishes[0].version, 2);
        CHECK(publishes[0].diagnostics.empty());
    }

    {
        auto publishes = send_and_collect_publishes(server, sink, make_did_change("file:///tmp/dccd_lsp_test.dc", 3, "module m;\ni32 y = unknown_name;\n"));
        REQUIRE(publishes.size() == 1);
        CHECK_EQ(*publishes[0].version, 3);
        REQUIRE(publishes[0].diagnostics.size() == 1);
        auto const& diag = publishes[0].diagnostics[0];
        CHECK(diag.message.find("unknown name `unknown_name`") != std::string::npos);
        CHECK_EQ(diag.range.start.line, 1u);
        CHECK_EQ(diag.range.start.character, 8u);
        CHECK_EQ(diag.range.end.line, 1u);
        CHECK_EQ(diag.range.end.character, 20u);
    }

    {
        auto publishes = send_and_collect_publishes(server, sink, make_did_close("file:///tmp/dccd_lsp_test.dc"));
        REQUIRE(publishes.size() == 1);
        CHECK(publishes[0].diagnostics.empty());
        REQUIRE(publishes[0].version.has_value());
        CHECK_EQ(*publishes[0].version, 3);
    }
}

TEST_CASE("parser recovery publishes only the primary and independent sibling error then clears")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"file:///tmp/dccd_lsp_parser_recovery.dc"};
    auto publishes =
        send_and_collect_publishes(server, sink, make_did_open(uri, 1, "module m;\ni32 poisoned = missing_call(1;\nvoid f() {\n    independent;\n}\n"));

    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].version.has_value());
    CHECK_EQ(*publishes[0].version, 1);
    REQUIRE(publishes[0].diagnostics.size() == 2);

    CHECK_EQ(publishes[0].diagnostics[0].message, "expected ')' to close call argument list, found ';'");
    CHECK_EQ(publishes[0].diagnostics[0].range.start.line, 1u);
    CHECK_EQ(publishes[0].diagnostics[0].range.start.character, 29u);
    CHECK_EQ(publishes[0].diagnostics[0].range.end.character, 30u);

    CHECK_EQ(publishes[0].diagnostics[1].message, "unknown name `independent`");
    CHECK_EQ(publishes[0].diagnostics[1].range.start.line, 3u);
    CHECK_EQ(publishes[0].diagnostics[1].range.start.character, 4u);
    CHECK_EQ(publishes[0].diagnostics[1].range.end.character, 15u);

    publishes = send_and_collect_publishes(server, sink, make_did_change(uri, 2, "module m;\nvoid f() {\n    return;\n}\n"));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].version.has_value());
    CHECK_EQ(*publishes[0].version, 2);
    CHECK(publishes[0].diagnostics.empty());
}

TEST_CASE("failed didChange on a closed document publishes empty diagnostics for the new version")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    {
        auto publishes = send_and_collect_publishes(server, sink, make_did_open("file:///tmp/dccd_lsp_test.dc", 1, "module m;\ni32 x = 0;\ni32 x = 1;\n"));
        REQUIRE(publishes.size() == 1);
        REQUIRE(publishes[0].diagnostics.size() == 1);
    }
    {
        auto publishes = send_and_collect_publishes(server, sink, make_did_close("file:///tmp/dccd_lsp_test.dc"));
        REQUIRE(publishes.size() == 1);
        CHECK(publishes[0].diagnostics.empty());
    }

    {
        auto publishes = send_and_collect_publishes(server, sink, make_did_change("file:///tmp/dccd_lsp_test.dc", 9, "module m;\ni32 broken = unknown;\n"));
        REQUIRE(publishes.size() == 1);
        REQUIRE(publishes[0].version.has_value());
        CHECK_EQ(*publishes[0].version, 9);
        CHECK(publishes[0].diagnostics.empty());
    }

    {
        auto publishes = send_and_collect_publishes(server, sink, make_did_open("file:///tmp/dccd_lsp_test.dc", 10, "module m;\ni32 broken = unknown;\n"));
        REQUIRE(publishes.size() == 1);
        REQUIRE(publishes[0].version.has_value());
        CHECK_EQ(*publishes[0].version, 10);
        REQUIRE(publishes[0].diagnostics.size() == 1);
    }
}

TEST_CASE("stale and duplicate didChange versions are rejected before mutation or recompile")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"file:///tmp/dccd_lsp_stale_change.dc"};
    auto text_v1 = "module m;\ni32 x = 0;\ni32 x = 1;\n";

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(uri, 1, text_v1));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].version.has_value());
    CHECK_EQ(*publishes[0].version, 1);
    REQUIRE(publishes[0].diagnostics.size() == 1);

    auto& sm = server.source_manager();
    auto const& st = server.workspace_index().stats();
    auto sync_before = st.sync_count;

    auto fid = sm.find_by_uri(uri);
    REQUIRE(fid.has_value());
    auto const* sf = sm.get(*fid);
    REQUIRE(sf != nullptr);
    auto revision_before = sf->content_revision();

    auto dup = send_and_collect_publishes(server, sink, make_did_change(uri, 1, "module m;\ni32 y = 0;\n"));
    CHECK(dup.empty());

    auto older = send_and_collect_publishes(server, sink, make_did_change(uri, 0, "module m;\ni32 z = 0;\n"));
    CHECK(older.empty());

    CHECK_EQ(sf->text(), text_v1);
    REQUIRE(sf->version().has_value());
    CHECK_EQ(*sf->version(), 1);
    CHECK_EQ(sf->content_revision(), revision_before);
    CHECK_EQ(st.sync_count, sync_before);

    publishes = send_and_collect_publishes(server, sink, make_did_change(uri, 2, "module m;\nvoid f() {}\n"));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].version.has_value());
    CHECK_EQ(*publishes[0].version, 2);
    CHECK(publishes[0].diagnostics.empty());
    CHECK_EQ(*sf->version(), 2);
    CHECK_NE(sf->content_revision(), revision_before);
    CHECK_LT(sync_before, st.sync_count);
}

TEST_CASE("older duplicate didOpen cannot overwrite an already-open newer document; reopen after close is allowed")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"file:///tmp/dccd_lsp_stale_open.dc"};
    auto text_v2 = "module m;\ni32 a = 0;\ni32 a = 1;\n";

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(uri, 2, text_v2));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].version.has_value());
    CHECK_EQ(*publishes[0].version, 2);
    REQUIRE(publishes[0].diagnostics.size() == 1);

    auto& sm = server.source_manager();
    auto const& st = server.workspace_index().stats();
    auto sync_before = st.sync_count;

    auto fid = sm.find_by_uri(uri);
    REQUIRE(fid.has_value());
    auto const* sf = sm.get(*fid);
    REQUIRE(sf != nullptr);
    auto revision_before = sf->content_revision();

    auto stale_open = send_and_collect_publishes(server, sink, make_did_open(uri, 1, "module m;\ni32 clean = 0;\n"));
    CHECK(stale_open.empty());

    CHECK_EQ(sf->text(), text_v2);
    REQUIRE(sf->version().has_value());
    CHECK_EQ(*sf->version(), 2);
    CHECK_EQ(sf->content_revision(), revision_before);
    CHECK_EQ(st.sync_count, sync_before);

    auto resync = send_and_collect_publishes(server, sink, make_did_open(uri, 2, "module m;\ni32 other = 0;\n"));
    REQUIRE(resync.size() == 1);
    REQUIRE(resync[0].version.has_value());
    CHECK_EQ(*resync[0].version, 2);
    CHECK_EQ(sf->text(), "module m;\ni32 other = 0;\n");
    CHECK_NE(sf->content_revision(), revision_before);
    CHECK_LT(sync_before, st.sync_count);

    publishes = send_and_collect_publishes(server, sink, make_did_open(uri, 3, "module m;\ni32 g = unknown_name;\n"));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].version.has_value());
    CHECK_EQ(*publishes[0].version, 3);
    REQUIRE(publishes[0].diagnostics.size() == 1);
    CHECK_EQ(sf->text(), "module m;\ni32 g = unknown_name;\n");
    CHECK_EQ(*sf->version(), 3);

    auto close_publishes = send_and_collect_publishes(server, sink, make_did_close(uri));
    REQUIRE(close_publishes.size() == 1);
    CHECK(close_publishes[0].diagnostics.empty());

    auto reopen = send_and_collect_publishes(server, sink, make_did_open(uri, 1, "module m;\ni32 reopened = 0;\ni32 reopened = 1;\n"));
    REQUIRE(reopen.size() == 1);
    REQUIRE(reopen[0].version.has_value());
    CHECK_EQ(*reopen[0].version, 1);
    REQUIRE(reopen[0].diagnostics.size() == 1);
}

TEST_CASE("errors in an imported disk file are published to that file and cleared")
{
    TempDir td;
    auto imp_path = td.path / "imp.dc";
    auto main_path = td.path / "main.dc";

    {
        std::ofstream out{imp_path};
        out << "module imp;\ni32 bad = unknown_name;\n";
    }

    auto imp_uri = dcc::sm::SourceManager::to_file_uri(imp_path);
    auto main_uri = dcc::sm::SourceManager::to_file_uri(main_path);

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    {
        std::ofstream out{main_path};
        out << "module main;\nimport imp;\n\nvoid f() {}\n";
    }

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(main_uri, 1, "module main;\nimport imp;\n\nvoid f() {}\n"));

    REQUIRE(publishes.size() == 1);
    CHECK_EQ(publishes[0].uri, imp_uri);
    REQUIRE(publishes[0].diagnostics.size() == 1);
    CHECK(publishes[0].diagnostics[0].message.find("unknown name `unknown_name`") != std::string::npos);
    CHECK(!publishes[0].version.has_value());

    {
        auto publishes2 = send_and_collect_publishes(server, sink, make_did_change(main_uri, 2, "module main;\n\nvoid f() {}\n"));
        REQUIRE(publishes2.size() == 1);
        CHECK_EQ(publishes2[0].uri, imp_uri);
        CHECK(publishes2[0].diagnostics.empty());
        CHECK(!publishes2[0].version.has_value());
    }
}

TEST_CASE("cross-file relatedInformation survives publication to the edited file")
{
    TempDir td;
    auto imp_path = td.path / "imp.dc";
    auto main_path = td.path / "main.dc";

    {
        std::ofstream out{imp_path};
        out << "module imp;\npublic struct S { i32 value; }\npublic i32 add(S s, i32 x) { return s.value + x; }\n";
    }

    auto imp_uri = dcc::sm::SourceManager::to_file_uri(imp_path);
    auto main_uri = dcc::sm::SourceManager::to_file_uri(main_path);

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto main_text = "module main;\nimport imp;\n\nvoid f() {\n    i32 x = 1;\n    x.add(2);\n}\n";
    {
        std::ofstream out{main_path};
        out << main_text;
    }

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(main_uri, 1, main_text));
    REQUIRE(publishes.size() == 1);

    auto const& main_publish = publishes[0];
    CHECK_EQ(main_publish.uri, main_uri);
    REQUIRE(main_publish.version.has_value());
    CHECK_EQ(*main_publish.version, 1);

    REQUIRE(main_publish.diagnostics.size() == 1);
    auto const& diag = main_publish.diagnostics[0];
    CHECK(diag.message.find("no matching UFCS function for `add` on receiver type `i32`") != std::string::npos);

    CHECK_EQ(diag.range.start.line, 5u);
    CHECK_EQ(diag.range.start.character, 4u);
    CHECK_EQ(diag.range.end.line, 5u);
    CHECK_EQ(diag.range.end.character, 5u);

    REQUIRE(diag.relatedInformation.has_value());
    REQUIRE(diag.relatedInformation->size() == 1);
    auto const& ri = diag.relatedInformation->at(0);
    CHECK(ri.message.find("candidate `add(S, i32)`") != std::string::npos);
    CHECK_EQ(ri.location.uri, imp_uri);
    CHECK_EQ(ri.location.range.start.line, 2u);
    CHECK_EQ(ri.location.range.start.character, 11u);
    CHECK_EQ(ri.location.range.end.line, 2u);
    CHECK_EQ(ri.location.range.end.character, 14u);
}

TEST_CASE("UFCS extra-argument errors narrow to the first extra argument")
{
    TempDir td;
    auto main_path = td.path / "main.dc";
    auto main_uri = dcc::sm::SourceManager::to_file_uri(main_path);

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto main_text = "module main;\n\nstruct S { i32 value; }\n\ni32 add(S s, i32 x) {\n    return s.value + x;\n}\n\nvoid extra() {\n    S s = {1};\n    "
                     "s.add(1, 2, 3);\n}\n";
    {
        std::ofstream out{main_path};
        out << main_text;
    }

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(main_uri, 1, main_text));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].diagnostics.size() == 1);

    auto const& diag = publishes[0].diagnostics[0];
    CHECK(diag.message.find("no matching UFCS function for `add` on receiver type `S`") != std::string::npos);

    CHECK_EQ(diag.range.start.line, 10u);
    CHECK_EQ(diag.range.start.character, 13u);
    CHECK_EQ(diag.range.end.line, 10u);
    CHECK_EQ(diag.range.end.character, 14u);
}

TEST_CASE("UFCS receiver-type errors narrow to the receiver expression")
{
    TempDir td;
    auto main_path = td.path / "main.dc";
    auto main_uri = dcc::sm::SourceManager::to_file_uri(main_path);

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto main_text =
        "module main;\n\nstruct S { i32 value; }\n\ni32 add(S s, i32 x) {\n    return s.value + x;\n}\n\nvoid receiver() {\n    i32 x = 1;\n    x.add(2);\n}\n";
    {
        std::ofstream out{main_path};
        out << main_text;
    }

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(main_uri, 1, main_text));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].diagnostics.size() == 1);

    auto const& diag = publishes[0].diagnostics[0];
    CHECK(diag.message.find("no matching UFCS function for `add` on receiver type `i32`") != std::string::npos);

    CHECK_EQ(diag.range.start.line, 10u);
    CHECK_EQ(diag.range.start.character, 4u);
    CHECK_EQ(diag.range.end.line, 10u);
    CHECK_EQ(diag.range.end.character, 5u);
}

SECTION("lsp: completion");

TEST_CASE("completion ranks locals and parameters above globals and filters type-only symbols in value contexts")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 val_global = 1;\nstruct val_struct {}\ni32 val_global_fn(i32 x) { return x; }\nvoid f(i32 val_param) {\n    i32 "
                            "val_local = 2;\n    i32 x = 1;\n    x = val|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    auto const* local = find_item(items, "val_local");
    auto const* param = find_item(items, "val_param");
    auto const* global = find_item(items, "val_global");
    auto const* global_fn = find_item(items, "val_global_fn");
    REQUIRE(local != nullptr);
    REQUIRE(param != nullptr);
    REQUIRE(global != nullptr);
    REQUIRE(global_fn != nullptr);

    REQUIRE(local->sortText.has_value());
    REQUIRE(param->sortText.has_value());
    REQUIRE(global->sortText.has_value());
    CHECK_LT(*local->sortText, *global->sortText);
    CHECK_LT(*param->sortText, *global->sortText);

    REQUIRE(!items.empty());
    REQUIRE(items.front().preselect.has_value());
    CHECK(*items.front().preselect);
    CHECK_EQ(items.front().label, "val_local");

    CHECK(!has_label(items, "val_struct"));
    CHECK(!has_label(items, "x"));
    CHECK(!has_label(items, "f"));
}

TEST_CASE("completion dedups shadowed names keeping the innermost local with its rank and type")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nbool shadow = true;\nvoid f() {\n    i32 shadow = 2;\n    i32 x = 1;\n    x = sha|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    std::size_t count = 0;
    dccd::protocol::CompletionItem const* item = nullptr;
    for (auto const& it : items)
        if (it.label == "shadow")
        {
            ++count;
            item = &it;
        }
    CHECK_EQ(count, 1u);
    REQUIRE(item != nullptr);
    REQUIRE(item->sortText.has_value());
    CHECK_EQ(*item->sortText, "0");
    REQUIRE(item->detail.has_value());
    CHECK_EQ(*item->detail, "i32");
}

TEST_CASE("completion enumerates fields and UFCS methods for `obj.`")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{
        "module m;\nstruct Point { i32 x; i32 y; }\ni32 area(Point p) { return p.x * p.y; }\nvoid f() {\n    Point p = {1, 2};\n    i32 q = p.|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    auto const* x = find_item(items, "x");
    auto const* y = find_item(items, "y");
    auto const* area = find_item(items, "area");
    REQUIRE(x != nullptr);
    REQUIRE(y != nullptr);
    REQUIRE(area != nullptr);

    CHECK(x->kind == dccd::protocol::CompletionItemKind::Field);
    CHECK(y->kind == dccd::protocol::CompletionItemKind::Field);
    CHECK(area->kind == dccd::protocol::CompletionItemKind::Method);

    REQUIRE(x->insertText.has_value());
    CHECK_EQ(*x->insertText, "x");
    REQUIRE(area->command.has_value());
    CHECK_EQ(area->command->command, dccd::protocol::kTriggerParameterHintsCommand);

    CHECK(!has_label(items, "q"));
    CHECK(!has_label(items, "Point"));
    CHECK(!has_label(items, "p"));
}

TEST_CASE("completion resolves the receiver type for `foo.ba` and prefix-filters")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nstruct Point { i32 bx; i32 by; i32 cx; }\nvoid f() {\n    Point p = {1, 2, 3};\n    i32 x = p.b|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    REQUIRE(has_label(items, "bx"));
    REQUIRE(has_label(items, "by"));
    CHECK(!has_label(items, "cx"));
    CHECK(!has_label(items, "p"));
    CHECK(!has_label(items, "x"));
}

TEST_CASE("completion resolves namespace path and prefix (`lib::bi|`)")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto lib_text =
        std::string{"module lib;\npublic i32 binary_search(i32 v) { return v; }\npublic struct bitmap { i32 w; }\npublic i32 other_fn() { return 0; }\n"};
    std::ignore = open_file(server, sink, td.path / "lib.dc", lib_text);

    auto text = std::string{"module main;\nimport lib;\nvoid f() {\n    i32 x = lib::bi|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    auto const* bin = find_item(items, "binary_search");
    auto const* bmp = find_item(items, "bitmap");
    REQUIRE(bin != nullptr);
    REQUIRE(bmp != nullptr);
    CHECK(bin->kind == dccd::protocol::CompletionItemKind::Function);
    CHECK(bmp->kind == dccd::protocol::CompletionItemKind::Struct);

    CHECK(!has_label(items, "other_fn"));
    CHECK(!has_label(items, "x"));
    CHECK(!has_label(items, "lib"));
}

TEST_CASE("completion resolves the dcc-core namespace path (`core::at|`)")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module main;\nimport core;\nvoid f() {\n    i32 x = core::at|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    REQUIRE(has_label(items, "atomic"));
    CHECK(!has_label(items, "x"));
}

TEST_CASE("completion preserves the prefix through whitespace for `if so|` and filters keywords")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 some_fn() { return 0; }\nvoid f() {\n    i32 x = 1;\n    if so| {\n    }\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    REQUIRE(has_label(items, "some_fn"));
    CHECK(!has_label(items, "sizeof"));
    CHECK(!has_label(items, "struct"));
    CHECK(!has_label(items, "import"));
    CHECK(!has_label(items, "while"));
}

TEST_CASE("completion offers `alignof` for `return al|` and filters same-case type names")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nstruct alpha {}\nvoid f() {\n    return al|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    auto const* al = find_item(items, "alignof");
    REQUIRE(al != nullptr);
    CHECK(al->kind == dccd::protocol::CompletionItemKind::Keyword);

    CHECK(!has_label(items, "alpha"));
}

TEST_CASE("completion value context excludes type keywords that match the prefix")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 consume(i32 x) { return x; }\nvoid f() {\n    i32 x = 1;\n    x = con|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    REQUIRE(has_label(items, "consume"));
    CHECK(!has_label(items, "const"));
    CHECK(!has_label(items, "struct"));
    CHECK(!has_label(items, "return"));
}

TEST_CASE("completion emits function snippets and preserves existing parens")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;

    {
        auto text = std::string{"module m;\ni32 compute(i32 a, i32 b) { return a + b; }\nvoid f() {\n    i32 x = comp|;\n}\n"};
        auto pos = position_of_marker(text, '|');
        text.erase(text.find('|'), 1);
        auto uri = open_file(server, sink, td.path / "main.dc", text);

        auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
        auto const* item = find_item(items, "compute");
        REQUIRE(item != nullptr);

        REQUIRE(item->insertText.has_value());
        CHECK_EQ(*item->insertText, "compute(${1}, ${2})");
        REQUIRE(item->insertTextFormat.has_value());
        CHECK_EQ(*item->insertTextFormat, dccd::protocol::InsertTextFormat::Snippet);

        REQUIRE(item->textEdit.has_value());
        CHECK_EQ(item->textEdit->newText, "compute(${1}, ${2})");
        CHECK_EQ(item->textEdit->range.start.line, pos.line);
        CHECK_EQ(item->textEdit->range.start.character, pos.character - 4u);
        CHECK_EQ(item->textEdit->range.end.character, pos.character);
        REQUIRE(item->command.has_value());
        CHECK_EQ(item->command->command, dccd::protocol::kTriggerParameterHintsCommand);
    }

    {
        auto text = std::string{"module m;\ni32 compute(i32 a, i32 b) { return a + b; }\nvoid f() {\n    i32 x = comp|(1, 2);\n}\n"};
        auto pos = position_of_marker(text, '|');
        text.erase(text.find('|'), 1);
        auto uri = open_file(server, sink, td.path / "main.dc", text);

        auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
        auto const* item = find_item(items, "compute");
        REQUIRE(item != nullptr);

        REQUIRE(item->insertText.has_value());
        CHECK_EQ(*item->insertText, "compute");
        REQUIRE(item->insertTextFormat.has_value());
        CHECK_EQ(*item->insertTextFormat, dccd::protocol::InsertTextFormat::PlainText);
        REQUIRE(item->textEdit.has_value());
        CHECK_EQ(item->textEdit->newText, "compute");
    }
}

TEST_CASE("completion works on partially malformed files")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nstruct S { i32 value; }\nvoid f() {\n    S s = {1};\n    i32 broken = ;\n    i32 x = s.|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    auto const* value = find_item(items, "value");
    REQUIRE(value != nullptr);
    CHECK(value->kind == dccd::protocol::CompletionItemKind::Field);
    CHECK(!has_label(items, "x"));
}

TEST_CASE("completion textEdit uses a UTF-16 replacement range with non-ASCII before the token")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 compute(i32 a, i32 b) { return a + b; }\nvoid f() {\n    \"caf\u00e9 latte\"; i32 x = comp|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
    auto const* item = find_item(items, "compute");
    REQUIRE(item != nullptr);
    REQUIRE(item->textEdit.has_value());

    CHECK_EQ(item->textEdit->range.start.line, pos.line);
    CHECK_EQ(item->textEdit->range.start.character, pos.character - 4u);
    CHECK_EQ(item->textEdit->range.start.character, 26u);
    CHECK_EQ(item->textEdit->range.end.character, pos.character);
    CHECK_EQ(item->textEdit->range.end.character, 30u);
}

TEST_CASE("asm completion detects placeholders after escapes and reports UTF-16 replacement ranges")
{
    {
        Sink sink;
        dccd::LanguageServer server{&sink.stream};
        initialize_server(server, sink);

        TempDir td;
        auto text = std::string{"module main;\nvoid f() {\n    asm @[inout(x in ecx)] { \"nop \\n %[x|]\" };\n}\n"};
        auto [src, pos] = strip_marker(text, '|');
        auto uri = open_file(server, sink, td.path / "main.dc", src);

        auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
        auto const* item = find_item(items, "x");
        REQUIRE(item != nullptr);
        REQUIRE(item->textEdit.has_value());
        CHECK_EQ(item->textEdit->newText, "x");
        CHECK_EQ(item->textEdit->range.start.line, 2u);
        CHECK_EQ(item->textEdit->range.start.character, 39u);
        CHECK_EQ(item->textEdit->range.end.line, 2u);
        CHECK_EQ(item->textEdit->range.end.character, 40u);
    }

    {
        Sink sink;
        dccd::LanguageServer server{&sink.stream};
        initialize_server(server, sink);

        TempDir td;
        auto text = std::string{"module main;\nvoid f() {\n    asm @[inout(y in ecx)] { \"\\u{25}[y|]\" };\n}\n"};
        auto [src, pos] = strip_marker(text, '|');
        auto uri = open_file(server, sink, td.path / "main.dc", src);

        auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
        auto const* item = find_item(items, "y");
        REQUIRE(item != nullptr);
        REQUIRE(item->textEdit.has_value());
        CHECK_EQ(item->textEdit->range.start.line, 2u);
        CHECK_EQ(item->textEdit->range.start.character, 37u);
        CHECK_EQ(item->textEdit->range.end.line, 2u);
        CHECK_EQ(item->textEdit->range.end.character, 38u);
    }

    {
        Sink sink;
        dccd::LanguageServer server{&sink.stream};
        initialize_server(server, sink);

        TempDir td;
        auto text = std::string{"module main;\nvoid f() {\n    asm @[inout(z in ecx)] { \"\u2192 %[z|]\" };\n}\n"};
        auto [src, pos] = strip_marker(text, '|');
        auto uri = open_file(server, sink, td.path / "main.dc", src);

        auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
        auto const* item = find_item(items, "z");
        REQUIRE(item != nullptr);
        REQUIRE(item->textEdit.has_value());
        CHECK_EQ(item->textEdit->range.start.line, 2u);
        CHECK_EQ(item->textEdit->range.start.character, 34u);
        CHECK_EQ(item->textEdit->range.end.line, 2u);
        CHECK_EQ(item->textEdit->range.end.character, 35u);
    }

    {
        Sink sink;
        dccd::LanguageServer server{&sink.stream};
        initialize_server(server, sink, {}, {"utf-8"});

        TempDir td;
        auto text = std::string{"module main;\nvoid f() {\n    asm @[inout(z in ecx)] { \"\u2192 %[z|]\" };\n}\n"};
        auto first_nl = text.find('\n');
        auto second_nl = text.find('\n', first_nl + 1);
        auto line2_byte = second_nl + 1;
        auto marker_byte = text.find('|');
        auto utf8_col = static_cast<std::uint32_t>(marker_byte - line2_byte);
        CHECK_EQ(utf8_col, 37u);
        auto [src, pos] = strip_marker(text, '|');
        auto uri = open_file(server, sink, td.path / "main.dc", src);

        auto items = request_completion_items(server, sink, uri, pos.line, utf8_col);
        auto const* item = find_item(items, "z");
        REQUIRE(item != nullptr);
        REQUIRE(item->textEdit.has_value());
        CHECK_EQ(item->textEdit->range.start.line, 2u);
        CHECK_EQ(item->textEdit->range.start.character, 36u);
        CHECK_EQ(item->textEdit->range.end.line, 2u);
        CHECK_EQ(item->textEdit->range.end.character, 37u);
    }
}

SECTION("lsp: signature help");

TEST_CASE("signature help selects the innermost call and reports the active parameter")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{
        "module m;\ni32 add(i32 a, i32 b) { return a + b; }\ni32 mul(i32 a, i32 b) { return a * b; }\nvoid f() {\n    i32 x = add(mul(1, |), 3);\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
    REQUIRE(help.has_value());
    REQUIRE(help->signatures.size() >= 1);
    CHECK(help->activeSignature < help->signatures.size());
    CHECK(help->signatures[help->activeSignature].label.find("mul") != std::string::npos);
    CHECK(help->signatures[help->activeSignature].label.find("add") == std::string::npos);
    CHECK_EQ(help->activeParameter, 1u);
}

TEST_CASE("signature help selects the outer call from its own argument position")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text =
        std::string{"module m;\ni32 add(i32 a, i32 b) { return a + b; }\ni32 mul(i32 a, i32 b) { return a * b; }\nvoid f() {\n    i32 x = add(1, |);\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
    REQUIRE(help.has_value());
    REQUIRE(help->signatures.size() >= 1);
    CHECK(help->signatures[help->activeSignature].label.find("add") != std::string::npos);
    CHECK_EQ(help->activeParameter, 1u);
}

TEST_CASE("signature help counts only top-level commas across nested calls")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 add3(i32 a, i32 b, i32 c) { return a + b + c; }\ni32 mul(i32 a, i32 b) { return a * b; }\nvoid f() {\n    i32 x = "
                            "add3(1, mul(2, 3), |);\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
    REQUIRE(help.has_value());
    CHECK(help->signatures[help->activeSignature].label.find("add3") != std::string::npos);

    CHECK_EQ(help->activeParameter, 2u);
}

TEST_CASE("signature help handles trailing comma and unclosed calls")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;

    {
        auto text = std::string{"module m;\ni32 add(i32 a, i32 b) { return a + b; }\nvoid f() {\n    i32 x = add(1, |);\n}\n"};
        auto pos = position_of_marker(text, '|');
        text.erase(text.find('|'), 1);
        auto uri = open_file(server, sink, td.path / "main.dc", text);

        auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
        REQUIRE(help.has_value());
        CHECK(help->signatures[help->activeSignature].label.find("add") != std::string::npos);
        CHECK_EQ(help->activeParameter, 1u);
    }

    {
        auto text = std::string{"module m;\ni32 add(i32 a, i32 b) { return a + b; }\nvoid f() {\n    i32 x = add(1, |\n"};
        auto pos = position_of_marker(text, '|');
        text.erase(text.find('|'), 1);
        auto uri = open_file(server, sink, td.path / "main.dc", text);

        auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
        REQUIRE(help.has_value());
        REQUIRE(help->signatures.size() >= 1);
        CHECK(help->signatures[help->activeSignature].label.find("add") != std::string::npos);
        CHECK_EQ(help->activeParameter, 1u);
    }
}

TEST_CASE("signature help applies the UFCS receiver offset")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text =
        std::string{"module m;\nstruct S { i32 value; }\ni32 add(S s, i32 x) { return s.value + x; }\nvoid f() {\n    S s = {1};\n    i32 y = s.add(|);\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
    REQUIRE(help.has_value());
    REQUIRE(help->signatures.size() >= 1);
    auto const& sig = help->signatures[help->activeSignature];
    CHECK(sig.label.find("add") != std::string::npos);
    CHECK(sig.label.find("S s") != std::string::npos);
    CHECK(sig.label.find("i32 x") != std::string::npos);
    CHECK_EQ(help->activeParameter, 1u);
}

TEST_CASE("signature help reports overload alternatives and a deterministic active signature")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{
        "module m;\ni32 pick(i32 a) { return a; }\ni32 pick(i32 a, i32 b, i32 c) { return a + b + c; }\nvoid f() {\n    i32 x = pick(1, 2, |);\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
    REQUIRE(help.has_value());
    REQUIRE(help->signatures.size() == 2);
    CHECK(help->signatures[help->activeSignature].label.find("pick(i32 a, i32 b, i32 c)") != std::string::npos);
    CHECK_EQ(help->activeParameter, 2u);

    CHECK(help->activeParameter < help->signatures[help->activeSignature].parameters.size());
}

TEST_CASE("signature help supports generic template functions")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 ident(T)(T v) { return 0; }\nvoid f() {\n    i32 x = ident(|);\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
    REQUIRE(help.has_value());
    REQUIRE(help->signatures.size() == 1);
    CHECK(help->signatures[0].label.find("ident") != std::string::npos);
    CHECK_EQ(help->activeParameter, 0u);
}

TEST_CASE("completion does not fire inside comments or string literals")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;

    {
        auto text = std::string{"module m;\ni32 value = 1;\nvoid f() {\n    // some| comment\n}\n"};
        auto pos = position_of_marker(text, '|');
        text.erase(text.find('|'), 1);
        auto uri = open_file(server, sink, td.path / "main.dc", text);

        auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
        CHECK(items.empty());
    }

    {
        auto text = std::string{"module m;\ni32 value = 1;\nvoid f() {\n    /* val| */\n}\n"};
        auto pos = position_of_marker(text, '|');
        text.erase(text.find('|'), 1);
        auto uri = open_file(server, sink, td.path / "main.dc", text);

        auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
        CHECK(items.empty());
    }

    {
        auto text = std::string{"module m;\ni32 value = 1;\nvoid f() {\n    i32 x = \"val|\";\n}\n"};
        auto pos = position_of_marker(text, '|');
        text.erase(text.find('|'), 1);
        auto uri = open_file(server, sink, td.path / "main.dc", text);

        auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
        CHECK(items.empty());
    }
}

TEST_CASE("completion does not trigger member completion inside a comment")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nstruct Point { i32 x; i32 y; }\nvoid f() {\n    Point p = {1, 2};\n    // p.| in a comment\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
    CHECK(!has_label(items, "x"));
    CHECK(!has_label(items, "y"));
}

TEST_CASE("completion sees an existing `(` across comments and never duplicates it")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 compute(i32 a, i32 b) { return a + b; }\nvoid f() {\n    i32 x = comp| /* hi */ (1, 2);\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);
    auto const* item = find_item(items, "compute");
    REQUIRE(item != nullptr);

    REQUIRE(item->insertText.has_value());
    CHECK_EQ(*item->insertText, "compute");
    REQUIRE(item->insertTextFormat.has_value());
    CHECK_EQ(*item->insertTextFormat, dccd::protocol::InsertTextFormat::PlainText);
    REQUIRE(item->textEdit.has_value());
    CHECK_EQ(item->textEdit->newText, "compute");

    auto text2 = std::string{"module m;\ni32 compute(i32 a, i32 b) { return a + b; }\nvoid f() {\n    i32 x = comp| // hi\n    (1, 2);\n}\n"};
    auto pos2 = position_of_marker(text2, '|');
    text2.erase(text2.find('|'), 1);
    auto uri2 = open_file(server, sink, td.path / "main.dc", text2);

    auto items2 = request_completion_items(server, sink, uri2, pos2.line, pos2.character);
    auto const* item2 = find_item(items2, "compute");
    REQUIRE(item2 != nullptr);
    REQUIRE(item2->insertText.has_value());
    CHECK_EQ(*item2->insertText, "compute");
}

TEST_CASE("completion does not leak current-module symbols for unresolved namespace paths")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto lib_text = std::string{"module lib;\npublic i32 binary_search(i32 v) { return v; }\npublic struct bitmap { i32 w; }\n"};
    std::ignore = open_file(server, sink, td.path / "lib.dc", lib_text);

    auto text = std::string{"module main;\nimport lib;\nvoid f() {\n    i32 x = missing::bi|;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto items = request_completion_items(server, sink, uri, pos.line, pos.character);

    CHECK(!has_label(items, "binary_search"));
    CHECK(!has_label(items, "bitmap"));
    CHECK(!has_label(items, "lib"));
    CHECK(!has_label(items, "x"));
}

TEST_CASE("signature help counts commas and parens inside comments and strings correctly")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;

    {
        auto text = std::string{
            "module m;\ni32 pick(i32 a) { return a; }\ni32 pick(i32 a, i32 b) { return a + b; }\nvoid f() {\n    i32 x = pick(1, /* , ( */ 2|);\n}\n"};
        auto pos = position_of_marker(text, '|');
        text.erase(text.find('|'), 1);
        auto uri = open_file(server, sink, td.path / "main.dc", text);

        auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
        REQUIRE(help.has_value());

        CHECK(help->signatures[help->activeSignature].label.find("pick(i32 a, i32 b)") != std::string::npos);
        CHECK_EQ(help->activeParameter, 1u);
    }

    {
        auto text =
            std::string{"module m;\ni32 pick(i32 a) { return a; }\ni32 pick(i32 a, i32 b) { return a + b; }\nvoid f() {\n    i32 x = pick(1, \"a,b\"|);\n}\n"};
        auto pos = position_of_marker(text, '|');
        text.erase(text.find('|'), 1);
        auto uri = open_file(server, sink, td.path / "main.dc", text);

        auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
        REQUIRE(help.has_value());
        CHECK(help->signatures[help->activeSignature].label.find("pick(i32 a, i32 b)") != std::string::npos);
        CHECK_EQ(help->activeParameter, 1u);
    }
}

TEST_CASE("signature help is not offered inside a function declaration parameter list")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nvoid f(i32 a, |);\nvoid g() {\n    i32 x = 1;\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
    CHECK(!help.has_value());
}

TEST_CASE("signature help selects the correct overload mid-argument (plain)")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 f(i32 a) { return a; }\ni32 f(i32 a, i32 b) { return a + b; }\nvoid g() {\n    i32 x = f(1, 2|);\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
    REQUIRE(help.has_value());
    REQUIRE(help->signatures.size() == 2);
    CHECK(help->signatures[help->activeSignature].label.find("f(i32 a, i32 b)") != std::string::npos);
    CHECK_EQ(help->activeParameter, 1u);
}

TEST_CASE("signature help selects the correct overload mid-argument (UFCS)")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nstruct S { i32 value; }\ni32 add(S s, i32 x) { return s.value + x; }\ni32 add(S s, i32 x, i32 y) { return s.value + x "
                            "+ y; }\nvoid g() {\n    S s = {1};\n    i32 y = s.add(5|);\n}\n"};
    auto pos = position_of_marker(text, '|');
    text.erase(text.find('|'), 1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto help = request_signature_help(server, sink, uri, pos.line, pos.character);
    REQUIRE(help.has_value());
    REQUIRE(help->signatures.size() == 2);
    CHECK(help->signatures[help->activeSignature].label.find("add(S s, i32 x)") != std::string::npos);
    CHECK_EQ(help->activeParameter, 1u);
}

SECTION("lsp: symbol navigation");

TEST_CASE("definition uses exact name ranges including fields")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{
        "module m;\nstruct Point {\n    i32 value;\n}\ni32 global = 1;\nvoid f() {\n    Point p = {1};\n    i32 x = p.val|ue;\n    i32 y = glob|al;\n}\n"};
    auto [t1, field_use] = strip_marker(text, '|');
    text = std::move(t1);
    auto [t2, global_use] = strip_marker(text, '|');
    text = std::move(t2);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto def = request_definition(server, sink, uri, field_use.line, field_use.character);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, uri);
    CHECK_EQ(def->range.start.line, 2u);
    CHECK_EQ(def->range.start.character, 8u);
    CHECK_EQ(def->range.end.line, 2u);
    CHECK_EQ(def->range.end.character, 13u);

    def = request_definition(server, sink, uri, global_use.line, global_use.character);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, uri);
    CHECK_EQ(def->range.start.line, 4u);
    CHECK_EQ(def->range.start.character, 4u);
    CHECK_EQ(def->range.end.line, 4u);
    CHECK_EQ(def->range.end.character, 10u);

    def = request_definition(server, sink, uri, 4u, 5u);
    REQUIRE(def.has_value());
    CHECK_EQ(def->range.start.line, 4u);
    CHECK_EQ(def->range.start.character, 4u);
    CHECK_EQ(def->range.end.character, 10u);

    def = request_definition(server, sink, uri, 6u, 5u);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, uri);
    CHECK_EQ(def->range.start.line, 1u);
    CHECK_EQ(def->range.start.character, 7u);
    CHECK_EQ(def->range.end.character, 12u);
}

TEST_CASE("UFCS methods with identical names on different receiver types stay distinct")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nstruct A { i32 x; }\nstruct B { i32 x; }\ni32 get(A a) { return a.x; }\ni32 get(B b) { return b.x; }\nvoid f() {\n    "
                            "A a = {1};\n    B b = {2};\n    i32 p = a.ge|t();\n    i32 q = b.get();\n}\n"};
    auto [t1, use_a] = strip_marker(text, '|');
    text = std::move(t1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto def = request_definition(server, sink, uri, use_a.line, use_a.character);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, uri);
    CHECK_EQ(def->range.start.line, 3u);
    CHECK_EQ(def->range.start.character, 4u);
    CHECK_EQ(def->range.end.character, 7u);

    auto refs = request_references(server, sink, uri, 3u, 5u, true);
    REQUIRE(refs.size() == 2);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 3u, 4u, 7u));
    CHECK(has_range(ranges, 8u, 14u, 17u));
    CHECK(!has_range(ranges, 9u, 14u, 17u));

    refs = request_references(server, sink, uri, 4u, 5u, true);
    REQUIRE(refs.size() == 2);
    ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 4u, 4u, 7u));
    CHECK(has_range(ranges, 9u, 14u, 17u));
}

TEST_CASE("parameter and field with the same name resolve independently")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nstruct S { i32 value; }\nvoid f(i32 value) {\n    S s = {1};\n    i32 x = s.valu|e;\n    i32 y = valu|e;\n}\n"};
    auto [t1, field_use] = strip_marker(text, '|');
    text = std::move(t1);
    auto [t2, param_use] = strip_marker(text, '|');
    text = std::move(t2);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto refs = request_references(server, sink, uri, field_use.line, field_use.character, true);
    REQUIRE(refs.size() == 2);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 15u, 20u));
    CHECK(has_range(ranges, 4u, 14u, 19u));
    CHECK(!has_range(ranges, 5u, 12u, 17u));

    refs = request_references(server, sink, uri, param_use.line, param_use.character, true);
    REQUIRE(refs.size() == 2);
    ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 2u, 11u, 16u));
    CHECK(has_range(ranges, 5u, 12u, 17u));
    CHECK(!has_range(ranges, 4u, 14u, 19u));
}

TEST_CASE("shadowed local and global resolve to their own references")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 shadow = 1;\nvoid f() {\n    i32 shadow = 2;\n    i32 x = shadow;\n}\nvoid g() {\n    i32 y = shadow;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto refs = request_references(server, sink, uri, 1u, 5u, true);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 4u, 10u));
    CHECK(has_range(ranges, 7u, 12u, 18u));
    CHECK(!has_range(ranges, 3u, 8u, 14u));
    CHECK(!has_range(ranges, 4u, 12u, 18u));

    refs = request_references(server, sink, uri, 3u, 9u, true);
    REQUIRE(refs.size() == 2);
    ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 3u, 8u, 14u));
    CHECK(has_range(ranges, 4u, 12u, 18u));
    CHECK(!has_range(ranges, 7u, 12u, 18u));
}

TEST_CASE("nested scopes pick the innermost declaration")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 x = 0;\nvoid f() {\n    i32 x = 1;\n    {\n        i32 x = 2;\n        i32 y = x;\n    }\n    i32 z = x;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto refs = request_references(server, sink, uri, 5u, 12u, true);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 5u, 12u, 13u));
    CHECK(has_range(ranges, 6u, 16u, 17u));
    CHECK(!has_range(ranges, 3u, 8u, 9u));
    CHECK(!has_range(ranges, 8u, 12u, 13u));
    CHECK(!has_range(ranges, 1u, 4u, 5u));

    refs = request_references(server, sink, uri, 3u, 8u, true);
    REQUIRE(refs.size() == 2);
    ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 3u, 8u, 9u));
    CHECK(has_range(ranges, 8u, 12u, 13u));

    refs = request_references(server, sink, uri, 1u, 4u, true);
    REQUIRE(refs.size() == 1);
    CHECK(has_range(locations_to_ranges(refs), 1u, 4u, 5u));
}

TEST_CASE("module-qualified definition and cross-file references")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto lib_text = std::string{"module lib;\npublic i32 binary_search(i32 v) { return v; }\npublic struct bitmap { i32 w; }\n"};
    auto lib_uri = open_file(server, sink, td.path / "lib.dc", lib_text);

    auto main_text = std::string{"module main;\nimport lib;\nvoid f() {\n    i32 x = lib::binary_sear|ch(1);\n    i32 y = lib::binary_search(2);\n}\n"};
    auto [t1, use_pos] = strip_marker(main_text, '|');
    main_text = std::move(t1);
    auto main_uri = open_file(server, sink, td.path / "main.dc", main_text);

    auto def = request_definition(server, sink, main_uri, use_pos.line, use_pos.character);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, lib_uri);
    CHECK_EQ(def->range.start.line, 1u);
    CHECK_EQ(def->range.start.character, 11u);
    CHECK_EQ(def->range.end.line, 1u);
    CHECK_EQ(def->range.end.character, 24u);

    def = request_definition(server, sink, main_uri, 1u, 8u);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, lib_uri);
    CHECK_EQ(def->range.start.line, 0u);
    CHECK_EQ(def->range.start.character, 0u);

    auto refs = request_references(server, sink, lib_uri, 1u, 12u, true);
    REQUIRE(refs.size() == 3);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 11u, 24u));
    CHECK(has_range(ranges, 3u, 17u, 30u));
    CHECK(has_range(ranges, 4u, 17u, 30u));
}

TEST_CASE("same-name symbols in unrelated files never cross-match")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto a_text = std::string{"module a;\npublic i32 shared = 1;\n"};
    auto a_uri = open_file(server, sink, td.path / "a.dc", a_text);

    auto b_text = std::string{"module b;\npublic i32 shared = 2;\n"};
    auto b_uri = open_file(server, sink, td.path / "b.dc", b_text);

    auto main_text = std::string{"module main;\nimport a;\nimport b;\nvoid f() {\n    i32 x = a::shared;\n    i32 y = b::shared;\n}\n"};
    auto main_uri = open_file(server, sink, td.path / "main.dc", main_text);

    auto refs = request_references(server, sink, a_uri, 1u, 11u, true);
    REQUIRE(refs.size() == 2);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 11u, 17u));
    CHECK(has_range(ranges, 4u, 15u, 21u));
    for (auto const& loc : refs)
        CHECK_NE(loc.uri, b_uri);

    refs = request_references(server, sink, b_uri, 1u, 11u, true);
    REQUIRE(refs.size() == 2);
    for (auto const& loc : refs)
        CHECK_NE(loc.uri, a_uri);
    ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 11u, 17u));
    CHECK(has_range(ranges, 5u, 15u, 21u));
}

TEST_CASE("generic template call resolves to the generic declaration for definition and references")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 ident(T)(T v) { return 0; }\nvoid f() {\n    i32 a = ide|nt(1);\n    i32 b = ident(2);\n}\n"};
    auto [t1, call_pos] = strip_marker(text, '|');
    text = std::move(t1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto def = request_definition(server, sink, uri, call_pos.line, call_pos.character);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, uri);
    CHECK_EQ(def->range.start.line, 1u);
    CHECK_EQ(def->range.start.character, 4u);
    CHECK_EQ(def->range.end.line, 1u);
    CHECK_EQ(def->range.end.character, 9u);

    auto refs = request_references(server, sink, uri, 1u, 5u, true);
    REQUIRE(refs.size() == 3);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 4u, 9u));
    CHECK(has_range(ranges, 3u, 12u, 17u));
    CHECK(has_range(ranges, 4u, 12u, 17u));
}

TEST_CASE("references includeDeclaration flag controls the declaration range")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 counter = 0;\nvoid f() {\n    i32 x = counter;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto refs = request_references(server, sink, uri, 1u, 5u, false);
    REQUIRE(refs.size() == 1);
    CHECK_EQ(refs[0].range.start.line, 3u);
    CHECK_EQ(refs[0].range.start.character, 12u);
    CHECK_EQ(refs[0].range.end.character, 19u);

    refs = request_references(server, sink, uri, 1u, 5u, true);
    REQUIRE(refs.size() == 2);
    CHECK(has_range(locations_to_ranges(refs), 1u, 4u, 11u));
    CHECK(has_range(locations_to_ranges(refs), 3u, 12u, 19u));
}

TEST_CASE("references and rename never touch comments or strings")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 counter = 0;\nvoid f() {\n    // counter\n    \"counter\";\n    i32 x = counter;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto refs = request_references(server, sink, uri, 1u, 5u, true);
    REQUIRE(refs.size() == 2);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 4u, 11u));
    CHECK(has_range(ranges, 5u, 12u, 19u));
    CHECK(!has_range(ranges, 3u, 8u, 15u));
    CHECK(!has_range(ranges, 4u, 5u, 12u));

    auto comment_res = send_position_request(server, sink, uri, 3u, 10u, "textDocument/definition");
    CHECK(!comment_res.value.has_value() || comment_res.value->is_null());
    auto string_res = send_position_request(server, sink, uri, 4u, 7u, "textDocument/definition");
    CHECK(!string_res.value.has_value() || string_res.value->is_null());

    auto outcome = request_rename(server, sink, uri, 5u, 14u, "total");
    REQUIRE(outcome.edit.has_value());
    CHECK(!outcome.error.has_value());
    auto it = outcome.edit->changes.find(uri);
    REQUIRE(it != outcome.edit->changes.end());
    CHECK(check_edit_set(it->second, "total", {{1u, 4u, 11u}, {5u, 12u, 19u}}));
}

TEST_CASE("documentHighlight reports only the current file")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto lib_text = std::string{"module lib;\npublic i32 counter = 0;\n"};
    auto lib_uri = open_file(server, sink, td.path / "lib.dc", lib_text);

    auto main_text = std::string{"module main;\nimport lib;\nvoid f() {\n    i32 x = lib::counter;\n}\n"};
    auto main_uri = open_file(server, sink, td.path / "main.dc", main_text);

    auto hl = request_highlights(server, sink, main_uri, 3u, 18u);
    REQUIRE(hl.size() == 1);
    CHECK_EQ(hl[0].start.line, 3u);
    CHECK_EQ(hl[0].start.character, 17u);
    CHECK_EQ(hl[0].end.character, 24u);

    hl = request_highlights(server, sink, lib_uri, 1u, 11u);
    REQUIRE(hl.size() == 1);
    CHECK_EQ(hl[0].start.line, 1u);
    CHECK_EQ(hl[0].start.character, 11u);
    CHECK_EQ(hl[0].end.character, 18u);
}

TEST_CASE("prepareRename returns exact range and placeholder for safe targets")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 counter = 0;\nvoid f() {\n    i32 x = counter;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto prep = request_prepare_rename(server, sink, uri, 1u, 5u);
    REQUIRE(!prep.refused);
    REQUIRE(prep.range.has_value());
    CHECK_EQ(prep.range->start.line, 1u);
    CHECK_EQ(prep.range->start.character, 4u);
    CHECK_EQ(prep.range->end.character, 11u);
    REQUIRE(prep.placeholder.has_value());
    CHECK_EQ(*prep.placeholder, "counter");

    prep = request_prepare_rename(server, sink, uri, 3u, 14u);
    REQUIRE(!prep.refused);
    REQUIRE(prep.range.has_value());
    CHECK_EQ(prep.range->start.line, 3u);
    CHECK_EQ(prep.range->start.character, 12u);
    CHECK_EQ(prep.range->end.character, 19u);
    REQUIRE(prep.placeholder.has_value());
    CHECK_EQ(*prep.placeholder, "counter");
}

TEST_CASE("prepareRename refuses module, import, unknown and unresolved targets")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto lib_text = std::string{"module lib;\npublic i32 value = 1;\n"};
    std::ignore = open_file(server, sink, td.path / "lib.dc", lib_text);

    auto text = std::string{"module m;\nimport lib;\nvoid f() {\n    i32 x = lib::value;\n    i32 y = missing_name;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto prep = request_prepare_rename(server, sink, uri, 0u, 7u);
    CHECK(prep.refused);

    prep = request_prepare_rename(server, sink, uri, 1u, 8u);
    CHECK(prep.refused);

    prep = request_prepare_rename(server, sink, uri, 3u, 18u);
    REQUIRE(!prep.refused);
    REQUIRE(prep.range.has_value());
    CHECK_EQ(prep.range->start.line, 3u);
    CHECK_EQ(prep.range->start.character, 17u);
    CHECK_EQ(prep.range->end.character, 22u);

    prep = request_prepare_rename(server, sink, uri, 4u, 12u);
    CHECK(prep.refused);
}

TEST_CASE("rename validates newName and refuses keywords and non-identifiers")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 counter = 0;\nvoid f() {\n    i32 x = counter;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto outcome = request_rename(server, sink, uri, 1u, 5u, "struct");
    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->message.find("not a valid identifier") != std::string::npos);
    CHECK(!outcome.edit.has_value());

    outcome = request_rename(server, sink, uri, 1u, 5u, "1abc");
    REQUIRE(outcome.error.has_value());
    outcome = request_rename(server, sink, uri, 1u, 5u, "a-b");
    REQUIRE(outcome.error.has_value());

    outcome = request_rename(server, sink, uri, 1u, 5u, "total");
    REQUIRE(outcome.edit.has_value());
    CHECK(!outcome.error.has_value());
    auto it = outcome.edit->changes.find(uri);
    REQUIRE(it != outcome.edit->changes.end());
    CHECK(check_edit_set(it->second, "total", {{1u, 4u, 11u}, {3u, 12u, 19u}}));
}

TEST_CASE("rename refuses module and import targets")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto lib_text = std::string{"module lib;\npublic i32 value = 1;\n"};
    auto lib_uri = open_file(server, sink, td.path / "lib.dc", lib_text);

    auto text = std::string{"module m;\nimport lib;\nvoid f() {\n    i32 x = lib::value;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto outcome = request_rename(server, sink, uri, 0u, 7u, "renamed");
    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->message.find("Cannot rename") != std::string::npos);

    outcome = request_rename(server, sink, uri, 1u, 8u, "renamed");
    REQUIRE(outcome.error.has_value());
}

TEST_CASE("cross-file rename produces exact edits in both files")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto lib_text = std::string{"module lib;\npublic i32 counter = 0;\n"};
    auto lib_uri = open_file(server, sink, td.path / "lib.dc", lib_text);

    auto main_text = std::string{"module main;\nimport lib;\nvoid f() {\n    i32 x = lib::counter;\n}\n"};
    auto main_uri = open_file(server, sink, td.path / "main.dc", main_text);

    auto outcome = request_rename(server, sink, main_uri, 3u, 17u, "total");
    REQUIRE(outcome.edit.has_value());
    CHECK(!outcome.error.has_value());
    REQUIRE(outcome.edit->changes.size() == 2);

    auto it = outcome.edit->changes.find(lib_uri);
    REQUIRE(it != outcome.edit->changes.end());
    CHECK(check_edit_set(it->second, "total", {{1u, 11u, 18u}}));

    it = outcome.edit->changes.find(main_uri);
    REQUIRE(it != outcome.edit->changes.end());
    CHECK(check_edit_set(it->second, "total", {{3u, 17u, 24u}}));
}

TEST_CASE("shadowed rename touches only the shadowed symbol, not the global")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 shadow = 1;\nvoid f() {\n    i32 shadow = 2;\n    i32 x = shadow;\n}\nvoid g() {\n    i32 y = shadow;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto outcome = request_rename(server, sink, uri, 3u, 9u, "local_shadow");
    REQUIRE(outcome.edit.has_value());
    REQUIRE(outcome.edit->changes.size() == 1);
    auto it = outcome.edit->changes.find(uri);
    REQUIRE(it != outcome.edit->changes.end());
    CHECK(check_edit_set(it->second, "local_shadow", {{3u, 8u, 14u}, {4u, 12u, 18u}}));
}

TEST_CASE("non-ASCII before definition, references, rename and hover keeps UTF-16 ranges exact")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\ni32 counter = 0;\nvoid f() {\n    \"caf\u00e9 latte\";\n    i32 x = coun|ter;\n}\n"};
    auto [t1, use_pos] = strip_marker(text, '|');
    text = std::move(t1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto def = request_definition(server, sink, uri, use_pos.line, use_pos.character);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, uri);
    CHECK_EQ(def->range.start.line, 1u);
    CHECK_EQ(def->range.start.character, 4u);
    CHECK_EQ(def->range.end.character, 11u);

    auto refs = request_references(server, sink, uri, use_pos.line, use_pos.character, true);
    REQUIRE(refs.size() == 2);
    CHECK(has_range(locations_to_ranges(refs), 1u, 4u, 11u));
    CHECK(has_range(locations_to_ranges(refs), 4u, 12u, 19u));

    auto prep = request_prepare_rename(server, sink, uri, use_pos.line, use_pos.character);
    REQUIRE(!prep.refused);
    REQUIRE(prep.range.has_value());
    CHECK_EQ(prep.range->start.line, 4u);
    CHECK_EQ(prep.range->start.character, 12u);
    CHECK_EQ(prep.range->end.character, 19u);

    auto outcome = request_rename(server, sink, uri, use_pos.line, use_pos.character, "total");
    REQUIRE(outcome.edit.has_value());
    auto it = outcome.edit->changes.find(uri);
    REQUIRE(it != outcome.edit->changes.end());
    CHECK(check_edit_set(it->second, "total", {{1u, 4u, 11u}, {4u, 12u, 19u}}));

    auto hover = request_hover(server, sink, uri, use_pos.line, use_pos.character);
    REQUIRE(hover.has_value());
    CHECK(hover->find("i32 counter") != std::string::npos);
}

TEST_CASE("enum variant definition and references use exact variant ranges")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nenum Color {\n    Red,\n    Green,\n}\nvoid f() {\n    Color c = Color::Re|d;\n    Color d = Color::Green;\n}\n"};
    auto [t1, use_pos] = strip_marker(text, '|');
    text = std::move(t1);
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto def = request_definition(server, sink, uri, use_pos.line, use_pos.character);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, uri);
    CHECK_EQ(def->range.start.line, 2u);
    CHECK_EQ(def->range.start.character, 4u);
    CHECK_EQ(def->range.end.character, 7u);

    auto refs = request_references(server, sink, uri, 2u, 5u, true);
    REQUIRE(refs.size() == 2);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 2u, 4u, 7u));
    CHECK(has_range(ranges, 6u, 21u, 24u));
    CHECK(!has_range(ranges, 7u, 21u, 26u));
}

TEST_CASE("rename of a field with same-named fields on other types is scoped to its owner")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto text = std::string{"module m;\nstruct A { i32 value; }\nstruct B { i32 value; }\nvoid f() {\n    A a = {1};\n    B b = {2};\n    i32 x = a.value;\n   "
                            " i32 y = b.value;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto outcome = request_rename(server, sink, uri, 1u, 15u, "amount");
    REQUIRE(outcome.edit.has_value());
    REQUIRE(outcome.edit->changes.size() == 1);
    auto it = outcome.edit->changes.find(uri);
    REQUIRE(it != outcome.edit->changes.end());
    CHECK(check_edit_set(it->second, "amount", {{1u, 15u, 20u}, {6u, 14u, 19u}}));
}

TEST_CASE("using-alias uses resolve to the target and refuse rename")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto lib_text = std::string{"module lib;\npublic i32 value = 1;\n"};
    auto lib_uri = open_file(server, sink, td.path / "lib.dc", lib_text);

    auto text = std::string{"module main;\nimport lib;\nusing V = lib::value;\nvoid f() {\n    i32 x = V;\n}\n"};
    auto uri = open_file(server, sink, td.path / "main.dc", text);

    auto resp = send_position_request(server, sink, uri, 4u, 12u, "textDocument/definition");
    REQUIRE(resp.value.has_value());
    REQUIRE(resp.value->is_array());
    REQUIRE(resp.value->array_size() == 2);
    auto const& arr = resp.value->as_array();
    auto alias_loc = dccd::protocol::LspLocation::from_json(arr[0]);
    auto target_loc = dccd::protocol::LspLocation::from_json(arr[1]);
    CHECK_EQ(alias_loc.uri, uri);
    CHECK_EQ(alias_loc.range.start.line, 2u);
    CHECK_EQ(alias_loc.range.start.character, 6u);
    CHECK_EQ(alias_loc.range.end.character, 7u);
    CHECK_EQ(target_loc.uri, lib_uri);
    CHECK_EQ(target_loc.range.start.line, 1u);
    CHECK_EQ(target_loc.range.start.character, 11u);
    CHECK_EQ(target_loc.range.end.character, 16u);

    auto outcome = request_rename(server, sink, uri, 4u, 12u, "renamed");
    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->message.find("Cannot rename") != std::string::npos);

    auto prep = request_prepare_rename(server, sink, uri, 4u, 12u);
    CHECK(prep.refused);
}

SECTION("lsp: workspace index integration (item 7 server-level coverage)");

TEST_CASE("workspace/symbol serves indexed graph symbols plus cached unlinked projections")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    TempDir td;
    initialize_server(server, sink, td.path);

    std::ignore = open_file(server, sink, td.path / "lib.dc", "module lib;\npublic i32 binary_search(i32 v) { return v; }\n");
    auto main_uri = open_file(server, sink, td.path / "main.dc", "module main;\nimport lib;\nvoid entry() { lib::binary_search(1); }\n");
    std::ignore = main_uri;

    {
        std::ofstream out{td.path / "extra.dc"};
        out << "module extra;\npublic i32 old_name() { return 1; }\n";
    }

    auto syms = request_workspace_symbols(server, sink, "");
    REQUIRE(has_symbol_info(syms, "binary_search"));
    CHECK(has_symbol_info(syms, "entry"));
    CHECK(has_symbol_info(syms, "old_name"));

    for (auto const& s : syms)
    {
        if (s.name == "binary_search")
        {
            CHECK_EQ(s.kind, dccd::protocol::SymbolKind::Function);
            CHECK(s.location.uri.ends_with("lib.dc"));
        }
        if (s.name == "old_name")
        {
            CHECK_EQ(s.kind, dccd::protocol::SymbolKind::Function);
            CHECK(s.location.uri.ends_with("extra.dc"));
        }
    }

    CHECK_EQ(server.workspace_index().unlinked_count(), 1u);
    CHECK(server.workspace_index().module_record("extra") == nullptr);
    CHECK(server.workspace_index().module_record("lib") != nullptr);
}

TEST_CASE("repeat workspace/symbol queries are served from cache without re-parse or re-extraction")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    TempDir td;
    initialize_server(server, sink, td.path);

    std::ignore = open_file(server, sink, td.path / "main.dc", "module main;\nvoid entry() {}\n");
    {
        std::ofstream out{td.path / "extra.dc"};
        out << "module extra;\npublic i32 old_name() { return 1; }\n";
    }

    auto const& st = server.workspace_index().stats();
    CHECK_EQ(st.unlinked_parsed, 0u);

    auto syms1 = request_workspace_symbols(server, sink, "old_name");
    REQUIRE(has_symbol_info(syms1, "old_name"));
    CHECK_EQ(st.unlinked_parsed, 1u);
    CHECK_EQ(st.workspace_queries_served_without_parse, 0u);
    auto extracted_after_first = st.modules_re_extracted;

    auto syms2 = request_workspace_symbols(server, sink, "old_name");
    REQUIRE(has_symbol_info(syms2, "old_name"));
    CHECK_EQ(st.unlinked_parsed, 1u);
    CHECK_EQ(st.unlinked_reused, 1u);
    CHECK_EQ(st.workspace_queries_served_without_parse, 1u);
    CHECK_EQ(st.modules_re_extracted, extracted_after_first);
}

TEST_CASE("didChange on an importer re-extracts only it and retains dependency records")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    TempDir td;
    initialize_server(server, sink, td.path);

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 value = 1;\n";
    }
    auto main_uri = open_file(server, sink, td.path / "main.dc", "module main;\nimport lib;\nvoid f() { i32 x = lib::value; }\n");

    auto const& st = server.workspace_index().stats();
    CHECK_EQ(st.modules_re_extracted, 2u);
    CHECK_EQ(server.workspace_index().module_count(), 2u);

    auto const* lib_before = server.workspace_index().module_record("lib");
    REQUIRE(lib_before != nullptr);
    auto lib_rev_before = lib_before->content_revision;
    CHECK_EQ(lib_before->symbols.size(), 1u);

    auto publishes =
        send_and_collect_publishes(server, sink, make_did_change(main_uri, 2, "module main;\nimport lib;\nvoid f() { i32 x = lib::value; }\nvoid g() {}\n"));
    std::ignore = publishes;

    CHECK_EQ(st.modules_re_extracted, 3u);
    CHECK_EQ(st.modules_retained, 1u);
    CHECK_EQ(server.workspace_index().module_count(), 2u);

    auto const* lib_after = server.workspace_index().module_record("lib");
    REQUIRE(lib_after != nullptr);
    CHECK_EQ(lib_after->content_revision, lib_rev_before);
    CHECK_EQ(lib_after->symbols.size(), 1u);

    auto const* main_after = server.workspace_index().module_record("main");
    REQUIRE(main_after != nullptr);
    CHECK_EQ(main_after->symbols.size(), 2u);
}

TEST_CASE("watched disk change invalidates the projection; the next query serves the new symbol, no ghost")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    TempDir td;
    initialize_server(server, sink, td.path);

    std::ignore = open_file(server, sink, td.path / "main.dc", "module main;\nvoid entry() {}\n");
    {
        std::ofstream out{td.path / "extra.dc"};
        out << "module extra;\npublic i32 old_name() { return 1; }\n";
    }

    auto syms1 = request_workspace_symbols(server, sink, "name");
    REQUIRE(has_symbol_info(syms1, "old_name"));
    CHECK_EQ(server.workspace_index().stats().unlinked_parsed, 1u);

    {
        std::ofstream out{td.path / "extra.dc"};
        out << "module extra;\npublic i32 new_name_x() { return 2; }\n";
    }

    auto extra_uri = dcc::sm::SourceManager::to_file_uri(td.path / "extra.dc");
    auto parsed = dccd::protocol::parse_rpc(make_watched_files_change(extra_uri));
    REQUIRE(parsed.has_value());
    std::ignore = server.handle_message(*parsed);
    CHECK(sink.drain().empty());

    auto syms2 = request_workspace_symbols(server, sink, "name");
    CHECK(!has_symbol_info(syms2, "old_name"));
    CHECK(has_symbol_info(syms2, "new_name_x"));
    CHECK_EQ(server.workspace_index().stats().unlinked_parsed, 2u);
}

TEST_CASE("cross-file references and rename are served from a fresh index with exact ranges")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    TempDir td;
    initialize_server(server, sink, td.path);

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 counter = 0;\n";
    }
    auto lib_uri = dcc::sm::SourceManager::to_file_uri(td.path / "lib.dc");
    auto main_uri = open_file(server, sink, td.path / "main.dc", "module main;\nimport lib;\nvoid f() {\n    i32 x = lib::counter;\n}\n");

    auto const* lib_rec = server.workspace_index().module_record("lib");
    REQUIRE(lib_rec != nullptr);
    auto& sm = server.source_manager();
    CHECK(server.workspace_index().module_fresh(lib_rec->file_id, sm.content_revision(lib_rec->file_id)));
    CHECK_EQ(server.workspace_index().stats().modules_re_extracted, 2u);

    auto refs = request_references(server, sink, lib_uri, 1u, 11u, true);
    REQUIRE(refs.size() == 2);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 11u, 18u));
    CHECK(has_range(ranges, 3u, 17u, 24u));

    auto outcome = request_rename(server, sink, main_uri, 3u, 17u, "total");
    REQUIRE(outcome.edit.has_value());
    REQUIRE(outcome.edit->changes.size() == 2);
    auto it = outcome.edit->changes.find(lib_uri);
    REQUIRE(it != outcome.edit->changes.end());
    CHECK(check_edit_set(it->second, "total", {{1u, 11u, 18u}}));
    it = outcome.edit->changes.find(main_uri);
    REQUIRE(it != outcome.edit->changes.end());
    CHECK(check_edit_set(it->second, "total", {{3u, 17u, 24u}}));
}

TEST_CASE("stale index revision forces live fallback; re-sync restores fresh state")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    TempDir td;
    initialize_server(server, sink, td.path);

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 counter = 0;\n";
    }
    auto lib_uri = dcc::sm::SourceManager::to_file_uri(td.path / "lib.dc");
    auto main_uri = open_file(server, sink, td.path / "main.dc", "module main;\nimport lib;\nvoid f() {\n    i32 x = lib::counter;\n}\n");

    auto& sm = server.source_manager();
    auto const* lib_rec = server.workspace_index().module_record("lib");
    REQUIRE(lib_rec != nullptr);
    auto lib_fid = lib_rec->file_id;
    CHECK(server.workspace_index().module_fresh(lib_fid, sm.content_revision(lib_fid)));

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 counter = 0;\npublic i32 counter2 = 1;\n";
    }
    REQUIRE(sm.refresh_disk_file(td.path / "lib.dc"));
    CHECK(!server.workspace_index().module_fresh(lib_fid, sm.content_revision(lib_fid)));

    auto refs = request_references(server, sink, lib_uri, 1u, 11u, true);
    REQUIRE(refs.size() == 2);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 11u, 18u));
    CHECK(has_range(ranges, 3u, 17u, 24u));

    auto publishes =
        send_and_collect_publishes(server, sink, make_did_change(main_uri, 2, "module main;\nimport lib;\nvoid f() {\n    i32 x = lib::counter;\n}\n"));
    std::ignore = publishes;

    auto const* lib_after = server.workspace_index().module_record("lib");
    REQUIRE(lib_after != nullptr);
    CHECK(server.workspace_index().module_fresh(lib_after->file_id, sm.content_revision(lib_after->file_id)));
    CHECK_EQ(lib_after->symbols.size(), 2u);

    refs = request_references(server, sink, lib_uri, 1u, 11u, true);
    REQUIRE(refs.size() == 2);
    ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 11u, 18u));
    CHECK(has_range(ranges, 3u, 17u, 24u));
}

SECTION("lsp: code actions");

TEST_CASE("code actions refuse a stale diagnostic cache and return after a fresh compile republishes")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"file:///tmp/dccd_lsp_code_action.dc"};

    std::string_view broken = "module m;\nvoid b() {\n    return 1;\n}\n";

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(uri, 1, broken));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].diagnostics.size() == 1);
    auto const& diag = publishes[0].diagnostics[0];
    CHECK(diag.message.find("void function cannot return a value") != std::string::npos);

    auto actions = request_code_actions(server, sink, uri, {diag});
    REQUIRE(actions.size() == 1);
    CHECK_EQ(actions[0].kind, "quickfix");
    CHECK(actions[0].title.find("remove the value from the return statement") != std::string::npos);

    REQUIRE(server.source_manager().update_in_memory(uri, std::string{broken}, 2).has_value());

    auto stale_actions = request_code_actions(server, sink, uri, {diag});
    CHECK(stale_actions.empty());

    auto republish = send_and_collect_publishes(server, sink, make_did_change(uri, 3, broken));
    REQUIRE(republish.size() == 1);
    REQUIRE(republish[0].diagnostics.size() == 1);

    auto fresh_actions = request_code_actions(server, sink, uri, {republish[0].diagnostics[0]});
    REQUIRE(fresh_actions.size() == 1);
    CHECK_EQ(fresh_actions[0].kind, "quickfix");
    CHECK(fresh_actions[0].title.find("remove the value from the return statement") != std::string::npos);
}

TEST_CASE("formatting ignores a stale graph without recompiling and still returns edits")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    TempDir td;
    initialize_server(server, sink, td.path);

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 value = 1;\n";
    }

    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\nimport lib;\nvoid f(){i32 x=1;}\n");

    auto const& st = server.workspace_index().stats();
    auto sync_before = st.sync_count;
    auto extracted_before = st.modules_re_extracted;

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 value = 1;\npublic i32 value2 = 2;\n";
    }
    REQUIRE(server.source_manager().refresh_disk_file(td.path / "lib.dc"));

    auto edits = request_formatting(server, sink, uri);
    REQUIRE(!edits.empty());
    REQUIRE(!edits[0].newText.empty());

    CHECK_EQ(server.workspace_index().stats().sync_count, sync_before);
    CHECK_EQ(server.workspace_index().stats().modules_re_extracted, extracted_before);
}

TEST_CASE("graph rebuild via a stale imported file invalidates cached quick fixes (graph generation)")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    TempDir td;
    initialize_server(server, sink, td.path);

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 counter = 0;\n";
    }
    std::string_view broken = "module main;\nimport lib;\nvoid b() {\n    return 1;\n}\n";
    auto main_uri = dcc::sm::SourceManager::to_file_uri(td.path / "main.dc");

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(main_uri, 1, broken));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].diagnostics.size() == 1);
    auto const& diag = publishes[0].diagnostics[0];
    CHECK(diag.message.find("void function cannot return a value") != std::string::npos);

    auto actions = request_code_actions(server, sink, main_uri, {diag});
    REQUIRE(actions.size() == 1);
    CHECK_EQ(actions[0].kind, "quickfix");

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 counter = 0;\npublic i32 counter2 = 1;\n";
    }
    REQUIRE(server.source_manager().refresh_disk_file(td.path / "lib.dc"));

    auto stale_actions = request_code_actions(server, sink, main_uri, {diag});
    CHECK(stale_actions.empty());

    auto republish = send_and_collect_publishes(server, sink, make_did_change(main_uri, 2, broken));
    REQUIRE(republish.size() == 1);
    REQUIRE(republish[0].diagnostics.size() == 1);

    auto fresh_actions = request_code_actions(server, sink, main_uri, {republish[0].diagnostics[0]});
    REQUIRE(fresh_actions.size() == 1);
    CHECK_EQ(fresh_actions[0].kind, "quickfix");
}

TEST_CASE("quick-fix inserts ';' at the exact structural location; applying it clears the diagnostic")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"file:///tmp/dccd_lsp_semi_fix.dc"};

    std::string_view broken = "module m;\nvoid f() {\n    i32 x = 0;\n    i32 y = 3;\n    defer x = 1 y = 2;\n}\n";

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(uri, 1, broken));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].diagnostics.size() == 1);
    auto const& diag = publishes[0].diagnostics[0];
    CHECK(diag.message.find("expected ';' after expression statement") != std::string::npos);

    auto actions = request_code_actions(server, sink, uri, {diag});
    REQUIRE(actions.size() == 1);
    auto const& action = actions[0];

    CHECK_EQ(action.kind, "quickfix");
    CHECK(action.title.find("insert ';' after the expression statement") != std::string::npos);

    REQUIRE(action.diagnostics.size() == 1);
    CHECK_EQ(action.diagnostics[0].message, diag.message);
    CHECK_EQ(action.diagnostics[0].range.start.line, diag.range.start.line);
    CHECK_EQ(action.diagnostics[0].range.start.character, diag.range.start.character);
    CHECK_EQ(action.diagnostics[0].range.end.line, diag.range.end.line);
    CHECK_EQ(action.diagnostics[0].range.end.character, diag.range.end.character);

    auto edit = extract_first_edit(action);
    REQUIRE(edit.has_value());
    CHECK_EQ(edit->newText, ";");
    CHECK_EQ(edit->range.start.line, 4u);
    CHECK_EQ(edit->range.start.character, 15u);
    CHECK_EQ(edit->range.end.line, 4u);
    CHECK_EQ(edit->range.end.character, 15u);

    CHECK_EQ(diag.range.start.line, 4u);
    CHECK_EQ(diag.range.start.character, 16u);
    CHECK_EQ(diag.range.end.character, 17u);

    auto fixed = apply_text_edit(std::string{broken}, *edit);
    CHECK_EQ(fixed, "module m;\nvoid f() {\n    i32 x = 0;\n    i32 y = 3;\n    defer x = 1; y = 2;\n}\n");
    auto republish = send_and_collect_publishes(server, sink, make_did_change(uri, 2, fixed));
    REQUIRE(republish.size() == 1);
    CHECK(republish[0].diagnostics.empty());
}

TEST_CASE("quick-fix deletes exactly the '?' token; deleting it leaves valid code")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"file:///tmp/dccd_lsp_qmark_fix.dc"};
    std::string_view broken = "module m;\ni32 g = 42?;\n";

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(uri, 1, broken));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].diagnostics.size() == 1);
    auto const& diag = publishes[0].diagnostics[0];
    CHECK(diag.message.find("cannot use `?` outside a function") != std::string::npos);

    auto actions = request_code_actions(server, sink, uri, {diag});
    REQUIRE(actions.size() == 1);
    auto const& action = actions[0];

    CHECK_EQ(action.kind, "quickfix");
    CHECK(action.title.find("remove the `?` operator") != std::string::npos);

    REQUIRE(action.diagnostics.size() == 1);
    CHECK_EQ(action.diagnostics[0].message, diag.message);

    auto edit = extract_first_edit(action);
    REQUIRE(edit.has_value());
    CHECK_EQ(edit->newText, "");
    CHECK_EQ(edit->range.start.line, 1u);
    CHECK_EQ(edit->range.start.character, 10u);
    CHECK_EQ(edit->range.end.line, 1u);
    CHECK_EQ(edit->range.end.character, 11u);

    CHECK_EQ(diag.range.start.line, 1u);
    CHECK_EQ(diag.range.start.character, 8u);
    CHECK_EQ(diag.range.end.character, 11u);

    auto fixed = apply_text_edit(std::string{broken}, *edit);
    CHECK_EQ(fixed, "module m;\ni32 g = 42;\n");
    auto republish = send_and_collect_publishes(server, sink, make_did_change(uri, 2, fixed));
    REQUIRE(republish.size() == 1);
    CHECK(republish[0].diagnostics.empty());
}

TEST_CASE("context diagnostics that do not match any cached diagnostic expose no quick fixes")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"file:///tmp/dccd_lsp_forged_fix.dc"};
    std::string_view broken = "module m;\nvoid f() {\n    i32 x = 0;\n    i32 y = 3;\n    defer x = 1 y = 2;\n}\n";

    auto publishes = send_and_collect_publishes(server, sink, make_did_open(uri, 1, broken));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].diagnostics.size() == 1);
    auto const& diag = publishes[0].diagnostics[0];

    auto genuine = request_code_actions(server, sink, uri, {diag});
    REQUIRE(genuine.size() == 1);

    auto forged_msg = diag;
    forged_msg.message = "completely unrelated message";
    auto forged_actions = request_code_actions(server, sink, uri, {forged_msg});
    CHECK(forged_actions.empty());

    auto forged_range = diag;
    forged_range.range.start.character += 1;
    forged_range.range.end.character += 1;
    auto range_actions = request_code_actions(server, sink, uri, {forged_range});
    CHECK(range_actions.empty());

    auto other_uri = std::string{"file:///tmp/dccd_lsp_other.dc"};
    auto other_actions = request_code_actions(server, sink, other_uri, {diag});
    CHECK(other_actions.empty());
}

TEST_CASE("watched change to an imported graph file rebuilds the graph and re-extracts the index")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    TempDir td;
    initialize_server(server, sink, td.path);

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 counter = 0;\n";
    }
    std::ignore = open_file(server, sink, td.path / "main.dc", "module main;\nimport lib;\nvoid f() { i32 x = lib::counter; }\n");

    auto const* lib_before = server.workspace_index().module_record("lib");
    REQUIRE(lib_before != nullptr);
    CHECK_EQ(lib_before->symbols.size(), 1u);
    auto const& st = server.workspace_index().stats();
    auto extracted_before = st.modules_re_extracted;

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 counter = 0;\npublic i32 counter2 = 1;\n";
    }
    auto lib_uri = dcc::sm::SourceManager::to_file_uri(td.path / "lib.dc");
    auto publishes = send_and_collect_publishes(server, sink, make_watched_files_change(lib_uri));
    std::ignore = publishes;

    auto const* lib_after = server.workspace_index().module_record("lib");
    REQUIRE(lib_after != nullptr);
    CHECK_EQ(lib_after->symbols.size(), 2u);
    CHECK(st.modules_re_extracted >= extracted_before + 1u);
    CHECK_EQ(server.workspace_index().module_count(), 2u);
}

SECTION("lsp: watched-files dynamic registration");

TEST_CASE("initialized sends exactly one client/registerCapability request when dynamic registration is supported")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink, {}, {}, true);

    auto initialized = parse_notification_rpc("initialized", JsonValue::empty_object());
    REQUIRE(initialized.has_value());

    std::ignore = server.handle_message(*initialized);
    auto frames = parse_lsp_stream(sink.drain());
    REQUIRE(frames.size() == 1);

    auto const& frame = frames[0];
    auto jsonrpc = frame.get_string("jsonrpc");
    REQUIRE(jsonrpc.has_value());
    CHECK_EQ(*jsonrpc, "2.0");

    auto method = frame.get_string("method");
    REQUIRE(method.has_value());
    CHECK_EQ(*method, std::string{dccd::protocol::kClientRegisterCapabilityMethod});

    auto const* id = frame.find_member("id");
    REQUIRE(id != nullptr);
    REQUIRE(id->is_string());
    CHECK_EQ(id->as_string(), std::string{dccd::protocol::kWatchedFilesRegistrationRequestId});

    auto const* params = frame.get_object("params");
    REQUIRE(params != nullptr);
    auto const* registrations = params->get_array("registrations");
    REQUIRE(registrations != nullptr);
    REQUIRE(registrations->array_size() == 1);
    auto const& reg = registrations->as_array()[0];
    auto reg_method = reg.get_string("method");
    REQUIRE(reg_method.has_value());
    CHECK_EQ(*reg_method, dccd::protocol::kWatchedFilesMethod);
    auto const* reg_opts = reg.get_object("registerOptions");
    REQUIRE(reg_opts != nullptr);
    auto const* watchers = reg_opts->get_array("watchers");
    REQUIRE(watchers != nullptr);
    REQUIRE(watchers->array_size() == 2);
    auto w0 = watchers->as_array()[0].get_string("globPattern");
    REQUIRE(w0.has_value());
    CHECK_EQ(*w0, "**/*.dc");
    auto w1 = watchers->as_array()[1].get_string("globPattern");
    REQUIRE(w1.has_value());
    CHECK_EQ(*w1, "**/dcc.json");

    std::ignore = server.handle_message(*initialized);
    std::ignore = server.handle_message(*initialized);
    CHECK(sink.drain().empty());
}

TEST_CASE("initialized as a request also registers exactly once")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink, {}, {}, true);

    auto req = dccd::protocol::build_request(JsonValue::integer(9), "initialized", JsonValue::empty_object());
    auto parsed = dccd::protocol::parse_rpc(req);
    REQUIRE(parsed.has_value());
    auto response = server.handle_message(*parsed);

    CHECK(!response.has_value());
    auto frames = parse_lsp_stream(sink.drain());
    REQUIRE(frames.size() == 1);
    auto method = frames[0].get_string("method");
    REQUIRE(method.has_value());
    CHECK_EQ(*method, std::string{dccd::protocol::kClientRegisterCapabilityMethod});

    auto initialized = parse_notification_rpc("initialized", JsonValue::empty_object());
    REQUIRE(initialized.has_value());
    std::ignore = server.handle_message(*initialized);
    CHECK(sink.drain().empty());
}

TEST_CASE("initialized sends no registerCapability when dynamic registration is unsupported")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto initialized = parse_notification_rpc("initialized", JsonValue::empty_object());
    REQUIRE(initialized.has_value());
    std::ignore = server.handle_message(*initialized);
    CHECK(sink.drain().empty());
}

TEST_CASE("a client response to registerCapability is safely ignored")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink, {}, {}, true);

    auto initialized = parse_notification_rpc("initialized", JsonValue::empty_object());
    REQUIRE(initialized.has_value());
    std::ignore = server.handle_message(*initialized);
    CHECK(!sink.drain().empty());

    auto resp = dccd::protocol::build_response(JsonValue::string_val(std::string{dccd::protocol::kWatchedFilesRegistrationRequestId}), JsonValue::null_val());
    auto parsed = dccd::protocol::parse_rpc(resp);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->is_response());

    auto handled = server.handle_message(*parsed);
    CHECK(!handled.has_value());
    CHECK(sink.drain().empty());

    std::ignore = server.handle_message(*initialized);
    CHECK(sink.drain().empty());
}

TEST_CASE("re-initialize resets the one-shot registration guard")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink, {}, {}, true);

    auto initialized = parse_notification_rpc("initialized", JsonValue::empty_object());
    REQUIRE(initialized.has_value());
    std::ignore = server.handle_message(*initialized);
    REQUIRE(!sink.drain().empty());

    initialize_server(server, sink, {}, {}, true);
    std::ignore = server.handle_message(*initialized);
    auto frames = parse_lsp_stream(sink.drain());
    REQUIRE(frames.size() == 1);
    auto method = frames[0].get_string("method");
    REQUIRE(method.has_value());
    CHECK_EQ(*method, std::string{dccd::protocol::kClientRegisterCapabilityMethod});
}

SECTION("lsp: cancellation ($/cancelRequest)");

TEST_CASE("cancelling an unknown request id is a no-op and never affects later requests")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\ni32 counter = 0;\nvoid f() {\n    i32 x = counter;\n}\n");

    auto cancel = dccd::protocol::parse_rpc(make_cancel_notification(dccd::protocol::RequestId::from_json(JsonValue::integer(12345))));
    REQUIRE(cancel.has_value());
    auto resp = server.handle_message(*cancel);
    CHECK(!resp.has_value());
    CHECK(sink.drain().empty());

    auto res = send_position_request(server, sink, uri, 1u, 5u, "textDocument/definition");
    CHECK(!res.error.has_value());
    REQUIRE(res.value.has_value());
    CHECK(!res.value->is_null());
}

TEST_CASE("cancelling after completion is a no-op and a reused id is never poisoned")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\ni32 counter = 0;\nvoid f() {\n    i32 x = counter;\n}\n");

    auto const& st = server.workspace_index().stats();
    auto sync_before = st.sync_count;

    auto res1 = send_position_request(server, sink, uri, 1u, 5u, "textDocument/definition");
    CHECK(!res1.error.has_value());
    REQUIRE(res1.value.has_value());
    CHECK_EQ(st.sync_count, sync_before);

    auto cancel = dccd::protocol::parse_rpc(make_cancel_notification(dccd::protocol::RequestId::from_json(JsonValue::string_val("1"))));
    REQUIRE(cancel.has_value());
    auto resp = server.handle_message(*cancel);
    CHECK(!resp.has_value());
    CHECK(sink.drain().empty());

    auto res2 = send_position_request(server, sink, uri, 1u, 5u, "textDocument/definition");
    CHECK(!res2.error.has_value());
    REQUIRE(res2.value.has_value());
    CHECK(!res2.value->is_null());
    CHECK_EQ(st.sync_count, sync_before);
}

TEST_CASE("numeric and string request ids are type-distinct for cancellation")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\ni32 counter = 0;\nvoid f() {\n    i32 x = counter;\n}\n");

    auto numeric_id = dccd::protocol::RequestId::from_json(JsonValue::integer(7));
    auto string_id = dccd::protocol::RequestId::from_json(JsonValue::string_val("7"));

    REQUIRE(server.cancellation_registry().register_pending(numeric_id));
    auto cancel = dccd::protocol::parse_rpc(make_cancel_notification(string_id));
    REQUIRE(cancel.has_value());
    std::ignore = server.handle_message(*cancel);
    CHECK(sink.drain().empty());

    CHECK(server.cancellation_registry().is_pending(numeric_id));
    CHECK(!server.cancellation_registry().is_cancelled(numeric_id));

    cancel = dccd::protocol::parse_rpc(make_cancel_notification(numeric_id));
    REQUIRE(cancel.has_value());
    std::ignore = server.handle_message(*cancel);
    CHECK(server.cancellation_registry().is_cancelled(numeric_id));

    server.cancellation_registry().finish(numeric_id);
    CHECK(!server.cancellation_registry().is_pending(numeric_id));
    CHECK(!server.cancellation_registry().is_cancelled(numeric_id));
    auto late_cancel = dccd::protocol::parse_rpc(make_cancel_notification(numeric_id));
    REQUIRE(late_cancel.has_value());
    std::ignore = server.handle_message(*late_cancel);
    CHECK(sink.drain().empty());

    REQUIRE(server.cancellation_registry().register_pending(numeric_id));
    CHECK(server.cancellation_registry().is_pending(numeric_id));
    CHECK(!server.cancellation_registry().is_cancelled(numeric_id));
    server.cancellation_registry().finish(numeric_id);
}

TEST_CASE("early cancellation avoids expensive work and returns exactly one RequestCancelled error")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\ni32 counter = 0;\nvoid f() {\n    i32 x = counter;\n}\n");

    auto const& st = server.workspace_index().stats();
    auto sync_before = st.sync_count;
    auto extracted_before = st.modules_re_extracted;

    auto id = dccd::protocol::RequestId::from_json(JsonValue::string_val("cancel-me"));
    REQUIRE(server.cancellation_registry().register_pending(id));

    auto cancel = dccd::protocol::parse_rpc(make_cancel_notification(id));
    REQUIRE(cancel.has_value());
    std::ignore = server.handle_message(*cancel);
    CHECK(sink.drain().empty());
    CHECK(server.cancellation_registry().is_cancelled(id));

    auto resp = send_request(server, sink, make_position_params_request(uri, 1u, 5u, "textDocument/definition", "cancel-me"));
    REQUIRE(resp.has_value());

    auto jsonrpc = resp->get_string("jsonrpc");
    REQUIRE(jsonrpc.has_value());
    CHECK_EQ(*jsonrpc, "2.0");

    auto const* err = resp->get_object("error");
    REQUIRE(err != nullptr);
    auto code = err->get_integer("code");
    REQUIRE(code.has_value());
    CHECK_EQ(*code, dccd::protocol::kErrorRequestCancelled);
    auto message = err->get_string("message");
    REQUIRE(message.has_value());
    CHECK_EQ(*message, "Request cancelled");

    auto const* resp_id = resp->find_member("id");
    REQUIRE(resp_id != nullptr);
    REQUIRE(resp_id->is_string());
    CHECK_EQ(resp_id->as_string(), "cancel-me");
    CHECK(resp->find_member("result") == nullptr);

    CHECK_EQ(st.sync_count, sync_before);
    CHECK_EQ(st.modules_re_extracted, extracted_before);

    CHECK(!server.cancellation_registry().is_pending(id));
    CHECK(!server.cancellation_registry().is_cancelled(id));
}

TEST_CASE("cancellation during an in-flight request is observed at a checkpoint and returns RequestCancelled")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;

    std::string big = "module main;\ni32 counter = 0;\n";
    for (int i = 0; i < 2000; ++i)
        big += std::format("i32 var{} = {};\n", i, i);
    big += "void f() {\n    i32 x = counter;\n}\n";
    auto uri = open_file(server, sink, td.path / "main.dc", big);

    auto id = dccd::protocol::RequestId::from_json(JsonValue::integer(777));
    auto params = dccd::protocol::JsonValue::empty_object();
    params.set("textDocument", make_text_document(uri));
    params.set("position", make_position(1u, 5u));
    auto request = dccd::protocol::build_request(JsonValue::integer(777), "textDocument/references", std::move(params));

    std::promise<std::optional<dccd::protocol::JsonValue>> result_promise;
    auto result_future = result_promise.get_future();

    std::thread worker{[&] {
        auto parsed = dccd::protocol::parse_rpc(request);
        if (!parsed)
        {
            result_promise.set_value(std::nullopt);
            return;
        }
        try
        {
            result_promise.set_value(server.handle_message(*parsed));
        }
        catch (...)
        {
            result_promise.set_value(std::nullopt);
        }
    }};

    auto& registry = server.cancellation_registry();
    bool seen_pending = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (registry.is_pending(id))
        {
            seen_pending = true;
            break;
        }
        std::this_thread::yield();
    }
    REQUIRE(seen_pending);

    std::ignore = registry.cancel(id);

    worker.join();
    auto response = result_future.get();
    REQUIRE(response.has_value());

    auto const* err = response->get_object("error");
    REQUIRE(err != nullptr);
    auto code = err->get_integer("code");
    REQUIRE(code.has_value());
    CHECK_EQ(*code, dccd::protocol::kErrorRequestCancelled);
    auto const* resp_id = response->find_member("id");
    REQUIRE(resp_id != nullptr);
    REQUIRE(resp_id->is_number());
    CHECK_EQ(resp_id->as_integer(), 777);
    CHECK(response->find_member("result") == nullptr);

    CHECK(!registry.is_pending(id));
    CHECK(!registry.is_cancelled(id));
}

namespace
{
    struct DecodedToken
    {
        std::uint32_t line{};
        std::uint32_t character{};
        std::uint32_t length{};
        std::uint32_t type{};
        std::uint32_t modifiers{};

        [[nodiscard]] auto operator<=>(DecodedToken const&) const = default;
    };

    [[nodiscard]] std::vector<DecodedToken> decode_delta(std::span<std::uint32_t const> data)
    {
        std::vector<DecodedToken> out;
        std::uint32_t line = 0;
        std::uint32_t character = 0;
        for (std::size_t i = 0; i + 4 < data.size(); i += 5)
        {
            line += data[i];
            character = (data[i] == 0) ? character + data[i + 1] : data[i + 1];
            out.push_back(DecodedToken{line, character, data[i + 2], data[i + 3], data[i + 4]});
        }
        return out;
    }

    [[nodiscard]] std::vector<DecodedToken> decode_delta(std::vector<std::uint32_t> const& data)
    {
        return decode_delta(std::span<std::uint32_t const>{data});
    }

} // namespace

TEST_CASE("delta_encode produces a monotonic non-overlapping delta stream")
{
    namespace st = dccd::semantic_tokens;

    std::vector<st::RawToken> raw = {
        {1, 0, 4, 6, 0}, {0, 5, 2, 4, 0}, {0, 2, 3, 1, 0}, {0, 2, 5, 2, 0}, {0, 1, 4, 3, 0}, {0, 5, 1, 5, 0},
    };

    auto data = st::delta_encode(std::move(raw));
    auto tokens = decode_delta(data);

    std::vector<DecodedToken> expected = {
        {0, 1, 4, 3, 0},
        {0, 5, 2, 4, 0},
        {1, 0, 4, 6, 0},
    };
    REQUIRE(tokens.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        CHECK_EQ(tokens[i], expected[i]);

    for (std::size_t i = 1; i < tokens.size(); ++i)
    {
        auto const& prev = tokens[i - 1];
        auto const& cur = tokens[i];
        if (prev.line == cur.line)
            CHECK(cur.character >= prev.character + prev.length);
        else
            CHECK_LT(prev.line, cur.line);
    }
}

TEST_CASE("semantic token legend indices are stable and capabilities serialize them")
{
    namespace st = dccd::semantic_tokens;
    namespace proto = dccd::protocol;

    CHECK_EQ(static_cast<std::size_t>(st::TokenType::Namespace), 0u);
    CHECK_EQ(static_cast<std::size_t>(st::TokenType::String), 17u);
    CHECK_EQ(static_cast<std::size_t>(st::TokenType::AsmPlaceholder), 20u);
    CHECK_EQ(static_cast<std::size_t>(st::TokenType::AsmRegister), 21u);

    REQUIRE(proto::token_types.size() == st::token_types.size());
    for (std::size_t i = 0; i < st::token_types.size(); ++i)
        CHECK_EQ(std::string_view{proto::token_types[i]}, std::string_view{st::token_types[i]});

    REQUIRE(proto::token_modifiers.size() == st::token_modifiers.size());
    for (std::size_t i = 0; i < st::token_modifiers.size(); ++i)
        CHECK_EQ(std::string_view{proto::token_modifiers[i]}, std::string_view{st::token_modifiers[i]});

    auto legend = proto::make_semantic_tokens_legend();
    auto const* tt = legend.get_array("tokenTypes");
    REQUIRE(tt != nullptr);
    REQUIRE(tt->array_size() == st::token_types.size());
    for (std::size_t i = 0; i < st::token_types.size(); ++i)
    {
        auto const& item = tt->as_array()[i];
        REQUIRE(item.is_string());
        CHECK_EQ(item.as_string(), std::string{st::token_types[i]});
    }
    auto const* tm = legend.get_array("tokenModifiers");
    REQUIRE(tm != nullptr);
    REQUIRE(tm->array_size() == st::token_modifiers.size());
    for (std::size_t i = 0; i < st::token_modifiers.size(); ++i)
    {
        auto const& item = tm->as_array()[i];
        REQUIRE(item.is_string());
        CHECK_EQ(item.as_string(), std::string{st::token_modifiers[i]});
    }
}

TEST_CASE("initialize serializes the semanticTokensProvider capability with the full legend")
{
    namespace st = dccd::semantic_tokens;

    Sink sink;
    dccd::LanguageServer server{&sink.stream};

    auto params = dccd::protocol::JsonValue::empty_object();
    auto init_req = dccd::protocol::build_request(JsonValue::integer(1), "initialize", std::move(params));
    auto parsed = dccd::protocol::parse_rpc(init_req);
    REQUIRE(parsed.has_value());
    auto response = server.handle_message(*parsed);
    REQUIRE(response.has_value());

    auto const* result = response->find_member("result");
    REQUIRE(result != nullptr);
    auto const* caps = result->get_object("capabilities");
    REQUIRE(caps != nullptr);
    auto const* stp = caps->get_object("semanticTokensProvider");
    REQUIRE(stp != nullptr);

    auto full = stp->get_bool("full");
    REQUIRE(full.has_value());
    CHECK(*full);

    auto const* legend = stp->get_object("legend");
    REQUIRE(legend != nullptr);
    auto const* tt = legend->get_array("tokenTypes");
    REQUIRE(tt != nullptr);
    REQUIRE(tt->array_size() == st::token_types.size());
    auto const* tm = legend->get_array("tokenModifiers");
    REQUIRE(tm != nullptr);
    REQUIRE(tm->array_size() == st::token_modifiers.size());
}

TEST_CASE("split_range emits one nonempty token per line, excluding CR/LF, in UTF-16 units")
{
    namespace st = dccd::semantic_tokens;
    dcc::sm::SourceManager mgr;

    std::string text = "/* \xC3\xA9\nxx \xF0\x9F\x98\x80 */";
    auto fid = mgr.open_in_memory("file:///t.dc", text, 1);
    REQUIRE(fid != dcc::sm::FileId::Invalid);

    dcc::sm::SourceRange full{{fid, 0}, {fid, static_cast<dcc::sm::Offset>(text.size())}};
    auto tokens = st::split_range(mgr, full, st::TokenType::Comment);
    REQUIRE(tokens.size() == 2u);

    CHECK_EQ(tokens[0].line, 0u);
    CHECK_EQ(tokens[0].character, 0u);
    CHECK_EQ(tokens[0].length, 4u);
    CHECK_EQ(tokens[0].type, static_cast<std::uint32_t>(st::TokenType::Comment));

    CHECK_EQ(tokens[1].line, 1u);
    CHECK_EQ(tokens[1].character, 0u);
    CHECK_EQ(tokens[1].length, 8u);
    CHECK_EQ(tokens[1].type, static_cast<std::uint32_t>(st::TokenType::Comment));

    std::string crlf_text = "a\r\nb";
    auto fid2 = mgr.open_in_memory("file:///crlf.dc", crlf_text, 1);
    REQUIRE(fid2 != dcc::sm::FileId::Invalid);
    dcc::sm::SourceRange crlf_range{{fid2, 0}, {fid2, static_cast<dcc::sm::Offset>(crlf_text.size())}};
    auto crlf_tokens = st::split_range(mgr, crlf_range, st::TokenType::String);
    REQUIRE(crlf_tokens.size() == 2u);
    CHECK_EQ(crlf_tokens[0].line, 0u);
    CHECK_EQ(crlf_tokens[0].character, 0u);
    CHECK_EQ(crlf_tokens[0].length, 1u);
    CHECK_EQ(crlf_tokens[1].line, 1u);
    CHECK_EQ(crlf_tokens[1].character, 0u);
    CHECK_EQ(crlf_tokens[1].length, 1u);

    std::string u16_text = "\xC3\xA9x";
    auto fid3 = mgr.open_in_memory("file:///u16.dc", u16_text, 1);
    REQUIRE(fid3 != dcc::sm::FileId::Invalid);
    dcc::sm::SourceRange tail{{fid3, 2}, {fid3, 3}};
    auto tail_tokens = st::split_range(mgr, tail, st::TokenType::Variable);
    REQUIRE(tail_tokens.size() == 1u);
    CHECK_EQ(tail_tokens[0].line, 0u);
    CHECK_EQ(tail_tokens[0].character, 1u);
    CHECK_EQ(tail_tokens[0].length, 1u);

    CHECK(st::split_range(mgr, dcc::sm::SourceRange{}, st::TokenType::Variable).empty());
    dcc::sm::SourceRange zero_len{{fid3, 1}, {fid3, 1}};
    CHECK(st::split_range(mgr, zero_len, st::TokenType::Variable).empty());
}

TEST_CASE("collect_tokens keeps asm placeholders and registers visible alongside string gaps")
{
    namespace st = dccd::semantic_tokens;
    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    std::string text = "module main;\nvoid f() {\ni32 x = 0;\nasm @[inout(x in ecx)] { \"incl %[x]; mov %%eax, %[x]\" };\n}\n";
    auto fid = session.open_in_memory("file:///asm.dc", text, 1);
    REQUIRE(fid != dcc::sm::FileId::Invalid);
    auto* tu = session.parse_file(fid);
    REQUIRE(tu != nullptr);

    auto data = st::collect_tokens(session.source_manager(), tu, fid);
    auto tokens = decode_delta(data);
    REQUIRE(!tokens.empty());

    std::vector<DecodedToken> line3;
    for (auto const& t : tokens)
        if (t.line == 3u)
            line3.push_back(t);

    std::vector<DecodedToken> expected = {
        {3, 12, 1, static_cast<std::uint32_t>(st::TokenType::Variable), 0},       {3, 25, 6, static_cast<std::uint32_t>(st::TokenType::String), 0},
        {3, 31, 4, static_cast<std::uint32_t>(st::TokenType::AsmPlaceholder), 0}, {3, 35, 6, static_cast<std::uint32_t>(st::TokenType::String), 0},
        {3, 41, 5, static_cast<std::uint32_t>(st::TokenType::AsmRegister), 0},    {3, 46, 2, static_cast<std::uint32_t>(st::TokenType::String), 0},
        {3, 48, 4, static_cast<std::uint32_t>(st::TokenType::AsmPlaceholder), 0}, {3, 52, 1, static_cast<std::uint32_t>(st::TokenType::String), 0},
    };
    REQUIRE(line3.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        CHECK_EQ(line3[i], expected[i]);

    auto has_type = [&](st::TokenType type) {
        return std::ranges::find_if(tokens, [&](DecodedToken const& t) { return t.type == static_cast<std::uint32_t>(type); }) != tokens.end();
    };
    CHECK(has_type(st::TokenType::AsmPlaceholder));
    CHECK(has_type(st::TokenType::AsmRegister));

    for (std::size_t i = 1; i < tokens.size(); ++i)
    {
        auto const& prev = tokens[i - 1];
        auto const& cur = tokens[i];
        if (prev.line == cur.line)
            CHECK(cur.character >= prev.character + prev.length);
        else
            CHECK_LT(prev.line, cur.line);
    }
}

TEST_CASE("collect_tokens finds declaration names beyond the old 512-byte search cap")
{
    namespace st = dccd::semantic_tokens;
    dcc::session::CompilerSession session{{.silent_diagnostics = true}};

    std::string attr = "@deprecated(\"";
    attr.append(600, 'A');
    attr += "\") ";
    std::string text = std::format("module m;\nenum E {{\n    {}LongVariantName,\n}};\n", attr);
    auto fid = session.open_in_memory("file:///cap.dc", text, 1);
    REQUIRE(fid != dcc::sm::FileId::Invalid);
    auto* tu = session.parse_file(fid);
    REQUIRE(tu != nullptr);

    auto data = st::collect_tokens(session.source_manager(), tu, fid);
    auto tokens = decode_delta(data);

    auto it = std::ranges::find_if(tokens, [](DecodedToken const& t) { return t.type == static_cast<std::uint32_t>(st::TokenType::EnumMember); });
    REQUIRE(it != tokens.end());
    CHECK_EQ(it->line, 2u);

    CHECK_EQ(it->character, 4u + 12u + 1u + 600u + 1u + 2u);
    CHECK_EQ(it->length, 15u);
    CHECK_EQ(it->modifiers, 1u);
}

TEST_CASE("collect_tokens aborts early when the cancellation callback fires")
{
    namespace st = dccd::semantic_tokens;
    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    std::string text = "module main;\n";
    for (int i = 0; i < 5000; ++i)
        text += std::format("i32 var{} = {};\n", i, i);
    auto fid = session.open_in_memory("file:///cancel.dc", text, 1);
    REQUIRE(fid != dcc::sm::FileId::Invalid);
    auto* tu = session.parse_file(fid);
    REQUIRE(tu != nullptr);

    auto data_full = st::collect_tokens(session.source_manager(), tu, fid);
    CHECK(!data_full.empty());

    int probe_count = 0;
    auto data_aborted = st::collect_tokens(session.source_manager(), tu, fid, [&] {
        ++probe_count;
        return true;
    });
    CHECK(data_aborted.empty());
    CHECK(probe_count > 0);

    auto data_kept = st::collect_tokens(session.source_manager(), tu, fid, [] { return false; });
    CHECK_EQ(data_kept.size(), data_full.size());
    if (data_kept.size() == data_full.size())
        for (std::size_t i = 0; i < data_full.size(); ++i)
            CHECK_EQ(data_kept[i], data_full[i]);
}

TEST_CASE("server semanticTokens/full measures non-ASCII tokens in UTF-16 units")
{
    namespace st = dccd::semantic_tokens;

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\nvoid f() {\n    const char* s = \"\xF0\x9F\x98\x80\"; i32 y = 0;\n}\n");
    auto resp = send_request(server, sink, make_semantic_tokens_request(uri));
    REQUIRE(resp.has_value());
    auto const* result = resp->find_member("result");
    REQUIRE(result != nullptr);
    auto const* data = result->get_array("data");
    REQUIRE(data != nullptr);

    std::vector<std::uint32_t> flat;
    for (auto const& v : data->as_array())
    {
        REQUIRE(v.is_number());
        flat.push_back(static_cast<std::uint32_t>(v.as_integer()));
    }
    auto tokens = decode_delta(flat);
    REQUIRE(!tokens.empty());

    auto str_it =
        std::ranges::find_if(tokens, [](DecodedToken const& t) { return t.line == 2u && t.type == static_cast<std::uint32_t>(st::TokenType::String); });
    REQUIRE(str_it != tokens.end());
    CHECK_EQ(str_it->character, 20u);
    CHECK_EQ(str_it->length, 4u);

    auto y_it = std::ranges::find_if(
        tokens, [](DecodedToken const& t) { return t.line == 2u && t.type == static_cast<std::uint32_t>(st::TokenType::Variable) && t.character == 30u; });
    REQUIRE(y_it != tokens.end());
    CHECK_EQ(y_it->length, 1u);
    CHECK_EQ(y_it->modifiers, 1u);
}

TEST_CASE("server negotiates utf-8 and measures non-ASCII tokens in bytes")
{
    namespace st = dccd::semantic_tokens;

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink, {}, {"utf-8"});

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\nvoid f() {\n    const char* s = \"\xF0\x9F\x98\x80\"; i32 y = 0;\n}\n");
    auto resp = send_request(server, sink, make_semantic_tokens_request(uri));
    REQUIRE(resp.has_value());
    auto const* result = resp->find_member("result");
    REQUIRE(result != nullptr);
    auto const* data = result->get_array("data");
    REQUIRE(data != nullptr);

    std::vector<std::uint32_t> flat;
    for (auto const& v : data->as_array())
    {
        REQUIRE(v.is_number());
        flat.push_back(static_cast<std::uint32_t>(v.as_integer()));
    }
    auto tokens = decode_delta(flat);
    REQUIRE(!tokens.empty());

    auto str_it =
        std::ranges::find_if(tokens, [](DecodedToken const& t) { return t.line == 2u && t.type == static_cast<std::uint32_t>(st::TokenType::String); });
    REQUIRE(str_it != tokens.end());
    CHECK_EQ(str_it->character, 20u);
    CHECK_EQ(str_it->length, 6u);

    auto y_it = std::ranges::find_if(
        tokens, [](DecodedToken const& t) { return t.line == 2u && t.type == static_cast<std::uint32_t>(st::TokenType::Variable) && t.character == 32u; });
    REQUIRE(y_it != tokens.end());
    CHECK_EQ(y_it->length, 1u);
    CHECK_EQ(y_it->modifiers, 1u);
}

TEST_CASE("server negotiates utf-32 and measures non-ASCII tokens in code points")
{
    namespace st = dccd::semantic_tokens;

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink, {}, {"utf-32"});

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\nvoid f() {\n    const char* s = \"\xF0\x9F\x98\x80\"; i32 y = 0;\n}\n");
    auto resp = send_request(server, sink, make_semantic_tokens_request(uri));
    REQUIRE(resp.has_value());
    auto const* result = resp->find_member("result");
    REQUIRE(result != nullptr);
    auto const* data = result->get_array("data");
    REQUIRE(data != nullptr);

    std::vector<std::uint32_t> flat;
    for (auto const& v : data->as_array())
    {
        REQUIRE(v.is_number());
        flat.push_back(static_cast<std::uint32_t>(v.as_integer()));
    }
    auto tokens = decode_delta(flat);
    REQUIRE(!tokens.empty());

    auto str_it =
        std::ranges::find_if(tokens, [](DecodedToken const& t) { return t.line == 2u && t.type == static_cast<std::uint32_t>(st::TokenType::String); });
    REQUIRE(str_it != tokens.end());
    CHECK_EQ(str_it->character, 20u);
    CHECK_EQ(str_it->length, 3u);

    auto y_it = std::ranges::find_if(
        tokens, [](DecodedToken const& t) { return t.line == 2u && t.type == static_cast<std::uint32_t>(st::TokenType::Variable) && t.character == 29u; });
    REQUIRE(y_it != tokens.end());
    CHECK_EQ(y_it->length, 1u);
    CHECK_EQ(y_it->modifiers, 1u);
}

TEST_CASE("server semanticTokens/full classifies lambda params as parameters")
{
    namespace st = dccd::semantic_tokens;

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc",
                         "module main;\n"
                         "i32 apply(i32(*)(i32) f) {\n"
                         "    return f(1);\n"
                         "}\n"
                         "void use() {\n"
                         "    i32 a = apply(|x -> x * 2);\n"
                         "    i32 b = apply(|i32 y -> y + 1);\n"
                         "}\n");
    auto resp = send_request(server, sink, make_semantic_tokens_request(uri));
    REQUIRE(resp.has_value());
    auto const* result = resp->find_member("result");
    REQUIRE(result != nullptr);
    auto const* data = result->get_array("data");
    REQUIRE(data != nullptr);

    std::vector<std::uint32_t> flat;
    for (auto const& v : data->as_array())
    {
        REQUIRE(v.is_number());
        flat.push_back(static_cast<std::uint32_t>(v.as_integer()));
    }
    auto tokens = decode_delta(flat);
    REQUIRE(!tokens.empty());

    auto x_it = std::ranges::find_if(
        tokens, [](DecodedToken const& t) { return t.line == 5u && t.type == static_cast<std::uint32_t>(st::TokenType::Parameter) && t.character == 19u; });
    REQUIRE(x_it != tokens.end());
    CHECK_EQ(x_it->length, 1u);
    CHECK_EQ(x_it->modifiers, static_cast<std::uint32_t>(st::TokenModifier::Declaration));

    auto y_it = std::ranges::find_if(
        tokens, [](DecodedToken const& t) { return t.line == 6u && t.type == static_cast<std::uint32_t>(st::TokenType::Parameter) && t.character == 23u; });
    REQUIRE(y_it != tokens.end());
    CHECK_EQ(y_it->length, 1u);
    CHECK_EQ(y_it->modifiers, static_cast<std::uint32_t>(st::TokenModifier::Declaration));
}

TEST_CASE("server picks the client's first supported encoding and echoes it in initialize")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};

    auto params = dccd::protocol::JsonValue::empty_object();
    auto encs = dccd::protocol::JsonValue::empty_array();
    encs.push_back(dccd::protocol::JsonValue::string_val("utf-32"));
    encs.push_back(dccd::protocol::JsonValue::string_val("utf-8"));
    auto general = dccd::protocol::JsonValue::empty_object();
    general.set("positionEncodings", std::move(encs));
    auto caps = dccd::protocol::JsonValue::empty_object();
    caps.set("general", std::move(general));
    params.set("capabilities", std::move(caps));

    auto init_req = dccd::protocol::build_request(JsonValue::integer(1), "initialize", std::move(params));
    auto parsed = dccd::protocol::parse_rpc(init_req);
    REQUIRE(parsed.has_value());
    auto response = server.handle_message(*parsed);
    REQUIRE(response.has_value());
    CHECK(sink.drain().empty());

    auto const* result = response->find_member("result");
    REQUIRE(result != nullptr);
    auto const* resp_caps = result->get_object("capabilities");
    REQUIRE(resp_caps != nullptr);
    auto enc = resp_caps->get_string("positionEncoding");
    REQUIRE(enc.has_value());
    CHECK_EQ(*enc, "utf-32");
    CHECK_EQ(server.source_manager().position_encoding(), dcc::sm::PositionEncoding::Utf32);
}

TEST_CASE("server semanticTokens/full places escaped asm placeholder tokens at raw source positions")
{
    namespace st = dccd::semantic_tokens;

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;

    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\nvoid f() {\n    asm { \"nop \\n %[x]; mov %%eax, %[x]\" };\n}\n");
    auto resp = send_request(server, sink, make_semantic_tokens_request(uri));
    REQUIRE(resp.has_value());
    auto const* result = resp->find_member("result");
    REQUIRE(result != nullptr);
    auto const* data = result->get_array("data");
    REQUIRE(data != nullptr);

    std::vector<std::uint32_t> flat;
    for (auto const& v : data->as_array())
    {
        REQUIRE(v.is_number());
        flat.push_back(static_cast<std::uint32_t>(v.as_integer()));
    }
    auto tokens = decode_delta(flat);
    REQUIRE(!tokens.empty());

    std::vector<DecodedToken> line2;
    for (auto const& t : tokens)
        if (t.line == 2u)
            line2.push_back(t);

    std::vector<DecodedToken> expected = {
        {2, 10, 8, static_cast<std::uint32_t>(st::TokenType::String), 0}, {2, 18, 4, static_cast<std::uint32_t>(st::TokenType::AsmPlaceholder), 0},
        {2, 22, 6, static_cast<std::uint32_t>(st::TokenType::String), 0}, {2, 28, 5, static_cast<std::uint32_t>(st::TokenType::AsmRegister), 0},
        {2, 33, 2, static_cast<std::uint32_t>(st::TokenType::String), 0}, {2, 35, 4, static_cast<std::uint32_t>(st::TokenType::AsmPlaceholder), 0},
        {2, 39, 1, static_cast<std::uint32_t>(st::TokenType::String), 0},
    };
    REQUIRE(line2.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        CHECK_EQ(line2[i], expected[i]);

    for (std::size_t i = 1; i < tokens.size(); ++i)
    {
        auto const& prev = tokens[i - 1];
        auto const& cur = tokens[i];
        if (prev.line == cur.line)
            CHECK(cur.character >= prev.character + prev.length);
        else
            CHECK_LT(prev.line, cur.line);
    }
}

TEST_CASE("server semanticTokens/full tokenizes a placeholder whose percent comes from an escape")
{
    namespace st = dccd::semantic_tokens;

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;

    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\nvoid f() {\n    asm { \"\\u{25}[y]\" };\n}\n");
    auto resp = send_request(server, sink, make_semantic_tokens_request(uri));
    REQUIRE(resp.has_value());
    auto const* result = resp->find_member("result");
    REQUIRE(result != nullptr);
    auto const* data = result->get_array("data");
    REQUIRE(data != nullptr);

    std::vector<std::uint32_t> flat;
    for (auto const& v : data->as_array())
    {
        REQUIRE(v.is_number());
        flat.push_back(static_cast<std::uint32_t>(v.as_integer()));
    }
    auto tokens = decode_delta(flat);

    std::vector<DecodedToken> line2;
    for (auto const& t : tokens)
        if (t.line == 2u)
            line2.push_back(t);

    std::vector<DecodedToken> expected = {
        {2, 10, 1, static_cast<std::uint32_t>(st::TokenType::String), 0},
        {2, 11, 9, static_cast<std::uint32_t>(st::TokenType::AsmPlaceholder), 0},
        {2, 20, 1, static_cast<std::uint32_t>(st::TokenType::String), 0},
    };
    REQUIRE(line2.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        CHECK_EQ(line2[i], expected[i]);
}

TEST_CASE("asm hover covers escaped placeholder raw ranges and register tokens after escapes")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\nvoid f() {\n    asm { \"nop \\n %[x]; mov %%eax, %[x]\" };\n}\n");

    auto res = send_position_request(server, sink, uri, 2u, 20u, "textDocument/hover");
    REQUIRE(res.value.has_value());
    REQUIRE(!res.value->is_null());
    auto const* contents = res.value->find_member("contents");
    REQUIRE(contents != nullptr);
    auto value = contents->get_string("value");
    REQUIRE(value.has_value());
    CHECK(value->find("operand `x`") != std::string::npos);
    auto const* hrange = res.value->find_member("range");
    REQUIRE(hrange != nullptr);
    auto hr = dccd::protocol::LspRange::from_json(*hrange);
    CHECK_EQ(hr.start.line, 2u);
    CHECK_EQ(hr.start.character, 18u);
    CHECK_EQ(hr.end.line, 2u);
    CHECK_EQ(hr.end.character, 22u);

    res = send_position_request(server, sink, uri, 2u, 30u, "textDocument/hover");
    REQUIRE(res.value.has_value());
    REQUIRE(!res.value->is_null());
    contents = res.value->find_member("contents");
    REQUIRE(contents != nullptr);
    value = contents->get_string("value");
    REQUIRE(value.has_value());
    CHECK(value->find("register `eax`") != std::string::npos);
    hrange = res.value->find_member("range");
    REQUIRE(hrange != nullptr);
    hr = dccd::protocol::LspRange::from_json(*hrange);
    CHECK_EQ(hr.start.line, 2u);
    CHECK_EQ(hr.start.character, 28u);
    CHECK_EQ(hr.end.line, 2u);
    CHECK_EQ(hr.end.character, 33u);
}

TEST_CASE("asm hover matches a cursor inside the escape that produced the placeholder percent")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\nvoid f() {\n    asm { \"\\u{25}[y]\" };\n}\n");

    for (std::uint32_t ch : {12u, 13u, 17u, 19u})
    {
        auto res = send_position_request(server, sink, uri, 2u, ch, "textDocument/hover");
        REQUIRE(res.value.has_value());
        REQUIRE(!res.value->is_null());
        auto const* contents = res.value->find_member("contents");
        REQUIRE(contents != nullptr);
        auto value = contents->get_string("value");
        REQUIRE(value.has_value());
        CHECK(value->find("operand `y`") != std::string::npos);
        auto const* hrange = res.value->find_member("range");
        REQUIRE(hrange != nullptr);
        auto hr = dccd::protocol::LspRange::from_json(*hrange);
        CHECK_EQ(hr.start.line, 2u);
        CHECK_EQ(hr.start.character, 11u);
        CHECK_EQ(hr.end.line, 2u);
        CHECK_EQ(hr.end.character, 20u);
    }
}

TEST_CASE("function-pointer alias parameter names stay name-free in hover output")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto path = td.path / "main.dc";

    auto check_hover = [&](std::string_view marker_source, std::string_view expected_fragment) {
        auto [t, pos] = strip_marker(std::string{marker_source}, '|');
        auto uri = open_file(server, sink, path, t);
        auto hover = request_hover(server, sink, uri, pos.line, pos.character);
        REQUIRE(hover.has_value());
        CHECK(hover->find(expected_fragment) != std::string::npos);

        CHECK(hover->find("value") == std::string::npos);
        CHECK(hover->find("enabled") == std::string::npos);
    };

    check_hover("module m;\n"
                "using Call|back = void(*)(i32 value, bool enabled);\n"
                "void invoke(Callback cb) { cb(1, true); }\n",
                "using Callback = void(*)(i32, bool)");

    check_hover("module m;\n"
                "using Callback = void(*)(i32 va|lue, bool enabled);\n"
                "void invoke(Callback cb) { cb(1, true); }\n",
                "void(*)(i32, bool)");

    check_hover("module m;\n"
                "using Callback = void(*)(i32 value, bool enabled);\n"
                "void invoke(Callback c|b) { cb(1, true); }\n",
                "void(*)(i32, bool) cb");

    check_hover("module m;\n"
                "using Callback = void(*)(i32 value, bool enabled);\n"
                "void inv|oke(Callback cb) { cb(1, true); }\n",
                "void invoke(void(*)(i32, bool) cb)");
}

TEST_CASE("server publishes undefined asm operand diagnostic on the raw escaped range")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    auto publishes = send_and_collect_publishes(
        server, sink,
        make_did_open(dcc::sm::SourceManager::to_file_uri(td.path / "main.dc"), 1, "module main;\nvoid f() {\n    asm { \"\\u{25}[missing]\" };\n}\n"));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].diagnostics.size() == 1);
    auto const& diag = publishes[0].diagnostics[0];
    CHECK(diag.message.find("undefined asm operand `missing`") != std::string::npos);

    CHECK_EQ(diag.range.start.line, 2u);
    CHECK_EQ(diag.range.start.character, 11u);
    CHECK_EQ(diag.range.end.line, 2u);
    CHECK_EQ(diag.range.end.character, 26u);
}

TEST_CASE("unknown position encodings are skipped and the first supported one wins")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};

    auto params = dccd::protocol::JsonValue::empty_object();
    auto encs = dccd::protocol::JsonValue::empty_array();
    encs.push_back(dccd::protocol::JsonValue::string_val("gbk"));
    encs.push_back(dccd::protocol::JsonValue::string_val("utf-8"));
    auto general = dccd::protocol::JsonValue::empty_object();
    general.set("positionEncodings", std::move(encs));
    auto caps = dccd::protocol::JsonValue::empty_object();
    caps.set("general", std::move(general));
    params.set("capabilities", std::move(caps));

    auto init_req = dccd::protocol::build_request(JsonValue::integer(1), "initialize", std::move(params));
    auto parsed = dccd::protocol::parse_rpc(init_req);
    REQUIRE(parsed.has_value());
    auto response = server.handle_message(*parsed);
    REQUIRE(response.has_value());

    auto const* result = response->find_member("result");
    REQUIRE(result != nullptr);
    auto const* resp_caps = result->get_object("capabilities");
    REQUIRE(resp_caps != nullptr);
    auto enc = resp_caps->get_string("positionEncoding");
    REQUIRE(enc.has_value());
    CHECK_EQ(*enc, "utf-8");
    CHECK_EQ(server.source_manager().position_encoding(), dcc::sm::PositionEncoding::Utf8);
}

TEST_CASE("clients without positionEncodings keep UTF-16 end to end")
{
    namespace st = dccd::semantic_tokens;

    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    CHECK_EQ(server.source_manager().position_encoding(), dcc::sm::PositionEncoding::Utf16);

    TempDir td;
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\nvoid f() {\n    const char* s = \"\xF0\x9F\x98\x80\"; i32 y = 0;\n}\n");
    auto resp = send_request(server, sink, make_semantic_tokens_request(uri));
    REQUIRE(resp.has_value());
    auto const* result = resp->find_member("result");
    REQUIRE(result != nullptr);
    auto const* data = result->get_array("data");
    REQUIRE(data != nullptr);

    std::vector<std::uint32_t> flat;
    for (auto const& v : data->as_array())
    {
        REQUIRE(v.is_number());
        flat.push_back(static_cast<std::uint32_t>(v.as_integer()));
    }
    auto tokens = decode_delta(flat);
    REQUIRE(!tokens.empty());

    auto str_it =
        std::ranges::find_if(tokens, [](DecodedToken const& t) { return t.line == 2u && t.type == static_cast<std::uint32_t>(st::TokenType::String); });
    REQUIRE(str_it != tokens.end());
    CHECK_EQ(str_it->character, 20u);
    CHECK_EQ(str_it->length, 4u);
}

TEST_CASE("non-ASCII definition positions round-trip in each encoding")
{

    std::string const text = "module main;\nconst char* s = \"\xF0\x9F\x98\x80\"; i32 y = 0;\nvoid f() {\n    i32 z = y;\n}\n";

    for (auto [enc_name, sm_enc] : {std::pair{"utf-8", dcc::sm::PositionEncoding::Utf8}, std::pair{"utf-16", dcc::sm::PositionEncoding::Utf16},
                                    std::pair{"utf-32", dcc::sm::PositionEncoding::Utf32}})
    {
        Sink sink;
        dccd::LanguageServer server{&sink.stream};
        initialize_server(server, sink, {}, {enc_name});

        TempDir td;
        auto uri = open_file(server, sink, td.path / "main.dc", text);

        auto def = request_definition(server, sink, uri, 3u, 12u);
        REQUIRE(def.has_value());
        CHECK_EQ(def->uri, uri);
        CHECK_EQ(def->range.start.line, 1u);

        std::uint32_t def_col = 0;
        std::uint32_t def_end_col = 0;
        switch (sm_enc)
        {
            case dcc::sm::PositionEncoding::Utf8:
                def_col = 28u;
                break;
            case dcc::sm::PositionEncoding::Utf16:
                def_col = 26u;
                break;
            case dcc::sm::PositionEncoding::Utf32:
                def_col = 25u;
                break;
        }
        def_end_col = def_col + 1u;
        CHECK_EQ(def->range.start.character, def_col);
        CHECK_EQ(def->range.end.character, def_end_col);

        auto& sm = server.source_manager();
        auto fid = sm.find_by_uri(uri);
        REQUIRE(fid.has_value());
        auto loc = sm.lsp_position_to_location(*fid, 1u, def_col);
        REQUIRE(loc.has_value());
        CHECK_EQ(loc->offset, 41u);
    }
}

TEST_CASE("semanticTokens/full refuses a stale graph with empty data")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"file:///tmp/dccd_lsp_stale_tokens.dc"};
    std::ignore = send_and_collect_publishes(server, sink, make_did_open(uri, 1, "module m;\ni32 x = 0;\n"));
    std::ignore = send_and_collect_publishes(server, sink, make_did_close(uri));

    std::ignore = send_and_collect_publishes(server, sink, make_did_change(uri, 2, "module m;\ni32 y = 0;\n"));

    auto resp = send_request(server, sink, make_semantic_tokens_request(uri));
    REQUIRE(resp.has_value());
    auto const* result = resp->find_member("result");
    REQUIRE(result != nullptr);
    auto const* data = result->get_array("data");
    REQUIRE(data != nullptr);
    CHECK_EQ(data->array_size(), 0u);
}

TEST_CASE("cancelled semanticTokens/full returns RequestCancelled")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;

    std::string big = "module main;\n";
    for (int i = 0; i < 40000; ++i)
        big += std::format("i32 var{} = {};\n", i, i);
    big += "void f() {\n    i32 x = var0;\n}\n";
    auto uri = open_file(server, sink, td.path / "main.dc", big);

    auto id = dccd::protocol::RequestId::from_json(JsonValue::integer(888));
    auto params = dccd::protocol::JsonValue::empty_object();
    params.set("textDocument", make_text_document(uri));
    auto request = dccd::protocol::build_request(JsonValue::integer(888), "textDocument/semanticTokens/full", std::move(params));

    std::promise<std::optional<dccd::protocol::JsonValue>> result_promise;
    auto result_future = result_promise.get_future();

    std::thread worker{[&] {
        auto parsed = dccd::protocol::parse_rpc(request);
        if (!parsed)
        {
            result_promise.set_value(std::nullopt);
            return;
        }
        try
        {
            result_promise.set_value(server.handle_message(*parsed));
        }
        catch (...)
        {
            result_promise.set_value(std::nullopt);
        }
    }};

    auto& registry = server.cancellation_registry();
    bool seen_pending = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (registry.is_pending(id))
        {
            seen_pending = true;
            break;
        }
        std::this_thread::yield();
    }
    REQUIRE(seen_pending);

    std::ignore = registry.cancel(id);

    worker.join();
    auto response = result_future.get();
    REQUIRE(response.has_value());

    auto const* err = response->get_object("error");
    REQUIRE(err != nullptr);
    auto code = err->get_integer("code");
    REQUIRE(code.has_value());
    CHECK_EQ(*code, dccd::protocol::kErrorRequestCancelled);
    auto const* resp_id = response->find_member("id");
    REQUIRE(resp_id != nullptr);
    REQUIRE(resp_id->is_number());
    CHECK_EQ(resp_id->as_integer(), 888);
    CHECK(response->find_member("result") == nullptr);

    CHECK(!registry.is_pending(id));
    CHECK(!registry.is_cancelled(id));
}

SECTION("lsp: range and on-type formatting");

namespace
{
    [[nodiscard]] dccd::protocol::JsonValue make_range_formatting_request(std::string const& uri, dccd::protocol::LspRange range, std::string id = "1")
    {
        auto td = dccd::protocol::JsonValue::empty_object();
        td.set("uri", dccd::protocol::JsonValue::string_val(uri));

        auto options = dccd::protocol::JsonValue::empty_object();
        options.set("tabSize", dccd::protocol::JsonValue::integer(4));
        options.set("insertSpaces", dccd::protocol::JsonValue::boolean(true));

        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", std::move(td));
        params.set("range", range.to_json());
        params.set("options", std::move(options));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::string_val(std::move(id)), "textDocument/rangeFormatting", std::move(params));
    }

    [[nodiscard]] dccd::protocol::JsonValue make_on_type_formatting_request(std::string const& uri, std::string_view ch, std::uint32_t line,
                                                                            std::uint32_t character, std::string id = "1")
    {
        auto td = dccd::protocol::JsonValue::empty_object();
        td.set("uri", dccd::protocol::JsonValue::string_val(uri));

        auto options = dccd::protocol::JsonValue::empty_object();
        options.set("tabSize", dccd::protocol::JsonValue::integer(4));
        options.set("insertSpaces", dccd::protocol::JsonValue::boolean(true));

        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", std::move(td));
        params.set("position", make_position(line, character));
        params.set("ch", dccd::protocol::JsonValue::string_val(std::string{ch}));
        params.set("options", std::move(options));
        return dccd::protocol::build_request(dccd::protocol::JsonValue::string_val(std::move(id)), "textDocument/onTypeFormatting", std::move(params));
    }

    [[nodiscard]] std::vector<dccd::protocol::TextEdit> request_range_formatting(dccd::LanguageServer& server, Sink& sink, std::string const& uri,
                                                                                 dccd::protocol::LspRange range, bool* got_null = nullptr)
    {
        auto resp = send_request(server, sink, make_range_formatting_request(uri, range));
        std::vector<dccd::protocol::TextEdit> out;
        if (!resp)
            return out;

        auto const* result_val = resp->find_member("result");
        if (!result_val || result_val->is_null())
        {
            if (got_null)
                *got_null = true;
            return out;
        }
        if (!result_val->is_array())
            return out;

        for (auto const& edit_json : result_val->as_array())
            out.push_back(dccd::protocol::TextEdit::from_json(edit_json));
        return out;
    }

    [[nodiscard]] std::vector<dccd::protocol::TextEdit> request_on_type_formatting(dccd::LanguageServer& server, Sink& sink, std::string const& uri,
                                                                                   std::string_view ch, std::uint32_t line, std::uint32_t character,
                                                                                   bool* got_null = nullptr)
    {
        auto resp = send_request(server, sink, make_on_type_formatting_request(uri, ch, line, character));
        std::vector<dccd::protocol::TextEdit> out;
        if (!resp)
            return out;

        auto const* result_val = resp->find_member("result");
        if (!result_val || result_val->is_null())
        {
            if (got_null)
                *got_null = true;
            return out;
        }
        if (!result_val->is_array())
            return out;

        for (auto const& edit_json : result_val->as_array())
            out.push_back(dccd::protocol::TextEdit::from_json(edit_json));
        return out;
    }

    [[nodiscard]] std::vector<dccd::protocol::TextEdit> request_formatting_full(dccd::LanguageServer& server, Sink& sink, std::string const& uri,
                                                                                bool* got_null = nullptr)
    {
        auto resp = send_request(server, sink, make_formatting_request(uri));
        std::vector<dccd::protocol::TextEdit> out;
        if (!resp)
            return out;

        auto const* result_val = resp->find_member("result");
        if (!result_val || result_val->is_null())
        {
            if (got_null)
                *got_null = true;
            return out;
        }
        if (!result_val->is_array())
            return out;

        for (auto const& edit_json : result_val->as_array())
            out.push_back(dccd::protocol::TextEdit::from_json(edit_json));
        return out;
    }

} // namespace

TEST_CASE("range formatting returns only the selected unformatted region and applies to the full-document formatter")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = open_file(server, sink, std::filesystem::temp_directory_path() / "dccd_lsp_range_fmt.dc", "module m;\nvoid f() {\n    i32 x=1;\n}\n");

    dccd::protocol::LspRange range;
    range.start.line = 2;
    range.start.character = 8;
    range.end.line = 2;
    range.end.character = 12;

    auto edits = request_range_formatting(server, sink, uri, range);
    REQUIRE(edits.size() == 1);
    CHECK_EQ(edits[0].range.start.line, 2u);
    CHECK_EQ(edits[0].range.start.character, 9u);
    CHECK_EQ(edits[0].range.end.character, 10u);
    CHECK_EQ(edits[0].newText, " = ");

    auto full = request_formatting_full(server, sink, uri);
    REQUIRE(full.size() == 1);
    std::string applied = "module m;\nvoid f() {\n    i32 x=1;\n}\n";
    auto at = applied.find("x=1");
    REQUIRE(at != std::string::npos);
    applied.replace(at + 1, 1, edits[0].newText);
    CHECK_EQ(applied, full[0].newText);
}

TEST_CASE("range formatting refuses when another needed change lies outside the selection")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = open_file(server, sink, std::filesystem::temp_directory_path() / "dccd_lsp_range_fmt_refuse.dc",
                         "module m;\nvoid f() {\n    i32 x=1;\n    i32 y=2;\n}\n");

    dccd::protocol::LspRange range;
    range.start.line = 2;
    range.start.character = 8;
    range.end.line = 2;
    range.end.character = 12;
    auto edits = request_range_formatting(server, sink, uri, range);
    CHECK(edits.empty());

    range.start.line = 2;
    range.start.character = 12;
    range.end.line = 2;
    range.end.character = 8;
    edits = request_range_formatting(server, sink, uri, range);
    CHECK(edits.empty());

    auto uri2 = open_file(server, sink, std::filesystem::temp_directory_path() / "dccd_lsp_range_fmt_noop.dc", "module m;\nvoid f() {\n    i32 x = 1;\n}\n");
    range.start.line = 2;
    range.start.character = 8;
    range.end.line = 2;
    range.end.character = 14;
    edits = request_range_formatting(server, sink, uri2, range);
    CHECK(edits.empty());
}

TEST_CASE("range formatting honors negotiated utf-8, utf-16 and utf-32 positions")
{
    for (auto enc : {std::string_view{"utf-8"}, std::string_view{"utf-16"}, std::string_view{"utf-32"}})
    {
        Sink sink;
        dccd::LanguageServer server{&sink.stream};
        initialize_server(server, sink, {}, {enc});

        std::string src = "module m;\nvoid f() {\n    const char* s=\"caf\u00e9\";\n    i32 y = 2;\n}\n";
        auto uri = open_file(server, sink, std::filesystem::temp_directory_path() / "dccd_lsp_range_fmt_enc.dc", src);

        std::uint32_t line_end = (enc == "utf-8") ? 26u : 25u;
        dccd::protocol::LspRange range;
        range.start.line = 2;
        range.start.character = 17;
        range.end.line = 2;
        range.end.character = line_end;

        auto edits = request_range_formatting(server, sink, uri, range);
        REQUIRE(edits.size() == 1);
        CHECK_EQ(edits[0].range.start.character, 17u);
        CHECK_EQ(edits[0].range.end.character, 18u);
        CHECK_EQ(edits[0].newText, " = ");

        auto full = request_formatting_full(server, sink, uri);
        REQUIRE(full.size() == 1);
        std::string applied{src};
        auto eq = applied.find('=');
        REQUIRE(eq != std::string::npos);
        applied.replace(eq, 1, edits[0].newText);
        CHECK_EQ(applied, full[0].newText);
    }
}

TEST_CASE("on-type '}' reindents and ';' cleans up through the LSP server")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = open_file(server, sink, std::filesystem::temp_directory_path() / "dccd_lsp_ontype_brace.dc",
                         "module m;\nvoid f() {\n    if (cond) {\n        i32 x = 1;\n   }\n}\n");

    auto edits = request_on_type_formatting(server, sink, uri, "}", 4, 4);
    REQUIRE(edits.size() == 1);
    CHECK_EQ(edits[0].newText, " ");
    CHECK_EQ(edits[0].range.start.line, 4u);
    CHECK_EQ(edits[0].range.start.character, 3u);
    CHECK_EQ(edits[0].range.end.character, 3u);

    auto uri2 = open_file(server, sink, std::filesystem::temp_directory_path() / "dccd_lsp_ontype_semi.dc", "module m;\nvoid f() {\n    i32 x=1;\n}\n");
    edits = request_on_type_formatting(server, sink, uri2, ";", 2, 12);
    REQUIRE(edits.size() == 1);
    CHECK_EQ(edits[0].newText, " = ");
    CHECK_EQ(edits[0].range.start.character, 9u);
    CHECK_EQ(edits[0].range.end.character, 10u);

    edits = request_on_type_formatting(server, sink, uri2, "}", 2, 12);
    CHECK(edits.empty());

    edits = request_on_type_formatting(server, sink, uri2, "", 2, 12);
    CHECK(edits.empty());
}

TEST_CASE("all formatting variants refuse a stale URI after a failed didChange")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"file:///tmp/dccd_lsp_fmt_stale.dc"};
    std::ignore = send_and_collect_publishes(server, sink, make_did_open(uri, 1, "module m;\ni32 x = 0;\n"));
    std::ignore = send_and_collect_publishes(server, sink, make_did_close(uri));

    std::ignore = send_and_collect_publishes(server, sink, make_did_change(uri, 2, "module m;\ni32 y = 0;\n"));

    bool got_null = false;
    auto edits = request_formatting_full(server, sink, uri, &got_null);
    CHECK(edits.empty());
    CHECK(got_null);

    dccd::protocol::LspRange range;
    range.start.line = 0;
    range.start.character = 0;
    range.end.line = 1;
    range.end.character = 0;
    got_null = false;
    edits = request_range_formatting(server, sink, uri, range, &got_null);
    CHECK(edits.empty());
    CHECK(got_null);

    got_null = false;
    edits = request_on_type_formatting(server, sink, uri, "}", 1, 0, &got_null);
    CHECK(edits.empty());
    CHECK(got_null);
}

TEST_CASE("range formatting does not recompile or resync the graph for a fresh buffer")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    TempDir td;
    initialize_server(server, sink, td.path);

    {
        std::ofstream out{td.path / "lib.dc"};
        out << "module lib;\npublic i32 value = 1;\n";
    }
    auto uri = open_file(server, sink, td.path / "main.dc", "module main;\n\nimport lib;\n\nvoid f() {\n    i32 x=1;\n}\n");

    auto const& st = server.workspace_index().stats();
    auto sync_before = st.sync_count;
    auto extracted_before = st.modules_re_extracted;

    dccd::protocol::LspRange range;
    range.start.line = 5;
    range.start.character = 4;
    range.end.line = 5;
    range.end.character = 12;
    auto edits = request_range_formatting(server, sink, uri, range);
    REQUIRE(edits.size() == 1);

    CHECK_EQ(server.workspace_index().stats().sync_count, sync_before);
    CHECK_EQ(server.workspace_index().stats().modules_re_extracted, extracted_before);
}

TEST_CASE("cancelled range formatting returns RequestCancelled at the post-format checkpoint")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;

    std::string big = "module main;\n";
    for (int i = 0; i < 20000; ++i)
        big += std::format("i32 var{} = {};\n", i, i);
    big += "void f() {\n    i32 x=1;\n}\n";
    auto uri = open_file(server, sink, td.path / "main.dc", big);

    auto id = dccd::protocol::RequestId::from_json(JsonValue::string_val("991"));
    dccd::protocol::LspRange range;
    range.start.line = 0;
    range.start.character = 0;
    range.end.line = static_cast<std::uint32_t>(20001);
    range.end.character = 0;
    auto request = make_range_formatting_request(uri, range, "991");

    std::promise<std::optional<dccd::protocol::JsonValue>> result_promise;
    auto result_future = result_promise.get_future();

    std::thread worker{[&] {
        auto parsed = dccd::protocol::parse_rpc(request);
        if (!parsed)
        {
            result_promise.set_value(std::nullopt);
            return;
        }
        try
        {
            result_promise.set_value(server.handle_message(*parsed));
        }
        catch (...)
        {
            result_promise.set_value(std::nullopt);
        }
    }};

    auto& registry = server.cancellation_registry();
    bool seen_pending = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (registry.is_pending(id))
        {
            seen_pending = true;
            break;
        }
        std::this_thread::yield();
    }
    REQUIRE(seen_pending);

    std::ignore = registry.cancel(id);

    worker.join();
    auto response = result_future.get();
    REQUIRE(response.has_value());

    auto const* err = response->get_object("error");
    REQUIRE(err != nullptr);
    auto code = err->get_integer("code");
    REQUIRE(code.has_value());
    CHECK_EQ(*code, dccd::protocol::kErrorRequestCancelled);
    auto const* resp_id = response->find_member("id");
    REQUIRE(resp_id != nullptr);
    REQUIRE(resp_id->is_string());
    CHECK_EQ(resp_id->as_string(), "991");
    CHECK(response->find_member("result") == nullptr);

    CHECK(!registry.is_pending(id));
    CHECK(!registry.is_cancelled(id));
}

SECTION("lsp: inlay hints");

namespace
{

    struct AnalyzedDoc
    {
        TempDir td;
        dcc::session::CompilerSession session;
        dcc::session::CompileOptions copts;
        dcc::ast::TranslationUnit const* tu{nullptr};
        dcc::sm::FileId fid{dcc::sm::FileId::Invalid};

        AnalyzedDoc() : session{{.silent_diagnostics = true}} {}

        bool analyze(std::string_view text)
        {
            auto path = td.path / "main.dc";
            {
                std::ofstream out{path};
                out << text;
            }
            auto result = session.analyze_entry(path, copts);
            if (!result.module)
                return false;

            auto* sema_ctx = session.sema_context();
            if (!sema_ctx)
                return false;

            auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();
            for (auto const& mod : graph.all())
                if (mod && mod->tu)
                {
                    tu = mod->tu;
                    fid = mod->file_id;
                    break;
                }
            return tu != nullptr;
        }

        [[nodiscard]] dcc::sm::SourceRange full_range() const
        {
            auto const* sf = session.source_manager().get(fid);
            auto const size = sf ? static_cast<dcc::sm::Offset>(sf->size()) : dcc::sm::Offset{0};
            return dcc::sm::SourceRange{{fid, 0}, {fid, size}};
        }

        [[nodiscard]] std::vector<dccd::protocol::InlayHint> collect(dccd::protocol::InlayHintOptions const& opts = {},
                                                                     dccd::inlay_hints::CancelCheck const& cancel = {})
        {
            auto formatter = [](dcc::types::Type const* ty) -> std::string { return dcc::sema::format_dcc_type(ty); };
            return dccd::inlay_hints::collect_inlay_hints(session.source_manager(), tu, full_range(), formatter, opts, cancel);
        }
    };

    [[nodiscard]] bool has_hint(std::vector<dccd::protocol::InlayHint> const& hints, std::uint32_t line, std::uint32_t character, std::string_view label)
    {
        for (auto const& h : hints)
            if (h.position.line == line && h.position.character == character && h.label == label)
                return true;
        return false;
    }

    [[nodiscard]] bool has_any_at(std::vector<dccd::protocol::InlayHint> const& hints, std::uint32_t line, std::uint32_t character)
    {
        for (auto const& h : hints)
            if (h.position.line == line && h.position.character == character)
                return true;
        return false;
    }

    [[nodiscard]] dccd::protocol::LspRange full_file_range(std::string_view text)
    {
        dccd::protocol::LspRange r;
        r.start.line = 0;
        r.start.character = 0;

        r.end.line = static_cast<std::uint32_t>(std::count(text.begin(), text.end(), '\n'));
        r.end.character = 0;
        return r;
    }

    [[nodiscard]] dccd::protocol::JsonValue make_inlay_hint_request(std::string const& uri, dccd::protocol::LspRange range, std::string id = "1")
    {
        auto td = dccd::protocol::JsonValue::empty_object();
        td.set("uri", dccd::protocol::JsonValue::string_val(uri));

        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("textDocument", std::move(td));
        params.set("range", range.to_json());
        return dccd::protocol::build_request(dccd::protocol::JsonValue::string_val(std::move(id)), "textDocument/inlayHint", std::move(params));
    }

    [[nodiscard]] std::vector<dccd::protocol::InlayHint> request_inlay_hints(dccd::LanguageServer& server, Sink& sink, std::string const& uri,
                                                                             dccd::protocol::LspRange range)
    {
        auto resp = send_request(server, sink, make_inlay_hint_request(uri, range));
        std::vector<dccd::protocol::InlayHint> out;
        if (!resp)
            return out;

        auto const* result_val = resp->find_member("result");
        if (!result_val || !result_val->is_array())
            return out;

        for (auto const& h_json : result_val->as_array())
        {
            dccd::protocol::InlayHint h;
            if (auto const* pos = h_json.find_member("position"))
            {
                if (auto l = pos->get_integer("line"))
                    h.position.line = static_cast<std::uint32_t>(*l);
                if (auto c = pos->get_integer("character"))
                    h.position.character = static_cast<std::uint32_t>(*c);
            }
            if (auto s = h_json.get_string("label"))
                h.label = std::move(*s);
            if (auto k = h_json.get_integer("kind"))
                h.kind = static_cast<std::int32_t>(*k);
            out.push_back(std::move(h));
        }
        return out;
    }

    [[nodiscard]] dccd::protocol::JsonValue make_inlay_hint_settings(std::optional<bool> type_hints = std::nullopt,
                                                                     std::optional<bool> parameter_hints = std::nullopt,
                                                                     std::optional<bool> suppress = std::nullopt)
    {
        auto hints = dccd::protocol::JsonValue::empty_object();
        if (type_hints)
            hints.set("typeHints", dccd::protocol::JsonValue::boolean(*type_hints));
        if (parameter_hints)
            hints.set("parameterHints", dccd::protocol::JsonValue::boolean(*parameter_hints));
        if (suppress)
            hints.set("suppressParameterNameMatches", dccd::protocol::JsonValue::boolean(*suppress));

        auto dcc_obj = dccd::protocol::JsonValue::empty_object();
        dcc_obj.set("inlayHints", std::move(hints));

        auto settings = dccd::protocol::JsonValue::empty_object();
        settings.set("dcc", std::move(dcc_obj));
        return settings;
    }

    void send_config(dccd::LanguageServer& server, Sink& sink, dccd::protocol::JsonValue settings)
    {
        auto params = dccd::protocol::JsonValue::empty_object();
        params.set("settings", std::move(settings));
        auto notif = dccd::protocol::build_notification("workspace/didChangeConfiguration", std::move(params));
        auto parsed = dccd::protocol::parse_rpc(notif);
        REQUIRE(parsed.has_value());
        std::ignore = server.handle_message(*parsed);
        CHECK(sink.drain().empty());
    }
} // namespace

TEST_CASE("direct collector emits type hints for inferred for-in bindings")
{
    AnalyzedDoc doc;
    std::string src = "module main;\n"
                      "\n"
                      "void f() {\n"
                      "    i32[4] arr = {1, 2, 3, 4};\n"
                      "    for item| in arr {\n"
                      "    }\n"
                      "    for &slot| in arr {\n"
                      "    }\n"
                      "}\n";
    auto [clean, item_end] = strip_marker(src, '|');
    auto [clean2, slot_end] = strip_marker(clean, '|');
    REQUIRE(doc.analyze(clean2));

    auto hints = doc.collect();
    REQUIRE(hints.size() == 2u);

    CHECK(has_hint(hints, item_end.line, item_end.character, ": i32"));

    CHECK(has_hint(hints, slot_end.line, slot_end.character, ": i32*"));
    CHECK_EQ(hints[0].kind.value_or(-1), dccd::protocol::InlayHintKind::Type);
    CHECK_EQ(hints[1].kind.value_or(-1), dccd::protocol::InlayHintKind::Type);
}

TEST_CASE("direct collector does not speculate for unresolved for-in iterables")
{
    AnalyzedDoc doc;
    std::string src = "module main;\n"
                      "\n"
                      "void f() {\n"
                      "    for ghost| in missing {\n"
                      "    }\n"
                      "}\n";
    auto [clean, ghost_end] = strip_marker(src, '|');
    REQUIRE(doc.analyze(clean));

    auto hints = doc.collect();
    CHECK(hints.empty());
}

TEST_CASE("direct collector emits no type hint when the for-in binding is explicitly typed")
{
    AnalyzedDoc doc;
    std::string src = "module main;\n"
                      "\n"
                      "void f() {\n"
                      "    i32[4] arr = {1, 2, 3, 4};\n"
                      "    for i32 item| in arr {\n"
                      "    }\n"
                      "}\n";
    auto [clean, item_end] = strip_marker(src, '|');
    REQUIRE(doc.analyze(clean));

    auto hints = doc.collect();
    CHECK(hints.empty());
}

TEST_CASE("direct collector maps regular, UFCS and pack arguments conservatively")
{
    AnalyzedDoc doc;
    std::string src = "module main;\n"
                      "\n"
                      "struct Point {\n"
                      "    i32 x;\n"
                      "}\n"
                      "\n"
                      "i32 add(i32 lhs, i32 rhs) {\n"
                      "    return lhs + rhs;\n"
                      "}\n"
                      "\n"
                      "i32 dist(Point p, Point q) {\n"
                      "    return p.x;\n"
                      "}\n"
                      "\n"
                      "i32 sum(T, U...)([]T items, U extra) {\n"
                      "    return 1;\n"
                      "}\n"
                      "\n"
                      "void f() {\n"
                      "    Point a;\n"
                      "    Point b;\n"
                      "    i32[4] arr = {1, 2, 3, 4};\n"
                      "    i32 s = add(1, 2);\n"
                      "    i32 d = a.dist(b);\n"
                      "    i32 t = sum(arr, 1, 2, 3);\n"
                      "}\n";
    REQUIRE(doc.analyze(src));

    auto hints = doc.collect();

    CHECK(has_hint(hints, 22, 16, "lhs:"));
    CHECK(has_hint(hints, 22, 19, "rhs:"));

    CHECK(has_hint(hints, 23, 19, "q:"));
    CHECK(!has_any_at(hints, 23, 16));

    CHECK(has_hint(hints, 24, 16, "items:"));
    CHECK(has_hint(hints, 24, 21, "extra_0:"));
    CHECK(has_hint(hints, 24, 24, "extra_1:"));
    CHECK(has_hint(hints, 24, 27, "extra_2:"));
}

TEST_CASE("direct collector suppresses parameter hints whose identifier matches the parameter name")
{
    AnalyzedDoc doc;
    std::string src = "module main;\n"
                      "\n"
                      "struct Point {\n"
                      "    i32 x;\n"
                      "}\n"
                      "\n"
                      "i32 dist(Point p, Point q) {\n"
                      "    return p.x;\n"
                      "}\n"
                      "\n"
                      "void f() {\n"
                      "    Point p;\n"
                      "    Point q;\n"
                      "    i32 d = dist(p, q);\n"
                      "    i32 e = dist(p, other);\n"
                      "}\n";
    REQUIRE(doc.analyze(src));

    auto defaults = doc.collect();
    CHECK(!has_any_at(defaults, 13, 17));
    CHECK(!has_any_at(defaults, 13, 20));
    CHECK(has_hint(defaults, 14, 20, "q:"));

    dccd::protocol::InlayHintOptions opts;
    opts.suppressParameterNameMatches = false;
    auto unsuppressed = doc.collect(opts);
    CHECK(has_hint(unsuppressed, 13, 17, "p:"));
    CHECK(has_hint(unsuppressed, 13, 20, "q:"));
    CHECK(has_hint(unsuppressed, 14, 20, "q:"));
}

TEST_CASE("direct collector never labels arguments beyond the parameter list")
{
    AnalyzedDoc doc;
    std::string src = "module main;\n"
                      "\n"
                      "i32 add(i32 lhs, i32 rhs) {\n"
                      "    return lhs + rhs;\n"
                      "}\n"
                      "\n"
                      "void f() {\n"
                      "    i32 s = add(1, 2, 3, 4);\n"
                      "}\n";
    REQUIRE(doc.analyze(src));

    auto hints = doc.collect();
    CHECK(hints.empty());
}

TEST_CASE("direct collector filters hints to the requested range")
{
    AnalyzedDoc doc;
    std::string src = "module main;\n"
                      "\n"
                      "i32 add(i32 lhs, i32 rhs) {\n"
                      "    return lhs + rhs;\n"
                      "}\n"
                      "\n"
                      "void f() {\n"
                      "    i32[4] arr = {1, 2, 3, 4};\n"
                      "    for item in arr {\n"
                      "    }\n"
                      "    i32 s = add(1, 2);\n"
                      "}\n";
    REQUIRE(doc.analyze(src));

    auto const& sm = doc.session.source_manager();
    auto fid = doc.fid;
    auto start = sm.lsp_position_to_location(fid, dcc::sm::Position{10, 0});
    auto end = sm.lsp_position_to_location(fid, dcc::sm::Position{10, 40});
    REQUIRE(start.has_value());
    REQUIRE(end.has_value());
    dcc::sm::SourceRange range{*start, *end};

    auto formatter = [](dcc::types::Type const* ty) -> std::string { return dcc::sema::format_dcc_type(ty); };
    auto hints = dccd::inlay_hints::collect_inlay_hints(sm, doc.tu, range, formatter);
    REQUIRE(hints.size() == 2u);
    CHECK(has_hint(hints, 10, 16, "lhs:"));
    CHECK(has_hint(hints, 10, 19, "rhs:"));
    CHECK(!has_any_at(hints, 8, 12));
}

TEST_CASE("direct collector emits type hints for contextually deduced lambda params")
{
    AnalyzedDoc doc;
    std::string src = "module main;\n"
                      "\n"
                      "i32 apply(i32(*)(i32) f) {\n"
                      "    return f(1);\n"
                      "}\n"
                      "i32 add(i32(*)(i32, i32) f) {\n"
                      "    return f(1, 2);\n"
                      "}\n"
                      "\n"
                      "void use() {\n"
                      "    i32 a = apply(|x -> x * 2);\n"
                      "    i32 b = add(|l, r -> l + r);\n"
                      "    i32 c = apply(|i32 y -> y + 1);\n"
                      "}\n";
    REQUIRE(doc.analyze(src));

    auto hints = doc.collect();
    REQUIRE(hints.size() == 6u);
    CHECK(has_hint(hints, 10, 20, ": i32"));
    CHECK(has_hint(hints, 11, 18, ": i32"));
    CHECK(has_hint(hints, 11, 21, ": i32"));
    CHECK(!has_any_at(hints, 12, 23));
}

TEST_CASE("direct collector sorts deterministically and never duplicates a hint")
{
    AnalyzedDoc doc;
    std::string src = "module main;\n"
                      "\n"
                      "i32 add(i32 lhs, i32 rhs) {\n"
                      "    return lhs + rhs;\n"
                      "}\n"
                      "\n"
                      "void f() {\n"
                      "    i32[4] arr = {1, 2, 3, 4};\n"
                      "    for item in arr {\n"
                      "    }\n"
                      "    i32 s = add(1, 2);\n"
                      "    i32 t = add(3, 4);\n"
                      "}\n";
    REQUIRE(doc.analyze(src));

    auto a = doc.collect();
    auto b = doc.collect();

    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        CHECK_EQ(a[i].position.line, b[i].position.line);
        CHECK_EQ(a[i].position.character, b[i].position.character);
        CHECK_EQ(a[i].label, b[i].label);
        CHECK_EQ(a[i].kind, b[i].kind);
    }

    for (std::size_t i = 1; i < a.size(); ++i)
    {
        auto const& prev = a[i - 1];
        auto const& cur = a[i];
        if (prev.position.line != cur.position.line)
            CHECK_LT(prev.position.line, cur.position.line);
        else if (prev.position.character != cur.position.character)
            CHECK_LT(prev.position.character, cur.position.character);
        else if (prev.kind != cur.kind)
            CHECK_LT(prev.kind.value_or(-1), cur.kind.value_or(-1));
        else
            CHECK(prev.label <= cur.label);
    }

    for (std::size_t i = 1; i < a.size(); ++i)
    {
        auto const& prev = a[i - 1];
        auto const& cur = a[i];
        bool duplicate =
            prev.position.line == cur.position.line && prev.position.character == cur.position.character && prev.kind == cur.kind && prev.label == cur.label;
        CHECK(!duplicate);
    }
}

TEST_CASE("direct collector aborts early with no partial results when the cancellation callback fires")
{
    AnalyzedDoc doc;
    std::string src = "module main;\n";
    for (int i = 0; i < 4000; ++i)
        src += std::format("i32 var{} = {};\n", i, i);
    src += "void f() {\n    i32[4] arr = {1, 2, 3, 4};\n    for item in arr {\n    }\n}\n";
    REQUIRE(doc.analyze(src));

    auto data_full = doc.collect();
    CHECK(!data_full.empty());

    int probe_count = 0;
    auto data_aborted = doc.collect({}, [&] {
        ++probe_count;
        return true;
    });
    CHECK(data_aborted.empty());
    CHECK(probe_count > 0);

    auto data_kept = doc.collect({}, [] { return false; });
    REQUIRE(data_kept.size() == data_full.size());
    for (std::size_t i = 0; i < data_full.size(); ++i)
        CHECK_EQ(data_kept[i].label, data_full[i].label);
}

TEST_CASE("server inlayHint returns type and parameter hints through the full pipeline")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    std::string src = "module main;\n"
                      "\n"
                      "i32 add(i32 lhs, i32 rhs) {\n"
                      "    return lhs + rhs;\n"
                      "}\n"
                      "\n"
                      "void f() {\n"
                      "    i32[4] arr = {1, 2, 3, 4};\n"
                      "    for item in arr {\n"
                      "    }\n"
                      "    i32 s = add(1, 2);\n"
                      "}\n";
    auto uri = open_file(server, sink, td.path / "main.dc", src);

    auto hints = request_inlay_hints(server, sink, uri, full_file_range(src));
    REQUIRE(hints.size() == 3u);
    CHECK(has_hint(hints, 8, 12, ": i32"));
    CHECK(has_hint(hints, 10, 16, "lhs:"));
    CHECK(has_hint(hints, 10, 19, "rhs:"));
    for (auto const& h : hints)
        CHECK(h.kind.has_value());
}

TEST_CASE("server config toggles typeHints and parameterHints independently and re-enables them")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    std::string src = "module main;\n"
                      "\n"
                      "i32 add(i32 lhs, i32 rhs) {\n"
                      "    return lhs + rhs;\n"
                      "}\n"
                      "\n"
                      "void f() {\n"
                      "    i32[4] arr = {1, 2, 3, 4};\n"
                      "    for item in arr {\n"
                      "    }\n"
                      "    i32 s = add(1, 2);\n"
                      "}\n";
    auto uri = open_file(server, sink, td.path / "main.dc", src);
    auto range = full_file_range(src);

    auto hints = request_inlay_hints(server, sink, uri, range);
    CHECK(has_hint(hints, 8, 12, ": i32"));
    CHECK(has_hint(hints, 10, 16, "lhs:"));

    send_config(server, sink, make_inlay_hint_settings(false, std::nullopt, std::nullopt));
    hints = request_inlay_hints(server, sink, uri, range);
    CHECK(!has_any_at(hints, 8, 12));
    CHECK(has_hint(hints, 10, 16, "lhs:"));

    send_config(server, sink, make_inlay_hint_settings(true, false, std::nullopt));
    hints = request_inlay_hints(server, sink, uri, range);
    CHECK(has_hint(hints, 8, 12, ": i32"));
    CHECK(!has_any_at(hints, 10, 16));
    CHECK(!has_any_at(hints, 10, 19));

    send_config(server, sink, make_inlay_hint_settings(true, true, std::nullopt));
    hints = request_inlay_hints(server, sink, uri, range);
    CHECK(has_hint(hints, 8, 12, ": i32"));
    CHECK(has_hint(hints, 10, 16, "lhs:"));
    CHECK(has_hint(hints, 10, 19, "rhs:"));
}

TEST_CASE("server config toggles suppressParameterNameMatches")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    std::string src = "module main;\n"
                      "\n"
                      "struct Point {\n"
                      "    i32 x;\n"
                      "}\n"
                      "\n"
                      "i32 dist(Point p, Point q) {\n"
                      "    return p.x;\n"
                      "}\n"
                      "\n"
                      "void f() {\n"
                      "    Point p;\n"
                      "    Point q;\n"
                      "    i32 d = dist(p, q);\n"
                      "}\n";
    auto uri = open_file(server, sink, td.path / "main.dc", src);
    auto range = full_file_range(src);

    auto hints = request_inlay_hints(server, sink, uri, range);
    CHECK(!has_any_at(hints, 13, 17));
    CHECK(!has_any_at(hints, 13, 20));

    send_config(server, sink, make_inlay_hint_settings(std::nullopt, std::nullopt, false));
    hints = request_inlay_hints(server, sink, uri, range);
    CHECK(has_hint(hints, 13, 17, "p:"));
    CHECK(has_hint(hints, 13, 20, "q:"));
}

TEST_CASE("server preserves defaults for absent and invalid config values")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    std::string src = "module main;\n"
                      "\n"
                      "i32 add(i32 lhs, i32 rhs) {\n"
                      "    return lhs + rhs;\n"
                      "}\n"
                      "\n"
                      "void f() {\n"
                      "    i32[4] arr = {1, 2, 3, 4};\n"
                      "    for item in arr {\n"
                      "    }\n"
                      "    i32 s = add(1, 2);\n"
                      "}\n";
    auto uri = open_file(server, sink, td.path / "main.dc", src);
    auto range = full_file_range(src);

    send_config(server, sink, make_inlay_hint_settings(false, std::nullopt, std::nullopt));
    auto hints = request_inlay_hints(server, sink, uri, range);
    CHECK(!has_any_at(hints, 8, 12));

    auto settings = make_inlay_hint_settings(true, std::nullopt, std::nullopt);
    auto dcc_obj = settings.get_object("dcc");
    REQUIRE(dcc_obj != nullptr);
    auto hints_obj = dcc_obj->get_object("inlayHints");
    REQUIRE(hints_obj != nullptr);
    auto bad = dccd::protocol::JsonValue::empty_object();
    bad.set("typeHints", dccd::protocol::JsonValue::string_val("yes"));

    auto bad_hints_obj = dccd::protocol::JsonValue::empty_object();
    bad_hints_obj.set("typeHints", dccd::protocol::JsonValue::string_val("yes"));
    auto bad_dcc_obj = dccd::protocol::JsonValue::empty_object();
    bad_dcc_obj.set("inlayHints", std::move(bad_hints_obj));
    auto bad_settings = dccd::protocol::JsonValue::empty_object();
    bad_settings.set("dcc", std::move(bad_dcc_obj));
    send_config(server, sink, std::move(bad_settings));
    hints = request_inlay_hints(server, sink, uri, range);
    CHECK(!has_any_at(hints, 8, 12));

    auto empty_settings = dccd::protocol::JsonValue::empty_object();
    auto empty_dcc = dccd::protocol::JsonValue::empty_object();
    empty_dcc.set("unrelated", dccd::protocol::JsonValue::boolean(true));
    empty_settings.set("dcc", std::move(empty_dcc));
    send_config(server, sink, std::move(empty_settings));
    hints = request_inlay_hints(server, sink, uri, range);
    CHECK(!has_any_at(hints, 8, 12));
}

TEST_CASE("server inlayHint refuses a stale graph with an empty result")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"file:///tmp/dccd_lsp_stale_inlay.dc"};
    std::string src = "module main;\n\nvoid f() {\n    i32[4] arr = {1, 2, 3, 4};\n    for item in arr {\n    }\n}\n";
    std::ignore = send_and_collect_publishes(server, sink, make_did_open(uri, 1, src));
    std::ignore = send_and_collect_publishes(server, sink, make_did_close(uri));

    std::ignore = send_and_collect_publishes(server, sink, make_did_change(uri, 2, src));

    auto resp = send_request(server, sink, make_inlay_hint_request(uri, full_file_range(src)));
    REQUIRE(resp.has_value());
    auto const* result = resp->find_member("result");
    REQUIRE(result != nullptr);
    REQUIRE(result->is_array());
    CHECK_EQ(result->array_size(), 0u);
}

TEST_CASE("cancelled inlayHint returns RequestCancelled and never a partial array")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;

    std::string big = "module main;\n";
    for (int i = 0; i < 40000; ++i)
        big += std::format("i32 var{} = {};\n", i, i);
    big += "void f() {\n    i32[4] arr = {1, 2, 3, 4};\n    for item in arr {\n    }\n}\n";
    auto uri = open_file(server, sink, td.path / "main.dc", big);

    auto id = dccd::protocol::RequestId::from_json(JsonValue::integer(777));
    auto params = dccd::protocol::JsonValue::empty_object();
    params.set("textDocument", make_text_document(uri));
    params.set("range", full_file_range(big).to_json());
    auto request = dccd::protocol::build_request(JsonValue::integer(777), "textDocument/inlayHint", std::move(params));

    std::promise<std::optional<dccd::protocol::JsonValue>> result_promise;
    auto result_future = result_promise.get_future();

    std::thread worker{[&] {
        auto parsed = dccd::protocol::parse_rpc(request);
        if (!parsed)
        {
            result_promise.set_value(std::nullopt);
            return;
        }
        try
        {
            result_promise.set_value(server.handle_message(*parsed));
        }
        catch (...)
        {
            result_promise.set_value(std::nullopt);
        }
    }};

    auto& registry = server.cancellation_registry();
    bool seen_pending = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (registry.is_pending(id))
        {
            seen_pending = true;
            break;
        }
        std::this_thread::yield();
    }
    REQUIRE(seen_pending);

    std::ignore = registry.cancel(id);

    worker.join();
    auto response = result_future.get();
    REQUIRE(response.has_value());

    auto const* err = response->get_object("error");
    REQUIRE(err != nullptr);
    auto code = err->get_integer("code");
    REQUIRE(code.has_value());
    CHECK_EQ(*code, dccd::protocol::kErrorRequestCancelled);
    auto const* resp_id = response->find_member("id");
    REQUIRE(resp_id != nullptr);
    REQUIRE(resp_id->is_number());
    CHECK_EQ(resp_id->as_integer(), 777);
    CHECK(response->find_member("result") == nullptr);

    CHECK(!registry.is_pending(id));
    CHECK(!registry.is_cancelled(id));
}

TEST_CASE("server inlayHint measures non-ASCII positions under utf-8, utf-16 and utf-32")
{
    for (auto enc : {std::string_view{"utf-8"}, std::string_view{"utf-16"}, std::string_view{"utf-32"}})
    {
        Sink sink;
        dccd::LanguageServer server{&sink.stream};
        initialize_server(server, sink, {}, {enc});

        TempDir td;

        std::string src = "module main;\n"
                          "\n"
                          "void f() {\n"
                          "    i32[4] arr = {1, 2, 3, 4};\n"
                          "    const char* s = \"\xC3\xA9\"; for item in arr {\n"
                          "    }\n"
                          "}\n";
        auto uri = open_file(server, sink, td.path / "main.dc", src);

        auto hints = request_inlay_hints(server, sink, uri, full_file_range(src));
        REQUIRE(!hints.empty());
        bool found = false;
        for (auto const& h : hints)
        {
            if (h.position.line == 4u && h.label == ": i32")
            {
                found = true;
                CHECK_EQ(h.position.character, (enc == "utf-8") ? 34u : 33u);
            }
        }
        CHECK(found);
    }
}

TEST_CASE("hint-only configuration changes avoid a semantic recompile")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    TempDir td;
    std::string src = "module main;\n\nvoid f() {\n    i32 x = \"not an int\";\n}\n";
    auto uri = open_file(server, sink, td.path / "main.dc", src);

    send_config(server, sink, make_inlay_hint_settings(false, false, false));
    CHECK(sink.drain().empty());

    auto hints = request_inlay_hints(server, sink, uri, full_file_range(src));
    CHECK(hints.empty());
}

TEST_CASE("virtual dccv didOpen compiles and publishes diagnostics on the virtual uri")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"dccv:Zm9v/main.dc"};
    auto publishes = send_and_collect_publishes(
        server, sink, make_did_open(uri, 1, "module main;\nvoid f() {\n    i32 x = 0;\n    i32 x = 1;\n}\n"));

    REQUIRE(publishes.size() == 1);
    auto const& publish = publishes[0];
    CHECK_EQ(publish.uri, uri);
    REQUIRE(publish.version.has_value());
    CHECK_EQ(*publish.version, 1);
    REQUIRE(publish.diagnostics.size() == 1);
    CHECK(publish.diagnostics[0].message.find("redefinition of `x`") != std::string::npos);
    CHECK_EQ(publish.diagnostics[0].range.start.line, 3u);
    CHECK_EQ(publish.diagnostics[0].range.start.character, 8u);
    CHECK_EQ(publish.diagnostics[0].range.end.character, 9u);
}

TEST_CASE("virtual dccv didChange recompiles and clears diagnostics on the virtual uri")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"dccv:Zm9v/main.dc"};
    {
        auto publishes = send_and_collect_publishes(
            server, sink, make_did_open(uri, 1, "module main;\ni32 x = 0;\ni32 x = 1;\n"));
        REQUIRE(publishes.size() == 1);
        REQUIRE(publishes[0].diagnostics.size() == 1);
    }

    auto publishes = send_and_collect_publishes(server, sink, make_did_change(uri, 2, "module main;\ni32 x = 0;\n"));
    REQUIRE(publishes.size() == 1);
    REQUIRE(publishes[0].version.has_value());
    CHECK_EQ(*publishes[0].version, 2);
    CHECK(publishes[0].diagnostics.empty());

    publishes = send_and_collect_publishes(server, sink, make_did_close(uri));
    REQUIRE(publishes.size() == 1);
    CHECK(publishes[0].diagnostics.empty());
}

TEST_CASE("virtual dccv documents answer definition, references, rename and semantic tokens")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"dccv:Zm9v/main.dc"};
    std::ignore = open_virtual_file(server, sink, uri, "module main;\ni32 counter = 0;\nvoid f() {\n    i32 x = counter;\n}\n");

    auto def = request_definition(server, sink, uri, 3u, 14u);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, uri);
    CHECK_EQ(def->range.start.line, 1u);
    CHECK_EQ(def->range.start.character, 4u);
    CHECK_EQ(def->range.end.line, 1u);
    CHECK_EQ(def->range.end.character, 11u);

    auto refs = request_references(server, sink, uri, 1u, 5u, true);
    REQUIRE(refs.size() == 2);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 4u, 11u));
    CHECK(has_range(ranges, 3u, 12u, 19u));
    for (auto const& loc : refs)
        CHECK_EQ(loc.uri, uri);

    auto outcome = request_rename(server, sink, uri, 3u, 14u, "total");
    REQUIRE(outcome.edit.has_value());
    CHECK(!outcome.error.has_value());
    auto it = outcome.edit->changes.find(uri);
    REQUIRE(it != outcome.edit->changes.end());
    CHECK(check_edit_set(it->second, "total", {{1u, 4u, 11u}, {3u, 12u, 19u}}));

    auto resp = send_request(server, sink, make_semantic_tokens_request(uri));
    REQUIRE(resp.has_value());
    auto const* result = resp->find_member("result");
    REQUIRE(result != nullptr);
    auto const* data = result->get_array("data");
    REQUIRE(data != nullptr);
    CHECK(!data->as_array().empty());
}

TEST_CASE("virtual dccv documents answer hover and completion")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"dccv:Zm9v/main.dc"};
    auto text = std::string{"module main;\npublic i32 added(i32 a, i32 b) { return a + b; }\nvoid f() {\n    i32 y = ad|ded(1, 2);\n}\n"};
    auto [src, pos] = strip_marker(text, '|');
    std::ignore = open_virtual_file(server, sink, uri, src);

    auto hover = request_hover(server, sink, uri, pos.line, pos.character);
    REQUIRE(hover.has_value());
    CHECK(hover->find("added") != std::string::npos);

    auto items = request_completion_items(server, sink, uri, 3u, 12u);
    CHECK(has_label(items, "added"));
}

TEST_CASE("virtual dccv imports resolve among sibling virtual documents")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto lib_uri = std::string{"dccv:Zm9v/lib.dc"};
    std::ignore = open_virtual_file(server, sink, lib_uri, "module lib;\npublic i32 binary_search(i32 v) { return v; }\n");

    auto main_uri = std::string{"dccv:Zm9v/main.dc"};
    auto main_text = std::string{"module main;\nimport lib;\nvoid f() {\n    i32 y = lib::binary_search(2);\n}\n"};
    std::ignore = open_virtual_file(server, sink, main_uri, main_text);

    auto def = request_definition(server, sink, main_uri, 3u, 21u);
    REQUIRE(def.has_value());
    CHECK_EQ(def->uri, lib_uri);
    CHECK_EQ(def->range.start.line, 1u);
    CHECK_EQ(def->range.start.character, 11u);
    CHECK_EQ(def->range.end.line, 1u);
    CHECK_EQ(def->range.end.character, 24u);

    auto refs = request_references(server, sink, lib_uri, 1u, 12u, true);
    REQUIRE(refs.size() == 2);
    auto ranges = locations_to_ranges(refs);
    CHECK(has_range(ranges, 1u, 11u, 24u));
    CHECK(has_range(ranges, 3u, 17u, 30u));
    for (auto const& loc : refs)
        CHECK(loc.uri == lib_uri || loc.uri == main_uri);

    auto outcome = request_rename(server, sink, main_uri, 3u, 21u, "search");
    REQUIRE(outcome.edit.has_value());
    REQUIRE(!outcome.error.has_value());
    auto lit = outcome.edit->changes.find(lib_uri);
    REQUIRE(lit != outcome.edit->changes.end());
    CHECK(check_edit_set(lit->second, "search", {{1u, 11u, 24u}}));
    auto mit = outcome.edit->changes.find(main_uri);
    REQUIRE(mit != outcome.edit->changes.end());
    CHECK(check_edit_set(mit->second, "search", {{3u, 17u, 30u}}));
}

TEST_CASE("closed virtual dccv documents publish empty diagnostics and return null results")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"dccv:Zm9v/main.dc"};
    {
        auto publishes = send_and_collect_publishes(
            server, sink, make_did_open(uri, 1, "module main;\ni32 x = 0;\ni32 x = 1;\n"));
        REQUIRE(publishes.size() == 1);
        REQUIRE(publishes[0].diagnostics.size() == 1);
    }

    auto closes = send_and_collect_publishes(server, sink, make_did_close(uri));
    REQUIRE(closes.size() == 1);
    CHECK(closes[0].diagnostics.empty());

    auto changes = send_and_collect_publishes(server, sink, make_did_change(uri, 2, "module main;\ni32 x = 0;\n"));
    REQUIRE(changes.size() == 1);
    REQUIRE(changes[0].version.has_value());
    CHECK_EQ(*changes[0].version, 2);
    CHECK(changes[0].diagnostics.empty());

    auto def = request_definition(server, sink, uri, 1u, 4u);
    CHECK(!def.has_value());
}

TEST_CASE("dcc-core didOpen remains rejected and publishes nothing")
{
    Sink sink;
    dccd::LanguageServer server{&sink.stream};
    initialize_server(server, sink);

    auto uri = std::string{"dcc-core:/core.dc"};
    auto publishes = send_and_collect_publishes(server, sink, make_did_open(uri, 1, "module core;\n"));
    CHECK(publishes.empty());

    auto def = request_definition(server, sink, uri, 0u, 0u);
    CHECK(!def.has_value());
}
