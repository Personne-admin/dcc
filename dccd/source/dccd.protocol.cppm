export module dccd.protocol;

import std;

export namespace dccd::protocol
{
    enum class JsonType : std::uint8_t
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    class JsonValue
    {
    public:
        JsonValue() : m_type{JsonType::Null} {}

        static JsonValue null_val() { return JsonValue{}; }

        static JsonValue boolean(bool v)
        {
            JsonValue jv;
            jv.m_type = JsonType::Bool;
            jv.m_bool = v;
            return jv;
        }

        static JsonValue number(double v)
        {
            JsonValue jv;
            jv.m_type = JsonType::Number;
            jv.m_number = v;
            return jv;
        }

        static JsonValue integer(std::int64_t v)
        {
            JsonValue jv;
            jv.m_type = JsonType::Number;
            jv.m_number = static_cast<double>(v);
            jv.m_is_integer = true;
            return jv;
        }

        static JsonValue string_val(std::string v)
        {
            JsonValue jv;
            jv.m_type = JsonType::String;
            jv.m_string = std::move(v);
            return jv;
        }

        static JsonValue array_val(std::vector<JsonValue> v)
        {
            JsonValue jv;
            jv.m_type = JsonType::Array;
            jv.m_array = std::move(v);
            return jv;
        }

        static JsonValue object_val(std::map<std::string, JsonValue, std::less<>> v)
        {
            JsonValue jv;
            jv.m_type = JsonType::Object;
            jv.m_object = std::move(v);
            return jv;
        }

        static JsonValue empty_object() { return object_val({}); }

        static JsonValue empty_array() { return array_val({}); }

        [[nodiscard]] JsonType type() const noexcept { return m_type; }

        [[nodiscard]] bool is_null() const noexcept { return m_type == JsonType::Null; }

        [[nodiscard]] bool is_bool() const noexcept { return m_type == JsonType::Bool; }

        [[nodiscard]] bool is_number() const noexcept { return m_type == JsonType::Number; }

        [[nodiscard]] bool is_string() const noexcept { return m_type == JsonType::String; }

        [[nodiscard]] bool is_array() const noexcept { return m_type == JsonType::Array; }

        [[nodiscard]] bool is_object() const noexcept { return m_type == JsonType::Object; }

        [[nodiscard]] bool as_bool() const noexcept { return m_bool; }

        [[nodiscard]] std::int64_t as_integer() const noexcept { return static_cast<std::int64_t>(m_number); }

        [[nodiscard]] double as_double() const noexcept { return m_number; }

        [[nodiscard]] std::string const& as_string() const { return m_string; }

        [[nodiscard]] std::vector<JsonValue> const& as_array() const { return m_array; }

        [[nodiscard]] std::map<std::string, JsonValue, std::less<>> const& as_object() const { return m_object; }

        [[nodiscard]] JsonValue const* find_member(std::string_view key) const noexcept
        {
            if (m_type != JsonType::Object)
                return nullptr;

            auto it = m_object.find(key);
            if (it == m_object.end())
                return nullptr;

            return &it->second;
        }

        [[nodiscard]] bool has_member(std::string_view key) const noexcept { return find_member(key) != nullptr; }

        [[nodiscard]] std::optional<std::string> get_string(std::string_view key) const
        {
            auto const* v = find_member(key);
            if (v && v->m_type == JsonType::String)
                return v->m_string;

            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::int64_t> get_integer(std::string_view key) const
        {
            auto const* v = find_member(key);
            if (v && v->m_type == JsonType::Number)
                return static_cast<std::int64_t>(v->m_number);

            return std::nullopt;
        }

        [[nodiscard]] std::optional<double> get_double(std::string_view key) const
        {
            auto const* v = find_member(key);
            if (v && v->m_type == JsonType::Number)
                return v->m_number;

            return std::nullopt;
        }

        [[nodiscard]] std::optional<bool> get_bool(std::string_view key) const
        {
            auto const* v = find_member(key);
            if (v && v->m_type == JsonType::Bool)
                return v->m_bool;

            return std::nullopt;
        }

        [[nodiscard]] JsonValue const* get_object(std::string_view key) const
        {
            auto const* v = find_member(key);
            if (v && v->m_type == JsonType::Object)
                return v;

            return nullptr;
        }

        [[nodiscard]] JsonValue const* get_array(std::string_view key) const
        {
            auto const* v = find_member(key);
            if (v && v->m_type == JsonType::Array)
                return v;

            return nullptr;
        }

        void set(std::string key, JsonValue val) { m_object[std::move(key)] = std::move(val); }

        void push_back(JsonValue val) { m_array.push_back(std::move(val)); }

        [[nodiscard]] std::size_t array_size() const noexcept { return m_type == JsonType::Array ? m_array.size() : 0; }

        [[nodiscard]] std::string serialize() const;

        [[nodiscard]] static std::optional<JsonValue> parse(std::string_view input);

    private:
        friend class JsonParserImpl;

        JsonType m_type{JsonType::Null};
        bool m_bool{false};
        double m_number{0.0};
        bool m_is_integer{false};
        std::string m_string;
        std::vector<JsonValue> m_array;
        std::map<std::string, JsonValue, std::less<>> m_object;

        static void serialize_string_content(std::string& out, std::string_view s);
        static void serialize_value(std::string& out, JsonValue const& v);
    };

    struct RpcInfo
    {
        std::string jsonrpc;
        std::optional<JsonValue> id;
        std::optional<std::string> method;
        std::optional<JsonValue> params;
        std::optional<JsonValue> result;
        std::optional<JsonValue> error;

        [[nodiscard]] bool is_request() const noexcept { return id.has_value() && method.has_value(); }

        [[nodiscard]] bool is_notification() const noexcept { return !id.has_value() && method.has_value(); }

        [[nodiscard]] bool is_response() const noexcept { return id.has_value() && !method.has_value(); }
    };

    [[nodiscard]] std::optional<RpcInfo> parse_rpc(JsonValue const& v)
    {
        if (!v.is_object())
            return std::nullopt;

        auto const* jsonrpc_val = v.find_member("jsonrpc");
        if (!jsonrpc_val || !jsonrpc_val->is_string() || jsonrpc_val->as_string() != "2.0")
            return std::nullopt;

        RpcInfo info;
        info.jsonrpc = jsonrpc_val->as_string();

        if (auto const* id_val = v.find_member("id"))
        {
            if (id_val->is_null() || id_val->is_string() || id_val->is_number())
                info.id = *id_val;
            else
                return std::nullopt;
        }

        if (auto const* method_val = v.find_member("method"))
        {
            if (!method_val->is_string())
                return std::nullopt;

            info.method = method_val->as_string();
        }

        if (auto const* params_val = v.find_member("params"))
            info.params = *params_val;

        if (auto const* result_val = v.find_member("result"))
            info.result = *result_val;

        if (auto const* error_val = v.find_member("error"))
            info.error = *error_val;

        if (!info.method.has_value() && !info.result.has_value() && !info.error.has_value())
            return std::nullopt;

        return info;
    }

    [[nodiscard]] JsonValue build_request(JsonValue id, std::string method, JsonValue params)
    {
        auto req = JsonValue::empty_object();
        req.set("jsonrpc", JsonValue::string_val("2.0"));
        req.set("method", JsonValue::string_val(std::move(method)));
        req.set("params", std::move(params));
        req.set("id", std::move(id));
        return req;
    }

    [[nodiscard]] JsonValue build_response(JsonValue id, JsonValue result)
    {
        auto res = JsonValue::empty_object();
        res.set("jsonrpc", JsonValue::string_val("2.0"));
        res.set("result", std::move(result));
        res.set("id", std::move(id));
        return res;
    }

    [[nodiscard]] JsonValue build_error_response(JsonValue id, int code, std::string message, std::optional<JsonValue> data = std::nullopt)
    {
        auto err = JsonValue::empty_object();
        err.set("code", JsonValue::integer(code));
        err.set("message", JsonValue::string_val(std::move(message)));
        if (data)
            err.set("data", std::move(*data));

        auto res = JsonValue::empty_object();
        res.set("jsonrpc", JsonValue::string_val("2.0"));
        res.set("error", std::move(err));
        res.set("id", std::move(id));
        return res;
    }

    [[nodiscard]] JsonValue build_notification(std::string method, JsonValue params)
    {
        auto notif = JsonValue::empty_object();
        notif.set("jsonrpc", JsonValue::string_val("2.0"));
        notif.set("method", JsonValue::string_val(std::move(method)));
        notif.set("params", std::move(params));
        return notif;
    }

    constexpr std::int32_t kErrorRequestCancelled = -32800;
    constexpr std::string_view kCancelRequestMethod = "$/cancelRequest";

    class RequestId
    {
    public:
        RequestId() = default;

        [[nodiscard]] static RequestId from_json(JsonValue const& v)
        {
            RequestId id;
            if (v.is_number())
                id.m_number = v.as_integer();
            else if (v.is_string())
                id.m_string = v.as_string();
            return id;
        }

        [[nodiscard]] bool valid() const noexcept { return m_number.has_value() || m_string.has_value(); }

        [[nodiscard]] bool is_number() const noexcept { return m_number.has_value(); }

        [[nodiscard]] bool is_string() const noexcept { return m_string.has_value(); }

        [[nodiscard]] std::int64_t number() const { return *m_number; }

        [[nodiscard]] std::string const& string_val() const { return *m_string; }

        [[nodiscard]] JsonValue to_json() const
        {
            if (m_number)
                return JsonValue::integer(*m_number);
            if (m_string)
                return JsonValue::string_val(*m_string);
            return JsonValue::null_val();
        }

        [[nodiscard]] bool operator==(RequestId const&) const = default;

        [[nodiscard]] std::strong_ordering operator<=>(RequestId const&) const = default;

    private:
        std::optional<std::int64_t> m_number;
        std::optional<std::string> m_string;
    };

    struct CancelParams
    {
        RequestId id;

        [[nodiscard]] static CancelParams from_json(JsonValue const& v)
        {
            CancelParams p;
            if (auto const* id_val = v.find_member("id"))
                p.id = RequestId::from_json(*id_val);

            return p;
        }
    };

    struct LspPosition
    {
        std::uint32_t line{};
        std::uint32_t character{};

        [[nodiscard]] static LspPosition from_json(JsonValue const& v)
        {
            LspPosition p;

            if (auto l = v.get_integer("line"))
                if (*l >= 0)
                    p.line = static_cast<std::uint32_t>(*l);
            if (auto c = v.get_integer("character"))
                if (*c >= 0)
                    p.character = static_cast<std::uint32_t>(*c);

            return p;
        }

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("line", JsonValue::integer(static_cast<std::int64_t>(line)));
            obj.set("character", JsonValue::integer(static_cast<std::int64_t>(character)));
            return obj;
        }
    };

