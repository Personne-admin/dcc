export module dccd.completion;

import std;
import dcc.sm;
import dcc.si;
import dcc.ast;
import dcc.types;
import dcc.sema;
import dcc.sema.scope;
import dcc.sema.type_helpers;
import dcc.session;
import dcc.query;
import dcc.target;
import dccd.protocol;

export namespace dccd::completion
{
    [[nodiscard]] protocol::CompletionList compute_completions(dcc::session::CompilerSession const& session, std::string_view uri, dcc::sm::Position cursor);

} // namespace dccd::completion

module :private;

namespace dccd::completion
{
    namespace
    {
        [[nodiscard]] constexpr bool is_ident_char(char c) noexcept
        {
            return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        }

        [[nodiscard]] protocol::CompletionItemKind symbol_kind_to_completion_kind(dcc::sema::SymbolKind sk)
        {
            using dcc::sema::SymbolKind;
            switch (sk)
            {
                case SymbolKind::Struct:
                    return protocol::CompletionItemKind::Struct;
                case SymbolKind::Union:
                    return protocol::CompletionItemKind::Struct;
                case SymbolKind::Enum:
                    return protocol::CompletionItemKind::Enum;
                case SymbolKind::TypeAlias:
                    return protocol::CompletionItemKind::Class;
                case SymbolKind::TemplateParam:
                    return protocol::CompletionItemKind::TypeParameter;
                case SymbolKind::Function:
                    return protocol::CompletionItemKind::Function;
                case SymbolKind::Variable:
                    return protocol::CompletionItemKind::Variable;
                case SymbolKind::ValueAlias:
                    return protocol::CompletionItemKind::Constant;
                case SymbolKind::EnumVariant:
                    return protocol::CompletionItemKind::EnumMember;
                case SymbolKind::Module:
                    return protocol::CompletionItemKind::Module;
                case SymbolKind::UsingGroup:
                    return protocol::CompletionItemKind::Module;
            }
            return protocol::CompletionItemKind::Text;
        }

        [[nodiscard]] protocol::CompletionItemKind field_kind()
        {
            return protocol::CompletionItemKind::Field;
        }

        [[nodiscard]] std::string format_type_str_local(dcc::types::Type const* ty)
        {
            return dcc::sema::format_dcc_type(ty);
        }

        [[nodiscard]] std::string func_signature_str(dcc::ast::FuncDecl const* fd)
        {
            if (!fd)
                return {};

            std::string ret = "void";
            if (fd->return_type && fd->return_type->sema.canonical)
                ret = format_type_str_local(dcc::sema::get_canonical(fd->return_type->sema));

            std::string sig = std::format("{} {}(", ret, fd->name);
            for (std::size_t i = 0; i < fd->params.size(); ++i)
            {
                if (i > 0)
                    sig += ", ";
                auto const& p = fd->params[i];
                if (p.type && p.type->sema.canonical)
                {
                    auto ty = dcc::sema::get_canonical(p.type->sema);
                    if (p.name.empty())
                        sig += format_type_str_local(ty);
                    else
                        sig += std::format("{} {}", format_type_str_local(ty), p.name);
                }
                else if (!p.name.empty())
                    sig += p.name;
            }
            sig += ")";
            return sig;
        }

        [[nodiscard]] dcc::sema::ModuleInfo const* find_module_by_file_id(dcc::sema::SemaContext const& sema, dcc::sm::FileId fid)
        {
            auto& graph = const_cast<dcc::sema::SemaContext&>(sema).graph();
            for (auto const& mod : graph.all())
                if (mod->file_id == fid)
                    return mod.get();

            return nullptr;
        }

        [[nodiscard]] dcc::sema::ModuleInfo const* find_module_by_uri(dcc::sema::SemaContext const& sema, dcc::sm::SourceManager const& sm,
                                                                      std::string_view uri)
        {
            auto& graph = const_cast<dcc::sema::SemaContext&>(sema).graph();
            for (auto const& mod : graph.all())
            {
                auto const* sf = sm.get(mod->file_id);
                if (sf && sf->uri() == uri)
                    return mod.get();
            }

            return nullptr;
        }

        struct ItemOptions
        {
            std::optional<std::string> detail;
            std::string sort_text{"2"};
            std::optional<std::string> insert_text;
            std::optional<std::int32_t> insert_text_format;
            bool signature_help_command{false};
        };

        void add_item(std::vector<protocol::CompletionItem>& items, std::string_view label, protocol::CompletionItemKind kind,
                      protocol::LspRange const* replacement_range, ItemOptions opts = {})
        {
            protocol::CompletionItem item;
            item.label = std::string{label};
            item.kind = kind;
            item.detail = std::move(opts.detail);
            item.sortText = std::move(opts.sort_text);

            std::string const& insert = opts.insert_text ? *opts.insert_text : item.label;
            if (replacement_range)
            {
                protocol::TextEdit edit;
                edit.range = *replacement_range;
                edit.newText = insert;
                item.textEdit = std::move(edit);
                item.insertText = insert;
            }
            else if (opts.insert_text)
                item.insertText = insert;

            if (opts.insert_text_format)
                item.insertTextFormat = *opts.insert_text_format;

            if (opts.signature_help_command)
            {
                protocol::Command cmd;
                cmd.title = "trigger signature help";
                cmd.command = std::string{protocol::kTriggerParameterHintsCommand};
                item.command = std::move(cmd);
            }

            items.push_back(std::move(item));
        }

