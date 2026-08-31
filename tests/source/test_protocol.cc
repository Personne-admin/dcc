import std;
import dccd.protocol;

#include "harness.hh"

namespace
{
    using dccd::protocol::JsonValue;

    [[nodiscard]] std::optional<dccd::protocol::RpcInfo> parse_notification(std::string_view method, JsonValue params)
    {
        auto notif = dccd::protocol::build_notification(std::string{method}, std::move(params));
        auto serialized = notif.serialize();
        auto parsed = JsonValue::parse(serialized);
        if (!parsed)
            return std::nullopt;

        return dccd::protocol::parse_rpc(*parsed);
    }

} // namespace

SECTION("protocol: diagnostic relatedInformation");

TEST_CASE("LspDiagnostic serializes relatedInformation with exact URI/range/message")
{
    dccd::protocol::LspDiagnostic diag;
    diag.range.start.line = 4;
    diag.range.start.character = 7;
    diag.range.end.line = 4;
    diag.range.end.character = 8;
    diag.severity = dccd::protocol::DiagnosticSeverity::Error;
    diag.source = "dcc";
    diag.message = "redefinition of type `A`";

    dccd::protocol::DiagnosticRelatedInformation ri;
    ri.location.uri = "file:///main.dc";
    ri.location.range.start.line = 3;
    ri.location.range.start.character = 7;
    ri.location.range.end.line = 3;
    ri.location.range.end.character = 8;
    ri.message = "previous definition was here";
    diag.relatedInformation = std::vector<dccd::protocol::DiagnosticRelatedInformation>{ri};

    auto obj = diag.to_json();
    auto serialized = obj.serialize();

    auto parsed = JsonValue::parse(serialized);
    REQUIRE(parsed.has_value());
    CHECK(parsed->has_member("range"));
    CHECK(parsed->has_member("relatedInformation"));

    auto const* ri_arr = parsed->get_array("relatedInformation");
    REQUIRE(ri_arr != nullptr);
    REQUIRE(ri_arr->array_size() == 1);
    auto const& first = ri_arr->as_array()[0];
    auto const* loc = first.get_object("location");
    REQUIRE(loc != nullptr);
    auto uri = loc->get_string("uri");
    REQUIRE(uri.has_value());
    CHECK_EQ(*uri, "file:///main.dc");

    auto const* r = loc->find_member("range");
    REQUIRE(r != nullptr);
    auto const* s = r->find_member("start");
    REQUIRE(s != nullptr);
    auto line = s->get_integer("line");
    auto ch = s->get_integer("character");
    REQUIRE(line.has_value());
    REQUIRE(ch.has_value());
    CHECK_EQ(*line, 3);
    CHECK_EQ(*ch, 7);

    auto msg = first.get_string("message");
    REQUIRE(msg.has_value());
    CHECK_EQ(*msg, "previous definition was here");

    auto back = dccd::protocol::LspDiagnostic::from_json(*parsed);
    REQUIRE(back.relatedInformation.has_value());
    REQUIRE(back.relatedInformation->size() == 1);
    CHECK_EQ(back.relatedInformation->at(0).message, "previous definition was here");
    CHECK_EQ(back.relatedInformation->at(0).location.uri, "file:///main.dc");
    CHECK_EQ(back.relatedInformation->at(0).location.range.start.line, 3u);
    CHECK_EQ(back.relatedInformation->at(0).location.range.start.character, 7u);
}

TEST_CASE("LspDiagnostic omits relatedInformation when absent or empty")
{
    dccd::protocol::LspDiagnostic diag;
    diag.range.start.line = 1;
    diag.range.start.character = 2;
    diag.range.end.line = 1;
    diag.range.end.character = 3;
    diag.message = "no related info";

    auto serialized = diag.to_json().serialize();
    CHECK(serialized.find("relatedInformation") == std::string::npos);

    diag.relatedInformation = std::vector<dccd::protocol::DiagnosticRelatedInformation>{};
    serialized = diag.to_json().serialize();
    CHECK(serialized.find("relatedInformation") == std::string::npos);

    auto parsed = JsonValue::parse(serialized);
    REQUIRE(parsed.has_value());
    auto back = dccd::protocol::LspDiagnostic::from_json(*parsed);
    CHECK(!back.relatedInformation.has_value());
}