    struct LspRange
    {
        LspPosition start;
        LspPosition end;

        [[nodiscard]] static LspRange from_json(JsonValue const& v)
        {
            LspRange r;
            if (auto const* s = v.find_member("start"))
                r.start = LspPosition::from_json(*s);
            if (auto const* e = v.find_member("end"))
                r.end = LspPosition::from_json(*e);

            return r;
        }

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("start", start.to_json());
            obj.set("end", end.to_json());
            return obj;
        }
    };

    struct LspLocation
    {
        std::string uri;
        LspRange range;

        [[nodiscard]] static LspLocation from_json(JsonValue const& v)
        {
            LspLocation loc;
            if (auto s = v.get_string("uri"))
                loc.uri = std::move(*s);
            if (auto const* r = v.find_member("range"))
                loc.range = LspRange::from_json(*r);
            return loc;
        }

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("uri", JsonValue::string_val(uri));
            obj.set("range", range.to_json());
            return obj;
        }
    };

    struct DiagnosticRelatedInformation
    {
        LspLocation location;
        std::string message;

        [[nodiscard]] static DiagnosticRelatedInformation from_json(JsonValue const& v)
        {
            DiagnosticRelatedInformation ri;
            if (auto const* loc = v.find_member("location"))
                ri.location = LspLocation::from_json(*loc);
            if (auto s = v.get_string("message"))
                ri.message = std::move(*s);
            return ri;
        }

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("location", location.to_json());
            obj.set("message", JsonValue::string_val(message));
            return obj;
        }
    };

    enum class CompletionItemKind : std::int32_t
    {
        Text = 1,
        Method = 2,
        Function = 3,
        Constructor = 4,
        Field = 5,
        Variable = 6,
        Class = 7,
        Interface = 8,
        Module = 9,
        Property = 10,
        Unit = 11,
        Value = 12,
        Enum = 13,
        Keyword = 14,
        Snippet = 15,
        Color = 16,
        File = 17,
        Reference = 18,
        Folder = 19,
        EnumMember = 20,
        Constant = 21,
        Struct = 22,
        Event = 23,
        Operator = 24,
        TypeParameter = 25,
    };

    struct TextEdit
    {
        LspRange range;
        std::string newText;

        [[nodiscard]] static TextEdit from_json(JsonValue const& v)
        {
            TextEdit edit;
            if (auto const* r = v.find_member("range"))
                edit.range = LspRange::from_json(*r);
            if (auto s = v.get_string("newText"))
                edit.newText = std::move(*s);
            return edit;
        }

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("range", range.to_json());
            obj.set("newText", JsonValue::string_val(newText));
            return obj;
        }
    };

    namespace InsertTextFormat
    {
        constexpr std::int32_t PlainText = 1;
        constexpr std::int32_t Snippet = 2;

    } // namespace InsertTextFormat

    struct Command
    {
        std::string title;
        std::string command;
        std::optional<JsonValue> arguments;

        [[nodiscard]] static Command from_json(JsonValue const& v)
        {
            Command cmd;
            if (auto s = v.get_string("title"))
                cmd.title = std::move(*s);
            if (auto s = v.get_string("command"))
                cmd.command = std::move(*s);
            if (auto const* args = v.find_member("arguments"))
                cmd.arguments = *args;
            return cmd;
        }

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("title", JsonValue::string_val(title));
            obj.set("command", JsonValue::string_val(command));
            if (arguments)
                obj.set("arguments", *arguments);
            return obj;
        }
    };

    constexpr std::string_view kTriggerParameterHintsCommand = "editor.action.triggerParameterHints";

    struct CompletionItem
    {
        std::string label;
        CompletionItemKind kind{CompletionItemKind::Text};
        std::optional<std::string> detail;
        std::optional<std::string> documentation;
        std::optional<std::string> sortText;
        std::optional<TextEdit> textEdit;
        std::optional<std::string> insertText;
        std::optional<std::int32_t> insertTextFormat;
        std::optional<bool> preselect;
        std::optional<Command> command;

        [[nodiscard]] static CompletionItem from_json(JsonValue const& v)
        {
            CompletionItem item;
            if (auto s = v.get_string("label"))
                item.label = std::move(*s);
            if (auto k = v.get_integer("kind"))
                item.kind = static_cast<CompletionItemKind>(*k);
            if (auto s = v.get_string("detail"))
                item.detail = std::move(*s);
            if (auto s = v.get_string("documentation"))
                item.documentation = std::move(*s);
            if (auto s = v.get_string("sortText"))
                item.sortText = std::move(*s);
            if (auto const* te = v.find_member("textEdit"))
                item.textEdit = TextEdit::from_json(*te);
            if (auto s = v.get_string("insertText"))
                item.insertText = std::move(*s);
            if (auto n = v.get_integer("insertTextFormat"))
                item.insertTextFormat = static_cast<std::int32_t>(*n);
            if (auto b = v.get_bool("preselect"))
                item.preselect = *b;
            if (auto const* cmd = v.find_member("command"))
                item.command = Command::from_json(*cmd);
            return item;
        }

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("label", JsonValue::string_val(label));
            obj.set("kind", JsonValue::integer(static_cast<std::int64_t>(kind)));
            if (detail)
                obj.set("detail", JsonValue::string_val(*detail));
            if (documentation)
                obj.set("documentation", JsonValue::string_val(*documentation));
            if (sortText)
                obj.set("sortText", JsonValue::string_val(*sortText));
            if (textEdit)
                obj.set("textEdit", textEdit->to_json());
            if (insertText)
                obj.set("insertText", JsonValue::string_val(*insertText));
            if (insertTextFormat)
                obj.set("insertTextFormat", JsonValue::integer(*insertTextFormat));
            if (preselect)
                obj.set("preselect", JsonValue::boolean(*preselect));
            if (command)
                obj.set("command", command->to_json());
            return obj;
        }
    };

    struct CompletionList
    {
        bool isIncomplete{false};
        std::vector<CompletionItem> items;

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("isIncomplete", JsonValue::boolean(isIncomplete));
            auto arr = JsonValue::empty_array();
            for (auto const& item : items)
                arr.push_back(item.to_json());
            obj.set("items", std::move(arr));
            return obj;
        }
    };

    enum class DiagnosticSeverity : std::int32_t
    {
        Error = 1,
        Warning = 2,
        Information = 3,
        Hint = 4,
    };

    struct LspDiagnostic
    {
        LspRange range;
        std::optional<DiagnosticSeverity> severity;
        std::optional<std::string> code;
        std::optional<std::string> source;
        std::string message;
        std::optional<std::vector<DiagnosticRelatedInformation>> relatedInformation;

        [[nodiscard]] static LspDiagnostic from_json(JsonValue const& v)
        {
            LspDiagnostic d;
            if (auto const* r = v.find_member("range"))
                d.range = LspRange::from_json(*r);
            if (auto s = v.get_string("message"))
                d.message = std::move(*s);
            if (auto s = v.get_integer("severity"))
                d.severity = static_cast<DiagnosticSeverity>(*s);
            if (auto s = v.get_string("code"))
                d.code = std::move(*s);
            if (auto s = v.get_string("source"))
                d.source = std::move(*s);
            if (auto const* ri = v.get_array("relatedInformation"))
            {
                std::vector<DiagnosticRelatedInformation> related;
                for (auto const& item : ri->as_array())
                    related.push_back(DiagnosticRelatedInformation::from_json(item));
                if (!related.empty())
                    d.relatedInformation = std::move(related);
            }
            return d;
        }

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("range", range.to_json());
            obj.set("message", JsonValue::string_val(message));
            if (severity)
                obj.set("severity", JsonValue::integer(static_cast<std::int64_t>(*severity)));

            if (code)
                obj.set("code", JsonValue::string_val(*code));

            if (source)
                obj.set("source", JsonValue::string_val(*source));

            if (relatedInformation && !relatedInformation->empty())
            {
                auto arr = JsonValue::empty_array();
                for (auto const& ri : *relatedInformation)
                    arr.push_back(ri.to_json());
                obj.set("relatedInformation", std::move(arr));
            }

            return obj;
        }
    };

    struct PublishDiagnosticsParams
    {
        std::string uri;
        std::optional<std::int64_t> version;
        std::vector<LspDiagnostic> diagnostics;

        [[nodiscard]] static PublishDiagnosticsParams from_json(JsonValue const& v)
        {
            PublishDiagnosticsParams p;
            if (auto s = v.get_string("uri"))
                p.uri = std::move(*s);
            if (auto n = v.get_integer("version"))
                p.version = *n;
            if (auto const* arr = v.get_array("diagnostics"))
                for (auto const& d : arr->as_array())
                    p.diagnostics.push_back(LspDiagnostic::from_json(d));
            return p;
        }

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("uri", JsonValue::string_val(uri));

            if (version)
                obj.set("version", JsonValue::integer(*version));

            auto arr = JsonValue::empty_array();
            for (auto const& d : diagnostics)
                arr.push_back(d.to_json());

            obj.set("diagnostics", std::move(arr));
            return obj;
        }
    };

    struct TextDocumentItem
    {
        std::string uri;
        std::string languageId;
        std::int64_t version{};
        std::string text;

        [[nodiscard]] static TextDocumentItem from_json(JsonValue const& v)
        {
            TextDocumentItem item;
            if (auto s = v.get_string("uri"))
                item.uri = std::move(*s);

            if (auto s = v.get_string("languageId"))
                item.languageId = std::move(*s);

            if (auto n = v.get_integer("version"))
                item.version = *n;

            if (auto s = v.get_string("text"))
                item.text = std::move(*s);

            return item;
        }
    };

    struct DidOpenTextDocumentParams
    {
        TextDocumentItem textDocument;

        [[nodiscard]] static DidOpenTextDocumentParams from_json(JsonValue const& v)
        {
            DidOpenTextDocumentParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentItem::from_json(*td);

            return p;
        }
    };

    struct VersionedTextDocumentIdentifier
    {
        std::string uri;
        std::int64_t version{};

        [[nodiscard]] static VersionedTextDocumentIdentifier from_json(JsonValue const& v)
        {
            VersionedTextDocumentIdentifier id;
            if (auto s = v.get_string("uri"))
                id.uri = std::move(*s);

            if (auto n = v.get_integer("version"))
                id.version = *n;

            return id;
        }
    };

    struct TextDocumentContentChangeEvent
    {
        std::string text;

        [[nodiscard]] static TextDocumentContentChangeEvent from_json(JsonValue const& v)
        {
            TextDocumentContentChangeEvent ev;
            if (auto s = v.get_string("text"))
                ev.text = std::move(*s);

            return ev;
        }
    };

    struct DidChangeTextDocumentParams
    {
        VersionedTextDocumentIdentifier textDocument;
        std::vector<TextDocumentContentChangeEvent> contentChanges;

        [[nodiscard]] static DidChangeTextDocumentParams from_json(JsonValue const& v)
        {
            DidChangeTextDocumentParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = VersionedTextDocumentIdentifier::from_json(*td);

            if (auto const* cc = v.find_member("contentChanges"))
                if (cc->is_array())
                    for (auto const& ev : cc->as_array())
                        p.contentChanges.push_back(TextDocumentContentChangeEvent::from_json(ev));

            return p;
        }
    };

    struct TextDocumentIdentifier
    {
        std::string uri;

        [[nodiscard]] static TextDocumentIdentifier from_json(JsonValue const& v)
        {
            TextDocumentIdentifier id;
            if (auto s = v.get_string("uri"))
                id.uri = std::move(*s);

            return id;
        }
    };

    struct CompletionParams
    {
        TextDocumentIdentifier textDocument;
        LspPosition position;

        [[nodiscard]] static CompletionParams from_json(JsonValue const& v)
        {
            CompletionParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);
            if (auto const* pos = v.find_member("position"))
                p.position = LspPosition::from_json(*pos);
            return p;
        }
    };

    struct DidCloseTextDocumentParams
    {
        std::string uri;

        [[nodiscard]] static DidCloseTextDocumentParams from_json(JsonValue const& v)
        {
            DidCloseTextDocumentParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.uri = TextDocumentIdentifier::from_json(*td).uri;

            return p;
        }
    };

    struct SemanticTokensParams
    {
        TextDocumentIdentifier textDocument;

        [[nodiscard]] static SemanticTokensParams from_json(JsonValue const& v)
        {
            SemanticTokensParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);

            return p;
        }
    };

    struct SemanticTokens
    {
        std::vector<std::uint32_t> data;

        [[nodiscard]] JsonValue to_json() const
        {
            auto arr = JsonValue::empty_array();
            for (auto v : data)
                arr.push_back(JsonValue::integer(static_cast<std::int64_t>(v)));

            auto obj = JsonValue::empty_object();
            obj.set("data", std::move(arr));
            return obj;
        }
    };

    constexpr std::array token_types = {
        "namespace", "type",   "class", "enum",    "interface", "struct",  "typeParameter", "parameter", "variable", "property",       "enumMember",
        "function",  "method", "macro", "keyword", "modifier",  "comment", "string",        "number",    "operator", "asmPlaceholder", "asmRegister",
    };

    constexpr std::array token_modifiers = {
        "declaration", "readonly", "static", "deprecated", "defaultLibrary",
    };

    [[nodiscard]] JsonValue make_semantic_tokens_legend()
    {
        auto legend = JsonValue::empty_object();

        auto tt_arr = JsonValue::empty_array();
        for (auto const& s : token_types)
            tt_arr.push_back(JsonValue::string_val(std::string{s}));
        legend.set("tokenTypes", std::move(tt_arr));

        auto tm_arr = JsonValue::empty_array();
        for (auto const& s : token_modifiers)
            tm_arr.push_back(JsonValue::string_val(std::string{s}));
        legend.set("tokenModifiers", std::move(tm_arr));

        return legend;
    }

    struct TextDocumentPositionParams
    {
        TextDocumentIdentifier textDocument;
        LspPosition position;

        [[nodiscard]] static TextDocumentPositionParams from_json(JsonValue const& v)
        {
            TextDocumentPositionParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);
            if (auto const* pos = v.find_member("position"))
                p.position = LspPosition::from_json(*pos);

            return p;
        }
    };

    struct HoverParams
    {
        TextDocumentIdentifier textDocument;
        LspPosition position;

        [[nodiscard]] static HoverParams from_json(JsonValue const& v)
        {
            HoverParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);
            if (auto const* pos = v.find_member("position"))
                p.position = LspPosition::from_json(*pos);

            return p;
        }
    };

    struct SignatureHelpParams
    {
        TextDocumentIdentifier textDocument;
        LspPosition position;

        [[nodiscard]] static SignatureHelpParams from_json(JsonValue const& v)
        {
            SignatureHelpParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);
            if (auto const* pos = v.find_member("position"))
                p.position = LspPosition::from_json(*pos);
            return p;
        }
    };

    struct DefinitionParams
    {
        TextDocumentIdentifier textDocument;
        LspPosition position;

        [[nodiscard]] static DefinitionParams from_json(JsonValue const& v)
        {
            DefinitionParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);
            if (auto const* pos = v.find_member("position"))
                p.position = LspPosition::from_json(*pos);

            return p;
        }
    };

    struct MarkupContent
    {
        std::string kind{"markdown"};
        std::string value;

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("kind", JsonValue::string_val(kind));
            obj.set("value", JsonValue::string_val(value));
            return obj;
        }
    };

    struct Hover
    {
        MarkupContent contents;
        std::optional<LspRange> range;

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("contents", contents.to_json());
            if (range)
                obj.set("range", range->to_json());
            return obj;
        }
    };

    struct ParameterInformation
    {
        std::string label;
        std::optional<std::string> documentation;

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("label", JsonValue::string_val(label));
            if (documentation)
                obj.set("documentation", JsonValue::string_val(*documentation));
            return obj;
        }
    };

    struct SignatureInformation
    {
        std::string label;
        std::vector<ParameterInformation> parameters;
        std::optional<std::string> documentation;
        std::optional<std::uint32_t> activeParameter;

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("label", JsonValue::string_val(label));
            if (documentation)
                obj.set("documentation", JsonValue::string_val(*documentation));
            auto params_arr = JsonValue::empty_array();
            for (auto const& p : parameters)
                params_arr.push_back(p.to_json());
            obj.set("parameters", std::move(params_arr));
            if (activeParameter)
                obj.set("activeParameter", JsonValue::integer(static_cast<std::int64_t>(*activeParameter)));
            return obj;
        }
    };

    struct SignatureHelp
    {
        std::vector<SignatureInformation> signatures;
        std::uint32_t activeSignature{0};
        std::uint32_t activeParameter{0};

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            auto sigs_arr = JsonValue::empty_array();
            for (auto const& s : signatures)
                sigs_arr.push_back(s.to_json());
            obj.set("signatures", std::move(sigs_arr));
            obj.set("activeSignature", JsonValue::integer(static_cast<std::int64_t>(activeSignature)));
            obj.set("activeParameter", JsonValue::integer(static_cast<std::int64_t>(activeParameter)));
            return obj;
        }
    };

    struct ReferenceContext
    {
        bool includeDeclaration{false};

        [[nodiscard]] static ReferenceContext from_json(JsonValue const& v)
        {
            ReferenceContext ctx;
            if (auto b = v.get_bool("includeDeclaration"))
                ctx.includeDeclaration = *b;
            return ctx;
        }
    };

    struct ReferenceParams
    {
        TextDocumentIdentifier textDocument;
        LspPosition position;
        ReferenceContext context;

        [[nodiscard]] static ReferenceParams from_json(JsonValue const& v)
        {
            ReferenceParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);
            if (auto const* pos = v.find_member("position"))
                p.position = LspPosition::from_json(*pos);
            if (auto const* ctx = v.find_member("context"))
                p.context = ReferenceContext::from_json(*ctx);
            return p;
        }
    };

    enum class DocumentHighlightKind : std::int32_t
    {
        Text = 1,
        Read = 2,
        Write = 3,
    };

    struct DocumentHighlight
    {
        LspRange range;
        std::optional<DocumentHighlightKind> kind;

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("range", range.to_json());
            if (kind)
                obj.set("kind", JsonValue::integer(static_cast<std::int64_t>(*kind)));

            return obj;
        }
    };

    struct FormattingOptions
    {
        std::uint32_t tabSize{4};
        bool insertSpaces{true};
        std::optional<bool> trimTrailingWhitespace;
        std::optional<bool> insertFinalNewline;
        std::optional<bool> trimFinalNewlines;

        [[nodiscard]] static FormattingOptions from_json(JsonValue const& v)
        {
            FormattingOptions opts;
            if (auto ts = v.get_integer("tabSize"))
                if (*ts > 0 && *ts <= 100)
                    opts.tabSize = static_cast<std::uint32_t>(*ts);

            if (auto is_opt = v.get_bool("insertSpaces"))
                opts.insertSpaces = *is_opt;

            if (auto b = v.get_bool("trimTrailingWhitespace"))
                opts.trimTrailingWhitespace = *b;

            if (auto b = v.get_bool("insertFinalNewline"))
                opts.insertFinalNewline = *b;

            if (auto b = v.get_bool("trimFinalNewlines"))
                opts.trimFinalNewlines = *b;

            return opts;
        }

        [[nodiscard]] bool insert_final_newline() const noexcept { return insertFinalNewline.value_or(true); }

        [[nodiscard]] bool trim_trailing_whitespace() const noexcept { return trimTrailingWhitespace.value_or(false); }

        [[nodiscard]] bool trim_final_newlines() const noexcept { return trimFinalNewlines.value_or(false); }
    };

    struct DocumentFormattingParams
    {
        TextDocumentIdentifier textDocument;
        FormattingOptions options;

        [[nodiscard]] static DocumentFormattingParams from_json(JsonValue const& v)
        {
            DocumentFormattingParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);

            if (auto const* opts = v.get_object("options"))
                p.options = FormattingOptions::from_json(*opts);

            return p;
        }
    };

    struct DocumentRangeFormattingParams
    {
        TextDocumentIdentifier textDocument;
        LspRange range;
        FormattingOptions options;

        [[nodiscard]] static DocumentRangeFormattingParams from_json(JsonValue const& v)
        {
            DocumentRangeFormattingParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);

            if (auto const* r = v.find_member("range"))
                p.range = LspRange::from_json(*r);

            if (auto const* opts = v.get_object("options"))
                p.options = FormattingOptions::from_json(*opts);

            return p;
        }
    };

    struct DocumentOnTypeFormattingParams
    {
        TextDocumentIdentifier textDocument;
        LspPosition position;
        std::string ch;
        FormattingOptions options;

        [[nodiscard]] static DocumentOnTypeFormattingParams from_json(JsonValue const& v)
        {
            DocumentOnTypeFormattingParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);

            if (auto const* pos = v.find_member("position"))
                p.position = LspPosition::from_json(*pos);

            if (auto s = v.get_string("ch"))
                p.ch = std::move(*s);

            if (auto const* opts = v.get_object("options"))
                p.options = FormattingOptions::from_json(*opts);

            return p;
        }
    };

    struct DocumentHighlightParams
    {
        TextDocumentIdentifier textDocument;
        LspPosition position;

        [[nodiscard]] static DocumentHighlightParams from_json(JsonValue const& v)
        {
            DocumentHighlightParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);

            if (auto const* pos = v.find_member("position"))
                p.position = LspPosition::from_json(*pos);

            return p;
        }
    };

    struct RenameParams
    {
        TextDocumentIdentifier textDocument;
        LspPosition position;
        std::string newName;

        [[nodiscard]] static RenameParams from_json(JsonValue const& v)
        {
            RenameParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);
            if (auto const* pos = v.find_member("position"))
                p.position = LspPosition::from_json(*pos);
            if (auto s = v.get_string("newName"))
                p.newName = std::move(*s);
            return p;
        }
    };

    struct PrepareRenameParams
    {
        TextDocumentIdentifier textDocument;
        LspPosition position;

        [[nodiscard]] static PrepareRenameParams from_json(JsonValue const& v)
        {
            PrepareRenameParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);
            if (auto const* pos = v.find_member("position"))
                p.position = LspPosition::from_json(*pos);
            return p;
        }
    };

    struct PrepareRenameResult
    {
        LspRange range;
        std::string placeholder;

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("range", range.to_json());
            obj.set("placeholder", JsonValue::string_val(placeholder));
            return obj;
        }
    };

    struct WorkspaceEdit
    {
        std::map<std::string, std::vector<TextEdit>, std::less<>> changes;

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            auto changes_obj = JsonValue::empty_object();
            for (auto const& [uri, edits] : changes)
            {
                auto arr = JsonValue::empty_array();
                for (auto const& edit : edits)
                    arr.push_back(edit.to_json());
                changes_obj.set(uri, std::move(arr));
            }
            obj.set("changes", std::move(changes_obj));
            return obj;
        }
    };

    struct CodeActionContext
    {
        std::vector<LspDiagnostic> diagnostics;

        [[nodiscard]] static CodeActionContext from_json(JsonValue const& v)
        {
            CodeActionContext ctx;
            if (auto const* arr = v.get_array("diagnostics"))
                for (auto const& d : arr->as_array())
                    ctx.diagnostics.push_back(LspDiagnostic::from_json(d));

            return ctx;
        }
    };

    struct CodeActionParams
    {
        TextDocumentIdentifier textDocument;
        LspRange range;
        CodeActionContext context;

        [[nodiscard]] static CodeActionParams from_json(JsonValue const& v)
        {
            CodeActionParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);
            if (auto const* r = v.find_member("range"))
                p.range = LspRange::from_json(*r);
            if (auto const* ctx = v.find_member("context"))
                p.context = CodeActionContext::from_json(*ctx);
            return p;
        }
    };

    constexpr std::string_view kCodeActionQuickFix = "quickfix";

    struct CodeAction
    {
        std::string title;
        std::string kind;
        std::vector<LspDiagnostic> diagnostics;
        WorkspaceEdit edit;

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("title", JsonValue::string_val(title));
            obj.set("kind", JsonValue::string_val(kind));
            if (!diagnostics.empty())
            {
                auto arr = JsonValue::empty_array();
                for (auto const& d : diagnostics)
                    arr.push_back(d.to_json());
                obj.set("diagnostics", std::move(arr));
            }
            obj.set("edit", edit.to_json());
            return obj;
        }
    };

    struct InlayHintParams
    {
        TextDocumentIdentifier textDocument;
        LspRange range;

        [[nodiscard]] static InlayHintParams from_json(JsonValue const& v)
        {
            InlayHintParams p;
            if (auto const* td = v.find_member("textDocument"))
                p.textDocument = TextDocumentIdentifier::from_json(*td);
            if (auto const* r = v.find_member("range"))
                p.range = LspRange::from_json(*r);
            return p;
        }
    };

    struct WorkspaceFolder
    {
        std::string uri;
        std::string name;

        [[nodiscard]] static WorkspaceFolder from_json(JsonValue const& v)
        {
            WorkspaceFolder wf;
            if (auto s = v.get_string("uri"))
                wf.uri = std::move(*s);
            if (auto s = v.get_string("name"))
                wf.name = std::move(*s);
            return wf;
        }
    };

    namespace PositionEncoding
    {
        constexpr std::string_view Utf8 = "utf-8";
        constexpr std::string_view Utf16 = "utf-16";
        constexpr std::string_view Utf32 = "utf-32";

    } // namespace PositionEncoding

    struct InitializeParams
    {
        std::optional<std::string> rootUri;
        std::optional<std::vector<WorkspaceFolder>> workspaceFolders;
        std::vector<std::string> positionEncodings;
        bool didChangeWatchedFilesDynamicRegistration{false};

        [[nodiscard]] static InitializeParams from_json(JsonValue const& v)
        {
            InitializeParams p;
            if (auto s = v.get_string("rootUri"))
                p.rootUri = std::move(*s);
            if (auto const* arr = v.get_array("workspaceFolders"))
            {
                std::vector<WorkspaceFolder> folders;
                for (auto const& f : arr->as_array())
                    folders.push_back(WorkspaceFolder::from_json(f));
                if (!folders.empty())
                    p.workspaceFolders = std::move(folders);
            }
            if (auto const* caps = v.get_object("capabilities"))
            {
                if (auto const* general = caps->get_object("general"))
                    if (auto const* encs = general->get_array("positionEncodings"))
                        for (auto const& e : encs->as_array())
                            if (e.is_string())
                                p.positionEncodings.push_back(e.as_string());

                if (auto const* workspace = caps->get_object("workspace"))
                    if (auto const* dwf = workspace->get_object("didChangeWatchedFiles"))
                        if (auto dyn = dwf->get_bool("dynamicRegistration"))
                            p.didChangeWatchedFilesDynamicRegistration = *dyn;
            }
            return p;
        }
    };

    struct WorkspaceSymbolParams
    {
        std::string query;

        [[nodiscard]] static WorkspaceSymbolParams from_json(JsonValue const& v)
        {
            WorkspaceSymbolParams p;
            if (auto s = v.get_string("query"))
                p.query = std::move(*s);
            return p;
        }
    };

    struct DidChangeConfigurationParams
    {
        std::optional<JsonValue> settings;

        [[nodiscard]] static DidChangeConfigurationParams from_json(JsonValue const& v)
        {
            DidChangeConfigurationParams p;
            if (auto const* s = v.find_member("settings"))
                p.settings = *s;
            return p;
        }
    };

    struct FileEvent
    {
        std::string uri;
        int type{};

        [[nodiscard]] static FileEvent from_json(JsonValue const& v)
        {
            FileEvent fe;
            if (auto s = v.get_string("uri"))
                fe.uri = std::move(*s);

            if (auto n = v.get_integer("type"))
                fe.type = static_cast<int>(*n);

            return fe;
        }
    };

    struct DidChangeWatchedFilesParams
    {
        std::vector<FileEvent> changes;

        [[nodiscard]] static DidChangeWatchedFilesParams from_json(JsonValue const& v)
        {
            DidChangeWatchedFilesParams p;
            if (auto const* arr = v.get_array("changes"))
                for (auto const& ev : arr->as_array())
                    p.changes.push_back(FileEvent::from_json(ev));
            return p;
        }
    };

    constexpr std::string_view kClientRegisterCapabilityMethod = "client/registerCapability";

    constexpr std::string_view kWatchedFilesMethod = "workspace/didChangeWatchedFiles";

    constexpr std::string_view kWatchedFilesRegistrationRequestId = "dccd-register-capability";
    constexpr std::string_view kWatchedFilesRegistrationId = "dccd-watched-files";

    [[nodiscard]] JsonValue build_register_capability_request()
    {
        auto watcher_dc = JsonValue::empty_object();
        watcher_dc.set("globPattern", JsonValue::string_val("**/*.dc"));
        auto watcher_json = JsonValue::empty_object();
        watcher_json.set("globPattern", JsonValue::string_val("**/dcc.json"));

        auto watchers = JsonValue::empty_array();
        watchers.push_back(std::move(watcher_dc));
        watchers.push_back(std::move(watcher_json));

        auto register_options = JsonValue::empty_object();
        register_options.set("watchers", std::move(watchers));

        auto registration = JsonValue::empty_object();
        registration.set("id", JsonValue::string_val(std::string{kWatchedFilesRegistrationId}));
        registration.set("method", JsonValue::string_val(std::string{kWatchedFilesMethod}));
        registration.set("registerOptions", std::move(register_options));

        auto registrations = JsonValue::empty_array();
        registrations.push_back(std::move(registration));

        auto params = JsonValue::empty_object();
        params.set("registrations", std::move(registrations));

        return build_request(JsonValue::string_val(std::string{kWatchedFilesRegistrationRequestId}), std::string{kClientRegisterCapabilityMethod},
                             std::move(params));
    }

    enum class SymbolKind : std::int32_t
    {
        File = 1,
        Module = 2,
        Namespace = 3,
        Package = 4,
        Class = 5,
        Method = 6,
        Property = 7,
        Field = 8,
        Constructor = 9,
        Enum = 10,
        Interface = 11,
        Function = 12,
        Variable = 13,
        Constant = 14,
        String = 15,
        Number = 16,
        Boolean = 17,
        Array = 18,
        Object = 19,
        Key = 20,
        Null = 21,
        EnumMember = 22,
        Struct = 23,
        Event = 24,
        Operator = 25,
        TypeParameter = 26,
    };

    struct SymbolInformation
    {
        std::string name;
        SymbolKind kind;
        LspLocation location;
        std::optional<std::string> containerName;

        [[nodiscard]] static SymbolInformation from_json(JsonValue const& v)
        {
            SymbolInformation s;
            if (auto n = v.get_string("name"))
                s.name = std::move(*n);
            if (auto k = v.get_integer("kind"))
                s.kind = static_cast<SymbolKind>(*k);
            if (auto const* loc = v.find_member("location"))
                s.location = LspLocation::from_json(*loc);
            if (auto const* c = v.find_member("containerName"))
                if (c->is_string())
                    s.containerName = c->as_string();
            return s;
        }

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("name", JsonValue::string_val(name));
            obj.set("kind", JsonValue::integer(static_cast<std::int64_t>(kind)));
            obj.set("location", location.to_json());
            if (containerName)
                obj.set("containerName", JsonValue::string_val(*containerName));
            return obj;
        }
    };

    namespace InlayHintKind
    {
        constexpr std::int32_t Type = 1;
        constexpr std::int32_t Parameter = 2;

    } // namespace InlayHintKind

    struct InlayHint
    {
        LspPosition position;
        std::string label;
        std::optional<std::int32_t> kind;
        std::optional<bool> paddingLeft;
        std::optional<bool> paddingRight;

        [[nodiscard]] JsonValue to_json() const
        {
            auto obj = JsonValue::empty_object();
            obj.set("position", position.to_json());
            obj.set("label", JsonValue::string_val(label));
            if (kind)
                obj.set("kind", JsonValue::integer(*kind));
            if (paddingLeft)
                obj.set("paddingLeft", JsonValue::boolean(*paddingLeft));
            if (paddingRight)
                obj.set("paddingRight", JsonValue::boolean(*paddingRight));
            return obj;
        }
    };

    struct InlayHintOptions
    {
        bool typeHints{true};
        bool parameterHints{true};
        bool suppressParameterNameMatches{true};

        [[nodiscard]] static JsonValue to_json()
        {
            auto obj = JsonValue::empty_object();
            obj.set("resolveProvider", JsonValue::boolean(false));
            return obj;
        }
    };

    [[nodiscard]] JsonValue make_initialize_result(std::string_view position_encoding = PositionEncoding::Utf16)
    {
        auto caps = JsonValue::empty_object();
        caps.set("textDocumentSync", JsonValue::integer(1));
        caps.set("hoverProvider", JsonValue::boolean(true));
        caps.set("definitionProvider", JsonValue::boolean(true));
        caps.set("referencesProvider", JsonValue::boolean(true));

        caps.set("positionEncoding", JsonValue::string_val(std::string{position_encoding}));

        auto rename_caps = JsonValue::empty_object();
        rename_caps.set("prepareProvider", JsonValue::boolean(true));
        caps.set("renameProvider", std::move(rename_caps));

        auto code_action_caps = JsonValue::empty_object();
        auto code_action_kinds = JsonValue::empty_array();
        code_action_kinds.push_back(JsonValue::string_val("quickfix"));
        code_action_caps.set("codeActionKinds", std::move(code_action_kinds));
        caps.set("codeActionProvider", std::move(code_action_caps));

        caps.set("documentFormattingProvider", JsonValue::boolean(true));
        caps.set("documentRangeFormattingProvider", JsonValue::boolean(true));

        auto on_type_provider = JsonValue::empty_object();
        on_type_provider.set("firstTriggerCharacter", JsonValue::string_val("}"));
        auto more_triggers = JsonValue::empty_array();
        more_triggers.push_back(JsonValue::string_val(";"));
        on_type_provider.set("moreTriggerCharacter", std::move(more_triggers));
        caps.set("documentOnTypeFormattingProvider", std::move(on_type_provider));

        caps.set("documentHighlightProvider", JsonValue::boolean(true));

        protocol::InlayHintOptions inlay_opts;
        caps.set("inlayHintProvider", inlay_opts.to_json());

        caps.set("workspaceSymbolProvider", JsonValue::boolean(true));

        auto stp = JsonValue::empty_object();
        stp.set("legend", make_semantic_tokens_legend());
        stp.set("full", JsonValue::boolean(true));
        caps.set("semanticTokensProvider", std::move(stp));

        auto completion_provider = JsonValue::empty_object();
        auto triggers = JsonValue::empty_array();
        triggers.push_back(JsonValue::string_val("."));
        triggers.push_back(JsonValue::string_val(":"));
        completion_provider.set("triggerCharacters", std::move(triggers));
        caps.set("completionProvider", std::move(completion_provider));

        auto sig_help_provider = JsonValue::empty_object();
        auto sig_triggers = JsonValue::empty_array();
        sig_triggers.push_back(JsonValue::string_val("("));
        sig_triggers.push_back(JsonValue::string_val(","));
        sig_help_provider.set("triggerCharacters", std::move(sig_triggers));
        caps.set("signatureHelpProvider", std::move(sig_help_provider));

        auto result = JsonValue::empty_object();
        result.set("capabilities", std::move(caps));
        return result;
    }

} // namespace dccd::protocol