        void dedup_and_sort(std::vector<protocol::CompletionItem>& items)
        {
            auto rank = [](protocol::CompletionItem const& item) -> std::string_view {
                return item.sortText ? std::string_view{*item.sortText} : std::string_view{"2"};
            };

            std::ranges::sort(items, [&](protocol::CompletionItem const& a, protocol::CompletionItem const& b) {
                auto ra = rank(a);
                auto rb = rank(b);
                if (ra != rb)
                    return ra < rb;
                auto cmp = a.label <=> b.label;
                if (cmp != 0)
                    return cmp < 0;
                return static_cast<std::int32_t>(a.kind) < static_cast<std::int32_t>(b.kind);
            });

            std::unordered_set<std::string> seen;
            std::vector<protocol::CompletionItem> out;
            out.reserve(items.size());
            for (auto& item : items)
            {
                auto key = std::format("{}-{}", item.label, static_cast<std::int32_t>(item.kind));
                if (seen.insert(std::move(key)).second)
                    out.push_back(std::move(item));
            }
            items = std::move(out);
        }

        struct CompletionContext
        {
            enum class Trigger : std::uint8_t
            {
                None,
                Dot,
                ColonColon
            };
            enum class ContextKind : std::uint8_t
            {
                Value,
                Type,
                Statement
            };
            Trigger trigger{Trigger::None};
            ContextKind kind{ContextKind::Value};
            std::string prefix;
            std::vector<std::string> namespace_path;
            std::size_t token_start{0};
            std::size_t token_end{0};
            std::size_t trigger_offset{0};
        };

        [[nodiscard]] CompletionContext::ContextKind determine_context_kind(std::string_view text, std::size_t token_start)
        {
            std::size_t p = token_start;
            while (p > 0 && (text[p - 1] == ' ' || text[p - 1] == '\t' || text[p - 1] == '\r' || text[p - 1] == '\n'))
                --p;

            if (p == 0)
                return CompletionContext::ContextKind::Statement;

            char c = text[p - 1];
            if (c == ':')
                return CompletionContext::ContextKind::Type;

            if (c == ';' || c == '{' || c == '}')
                return CompletionContext::ContextKind::Statement;

            if (c == '(' || c == '=' || c == ',' || c == '[' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '!' || c == '<' || c == '>' ||
                c == '&' || c == '|' || c == '^' || c == '~' || c == '?')
                return CompletionContext::ContextKind::Value;

            std::size_t word_end = p;
            std::size_t q = p;
            while (q > 0 && is_ident_char(text[q - 1]))
                --q;
            std::string_view word = text.substr(q, word_end - q);

            if (word == "return" || word == "if" || word == "while" || word == "for" || word == "match" || word == "else" || word == "do" || word == "in" ||
                word == "as")
                return CompletionContext::ContextKind::Value;

            if (word == "struct" || word == "enum" || word == "union" || word == "using" || word == "const" || word == "volatile" || word == "restrict")
                return CompletionContext::ContextKind::Type;

            if (word == "import" || word == "module" || word == "public" || word == "static" || word == "extern")
                return CompletionContext::ContextKind::Statement;

            return CompletionContext::ContextKind::Value;
        }

        [[nodiscard]] CompletionContext detect_context(dcc::sm::SourceFile const& file, dcc::sm::Offset cursor_offset)
        {
            CompletionContext ctx;
            auto text = file.text();
            if (cursor_offset > static_cast<dcc::sm::Offset>(text.size()))
                return ctx;

            auto cur = static_cast<std::size_t>(cursor_offset);

            std::size_t start = cur;
            if (cur > 0 && is_ident_char(text[cur - 1]))
            {
                start = cur - 1;
                while (start > 0 && is_ident_char(text[start - 1]))
                    --start;
                ctx.prefix = std::string{text.substr(start, cur - start)};
            }
            ctx.token_start = start;
            ctx.token_end = cur;
            while (ctx.token_end < text.size() && is_ident_char(text[ctx.token_end]))
                ++ctx.token_end;

            std::size_t p = start;
            while (p > 0 && (text[p - 1] == ' ' || text[p - 1] == '\t' || text[p - 1] == '\r' || text[p - 1] == '\n'))
                --p;

            if (p > 0 && text[p - 1] == '.')
            {
                ctx.trigger = CompletionContext::Trigger::Dot;
                ctx.trigger_offset = p - 1;
            }
            else if (p > 1 && text[p - 1] == ':' && text[p - 2] == ':')
            {
                ctx.trigger = CompletionContext::Trigger::ColonColon;
                ctx.trigger_offset = p - 2;

                std::size_t q = p - 2;
                while (q > 0)
                {
                    if (is_ident_char(text[q - 1]))
                    {
                        std::size_t seg_end = q;
                        --q;
                        while (q > 0 && is_ident_char(text[q - 1]))
                            --q;
                        ctx.namespace_path.push_back(std::string{text.substr(q, seg_end - q)});
                        if (q >= 2 && text[q - 2] == ':' && text[q - 1] == ':')
                        {
                            q -= 2;
                            continue;
                        }
                        break;
                    }
                    break;
                }
                std::ranges::reverse(ctx.namespace_path);
            }
            else
            {
                ctx.trigger = CompletionContext::Trigger::None;
                ctx.kind = determine_context_kind(text, start);
            }

            return ctx;
        }

        [[nodiscard]] std::optional<protocol::LspRange> compute_replacement_range(dcc::sm::SourceManager const& sm, dcc::sm::FileId fid,
                                                                                  CompletionContext const& ctx)
        {
            auto start = sm.location_to_lsp_position(dcc::sm::Location{fid, static_cast<dcc::sm::Offset>(ctx.token_start)});
            auto end = sm.location_to_lsp_position(dcc::sm::Location{fid, static_cast<dcc::sm::Offset>(ctx.token_end)});
            if (!start || !end)
                return std::nullopt;

            protocol::LspRange range;
            range.start.line = start->line;
            range.start.character = start->character;
            range.end.line = end->line;
            range.end.character = end->character;
            return range;
        }