TEST_CASE("relatedInformation is parsed from incoming diagnostics")
{
    auto diag_json = JsonValue::parse(
        R"({"range":{"start":{"line":2,"character":0},"end":{"line":2,"character":1}},"message":"err","severity":1,)"
        R"("relatedInformation":[{"location":{"uri":"file:///x.dc","range":{"start":{"line":0,"character":4},"end":{"line":0,"character":5}}},"message":"prev"}]})");
    REQUIRE(diag_json.has_value());

    auto diag = dccd::protocol::LspDiagnostic::from_json(*diag_json);
    REQUIRE(diag.relatedInformation.has_value());
    REQUIRE(diag.relatedInformation->size() == 1);
    auto const& ri = diag.relatedInformation->at(0);
    CHECK_EQ(ri.location.uri, "file:///x.dc");
    CHECK_EQ(ri.message, "prev");
    CHECK_EQ(ri.location.range.start.line, 0u);
    CHECK_EQ(ri.location.range.start.character, 4u);
    CHECK_EQ(ri.location.range.end.character, 5u);
}

SECTION("protocol: completion item extensions");

TEST_CASE("CompletionItem round-trips textEdit, insertText, insertTextFormat, sortText, preselect and command")
{
    dccd::protocol::CompletionItem item;
    item.label = "compute";
    item.kind = dccd::protocol::CompletionItemKind::Function;
    item.detail = "i32 compute(i32 a, i32 b)";
    item.documentation = "Computes the answer.";
    item.sortText = "0compute";
    item.insertText = "compute(${1}, ${2})";
    item.insertTextFormat = dccd::protocol::InsertTextFormat::Snippet;
    item.preselect = true;

    dccd::protocol::Command cmd;
    cmd.title = "trigger signature help";
    cmd.command = std::string{dccd::protocol::kTriggerParameterHintsCommand};
    item.command = cmd;

    dccd::protocol::TextEdit edit;
    edit.range.start.line = 3;
    edit.range.start.character = 12;
    edit.range.end.line = 3;
    edit.range.end.character = 16;
    edit.newText = "compute(${1}, ${2})";
    item.textEdit = edit;

    auto obj = item.to_json();
    auto serialized = obj.serialize();
    auto parsed = JsonValue::parse(serialized);
    REQUIRE(parsed.has_value());

    auto label = parsed->get_string("label");
    REQUIRE(label.has_value());
    CHECK_EQ(*label, "compute");

    auto kind = parsed->get_integer("kind");
    REQUIRE(kind.has_value());
    CHECK_EQ(*kind, static_cast<std::int64_t>(dccd::protocol::CompletionItemKind::Function));

    auto sort = parsed->get_string("sortText");
    REQUIRE(sort.has_value());
    CHECK_EQ(*sort, "0compute");

    auto insert = parsed->get_string("insertText");
    REQUIRE(insert.has_value());
    CHECK_EQ(*insert, "compute(${1}, ${2})");

    auto fmt = parsed->get_integer("insertTextFormat");
    REQUIRE(fmt.has_value());
    CHECK_EQ(*fmt, dccd::protocol::InsertTextFormat::Snippet);

    auto pre = parsed->get_bool("preselect");
    REQUIRE(pre.has_value());
    CHECK(*pre);

    auto const* cmd_val = parsed->find_member("command");
    REQUIRE(cmd_val != nullptr);
    auto cmd_name = cmd_val->get_string("command");
    REQUIRE(cmd_name.has_value());
    CHECK_EQ(*cmd_name, dccd::protocol::kTriggerParameterHintsCommand);

    auto const* te = parsed->find_member("textEdit");
    REQUIRE(te != nullptr);
    auto new_text = te->get_string("newText");
    REQUIRE(new_text.has_value());
    CHECK_EQ(*new_text, "compute(${1}, ${2})");
    auto const* r = te->find_member("range");
    REQUIRE(r != nullptr);
    auto const* s = r->find_member("start");
    REQUIRE(s != nullptr);
    auto start_char = s->get_integer("character");
    REQUIRE(start_char.has_value());
    CHECK_EQ(*start_char, 12);

    auto back = dccd::protocol::CompletionItem::from_json(*parsed);
    CHECK_EQ(back.label, "compute");
    CHECK(back.kind == dccd::protocol::CompletionItemKind::Function);
    REQUIRE(back.sortText.has_value());
    CHECK_EQ(*back.sortText, "0compute");
    REQUIRE(back.insertText.has_value());
    CHECK_EQ(*back.insertText, "compute(${1}, ${2})");
    REQUIRE(back.insertTextFormat.has_value());
    CHECK_EQ(*back.insertTextFormat, dccd::protocol::InsertTextFormat::Snippet);
    REQUIRE(back.preselect.has_value());
    CHECK(*back.preselect);
    REQUIRE(back.command.has_value());
    CHECK_EQ(back.command->command, dccd::protocol::kTriggerParameterHintsCommand);
    REQUIRE(back.textEdit.has_value());
    CHECK_EQ(back.textEdit->newText, "compute(${1}, ${2})");
    CHECK_EQ(back.textEdit->range.start.character, 12u);
    CHECK_EQ(back.textEdit->range.end.character, 16u);
}

TEST_CASE("CompletionItem omits optional fields when absent")
{
    dccd::protocol::CompletionItem item;
    item.label = "x";
    item.kind = dccd::protocol::CompletionItemKind::Variable;

    auto serialized = item.to_json().serialize();
    CHECK(serialized.find("sortText") == std::string::npos);
    CHECK(serialized.find("textEdit") == std::string::npos);
    CHECK(serialized.find("insertText") == std::string::npos);
    CHECK(serialized.find("insertTextFormat") == std::string::npos);
    CHECK(serialized.find("preselect") == std::string::npos);
    CHECK(serialized.find("command") == std::string::npos);

    auto parsed = JsonValue::parse(serialized);
    REQUIRE(parsed.has_value());
    auto back = dccd::protocol::CompletionItem::from_json(*parsed);
    CHECK_EQ(back.label, "x");
    CHECK(!back.sortText.has_value());
    CHECK(!back.textEdit.has_value());
    CHECK(!back.insertText.has_value());
    CHECK(!back.insertTextFormat.has_value());
    CHECK(!back.preselect.has_value());
    CHECK(!back.command.has_value());
}

TEST_CASE("TextEdit round-trips range and newText")
{
    dccd::protocol::TextEdit edit;
    edit.range.start.line = 1;
    edit.range.start.character = 2;
    edit.range.end.line = 1;
    edit.range.end.character = 6;
    edit.newText = "foo(${1})";

    auto parsed = JsonValue::parse(edit.to_json().serialize());
    REQUIRE(parsed.has_value());
    auto back = dccd::protocol::TextEdit::from_json(*parsed);
    CHECK_EQ(back.newText, "foo(${1})");
    CHECK_EQ(back.range.start.line, 1u);
    CHECK_EQ(back.range.start.character, 2u);
    CHECK_EQ(back.range.end.character, 6u);
}

TEST_CASE("SignatureHelp serializes signatures, activeSignature and activeParameter")
{
    dccd::protocol::SignatureHelp help;
    help.activeSignature = 1;
    help.activeParameter = 2;

    dccd::protocol::SignatureInformation sig0;
    sig0.label = "i32 pick(i32 a)";
    dccd::protocol::ParameterInformation p0;
    p0.label = "i32 a";
    sig0.parameters.push_back(p0);
    sig0.activeParameter = 0;

    dccd::protocol::SignatureInformation sig1;
    sig1.label = "i32 pick(i32 a, i32 b)";
    dccd::protocol::ParameterInformation p1a;
    p1a.label = "i32 a";
    dccd::protocol::ParameterInformation p1b;
    p1b.label = "i32 b";
    sig1.parameters.push_back(p1a);
    sig1.parameters.push_back(p1b);
    sig1.activeParameter = 2;

    help.signatures.push_back(sig0);
    help.signatures.push_back(sig1);

    auto parsed = JsonValue::parse(help.to_json().serialize());
    REQUIRE(parsed.has_value());
    auto active_sig = parsed->get_integer("activeSignature");
    REQUIRE(active_sig.has_value());
    CHECK_EQ(*active_sig, 1);
    auto active_param = parsed->get_integer("activeParameter");
    REQUIRE(active_param.has_value());
    CHECK_EQ(*active_param, 2);

    auto const* sigs = parsed->get_array("signatures");
    REQUIRE(sigs != nullptr);
    REQUIRE(sigs->array_size() == 2);
    auto const& sig1_json = sigs->as_array()[1];
    auto label = sig1_json.get_string("label");
    REQUIRE(label.has_value());
    CHECK_EQ(*label, "i32 pick(i32 a, i32 b)");
    auto ap = sig1_json.get_integer("activeParameter");
    REQUIRE(ap.has_value());
    CHECK_EQ(*ap, 2);
    auto const* params = sig1_json.get_array("parameters");
    REQUIRE(params != nullptr);
    REQUIRE(params->array_size() == 2);
}

SECTION("protocol: publishDiagnostics version");

TEST_CASE("PublishDiagnosticsParams carries an optional version")
{
    dccd::protocol::PublishDiagnosticsParams params;
    params.uri = "file:///main.dc";
    params.version = 7;
    params.diagnostics.clear();

    auto serialized = params.to_json().serialize();
    auto parsed = JsonValue::parse(serialized);
    REQUIRE(parsed.has_value());
    auto version = parsed->get_integer("version");
    REQUIRE(version.has_value());
    CHECK_EQ(*version, 7);

    auto back = dccd::protocol::PublishDiagnosticsParams::from_json(*parsed);
    REQUIRE(back.version.has_value());
    CHECK_EQ(*back.version, 7);
    CHECK_EQ(back.uri, "file:///main.dc");
}

TEST_CASE("PublishDiagnosticsParams omits version when absent")
{
    dccd::protocol::PublishDiagnosticsParams params;
    params.uri = "file:///main.dc";

    auto serialized = params.to_json().serialize();
    CHECK(serialized.find("\"version\"") == std::string::npos);

    auto parsed = JsonValue::parse(serialized);
    REQUIRE(parsed.has_value());
    auto back = dccd::protocol::PublishDiagnosticsParams::from_json(*parsed);
    CHECK(!back.version.has_value());
}

TEST_CASE("empty publishDiagnostics payload serializes and round-trips")
{
    dccd::protocol::PublishDiagnosticsParams params;
    params.uri = "file:///main.dc";
    params.version = 3;

    auto notif = dccd::protocol::build_notification("textDocument/publishDiagnostics", params.to_json());
    auto rpc = parse_notification("textDocument/publishDiagnostics", params.to_json());
    REQUIRE(rpc.has_value());
    REQUIRE(rpc->params.has_value());

    auto back = dccd::protocol::PublishDiagnosticsParams::from_json(*rpc->params);
    REQUIRE(back.version.has_value());
    CHECK_EQ(*back.version, 3);
    CHECK(back.diagnostics.empty());
}

SECTION("protocol: prepareRename");

TEST_CASE("initialize advertises renameProvider with prepareProvider")
{
    auto result = dccd::protocol::make_initialize_result();
    auto const* caps = result.get_object("capabilities");
    REQUIRE(caps != nullptr);

    auto const* rename = caps->get_object("renameProvider");
    REQUIRE(rename != nullptr);
    auto prep = rename->get_bool("prepareProvider");
    REQUIRE(prep.has_value());
    CHECK(*prep);
}

TEST_CASE("initialize advertises codeActionProvider with the quickfix kind explicitly")
{
    auto result = dccd::protocol::make_initialize_result();
    auto const* caps = result.get_object("capabilities");
    REQUIRE(caps != nullptr);

    auto const* code_action = caps->get_object("codeActionProvider");
    REQUIRE(code_action != nullptr);

    auto const* kinds = code_action->get_array("codeActionKinds");
    REQUIRE(kinds != nullptr);
    REQUIRE(kinds->array_size() == 1);
    auto const& kind_json = kinds->as_array()[0];
    REQUIRE(kind_json.is_string());
    CHECK_EQ(kind_json.as_string(), "quickfix");

    auto parsed = JsonValue::parse(result.serialize());
    REQUIRE(parsed.has_value());
    auto const* parsed_caps = parsed->get_object("capabilities");
    REQUIRE(parsed_caps != nullptr);
    auto const* parsed_ca = parsed_caps->get_object("codeActionProvider");
    REQUIRE(parsed_ca != nullptr);
    auto const* parsed_kinds = parsed_ca->get_array("codeActionKinds");
    REQUIRE(parsed_kinds != nullptr);
    REQUIRE(parsed_kinds->array_size() == 1);
    auto const& parsed_kind_json = parsed_kinds->as_array()[0];
    REQUIRE(parsed_kind_json.is_string());
    CHECK_EQ(parsed_kind_json.as_string(), "quickfix");
}

TEST_CASE("initialize advertises inlayHintProvider as options with resolveProvider false")
{
    auto result = dccd::protocol::make_initialize_result();
    auto const* caps = result.get_object("capabilities");
    REQUIRE(caps != nullptr);

    auto const* ihp = caps->get_object("inlayHintProvider");
    REQUIRE(ihp != nullptr);
    auto resolve = ihp->get_bool("resolveProvider");
    REQUIRE(resolve.has_value());
    CHECK(!*resolve);

    CHECK(!ihp->has_member("typeHints"));
    CHECK(!ihp->has_member("parameterHints"));
    CHECK(!ihp->has_member("suppressParameterNameMatches"));

    auto parsed = JsonValue::parse(result.serialize());
    REQUIRE(parsed.has_value());
    auto const* parsed_caps = parsed->get_object("capabilities");
    REQUIRE(parsed_caps != nullptr);
    auto const* parsed_ihp = parsed_caps->get_object("inlayHintProvider");
    REQUIRE(parsed_ihp != nullptr);
    auto parsed_resolve = parsed_ihp->get_bool("resolveProvider");
    REQUIRE(parsed_resolve.has_value());
    CHECK(!*parsed_resolve);
}

TEST_CASE("InlayHintOptions defaults keep every hint class enabled and serialize only resolveProvider")
{
    dccd::protocol::InlayHintOptions opts;
    CHECK(opts.typeHints);
    CHECK(opts.parameterHints);
    CHECK(opts.suppressParameterNameMatches);

    auto json = opts.to_json();
    auto resolve = json.get_bool("resolveProvider");
    REQUIRE(resolve.has_value());
    CHECK(!*resolve);
    CHECK(!json.has_member("typeHints"));
    CHECK(!json.has_member("parameterHints"));
    CHECK(!json.has_member("suppressParameterNameMatches"));
}

TEST_CASE("InlayHint serializes position, label, kind and padding")
{
    dccd::protocol::InlayHint h;
    h.position.line = 3;
    h.position.character = 7;
    h.label = "value:";
    h.kind = dccd::protocol::InlayHintKind::Parameter;
    h.paddingRight = true;

    auto json = h.to_json();
    auto const* pos = json.find_member("position");
    REQUIRE(pos != nullptr);
    auto line = pos->get_integer("line");
    auto ch = pos->get_integer("character");
    REQUIRE(line.has_value());
    REQUIRE(ch.has_value());
    CHECK_EQ(*line, 3);
    CHECK_EQ(*ch, 7);
    auto label = json.get_string("label");
    REQUIRE(label.has_value());
    CHECK_EQ(*label, "value:");
    auto kind = json.get_integer("kind");
    REQUIRE(kind.has_value());
    CHECK_EQ(*kind, dccd::protocol::InlayHintKind::Parameter);
    auto pad = json.get_bool("paddingRight");
    REQUIRE(pad.has_value());
    CHECK(*pad);

    dccd::protocol::InlayHint t;
    t.position.line = 0;
    t.position.character = 4;
    t.label = ": i32";
    t.kind = dccd::protocol::InlayHintKind::Type;
    auto tjson = t.to_json();
    auto tkind = tjson.get_integer("kind");
    REQUIRE(tkind.has_value());
    CHECK_EQ(*tkind, dccd::protocol::InlayHintKind::Type);
    CHECK(!tjson.has_member("paddingLeft"));
    CHECK(!tjson.has_member("paddingRight"));
}

TEST_CASE("PrepareRenameResult serializes range and placeholder")
{
    dccd::protocol::PrepareRenameResult result;
    result.range.start.line = 4;
    result.range.start.character = 12;
    result.range.end.line = 4;
    result.range.end.character = 19;
    result.placeholder = "counter";

    auto parsed = JsonValue::parse(result.to_json().serialize());
    REQUIRE(parsed.has_value());
    auto const* r = parsed->find_member("range");
    REQUIRE(r != nullptr);
    auto const* s = r->find_member("start");
    REQUIRE(s != nullptr);
    auto line = s->get_integer("line");
    auto ch = s->get_integer("character");
    REQUIRE(line.has_value());
    REQUIRE(ch.has_value());
    CHECK_EQ(*line, 4);
    CHECK_EQ(*ch, 12);

    auto placeholder = parsed->get_string("placeholder");
    REQUIRE(placeholder.has_value());
    CHECK_EQ(*placeholder, "counter");
}

TEST_CASE("PrepareRenameParams parses textDocument and position")
{
    auto json = JsonValue::parse(R"({"textDocument":{"uri":"file:///main.dc"},"position":{"line":7,"character":13}})");
    REQUIRE(json.has_value());
    auto params = dccd::protocol::PrepareRenameParams::from_json(*json);
    CHECK_EQ(params.textDocument.uri, "file:///main.dc");
    CHECK_EQ(params.position.line, 7u);
    CHECK_EQ(params.position.character, 13u);
}

SECTION("protocol: cancellation ($/cancelRequest)");

TEST_CASE("RequestCancelled error code is -32800 and the method name is $/cancelRequest")
{
    CHECK_EQ(dccd::protocol::kErrorRequestCancelled, -32800);
    CHECK_EQ(std::string{dccd::protocol::kCancelRequestMethod}, "$/cancelRequest");
}

TEST_CASE("RequestId distinguishes numeric and string ids")
{
    auto num = dccd::protocol::RequestId::from_json(JsonValue::integer(1));
    auto str = dccd::protocol::RequestId::from_json(JsonValue::string_val("1"));

    REQUIRE(num.valid());
    REQUIRE(str.valid());
    CHECK(num.is_number());
    CHECK(str.is_string());
    CHECK_EQ(num.number(), 1);
    CHECK_EQ(str.string_val(), "1");

    CHECK(num != str);
    CHECK(num == dccd::protocol::RequestId::from_json(JsonValue::integer(1)));
    CHECK(str == dccd::protocol::RequestId::from_json(JsonValue::string_val("1")));

    auto invalid = dccd::protocol::RequestId::from_json(JsonValue::null_val());
    CHECK(!invalid.valid());
    CHECK(invalid != num);
    CHECK(invalid != str);
}

TEST_CASE("RequestId round-trips through JSON preserving the id type")
{
    auto num = dccd::protocol::RequestId::from_json(JsonValue::integer(7));
    auto parsed_num_json = JsonValue::parse(num.to_json().serialize());
    REQUIRE(parsed_num_json.has_value());
    auto parsed_num = dccd::protocol::RequestId::from_json(*parsed_num_json);
    CHECK(parsed_num.is_number());
    CHECK_EQ(parsed_num.number(), 7);

    auto str = dccd::protocol::RequestId::from_json(JsonValue::string_val("7"));
    auto parsed_str_json = JsonValue::parse(str.to_json().serialize());
    REQUIRE(parsed_str_json.has_value());
    auto parsed_str = dccd::protocol::RequestId::from_json(*parsed_str_json);
    CHECK(parsed_str.is_string());
    CHECK_EQ(parsed_str.string_val(), "7");

    CHECK(parsed_num != parsed_str);
}

TEST_CASE("CancelParams parses numeric and string ids")
{
    auto num_json = JsonValue::parse(R"({"id": 42})");
    REQUIRE(num_json.has_value());
    auto num = dccd::protocol::CancelParams::from_json(*num_json);
    CHECK(num.id.is_number());
    CHECK_EQ(num.id.number(), 42);

    auto str_json = JsonValue::parse(R"({"id": "abc"})");
    REQUIRE(str_json.has_value());
    auto str = dccd::protocol::CancelParams::from_json(*str_json);
    CHECK(str.id.is_string());
    CHECK_EQ(str.id.string_val(), "abc");

    auto missing_json = JsonValue::parse(R"({})");
    REQUIRE(missing_json.has_value());
    auto missing = dccd::protocol::CancelParams::from_json(*missing_json);
    CHECK(!missing.id.valid());
}

TEST_CASE("$/cancelRequest notification round-trips through build/parse RPC")
{
    auto params = JsonValue::empty_object();
    params.set("id", JsonValue::string_val("req-1"));
    auto rpc = parse_notification(std::string{dccd::protocol::kCancelRequestMethod}, params);
    REQUIRE(rpc.has_value());
    CHECK(rpc->is_notification());
    REQUIRE(rpc->method.has_value());
    CHECK_EQ(*rpc->method, dccd::protocol::kCancelRequestMethod);
    REQUIRE(rpc->params.has_value());

    auto cancel = dccd::protocol::CancelParams::from_json(*rpc->params);
    CHECK(cancel.id.is_string());
    CHECK_EQ(cancel.id.string_val(), "req-1");
}

TEST_CASE("RequestCancelled error response has the exact shape with the target id")
{
    auto id = JsonValue::integer(99);
    auto resp = dccd::protocol::build_error_response(id, dccd::protocol::kErrorRequestCancelled, "Request cancelled");
    auto parsed = JsonValue::parse(resp.serialize());
    REQUIRE(parsed.has_value());

    auto jsonrpc = parsed->get_string("jsonrpc");
    REQUIRE(jsonrpc.has_value());
    CHECK_EQ(*jsonrpc, "2.0");

    auto const* err = parsed->get_object("error");
    REQUIRE(err != nullptr);
    auto code = err->get_integer("code");
    REQUIRE(code.has_value());
    CHECK_EQ(*code, -32800);
    auto message = err->get_string("message");
    REQUIRE(message.has_value());
    CHECK_EQ(*message, "Request cancelled");

    auto const* resp_id = parsed->find_member("id");
    REQUIRE(resp_id != nullptr);
    REQUIRE(resp_id->is_number());
    CHECK_EQ(resp_id->as_integer(), 99);
    CHECK(parsed->find_member("result") == nullptr);
}

TEST_CASE("RequestCancelled error response preserves a string target id")
{
    auto id = JsonValue::string_val("target-1");
    auto resp = dccd::protocol::build_error_response(id, dccd::protocol::kErrorRequestCancelled, "Request cancelled");
    auto parsed = JsonValue::parse(resp.serialize());
    REQUIRE(parsed.has_value());

    auto const* resp_id = parsed->find_member("id");
    REQUIRE(resp_id != nullptr);
    REQUIRE(resp_id->is_string());
    CHECK_EQ(resp_id->as_string(), "target-1");
}

SECTION("protocol: position encoding negotiation");

TEST_CASE("LspPosition rejects negative line/character without unsigned wrap")
{
    auto json = JsonValue::parse(R"({"line":-1,"character":-5})");
    REQUIRE(json.has_value());
    auto pos = dccd::protocol::LspPosition::from_json(*json);
    CHECK_EQ(pos.line, 0u);
    CHECK_EQ(pos.character, 0u);

    json = JsonValue::parse(R"({"line":3,"character":-2})");
    REQUIRE(json.has_value());
    pos = dccd::protocol::LspPosition::from_json(*json);
    CHECK_EQ(pos.line, 3u);
    CHECK_EQ(pos.character, 0u);

    json = JsonValue::parse(R"({"line":-4,"character":7})");
    REQUIRE(json.has_value());
    pos = dccd::protocol::LspPosition::from_json(*json);
    CHECK_EQ(pos.line, 0u);
    CHECK_EQ(pos.character, 7u);

    json = JsonValue::parse(R"({"line":2,"character":9})");
    REQUIRE(json.has_value());
    pos = dccd::protocol::LspPosition::from_json(*json);
    CHECK_EQ(pos.line, 2u);
    CHECK_EQ(pos.character, 9u);
}

TEST_CASE("LspRange rejects negative positions in start and end")
{
    auto json = JsonValue::parse(R"({"start":{"line":0,"character":-1},"end":{"line":1,"character":2}} )");
    REQUIRE(json.has_value());
    auto range = dccd::protocol::LspRange::from_json(*json);
    CHECK_EQ(range.start.character, 0u);
    CHECK_EQ(range.end.line, 1u);
    CHECK_EQ(range.end.character, 2u);
}

TEST_CASE("InitializeParams parses capabilities.general.positionEncodings in order")
{
    auto json = JsonValue::parse(R"({"rootUri":"file:///ws","capabilities":{"general":{"positionEncodings":["utf-8","utf-32"]}}})");
    REQUIRE(json.has_value());
    auto params = dccd::protocol::InitializeParams::from_json(*json);
    REQUIRE(params.positionEncodings.size() == 2u);
    CHECK_EQ(params.positionEncodings[0], "utf-8");
    CHECK_EQ(params.positionEncodings[1], "utf-32");

    json = JsonValue::parse(R"({"capabilities":{"general":{"positionEncodings":["utf-16"]}}})");
    REQUIRE(json.has_value());
    params = dccd::protocol::InitializeParams::from_json(*json);
    REQUIRE(params.positionEncodings.size() == 1u);
    CHECK_EQ(params.positionEncodings[0], "utf-16");

    json = JsonValue::parse(R"({"capabilities":{"general":{"positionEncodings":["utf-8",7,"utf-16"]}}})");
    REQUIRE(json.has_value());
    params = dccd::protocol::InitializeParams::from_json(*json);
    REQUIRE(params.positionEncodings.size() == 2u);
    CHECK_EQ(params.positionEncodings[0], "utf-8");
    CHECK_EQ(params.positionEncodings[1], "utf-16");
}

TEST_CASE("InitializeParams without capabilities yields no position encodings")
{
    auto json = JsonValue::parse(R"({"rootUri":"file:///ws"})");
    REQUIRE(json.has_value());
    auto params = dccd::protocol::InitializeParams::from_json(*json);
    CHECK(params.positionEncodings.empty());

    json = JsonValue::parse(R"({})");
    REQUIRE(json.has_value());
    params = dccd::protocol::InitializeParams::from_json(*json);
    CHECK(params.positionEncodings.empty());
}

TEST_CASE("make_initialize_result defaults to utf-16 and honors an explicit encoding")
{
    auto result = dccd::protocol::make_initialize_result();
    auto const* caps = result.get_object("capabilities");
    REQUIRE(caps != nullptr);
    auto enc = caps->get_string("positionEncoding");
    REQUIRE(enc.has_value());
    CHECK_EQ(*enc, "utf-16");

    auto parsed = JsonValue::parse(result.serialize());
    REQUIRE(parsed.has_value());
    auto const* parsed_caps = parsed->get_object("capabilities");
    REQUIRE(parsed_caps != nullptr);
    auto parsed_enc = parsed_caps->get_string("positionEncoding");
    REQUIRE(parsed_enc.has_value());
    CHECK_EQ(*parsed_enc, "utf-16");

    result = dccd::protocol::make_initialize_result("utf-8");
    caps = result.get_object("capabilities");
    REQUIRE(caps != nullptr);
    enc = caps->get_string("positionEncoding");
    REQUIRE(enc.has_value());
    CHECK_EQ(*enc, "utf-8");

    result = dccd::protocol::make_initialize_result("utf-32");
    caps = result.get_object("capabilities");
    REQUIRE(caps != nullptr);
    enc = caps->get_string("positionEncoding");
    REQUIRE(enc.has_value());
    CHECK_EQ(*enc, "utf-32");
}

SECTION("protocol: watched-files registration");

TEST_CASE("initialize result does not advertise a static didChangeWatchedFiles capability")
{
    auto result = dccd::protocol::make_initialize_result();
    auto const* caps = result.get_object("capabilities");
    REQUIRE(caps != nullptr);

    CHECK(caps->find_member("workspace") == nullptr);
}

TEST_CASE("InitializeParams parses capabilities.workspace.didChangeWatchedFiles.dynamicRegistration")
{
    auto json = JsonValue::parse(R"({"capabilities":{"workspace":{"didChangeWatchedFiles":{"dynamicRegistration":true}}}})");
    REQUIRE(json.has_value());
    auto params = dccd::protocol::InitializeParams::from_json(*json);
    CHECK(params.didChangeWatchedFilesDynamicRegistration);

    json = JsonValue::parse(R"({"capabilities":{"workspace":{"didChangeWatchedFiles":{"dynamicRegistration":false}}}})");
    REQUIRE(json.has_value());
    params = dccd::protocol::InitializeParams::from_json(*json);
    CHECK(!params.didChangeWatchedFilesDynamicRegistration);

    json = JsonValue::parse(R"({"capabilities":{"workspace":{}}})");
    REQUIRE(json.has_value());
    params = dccd::protocol::InitializeParams::from_json(*json);
    CHECK(!params.didChangeWatchedFilesDynamicRegistration);

    json = JsonValue::parse(R"({})");
    REQUIRE(json.has_value());
    params = dccd::protocol::InitializeParams::from_json(*json);
    CHECK(!params.didChangeWatchedFilesDynamicRegistration);

    json = JsonValue::parse(R"({"capabilities":{"workspace":{"didChangeWatchedFiles":{"dynamicRegistration":"yes"}}}})");
    REQUIRE(json.has_value());
    params = dccd::protocol::InitializeParams::from_json(*json);
    CHECK(!params.didChangeWatchedFilesDynamicRegistration);
}

TEST_CASE("build_register_capability_request emits the exact LSP RegistrationParams shape")
{
    auto req = dccd::protocol::build_register_capability_request();

    auto jsonrpc = req.get_string("jsonrpc");
    REQUIRE(jsonrpc.has_value());
    CHECK_EQ(*jsonrpc, "2.0");

    auto method = req.get_string("method");
    REQUIRE(method.has_value());
    CHECK_EQ(*method, dccd::protocol::kClientRegisterCapabilityMethod);

    auto const* id = req.find_member("id");
    REQUIRE(id != nullptr);
    REQUIRE(id->is_string());
    CHECK_EQ(id->as_string(), std::string{dccd::protocol::kWatchedFilesRegistrationRequestId});

    auto const* params = req.get_object("params");
    REQUIRE(params != nullptr);
    auto const* registrations = params->get_array("registrations");
    REQUIRE(registrations != nullptr);
    REQUIRE(registrations->array_size() == 1);

    auto const& reg = registrations->as_array()[0];
    auto reg_id = reg.get_string("id");
    REQUIRE(reg_id.has_value());
    CHECK_EQ(*reg_id, std::string{dccd::protocol::kWatchedFilesRegistrationId});
    auto reg_method = reg.get_string("method");
    REQUIRE(reg_method.has_value());
    CHECK_EQ(*reg_method, dccd::protocol::kWatchedFilesMethod);

    auto const* reg_opts = reg.get_object("registerOptions");
    REQUIRE(reg_opts != nullptr);
    auto const* watchers = reg_opts->get_array("watchers");
    REQUIRE(watchers != nullptr);
    REQUIRE(watchers->array_size() == 3);

    auto w0 = watchers->as_array()[0].get_string("globPattern");
    REQUIRE(w0.has_value());
    CHECK_EQ(*w0, "**/*.dc");
    auto w1 = watchers->as_array()[1].get_string("globPattern");
    REQUIRE(w1.has_value());
    CHECK_EQ(*w1, "**/dcc.json");
    auto w2 = watchers->as_array()[2].get_string("globPattern");
    REQUIRE(w2.has_value());
    CHECK_EQ(*w2, "**/compile_commands.json");

    auto parsed = JsonValue::parse(req.serialize());
    REQUIRE(parsed.has_value());
    auto parsed_method = parsed->get_string("method");
    REQUIRE(parsed_method.has_value());
    CHECK_EQ(*parsed_method, dccd::protocol::kClientRegisterCapabilityMethod);
    auto const* parsed_id = parsed->find_member("id");
    REQUIRE(parsed_id != nullptr);
    REQUIRE(parsed_id->is_string());
    CHECK_EQ(parsed_id->as_string(), std::string{dccd::protocol::kWatchedFilesRegistrationRequestId});
    auto const* parsed_params = parsed->get_object("params");
    REQUIRE(parsed_params != nullptr);
    auto const* parsed_regs = parsed_params->get_array("registrations");
    REQUIRE(parsed_regs != nullptr);
    REQUIRE(parsed_regs->array_size() == 1);
    auto const& parsed_reg = parsed_regs->as_array()[0];
    auto parsed_reg_method = parsed_reg.get_string("method");
    REQUIRE(parsed_reg_method.has_value());
    CHECK_EQ(*parsed_reg_method, dccd::protocol::kWatchedFilesMethod);
}

TEST_CASE("registerCapability request parses as a JSON-RPC request carrying the stable string id")
{
    auto req = dccd::protocol::build_register_capability_request();
    auto parsed = dccd::protocol::parse_rpc(req);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->is_request());
    REQUIRE(parsed->id.has_value());
    REQUIRE(parsed->id->is_string());
    CHECK_EQ(parsed->id->as_string(), std::string{dccd::protocol::kWatchedFilesRegistrationRequestId});
    REQUIRE(parsed->method.has_value());
    CHECK_EQ(*parsed->method, dccd::protocol::kClientRegisterCapabilityMethod);
    REQUIRE(parsed->params.has_value());
}

TEST_CASE("a client JSON-RPC response to registerCapability parses as a response without a method")
{
    auto res = dccd::protocol::build_response(JsonValue::string_val(std::string{dccd::protocol::kWatchedFilesRegistrationRequestId}), JsonValue::null_val());
    auto parsed = dccd::protocol::parse_rpc(res);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->is_response());
    REQUIRE(!parsed->is_request());
    REQUIRE(!parsed->is_notification());
    REQUIRE(parsed->id.has_value());
    REQUIRE(parsed->id->is_string());
    CHECK_EQ(parsed->id->as_string(), std::string{dccd::protocol::kWatchedFilesRegistrationRequestId});
    REQUIRE(parsed->result.has_value());
    CHECK(!parsed->method.has_value());
}

SECTION("protocol: range and on-type formatting");

TEST_CASE("FormattingOptions parses trimTrailingWhitespace, insertFinalNewline and trimFinalNewlines")
{
    auto json = JsonValue::parse(R"({"tabSize":2,"insertSpaces":false,"trimTrailingWhitespace":true,"insertFinalNewline":false,"trimFinalNewlines":true})");
    REQUIRE(json.has_value());
    auto opts = dccd::protocol::FormattingOptions::from_json(*json);
    CHECK_EQ(opts.tabSize, 2u);
    CHECK(!opts.insertSpaces);
    REQUIRE(opts.trimTrailingWhitespace.has_value());
    CHECK(*opts.trimTrailingWhitespace);
    REQUIRE(opts.insertFinalNewline.has_value());
    CHECK(!*opts.insertFinalNewline);
    REQUIRE(opts.trimFinalNewlines.has_value());
    CHECK(*opts.trimFinalNewlines);

    CHECK(!opts.insert_final_newline());
    CHECK(opts.trim_trailing_whitespace());
    CHECK(opts.trim_final_newlines());
}

TEST_CASE("FormattingOptions defaults preserve the existing whole-document output policy")
{
    auto json = JsonValue::parse(R"({"tabSize":4,"insertSpaces":true})");
    REQUIRE(json.has_value());
    auto opts = dccd::protocol::FormattingOptions::from_json(*json);
    CHECK_EQ(opts.tabSize, 4u);
    CHECK(opts.insertSpaces);
    CHECK(!opts.trimTrailingWhitespace.has_value());
    CHECK(!opts.insertFinalNewline.has_value());
    CHECK(!opts.trimFinalNewlines.has_value());

    CHECK(opts.insert_final_newline());
    CHECK(!opts.trim_trailing_whitespace());
    CHECK(!opts.trim_final_newlines());
}

TEST_CASE("FormattingOptions rejects non-bool trim members and out-of-range tab sizes")
{
    auto json = JsonValue::parse(R"({"tabSize":0,"insertSpaces":true,"trimTrailingWhitespace":"yes","insertFinalNewline":1,"trimFinalNewlines":null})");
    REQUIRE(json.has_value());
    auto opts = dccd::protocol::FormattingOptions::from_json(*json);
    CHECK_EQ(opts.tabSize, 4u);
    CHECK(!opts.trimTrailingWhitespace.has_value());
    CHECK(!opts.insertFinalNewline.has_value());
    CHECK(!opts.trimFinalNewlines.has_value());
}

TEST_CASE("DocumentRangeFormattingParams parses textDocument, range and options")
{
    auto json = JsonValue::parse(R"({"textDocument":{"uri":"file:///main.dc"},"range":{"start":{"line":2,"character":4},"end":{"line":2,"character":9}},)"
                                 R"("options":{"tabSize":2,"insertSpaces":true,"trimTrailingWhitespace":true}})");
    REQUIRE(json.has_value());
    auto params = dccd::protocol::DocumentRangeFormattingParams::from_json(*json);
    CHECK_EQ(params.textDocument.uri, "file:///main.dc");
    CHECK_EQ(params.range.start.line, 2u);
    CHECK_EQ(params.range.start.character, 4u);
    CHECK_EQ(params.range.end.line, 2u);
    CHECK_EQ(params.range.end.character, 9u);
    CHECK_EQ(params.options.tabSize, 2u);
    REQUIRE(params.options.trimTrailingWhitespace.has_value());
    CHECK(*params.options.trimTrailingWhitespace);

    json = JsonValue::parse(R"({"textDocument":{"uri":"file:///main.dc"}})");
    REQUIRE(json.has_value());
    params = dccd::protocol::DocumentRangeFormattingParams::from_json(*json);
    CHECK_EQ(params.textDocument.uri, "file:///main.dc");
    CHECK_EQ(params.range.start.line, 0u);
    CHECK_EQ(params.options.tabSize, 4u);
}

TEST_CASE("DocumentOnTypeFormattingParams parses textDocument, position, ch and options")
{
    auto json = JsonValue::parse(R"({"textDocument":{"uri":"file:///main.dc"},"position":{"line":3,"character":2},"ch":"}",)"
                                 R"("options":{"tabSize":2,"insertSpaces":false}})");
    REQUIRE(json.has_value());
    auto params = dccd::protocol::DocumentOnTypeFormattingParams::from_json(*json);
    CHECK_EQ(params.textDocument.uri, "file:///main.dc");
    CHECK_EQ(params.position.line, 3u);
    CHECK_EQ(params.position.character, 2u);
    CHECK_EQ(params.ch, "}");
    CHECK_EQ(params.options.tabSize, 2u);
    CHECK(!params.options.insertSpaces);

    json = JsonValue::parse(R"({"textDocument":{"uri":"file:///main.dc"},"position":{"line":0,"character":0}})");
    REQUIRE(json.has_value());
    params = dccd::protocol::DocumentOnTypeFormattingParams::from_json(*json);
    CHECK(params.ch.empty());
}

TEST_CASE("initialize advertises range and on-type formatting capabilities")
{
    auto result = dccd::protocol::make_initialize_result();
    auto const* caps = result.get_object("capabilities");
    REQUIRE(caps != nullptr);

    auto range_provider = caps->get_bool("documentRangeFormattingProvider");
    REQUIRE(range_provider.has_value());
    CHECK(*range_provider);

    auto const* on_type = caps->get_object("documentOnTypeFormattingProvider");
    REQUIRE(on_type != nullptr);
    auto first = on_type->get_string("firstTriggerCharacter");
    REQUIRE(first.has_value());
    CHECK_EQ(*first, "}");
    auto const* more = on_type->get_array("moreTriggerCharacter");
    REQUIRE(more != nullptr);
    REQUIRE(more->array_size() == 1);
    auto const& more_json = more->as_array()[0];
    REQUIRE(more_json.is_string());
    CHECK_EQ(more_json.as_string(), ";");

    auto parsed = JsonValue::parse(result.serialize());
    REQUIRE(parsed.has_value());
    auto const* parsed_caps = parsed->get_object("capabilities");
    REQUIRE(parsed_caps != nullptr);
    auto parsed_range = parsed_caps->get_bool("documentRangeFormattingProvider");
    REQUIRE(parsed_range.has_value());
    CHECK(*parsed_range);
    auto const* parsed_on_type = parsed_caps->get_object("documentOnTypeFormattingProvider");
    REQUIRE(parsed_on_type != nullptr);
    auto parsed_first = parsed_on_type->get_string("firstTriggerCharacter");
    REQUIRE(parsed_first.has_value());
    CHECK_EQ(*parsed_first, "}");
    auto const* parsed_more = parsed_on_type->get_array("moreTriggerCharacter");
    REQUIRE(parsed_more != nullptr);
    REQUIRE(parsed_more->array_size() == 1);
    REQUIRE(parsed_more->as_array()[0].is_string());
    CHECK_EQ(parsed_more->as_array()[0].as_string(), ";");
}