module :private;

namespace dccd::protocol
{
    class JsonParserImpl
    {
    public:
        explicit JsonParserImpl(std::string_view input) : m_input{input} {}

        [[nodiscard]] std::optional<JsonValue> parse()
        {
            skip_ws();
            auto val = parse_value();
            if (!val)
                return std::nullopt;

            skip_ws();
            if (m_pos != m_input.size())
                return std::nullopt;

            return val;
        }

    private:
        std::string_view m_input;
        std::size_t m_pos{};

        void skip_ws()
        {
            while (m_pos < m_input.size())
            {
                char c = m_input[m_pos];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                    ++m_pos;
                else
                    break;
            }
        }

        [[nodiscard]] char peek() const noexcept
        {
            if (m_pos < m_input.size())
                return m_input[m_pos];

            return '\0';
        }

        char advance()
        {
            if (m_pos < m_input.size())
                return m_input[m_pos++];

            return '\0';
        }

        [[nodiscard]] std::optional<JsonValue> parse_value()
        {
            skip_ws();
            char c = peek();

            switch (c)
            {
                case 'n':
                    return parse_literal("null", JsonValue::null_val());
                case 't':
                    return parse_literal("true", JsonValue::boolean(true));
                case 'f':
                    return parse_literal("false", JsonValue::boolean(false));
                case '"':
                    return parse_string();
                case '[':
                    return parse_array();
                case '{':
                    return parse_object();
                case '-':
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                    return parse_number();
                default:
                    return std::nullopt;
            }
        }