        void add_field_completions(std::vector<protocol::CompletionItem>& items, dcc::ast::Decl const& decl, std::string_view prefix,
                                   protocol::LspRange const* replacement_range)
        {
            using dcc::ast::DeclKind;
            switch (decl.kind)
            {
                case DeclKind::Struct: {
                    auto const* sd = static_cast<dcc::ast::StructDecl const*>(&decl);
                    for (auto const& f : sd->fields)
                    {
                        if (!prefix.empty() && !f.name.starts_with(prefix))
                            continue;

                        ItemOptions opts;
                        opts.sort_text = "1";
                        opts.insert_text = std::string{f.name};
                        opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                        if (f.type && f.type->sema.canonical)
                            opts.detail = format_type_str_local(dcc::sema::get_canonical(f.type->sema));
                        add_item(items, f.name, field_kind(), replacement_range, std::move(opts));
                    }
                    break;
                }
                case DeclKind::Union: {
                    auto const* ud = static_cast<dcc::ast::UnionDecl const*>(&decl);
                    for (auto const& f : ud->fields)
                    {
                        if (!prefix.empty() && !f.name.starts_with(prefix))
                            continue;

                        ItemOptions opts;
                        opts.sort_text = "1";
                        opts.insert_text = std::string{f.name};
                        opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                        if (f.type && f.type->sema.canonical)
                            opts.detail = format_type_str_local(dcc::sema::get_canonical(f.type->sema));
                        add_item(items, f.name, field_kind(), replacement_range, std::move(opts));
                    }
                    break;
                }
                case DeclKind::Enum: {
                    auto const* ed = static_cast<dcc::ast::EnumDecl const*>(&decl);
                    for (auto const& v : ed->variants)
                    {
                        if (!prefix.empty() && !v.name.starts_with(prefix))
                            continue;

                        ItemOptions opts;
                        opts.sort_text = "1";
                        opts.insert_text = std::string{v.name};
                        opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                        add_item(items, v.name, protocol::CompletionItemKind::EnumMember, replacement_range, std::move(opts));
                    }
                    break;
                }
                default:
                    break;
            }
        }

        [[nodiscard]] dcc::ast::Decl const* nominal_decl(dcc::types::Type const* ty)
        {
            if (!ty)
                return nullptr;

            using dcc::types::TypeKind;
            switch (ty->kind)
            {
                case TypeKind::Struct:
                    return reinterpret_cast<dcc::ast::Decl const*>(static_cast<dcc::types::StructType const*>(ty)->decl);
                case TypeKind::Union:
                    return reinterpret_cast<dcc::ast::Decl const*>(static_cast<dcc::types::UnionType const*>(ty)->decl);
                case TypeKind::Enum:
                    return reinterpret_cast<dcc::ast::Decl const*>(static_cast<dcc::types::EnumType const*>(ty)->decl);
                case TypeKind::Pointer:
                    return nominal_decl(static_cast<dcc::types::PointerType const*>(ty)->pointee);
                case TypeKind::Nominal:
                    return nominal_decl(static_cast<dcc::types::NominalType const*>(ty)->underlying);
                default:
                    return nullptr;
            }
        }

        [[nodiscard]] bool receiver_compatible(dcc::types::Type const* param0, dcc::types::Type const* receiver)
        {
            if (!param0 || !receiver)
                return false;

            if (param0 == receiver)
                return true;

            if (auto const* param_ptr = dcc::types::type_cast<dcc::types::PointerType>(param0))
            {
                if (param_ptr->pointee == receiver)
                    return true;
                if (receiver->kind == dcc::types::TypeKind::Pointer && param_ptr->pointee->kind == dcc::types::TypeKind::TemplateParam)
                    return true;
            }

            if (auto const* recv_ptr = dcc::types::type_cast<dcc::types::PointerType>(receiver))
                if (recv_ptr->pointee == param0)
                    return true;

            if (receiver->kind == dcc::types::TypeKind::Array && param0->kind == dcc::types::TypeKind::Slice)
            {
                auto const* recv_arr = dcc::types::type_cast<dcc::types::ArrayType>(receiver);
                auto const* param_slice = dcc::types::type_cast<dcc::types::SliceType>(param0);
                if (recv_arr && param_slice && recv_arr->element == param_slice->element)
                    return true;
            }

            if (param0->kind == dcc::types::TypeKind::TemplateParam || param0->kind == dcc::types::TypeKind::TypePack)
                return true;

            return false;
        }

        [[nodiscard]] bool following_paren(std::string_view text, std::size_t token_end) noexcept
        {
            std::size_t i = token_end;
            for (;;)
            {
                while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
                    ++i;

                if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/')
                {
                    while (i < text.size() && text[i] != '\n')
                        ++i;
                    continue;
                }
                if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '*')
                {
                    i += 2;
                    while (i + 1 < text.size() && (text[i] != '*' || text[i + 1] != '/'))
                        ++i;
                    if (i + 1 < text.size())
                        i += 2;
                    continue;
                }
                break;
            }
            return i < text.size() && text[i] == '(';
        }

        struct SnippetInfo
        {
            std::string insert_text;
            std::int32_t format{protocol::InsertTextFormat::PlainText};
        };

        [[nodiscard]] SnippetInfo make_function_insert(std::string_view name, dcc::ast::FuncDecl const* fd, bool ufcs_receiver, bool paren_follows)
        {
            if (paren_follows)
                return {std::string{name}, protocol::InsertTextFormat::PlainText};

            std::string text{name};
            text += "(";
            std::size_t start = ufcs_receiver ? 1 : 0;
            int tab = 1;
            for (std::size_t i = start; i < fd->params.size(); ++i)
            {
                if (i > start)
                    text += ", ";
                text += "${" + std::to_string(tab++) + "}";
            }
            text += ")";
            return {std::move(text), protocol::InsertTextFormat::Snippet};
        }

        void add_ufcs_method_completions(std::vector<protocol::CompletionItem>& items, dcc::session::CompilerSession const& session,
                                         dcc::sema::ModuleInfo const* module, dcc::sema::Scope const* local_scope, dcc::types::Type const* receiver_type,
                                         std::string_view prefix, std::string_view source_text, std::size_t token_end,
                                         protocol::LspRange const* replacement_range)
        {
            std::ignore = session;
            if (!receiver_type)
                return;

            std::vector<dcc::sema::Scope const*> scopes;
            if (module)
            {
                if (module->ufcs_scope)
                    scopes.push_back(module->ufcs_scope);
                if (module->own_scope)
                    scopes.push_back(module->own_scope);
                for (auto const& imp : module->imports)
                    if (imp.target && imp.target->export_scope)
                        scopes.push_back(imp.target->export_scope);
            }
            if (local_scope)
                scopes.push_back(local_scope);

            std::unordered_set<dcc::ast::Decl const*> seen;

            auto consider = [&](dcc::sema::Symbol const& sym) {
                if (!sym.decl || sym.decl->kind != dcc::ast::DeclKind::Func)
                    return;
                if (!seen.insert(sym.decl).second)
                    return;

                auto const* fd = static_cast<dcc::ast::FuncDecl const*>(sym.decl);
                if (fd->params.empty())
                    return;

                dcc::types::Type const* param0 = nullptr;
                if (fd->params[0].type && fd->params[0].type->sema.canonical)
                    param0 = dcc::sema::get_canonical(fd->params[0].type->sema);

                if (!receiver_compatible(param0, receiver_type))
                    return;

                ItemOptions opts;
                opts.sort_text = "1";
                opts.detail = func_signature_str(fd);
                opts.signature_help_command = true;

                auto snippet = make_function_insert(sym.name, fd, true, following_paren(source_text, token_end));
                opts.insert_text = std::move(snippet.insert_text);
                opts.insert_text_format = snippet.format;

                add_item(items, sym.name, protocol::CompletionItemKind::Method, replacement_range, std::move(opts));
            };

            for (auto const* scope : scopes)
            {
                if (!scope)
                    continue;
                for (auto const& [name, binding] : scope->bindings())
                {
                    if (name.empty() || (!prefix.empty() && !name.starts_with(prefix)))
                        continue;
                    for (auto const& vs : binding.value_syms)
                        consider(vs);
                }
            }
        }

        [[nodiscard]] std::string_view extract_receiver_name(dcc::sm::SourceFile const& file, dcc::sm::Offset trigger_offset)
        {
            auto text = file.text();
            if (trigger_offset == 0 || trigger_offset > static_cast<dcc::sm::Offset>(text.size()))
                return {};

            auto pos = static_cast<std::ptrdiff_t>(trigger_offset) - 1;
            while (pos >= 0 && (text[static_cast<std::size_t>(pos)] == ' ' || text[static_cast<std::size_t>(pos)] == '\t'))
                --pos;

            if (pos < 0)
                return {};

            auto end = static_cast<std::size_t>(pos) + 1;
            while (pos >= 0)
            {
                char c = text[static_cast<std::size_t>(pos)];
                if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                    --pos;
                else
                    break;
            }

            if (static_cast<std::size_t>(pos) + 1 < end)
                return text.substr(static_cast<std::size_t>(pos) + 1, end - (static_cast<std::size_t>(pos) + 1));

            return {};
        }

        [[nodiscard]] dcc::types::Type const* resolve_receiver_type(dcc::session::CompilerSession const& session, dcc::sema::ModuleInfo const* module,
                                                                    dcc::sm::FileId fid, dcc::sm::SourceFile const& file, CompletionContext const& ctx,
                                                                    dcc::sm::Location cursor_loc)
        {
            auto const& sm = session.source_manager();

            if (auto node = dcc::query::find_node_at(session, cursor_loc))
            {
                if (node->expr)
                {
                    if (node->expr->kind == dcc::ast::ExprKind::FieldAccess)
                    {
                        auto const* fa = static_cast<dcc::ast::FieldAccessExpr const*>(node->expr);
                        if (fa->object)
                            if (auto t = dcc::sema::get_resolved_type(fa->object->sema))
                                return t;
                    }
                    else if (node->resolved_type)
                        return node->resolved_type;
                }
            }

            if (ctx.trigger_offset > 0)
            {
                auto loc = sm.location(fid, static_cast<dcc::sm::Offset>(ctx.trigger_offset - 1));
                if (auto node = dcc::query::find_node_at(session, loc))
                {
                    if (node->expr && node->resolved_type)
                        return node->resolved_type;
                    if (node->type_expr && node->resolved_type)
                        return node->resolved_type;
                }
            }

            auto receiver_name = extract_receiver_name(file, static_cast<dcc::sm::Offset>(ctx.trigger_offset));
            if (!receiver_name.empty())
            {
                auto local_ctx = dcc::query::collect_local_context(session, fid, cursor_loc);
                if (auto t = local_ctx.local_type(receiver_name))
                    return t;

                if (module)
                {
                    auto interned = const_cast<dcc::si::string_interner&>(session.interner()).intern(receiver_name);
                    auto syms = module->own_scope->lookup_values(interned);
                    for (auto const& sym : syms)
                    {
                        if (sym.decl && sym.decl->kind == dcc::ast::DeclKind::Var)
                        {
                            auto const* vd = static_cast<dcc::ast::VarDecl const*>(sym.decl);
                            if (vd->type && vd->type->sema.canonical)
                                return dcc::sema::get_canonical(vd->type->sema);
                        }
                    }
                }
            }

            return nullptr;
        }