        [[nodiscard]] std::optional<JsonValue> parse_literal(char const* expected, JsonValue val)
        {
            for (char const* p = expected; *p; ++p)
                if (advance() != *p)
                    return std::nullopt;

            return val;
        }

        [[nodiscard]] std::optional<JsonValue> parse_string()
        {
            if (advance() != '"')
                return std::nullopt;

            std::string result;
            result.reserve(64);

            while (true)
            {
                if (m_pos >= m_input.size())
                    return std::nullopt;

                char c = advance();
                if (c == '"')
                    return JsonValue::string_val(std::move(result));

                if (c == '\\')
                {
                    if (m_pos >= m_input.size())
                        return std::nullopt;

                    char esc = advance();
                    switch (esc)
                    {
                        case '"':
                            result += '"';
                            break;
                        case '\\':
                            result += '\\';
                            break;
                        case '/':
                            result += '/';
                            break;
                        case 'b':
                            result += '\b';
                            break;
                        case 'f':
                            result += '\f';
                            break;
                        case 'n':
                            result += '\n';
                            break;
                        case 'r':
                            result += '\r';
                            break;
                        case 't':
                            result += '\t';
                            break;
                        case 'u': {
                            auto cp = parse_unicode_escape();
                            if (!cp)
                                return std::nullopt;

                            encode_utf8(*cp, result);
                            break;
                        }
                        default:
                            return std::nullopt;
                    }
                }
                else if (static_cast<unsigned char>(c) < 0x20u)
                    return std::nullopt;
                else
                    result += c;
            }
        }