        void handle_member_access_completion(std::vector<protocol::CompletionItem>& items, dcc::session::CompilerSession const& session,
                                             dcc::sema::ModuleInfo const* module, dcc::sm::FileId fid, dcc::sm::SourceFile const& file,
                                             CompletionContext const& ctx, dcc::sm::Location cursor_loc, protocol::LspRange const* replacement_range)
        {
            auto receiver_type = resolve_receiver_type(session, module, fid, file, ctx, cursor_loc);
            if (!receiver_type)
                return;

            if (auto const* decl = nominal_decl(receiver_type))
                add_field_completions(items, *decl, ctx.prefix, replacement_range);

            auto local_ctx = dcc::query::collect_local_context(session, fid, cursor_loc);
            add_ufcs_method_completions(items, session, module, local_ctx.scope, receiver_type, ctx.prefix, file.text(), ctx.token_end, replacement_range);
        }

        void add_scope_bindings(std::vector<protocol::CompletionItem>& items, dcc::sema::Scope const& scope, std::string_view prefix,
                                std::string_view source_text, std::size_t token_end, protocol::LspRange const* replacement_range)
        {
            for (auto const& [name, binding] : scope.bindings())
            {
                if (name.empty() || (!prefix.empty() && !name.starts_with(prefix)))
                    continue;

                if (binding.has_type)
                {
                    ItemOptions opts;
                    opts.sort_text = "2";
                    opts.insert_text = std::string{name};
                    opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                    add_item(items, name, symbol_kind_to_completion_kind(binding.type_sym.kind), replacement_range, std::move(opts));
                }
                for (auto const& vs : binding.value_syms)
                {
                    ItemOptions opts;
                    opts.sort_text = "2";
                    if (vs.kind == dcc::sema::SymbolKind::Function)
                    {
                        auto const* fd = static_cast<dcc::ast::FuncDecl const*>(vs.decl);
                        opts.detail = func_signature_str(fd);
                        opts.signature_help_command = true;
                        auto snippet = make_function_insert(name, fd, false, following_paren(source_text, token_end));
                        opts.insert_text = std::move(snippet.insert_text);
                        opts.insert_text_format = snippet.format;
                    }
                    else
                    {
                        opts.insert_text = std::string{name};
                        opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                    }
                    add_item(items, name, symbol_kind_to_completion_kind(vs.kind), replacement_range, std::move(opts));
                }
                if (binding.has_namespace)
                {
                    ItemOptions opts;
                    opts.sort_text = "2";
                    opts.insert_text = std::string{name};
                    opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                    add_item(items, name, symbol_kind_to_completion_kind(binding.namespace_sym.kind), replacement_range, std::move(opts));
                }
            }
        }

        void handle_namespace_completion(std::vector<protocol::CompletionItem>& items, dcc::session::CompilerSession const& session,
                                         dcc::sema::ModuleInfo const* module, CompletionContext const& ctx, std::string_view source_text,
                                         protocol::LspRange const* replacement_range)
        {
            if (!module)
                return;

            auto* sema_ctx = session.sema_context();
            if (!sema_ctx)
                return;

            auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();

            auto& interner = const_cast<dcc::si::string_interner&>(session.interner());
            dcc::sema::Scope const* target = module->own_scope;
            if (target)
            {
                for (std::size_t i = 0; i < ctx.namespace_path.size(); ++i)
                {
                    auto seg = interner.intern(ctx.namespace_path[i]);
                    dcc::sema::Scope const* next = nullptr;
                    if (i == 0)
                        next = target->lookup_namespace(seg);
                    else
                        next = target->lookup_namespace_local(seg);

                    if (!next)
                    {
                        target = nullptr;
                        break;
                    }
                    target = next;
                }
            }

            if (target)
                add_scope_bindings(items, *target, ctx.prefix, source_text, ctx.token_end, replacement_range);

            for (auto const& other_mod : graph.all())
            {
                auto const& segments = other_mod->canonical_path.segments();
                if (segments.size() <= ctx.namespace_path.size())
                    continue;

                bool prefix_match = true;
                for (std::size_t i = 0; i < ctx.namespace_path.size(); ++i)
                {
                    if (segments[i] != ctx.namespace_path[i])
                    {
                        prefix_match = false;
                        break;
                    }
                }
                if (!prefix_match)
                    continue;

                std::string_view next_seg = segments[ctx.namespace_path.size()];
                if (!ctx.prefix.empty() && !next_seg.starts_with(ctx.prefix))
                    continue;

                ItemOptions opts;
                opts.sort_text = "2";
                opts.insert_text = std::string{next_seg};
                opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                add_item(items, next_seg, protocol::CompletionItemKind::Module, replacement_range, std::move(opts));
            }
        }

        void add_keyword_completions(std::vector<protocol::CompletionItem>& items, CompletionContext::ContextKind kind, std::string_view prefix,
                                     protocol::LspRange const* replacement_range)
        {
            static constexpr std::array value_keywords = {
                std::string_view{"true"},    std::string_view{"false"},    std::string_view{"null"},     std::string_view{"sizeof"},
                std::string_view{"alignof"}, std::string_view{"offsetof"}, std::string_view{"compiles"}, std::string_view{"asm"},
            };
            static constexpr std::array type_keywords = {
                std::string_view{"u8"},    std::string_view{"i8"},       std::string_view{"u16"},      std::string_view{"i16"},    std::string_view{"u32"},
                std::string_view{"i32"},   std::string_view{"u64"},      std::string_view{"i64"},      std::string_view{"f32"},    std::string_view{"f64"},
                std::string_view{"char"},  std::string_view{"void"},     std::string_view{"bool"},     std::string_view{"null_t"}, std::string_view{"usize"},
                std::string_view{"isize"}, std::string_view{"struct"},   std::string_view{"enum"},     std::string_view{"union"},  std::string_view{"using"},
                std::string_view{"const"}, std::string_view{"volatile"}, std::string_view{"restrict"},
            };
            static constexpr std::array statement_keywords = {
                std::string_view{"if"},     std::string_view{"while"},    std::string_view{"for"},    std::string_view{"do"},     std::string_view{"return"},
                std::string_view{"break"},  std::string_view{"continue"}, std::string_view{"defer"},  std::string_view{"match"},  std::string_view{"import"},
                std::string_view{"module"}, std::string_view{"public"},   std::string_view{"static"}, std::string_view{"extern"}, std::string_view{"using"},
                std::string_view{"struct"}, std::string_view{"enum"},     std::string_view{"union"},  std::string_view{"const"},  std::string_view{"asm"},
                std::string_view{"u8"},     std::string_view{"i8"},       std::string_view{"u16"},    std::string_view{"i16"},    std::string_view{"u32"},
                std::string_view{"i32"},    std::string_view{"u64"},      std::string_view{"i64"},    std::string_view{"f32"},    std::string_view{"f64"},
                std::string_view{"char"},   std::string_view{"void"},     std::string_view{"bool"},   std::string_view{"null_t"}, std::string_view{"usize"},
                std::string_view{"isize"},
            };

            std::span<std::string_view const> words;
            switch (kind)
            {
                case CompletionContext::ContextKind::Value:
                    words = value_keywords;
                    break;
                case CompletionContext::ContextKind::Type:
                    words = type_keywords;
                    break;
                case CompletionContext::ContextKind::Statement:
                    words = statement_keywords;
                    break;
            }

            for (auto w : words)
            {
                if (!prefix.empty() && !w.starts_with(prefix))
                    continue;

                ItemOptions opts;
                opts.sort_text = "3";
                opts.insert_text = std::string{w};
                opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                add_item(items, w, protocol::CompletionItemKind::Keyword, replacement_range, std::move(opts));
            }
        }

        void add_scope_symbols_for_context(std::vector<protocol::CompletionItem>& items, dcc::sema::Scope const& scope, std::string_view prefix,
                                           CompletionContext::ContextKind kind, std::string_view source_text, std::size_t token_end,
                                           protocol::LspRange const* replacement_range)
        {
            if (kind == CompletionContext::ContextKind::Statement)
            {
                add_scope_bindings(items, scope, prefix, source_text, token_end, replacement_range);
                return;
            }

            for (auto const& [name, binding] : scope.bindings())
            {
                if (name.empty() || (!prefix.empty() && !name.starts_with(prefix)))
                    continue;

                if (kind == CompletionContext::ContextKind::Value)
                {
                    for (auto const& vs : binding.value_syms)
                    {
                        ItemOptions opts;
                        opts.sort_text = "2";
                        if (vs.kind == dcc::sema::SymbolKind::Function)
                        {
                            auto const* fd = static_cast<dcc::ast::FuncDecl const*>(vs.decl);
                            opts.detail = func_signature_str(fd);
                            opts.signature_help_command = true;
                            auto snippet = make_function_insert(name, fd, false, following_paren(source_text, token_end));
                            opts.insert_text = std::move(snippet.insert_text);
                            opts.insert_text_format = snippet.format;
                        }
                        else
                        {
                            opts.insert_text = std::string{name};
                            opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                        }
                        add_item(items, name, symbol_kind_to_completion_kind(vs.kind), replacement_range, std::move(opts));
                    }
                    if (binding.has_namespace)
                    {
                        ItemOptions opts;
                        opts.sort_text = "2";
                        opts.insert_text = std::string{name};
                        opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                        add_item(items, name, symbol_kind_to_completion_kind(binding.namespace_sym.kind), replacement_range, std::move(opts));
                    }
                }
                else if (kind == CompletionContext::ContextKind::Type)
                {
                    if (binding.has_type)
                    {
                        ItemOptions opts;
                        opts.sort_text = "2";
                        opts.insert_text = std::string{name};
                        opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                        add_item(items, name, symbol_kind_to_completion_kind(binding.type_sym.kind), replacement_range, std::move(opts));
                    }
                    if (binding.has_namespace)
                    {
                        ItemOptions opts;
                        opts.sort_text = "2";
                        opts.insert_text = std::string{name};
                        opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                        add_item(items, name, symbol_kind_to_completion_kind(binding.namespace_sym.kind), replacement_range, std::move(opts));
                    }
                }
            }
        }

        void add_local_completions(std::vector<protocol::CompletionItem>& items, dcc::query::LocalContext const& local_ctx, std::string_view prefix,
                                   protocol::LspRange const* replacement_range)
        {
            for (auto const& local : local_ctx.locals)
            {
                if (!prefix.empty() && !local.name.starts_with(prefix))
                    continue;

                ItemOptions opts;
                opts.sort_text = "0";
                opts.insert_text = std::string{local.name};
                opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                if (local.type)
                    opts.detail = format_type_str_local(local.type);

                protocol::CompletionItemKind kind = protocol::CompletionItemKind::Variable;
                if (local.kind == dcc::query::LocalSymbolKind::TemplateParam)
                    kind = protocol::CompletionItemKind::TypeParameter;

                add_item(items, local.name, kind, replacement_range, std::move(opts));
            }
        }