        [[nodiscard]] std::optional<char32_t> parse_unicode_escape()
        {
            auto hex = parse_hex4();
            if (!hex)
                return std::nullopt;

            char32_t cp = *hex;

            if (cp >= 0xD800u && cp <= 0xDBFFu)
            {
                if (m_pos + 6 > m_input.size())
                    return std::nullopt;

                if (m_input[m_pos] != '\\' || m_input[m_pos + 1] != 'u')
                    return std::nullopt;

                m_pos += 2;

                auto low = parse_hex4();
                if (!low || *low < 0xDC00u || *low > 0xDFFFu)
                    return std::nullopt;

                cp = 0x10000u + ((cp - 0xD800u) << 10) + (*low - 0xDC00u);
            }

            return cp;
        }

        [[nodiscard]] std::optional<std::uint16_t> parse_hex4()
        {
            if (m_pos + 4 > m_input.size())
                return std::nullopt;

            std::uint16_t value = 0;
            for (std::size_t i = 0; i < 4; ++i)
            {
                char c = m_input[m_pos + i];
                int digit = -1;
                if (c >= '0' && c <= '9')
                    digit = static_cast<int>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    digit = static_cast<int>(c - 'a') + 10;
                else if (c >= 'A' && c <= 'F')
                    digit = static_cast<int>(c - 'A') + 10;
                else
                    return std::nullopt;

                value = static_cast<std::uint16_t>((value << 4) | static_cast<std::uint16_t>(digit));
            }

            m_pos += 4;
            return value;
        }