        static void find_asm_node_in_tu(dcc::session::CompilerSession const& session, dcc::sm::FileId fid, dcc::sm::Offset cursor_offset,
                                        dcc::ast::AsmStmt const*& out_stmt, dcc::ast::AsmExpr const*& out_expr);

        [[nodiscard]] bool try_asm_operand_completion(std::vector<protocol::CompletionItem>& items, dcc::session::CompilerSession const& session,
                                                      dcc::sm::FileId fid, dcc::sm::Offset cursor_offset, dcc::sm::SourceFile const& file,
                                                      std::optional<dcc::query::NodeAtLocation> const& node, std::string_view prefix,
                                                      protocol::LspRange const* replacement_range)
        {
            dcc::ast::AsmStmt const* asm_stmt = nullptr;
            dcc::ast::AsmExpr const* asm_expr = nullptr;

            if (node && node->stmt && node->stmt->kind == dcc::ast::StmtKind::Asm)
                asm_stmt = static_cast<dcc::ast::AsmStmt const*>(node->stmt);
            if (node && node->expr && node->expr->kind == dcc::ast::ExprKind::Asm)
                asm_expr = static_cast<dcc::ast::AsmExpr const*>(node->expr);

            if (!asm_stmt && !asm_expr)
                find_asm_node_in_tu(session, fid, cursor_offset, asm_stmt, asm_expr);

            auto const* operands_owner = asm_stmt ? &asm_stmt->operands : (asm_expr ? &asm_expr->operands : nullptr);
            auto const* spans_owner = asm_stmt ? &asm_stmt->placeholder_spans : (asm_expr ? &asm_expr->placeholder_spans : nullptr);
            auto const* tpl_range = asm_stmt ? &asm_stmt->template_range : (asm_expr ? &asm_expr->template_range : nullptr);

            if (!operands_owner || !spans_owner || !tpl_range || !tpl_range->valid())
                return false;

            if (cursor_offset < tpl_range->begin.offset || cursor_offset > tpl_range->end.offset)
                return false;

            auto content_base = tpl_range->begin.offset + 1;

            bool inside_placeholder = false;
            std::string partial_name;

            auto const text = file.text();

            for (auto const& span : *spans_owner)
            {
                if (span.kind == dcc::ast::AsmPlaceholderSpan::Kind::RegLiteral)
                    continue;

                dcc::sm::Offset span_start;
                dcc::sm::Offset span_end;
                if (span.raw_range.valid() && span.raw_range.begin.fileId == tpl_range->begin.fileId)
                {
                    span_start = span.raw_range.begin.offset;
                    span_end = span.raw_range.end.offset;
                }
                else
                {
                    span_start = content_base + static_cast<dcc::sm::Offset>(span.byte_offset);
                    span_end = span_start + static_cast<dcc::sm::Offset>(span.byte_length);
                }

                auto name_start = span_start + 2;
                if (span_end <= static_cast<dcc::sm::Offset>(text.size()))
                {
                    auto bracket = text.find('[', static_cast<std::size_t>(span_start));
                    if (bracket != std::string_view::npos && bracket < static_cast<std::size_t>(span_end))
                        name_start = static_cast<dcc::sm::Offset>(bracket) + 1;
                }

                if (cursor_offset >= span_start && cursor_offset <= span_end)
                {
                    inside_placeholder = true;
                    if (cursor_offset > name_start)
                    {
                        auto len = static_cast<std::size_t>(cursor_offset - name_start);
                        if (name_start + static_cast<dcc::sm::Offset>(len) <= static_cast<dcc::sm::Offset>(text.size()))
                            partial_name = std::string{text.substr(static_cast<std::size_t>(name_start), len)};
                    }
                    break;
                }

                if (cursor_offset >= span_start && cursor_offset <= name_start)
                {
                    inside_placeholder = true;
                    partial_name.clear();
                    break;
                }
            }

            if (!inside_placeholder)
            {
                auto rel_offset = static_cast<std::int64_t>(cursor_offset) - static_cast<std::int64_t>(content_base);
                if (rel_offset >= 2)
                {
                    auto search_start = static_cast<std::size_t>(content_base);
                    auto search_end = static_cast<std::size_t>(cursor_offset);
                    if (search_end <= text.size() && search_start < search_end)
                    {
                        auto search_area = text.substr(search_start, search_end - search_start);
                        auto last_pct = search_area.rfind('%');
                        if (last_pct != std::string_view::npos && last_pct + 1 < search_area.size() && search_area[last_pct + 1] == '[')
                        {
                            inside_placeholder = true;
                            auto name_start = last_pct + 2;
                            if (name_start < search_area.size())
                                partial_name = std::string{search_area.substr(name_start)};
                        }
                    }
                }
            }

            if (!inside_placeholder)
                return false;

            for (auto const& op : *operands_owner)
            {
                if (!prefix.empty() && !op.placeholder.starts_with(prefix))
                    continue;

                if (!partial_name.empty() && !op.placeholder.starts_with(partial_name))
                    continue;

                std::string detail;
                switch (op.direction)
                {
                    case dcc::ast::AsmOperandDirection::Out:
                        detail = "out";
                        break;
                    case dcc::ast::AsmOperandDirection::In:
                        detail = "in";
                        break;
                    case dcc::ast::AsmOperandDirection::InOut:
                        detail = "inout";
                        break;
                }
                detail += ", ";
                switch (op.placement_kind)
                {
                    case dcc::ast::AsmPlacementKind::Reg:
                        detail += op.reg_name.empty() ? "reg" : std::format("reg[{}]", op.reg_name);
                        break;
                    case dcc::ast::AsmPlacementKind::RegPair:
                        detail += std::format("pair[{},{}]", op.reg_name, op.reg_name2);
                        break;
                    case dcc::ast::AsmPlacementKind::Mem:
                        detail += "mem";
                        break;
                    case dcc::ast::AsmPlacementKind::Imm:
                        detail += "imm";
                        break;
                }

                ItemOptions opts;
                opts.sort_text = "0";
                opts.detail = std::move(detail);
                opts.insert_text = std::string{op.placeholder};
                opts.insert_text_format = protocol::InsertTextFormat::PlainText;
                add_item(items, op.placeholder, protocol::CompletionItemKind::Variable, replacement_range, std::move(opts));
            }

            // TODO(asm): instruction mnemonic completion
            return !items.empty();
        }

        static void find_asm_node_in_tu(dcc::session::CompilerSession const& session, dcc::sm::FileId fid, dcc::sm::Offset cursor_offset,
                                        dcc::ast::AsmStmt const*& out_stmt, dcc::ast::AsmExpr const*& out_expr)
        {
            dcc::ast::TranslationUnit const* tu = nullptr;
            {
                auto* sema_ctx = session.sema_context();
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
                tu = const_cast<dcc::session::CompilerSession&>(session).parse_file(fid);

            if (!tu)
                return;

            auto in_template = [&](dcc::sm::SourceRange const& tpl_range) -> bool {
                if (!tpl_range.valid())
                    return false;

                if (tpl_range.begin.fileId != fid)
                    return false;

                return cursor_offset >= tpl_range.begin.offset && cursor_offset <= tpl_range.end.offset;
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
                if (!stmt || out_stmt)
                    return;

                if (stmt->kind == dcc::ast::StmtKind::Asm)
                {
                    auto const* s = static_cast<dcc::ast::AsmStmt const*>(stmt);
                    if (in_template(s->template_range))
                    {
                        out_stmt = s;
                        return;
                    }
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
                if (!expr || out_expr)
                    return;

                if (expr->kind == dcc::ast::ExprKind::Asm)
                {
                    auto const* e = static_cast<dcc::ast::AsmExpr const*>(expr);
                    if (in_template(e->template_range))
                    {
                        out_expr = e;
                        return;
                    }
                }
            };

            for (auto* d : tu->imports)
                walk_decls(d);
            for (auto* d : tu->decls)
                walk_decls(d);
        }

    } // anonymous namespace

    protocol::CompletionList compute_completions(dcc::session::CompilerSession const& session, std::string_view uri, dcc::sm::Position cursor)
    {
        protocol::CompletionList result;
        result.isIncomplete = false;

        auto const& sm = session.source_manager();
        auto fid_opt = sm.find_by_uri(uri);
        if (!fid_opt)
        {
            std::println(std::cerr, "[dccd] compute_completions: cannot find file for uri {}", uri);
            return result;
        }
        auto fid = *fid_opt;

        auto const* file = sm.get(fid);
        if (!file)
        {
            std::println(std::cerr, "[dccd] compute_completions: no source file for fid");
            return result;
        }

        auto loc_result = sm.lsp_position_to_location(fid, cursor);
        if (!loc_result)
        {
            std::println(std::cerr, "[dccd] compute_completions: cannot convert position");
            return result;
        }
        auto cursor_offset = loc_result->offset;

        auto ctx = detect_context(*file, cursor_offset);

        auto* sema_ctx = session.sema_context();
        if (!sema_ctx)
            return result;

        auto const* module = find_module_by_uri(*sema_ctx, sm, uri);
        if (!module)
            module = find_module_by_file_id(*sema_ctx, fid);

        if (!module)
        {
            std::println(std::cerr, "[dccd] compute_completions: no module for uri {}", uri);
            return result;
        }

        auto replacement_range = compute_replacement_range(sm, fid, ctx);
        auto region = dcc::query::source_region_at(session, fid, cursor_offset);

        if (ctx.trigger == CompletionContext::Trigger::None)
            if (auto node = dcc::query::find_node_at(session, *loc_result))
                if (node->type_expr)
                    ctx.kind = CompletionContext::ContextKind::Type;

        switch (ctx.trigger)
        {
            case CompletionContext::Trigger::Dot:
                if (!region.in_string_or_comment())
                    handle_member_access_completion(result.items, session, module, fid, *file, ctx, *loc_result,
                                                    replacement_range ? &*replacement_range : nullptr);
                break;
            case CompletionContext::Trigger::ColonColon:
                if (!region.in_string_or_comment())
                    handle_namespace_completion(result.items, session, module, ctx, file->text(), replacement_range ? &*replacement_range : nullptr);
                break;
            case CompletionContext::Trigger::None: {
                auto node = dcc::query::find_node_at(session, fid, cursor);
                if (!node || !node->has_ast_node())
                    std::println(std::cerr, "[dccd] compute_completions: no AST node at cursor position");

                if (try_asm_operand_completion(result.items, session, fid, cursor_offset, *file, node, ctx.prefix,
                                               replacement_range ? &*replacement_range : nullptr))
                    break;

                if (region.in_string_or_comment())
                    break;

                auto local_ctx = dcc::query::collect_local_context(session, fid, *loc_result);

                add_local_completions(result.items, local_ctx, ctx.prefix, replacement_range ? &*replacement_range : nullptr);

                if (module->own_scope)
                    add_scope_symbols_for_context(result.items, *module->own_scope, ctx.prefix, ctx.kind, file->text(), ctx.token_end,
                                                  replacement_range ? &*replacement_range : nullptr);

                add_keyword_completions(result.items, ctx.kind, ctx.prefix, replacement_range ? &*replacement_range : nullptr);
                break;
            }
        }
        dedup_and_sort(result.items);
        if (!result.items.empty())
            result.items.front().preselect = true;

        return result;
    }

} // namespace dccd::completion