        static void encode_utf8(char32_t cp, std::string& out)
        {
            if (cp <= 0x7Fu)
                out += static_cast<char>(cp);
            else if (cp <= 0x7FFu)
            {
                out += static_cast<char>(0xC0u | (cp >> 6));
                out += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
            else if (cp <= 0xFFFFu)
            {
                out += static_cast<char>(0xE0u | (cp >> 12));
                out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                out += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
            else if (cp <= 0x10FFFFu)
            {
                out += static_cast<char>(0xF0u | (cp >> 18));
                out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
                out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                out += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
        }

        [[nodiscard]] std::optional<JsonValue> parse_number()
        {
            std::size_t start = m_pos;
            bool is_integer = true;

            if (peek() == '-')
                advance();

            if (peek() == '0')
                advance();
            else if (peek() >= '1' && peek() <= '9')
            {
                while (peek() >= '0' && peek() <= '9')
                    advance();
            }
            else
                return std::nullopt;

            if (peek() == '.')
            {
                is_integer = false;
                advance();
                if (peek() < '0' || peek() > '9')
                    return std::nullopt;

                while (peek() >= '0' && peek() <= '9')
                    advance();
            }

            if (peek() == 'e' || peek() == 'E')
            {
                is_integer = false;
                advance();
                if (peek() == '+' || peek() == '-')
                    advance();

                if (peek() < '0' || peek() > '9')
                    return std::nullopt;

                while (peek() >= '0' && peek() <= '9')
                    advance();
            }

            std::string_view num_str = m_input.substr(start, m_pos - start);
            double val;
            auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), val);
            if (ec != std::errc{})
                return std::nullopt;

            JsonValue jv;
            jv.m_type = JsonType::Number;
            jv.m_number = val;
            jv.m_is_integer = is_integer;
            return jv;
        }

        [[nodiscard]] std::optional<JsonValue> parse_array()
        {
            if (advance() != '[')
                return std::nullopt;

            skip_ws();

            std::vector<JsonValue> arr;

            if (peek() == ']')
            {
                advance();
                return JsonValue::array_val(std::move(arr));
            }

            while (true)
            {
                skip_ws();
                auto val = parse_value();
                if (!val)
                    return std::nullopt;

                arr.push_back(std::move(*val));

                skip_ws();
                char c = advance();
                if (c == ']')
                    break;

                if (c != ',')
                    return std::nullopt;
            }

            return JsonValue::array_val(std::move(arr));
        }

        [[nodiscard]] std::optional<JsonValue> parse_object()
        {
            if (advance() != '{')
                return std::nullopt;

            skip_ws();

            std::map<std::string, JsonValue, std::less<>> obj;

            if (peek() == '}')
            {
                advance();
                return JsonValue::object_val(std::move(obj));
            }

            while (true)
            {
                skip_ws();
                if (peek() != '"')
                    return std::nullopt;

                auto key_val = parse_string();
                if (!key_val || !key_val->is_string())
                    return std::nullopt;

                std::string key = key_val->as_string();

                skip_ws();
                if (advance() != ':')
                    return std::nullopt;

                skip_ws();
                auto val = parse_value();
                if (!val)
                    return std::nullopt;

                obj[std::move(key)] = std::move(*val);

                skip_ws();
                char c = advance();
                if (c == '}')
                    break;

                if (c != ',')
                    return std::nullopt;
            }

            return JsonValue::object_val(std::move(obj));
        }
    };

    void JsonValue::serialize_string_content(std::string& out, std::string_view s)
    {
        for (char c : s)
        {
            switch (c)
            {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20u)
                    {
                        out += "\\u00";
                        out += ("0123456789abcdef")[static_cast<unsigned char>(c) >> 4];
                        out += ("0123456789abcdef")[static_cast<unsigned char>(c) & 0xFu];
                    }
                    else
                    {
                        out += c;
                    }
                    break;
            }
        }
    }

    void JsonValue::serialize_value(std::string& out, JsonValue const& v)
    {
        switch (v.m_type)
        {
            case JsonType::Null:
                out += "null";
                break;

            case JsonType::Bool:
                out += v.m_bool ? "true" : "false";
                break;

            case JsonType::Number:
                if (v.m_is_integer)
                {
                    auto i = static_cast<std::int64_t>(v.m_number);
                    out += std::to_string(i);
                }
                else
                {
                    char buf[64];
                    auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v.m_number);
                    if (ec == std::errc{})
                        out.append(buf, ptr);
                    else
                        out += "0";
                }
                break;

            case JsonType::String:
                out += '"';
                serialize_string_content(out, v.m_string);
                out += '"';
                break;

            case JsonType::Array:
                out += '[';
                for (std::size_t i = 0; i < v.m_array.size(); ++i)
                {
                    if (i > 0)
                        out += ',';
                    serialize_value(out, v.m_array[i]);
                }
                out += ']';
                break;

            case JsonType::Object:
                out += '{';
                bool first = true;
                for (auto const& [key, val] : v.m_object)
                {
                    if (!first)
                        out += ',';
                    first = false;

                    out += '"';
                    serialize_string_content(out, key);
                    out += '"';
                    out += ':';
                    serialize_value(out, val);
                }
                out += '}';
                break;
        }
    }

    std::string JsonValue::serialize() const
    {
        std::string result;
        result.reserve(256);
        serialize_value(result, *this);
        return result;
    }

    std::optional<JsonValue> JsonValue::parse(std::string_view input)
    {
        JsonParserImpl parser{input};
        return parser.parse();
    }

} // namespace dccd::protocol
