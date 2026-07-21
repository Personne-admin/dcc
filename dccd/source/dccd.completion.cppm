export module dccd.completion;

import std;
import dcc.sm;
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

        void dedup_and_sort(std::vector<protocol::CompletionItem>& items)
        {
            std::ranges::sort(items, [](protocol::CompletionItem const& a, protocol::CompletionItem const& b) {
                auto cmp = a.label <=> b.label;
                if (cmp != 0)
                    return cmp < 0;
                return static_cast<std::int32_t>(a.kind) < static_cast<std::int32_t>(b.kind);
            });

            auto [first, last] = std::ranges::unique(
                items, [](protocol::CompletionItem const& a, protocol::CompletionItem const& b) { return a.label == b.label && a.kind == b.kind; });

            items.erase(first, last);
        }

        void add_item(std::vector<protocol::CompletionItem>& items, std::string_view label, protocol::CompletionItemKind kind,
                      std::optional<std::string> detail = std::nullopt)
        {
            protocol::CompletionItem item;
            item.label = std::string{label};
            item.kind = kind;
            item.detail = std::move(detail);
            items.push_back(std::move(item));
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

        void add_field_completions(std::vector<protocol::CompletionItem>& items, dcc::ast::Decl const& decl)
        {
            using dcc::ast::DeclKind;
            switch (decl.kind)
            {
                case DeclKind::Struct: {
                    auto const* sd = static_cast<dcc::ast::StructDecl const*>(&decl);
                    for (auto const& f : sd->fields)
                    {
                        std::optional<std::string> detail;
                        if (f.type && f.type->sema.canonical)
                        {
                            auto ty = dcc::sema::get_canonical(f.type->sema);
                            detail = format_type_str_local(ty);
                        }
                        add_item(items, f.name, field_kind(), std::move(detail));
                    }
                    break;
                }
                case DeclKind::Union: {
                    auto const* ud = static_cast<dcc::ast::UnionDecl const*>(&decl);
                    for (auto const& f : ud->fields)
                    {
                        std::optional<std::string> detail;
                        if (f.type && f.type->sema.canonical)
                        {
                            auto ty = dcc::sema::get_canonical(f.type->sema);
                            detail = format_type_str_local(ty);
                        }
                        add_item(items, f.name, field_kind(), std::move(detail));
                    }
                    break;
                }
                case DeclKind::Enum: {
                    auto const* ed = static_cast<dcc::ast::EnumDecl const*>(&decl);
                    for (auto const& v : ed->variants)
                        add_item(items, v.name, protocol::CompletionItemKind::EnumMember);
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

        void add_function_completions_from_scope(std::vector<protocol::CompletionItem>& items, dcc::sema::Scope const& scope,
                                                 dcc::types::Type const* receiver_type, std::string_view prefix)
        {
            for (auto const& [name, binding] : scope.bindings())
            {
                if (name.empty() || (!prefix.empty() && !name.starts_with(prefix)))
                    continue;

                for (auto const& vs : binding.value_syms)
                {
                    if (vs.kind != dcc::sema::SymbolKind::Function)
                        continue;

                    auto const* fd = vs.decl ? dcc::ast::node_cast<dcc::ast::FuncDecl>(vs.decl) : nullptr;
                    if (!fd)
                        continue;

                    bool include = true;
                    if (receiver_type && !fd->params.empty() && fd->params[0].type && fd->params[0].type->sema.canonical)
                    {
                        auto first_param_type = dcc::sema::get_canonical(fd->params[0].type->sema);
                        include = (first_param_type == receiver_type);
                    }

                    if (include)
                        add_item(items, name, protocol::CompletionItemKind::Method);
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

        [[nodiscard]] dcc::types::Type const* resolve_type_in_tu(dcc::sema::ModuleInfo const& module, std::string_view name)
        {
            if (!module.tu)
                return nullptr;

            auto type_from_var = [](dcc::ast::VarDecl const* vd) -> dcc::types::Type const* {
                if (vd && vd->type && vd->type->sema.canonical)
                    return dcc::sema::get_canonical(vd->type->sema);

                return nullptr;
            };

            std::function<dcc::types::Type const*(dcc::ast::Stmt const*)> walk_stmt_for_type;
            std::function<dcc::types::Type const*(dcc::ast::Block const&)> walk_block_for_type;
            std::function<dcc::types::Type const*(dcc::ast::Decl const*)> walk_decl_for_type;

            walk_stmt_for_type = [&](dcc::ast::Stmt const* stmt) -> dcc::types::Type const* {
                if (!stmt)
                    return nullptr;

                using dcc::ast::StmtKind;
                switch (stmt->kind)
                {
                    case StmtKind::DeclStmt: {
                        auto const* ds = static_cast<dcc::ast::DeclStmt const*>(stmt);
                        if (ds->decl)
                        {
                            if (auto const* vd = dcc::ast::node_cast<dcc::ast::VarDecl>(ds->decl))
                                if (vd->name == name)
                                    return type_from_var(vd);

                            if (auto t = walk_decl_for_type(ds->decl))
                                return t;
                        }
                        break;
                    }
                    case StmtKind::While: {
                        auto const* ws = static_cast<dcc::ast::WhileStmt const*>(stmt);
                        if (auto t = walk_block_for_type(ws->body))
                            return t;
                        break;
                    }
                    case StmtKind::DoWhile: {
                        auto const* dws = static_cast<dcc::ast::DoWhileStmt const*>(stmt);
                        if (auto t = walk_block_for_type(dws->body))
                            return t;
                        break;
                    }
                    case StmtKind::For: {
                        auto const* fs = static_cast<dcc::ast::ForStmt const*>(stmt);
                        if (fs->init)
                            if (auto t = walk_stmt_for_type(fs->init))
                                return t;

                        if (auto t = walk_block_for_type(fs->body))
                            return t;
                        break;
                    }
                    case StmtKind::ForIn: {
                        auto const* fis = static_cast<dcc::ast::ForInStmt const*>(stmt);
                        if (auto t = walk_block_for_type(fis->body))
                            return t;
                        break;
                    }
                    case StmtKind::Defer: {
                        auto const* ds = static_cast<dcc::ast::DeferStmt const*>(stmt);
                        if (ds->body)
                            if (auto t = walk_stmt_for_type(ds->body))
                                return t;

                        break;
                    }
                    case StmtKind::StaticIf: {
                        auto const* sis = static_cast<dcc::ast::StaticIfStmt const*>(stmt);
                        if (auto t = walk_block_for_type(sis->then_block))
                            return t;

                        if (sis->else_branch)
                            if (auto t = walk_stmt_for_type(sis->else_branch))
                                return t;

                        break;
                    }
                    case StmtKind::StaticMatch: {
                        auto const* sms = static_cast<dcc::ast::StaticMatchStmt const*>(stmt);
                        for (auto const& arm : sms->arms)
                        {
                            if (arm.body)
                                ;
                        }
                        break;
                    }
                    case StmtKind::Ambiguous: {
                        auto const* as = static_cast<dcc::ast::AmbiguousStmt const*>(stmt);
                        if (as->as_decl)
                        {
                            if (auto const* vd = dcc::ast::node_cast<dcc::ast::VarDecl>(as->as_decl))
                                if (vd->name == name)
                                    return type_from_var(vd);

                            if (auto t = walk_decl_for_type(as->as_decl))
                                return t;
                        }
                        break;
                    }
                    default:
                        break;
                }
                return nullptr;
            };

            walk_block_for_type = [&](dcc::ast::Block const& block) -> dcc::types::Type const* {
                for (auto* s : block.stmts)
                    if (auto t = walk_stmt_for_type(s))
                        return t;

                return nullptr;
            };

            walk_decl_for_type = [&](dcc::ast::Decl const* decl) -> dcc::types::Type const* {
                if (!decl)
                    return nullptr;

                using dcc::ast::DeclKind;
                switch (decl->kind)
                {
                    case DeclKind::Func: {
                        auto const* fd = static_cast<dcc::ast::FuncDecl const*>(decl);
                        for (auto const& p : fd->params)
                            if (p.name == name && p.type && p.type->sema.canonical)
                                return dcc::sema::get_canonical(p.type->sema);

                        if (fd->body.has_value())
                            if (auto t = walk_block_for_type(*fd->body))
                                return t;

                        break;
                    }
                    case DeclKind::Var: {
                        auto const* vd = static_cast<dcc::ast::VarDecl const*>(decl);
                        if (vd->name == name)
                            return type_from_var(vd);

                        break;
                    }
                    default:
                        break;
                }
                return nullptr;
            };

            if (module.own_scope)
            {
                auto syms = module.own_scope->lookup_values(name);
                if (!syms.empty())
                {
                    auto const& sym = syms.front();
                    if (sym.decl)
                        if (auto const* vd = dcc::ast::node_cast<dcc::ast::VarDecl>(sym.decl))
                            return type_from_var(vd);
                }
            }

            for (auto* d : module.tu->decls)
                if (auto t = walk_decl_for_type(d))
                    return t;

            return nullptr;
        }

        void handle_member_access_completion(std::vector<protocol::CompletionItem>& items, dcc::session::CompilerSession const& session,
                                             dcc::sema::ModuleInfo const* module, std::string_view prefix, dcc::sm::FileId fid, dcc::sm::Location cursor_loc,
                                             dcc::sm::Offset trigger_offset)
        {
            dcc::types::Type const* receiver_type = nullptr;
            auto const& sm = session.source_manager();
            auto const* file = sm.get(fid);

            if (module && file)
            {
                if (auto pos_result = sm.location_to_lsp_position(cursor_loc))
                {
                    auto pos = *pos_result;
                    for (int offset = 1; offset <= 3 && !receiver_type; ++offset)
                    {
                        if (pos.character >= static_cast<std::uint32_t>(offset))
                        {
                            auto query_pos = dcc::sm::Position{pos.line, pos.character - static_cast<std::uint32_t>(offset)};
                            auto node = dcc::query::find_node_at(session, module->file_id, query_pos);
                            if (node && node->expr)
                                receiver_type = node->resolved_type;
                        }
                    }
                }
            }

            if (!receiver_type && file && module && trigger_offset > 0)
            {
                auto receiver_name = extract_receiver_name(*file, trigger_offset);
                if (!receiver_name.empty())
                    receiver_type = resolve_type_in_tu(*module, receiver_name);
            }

            if (receiver_type)
            {
                if (auto const* decl = nominal_decl(receiver_type))
                    add_field_completions(items, *decl);

                if (module && module->own_scope)
                    add_function_completions_from_scope(items, *module->own_scope, receiver_type, prefix);
                if (module && module->export_scope && module->export_scope != module->own_scope)
                    add_function_completions_from_scope(items, *module->export_scope, receiver_type, prefix);
            }
            else
            {
                if (module && module->own_scope)
                {
                    for (auto const& [name, binding] : module->own_scope->bindings())
                    {
                        if (name.empty() || (!prefix.empty() && !name.starts_with(prefix)))
                            continue;

                        for (auto const& vs : binding.value_syms)
                            if (vs.kind == dcc::sema::SymbolKind::Function)
                                add_item(items, name, protocol::CompletionItemKind::Method);
                    }
                }
            }
        }

        void handle_namespace_completion(std::vector<protocol::CompletionItem>& items, dcc::session::CompilerSession const& session,
                                         dcc::sema::ModuleInfo const* module, std::string_view prefix)
        {
            auto* sema_ctx = session.sema_context();
            if (!sema_ctx || !module)
                return;

            auto& graph = const_cast<dcc::sema::SemaContext*>(sema_ctx)->graph();

            dcc::sema::Scope const* target_scope = module->own_scope;

            for (auto const& imp : module->imports)
            {
                if (imp.target && imp.target->export_scope)
                {
                    for (auto const& [name, binding] : imp.target->export_scope->bindings())
                    {
                        if (name.empty() || (!prefix.empty() && !name.starts_with(prefix)))
                            continue;
                        if (binding.has_type)
                            add_item(items, name, symbol_kind_to_completion_kind(binding.type_sym.kind));
                        for (auto const& vs : binding.value_syms)
                            add_item(items, name, symbol_kind_to_completion_kind(vs.kind));
                        if (binding.has_namespace)
                            add_item(items, name, symbol_kind_to_completion_kind(binding.namespace_sym.kind));
                    }
                }
            }

            if (target_scope)
            {
                for (auto const& [name, binding] : target_scope->bindings())
                {
                    if (name.empty() || (!prefix.empty() && !name.starts_with(prefix)))
                        continue;
                    if (binding.has_type)
                        add_item(items, name, symbol_kind_to_completion_kind(binding.type_sym.kind));
                    for (auto const& vs : binding.value_syms)
                        add_item(items, name, symbol_kind_to_completion_kind(vs.kind));
                    if (binding.has_namespace)
                        add_item(items, name, symbol_kind_to_completion_kind(binding.namespace_sym.kind));
                }
            }

            for (auto const& other_mod : graph.all())
            {
                if (other_mod.get() == module)
                    continue;

                auto const& segments = other_mod->canonical_path.segments();
                if (!segments.empty())
                {
                    std::string_view mod_name = segments[0];
                    if (!mod_name.empty() && (prefix.empty() || mod_name.starts_with(prefix)))
                        add_item(items, mod_name, protocol::CompletionItemKind::Module);
                }
            }
        }

        struct CompletionContext
        {
            enum class Trigger : std::uint8_t
            {
                None,
                Dot,
                ColonColon
            };
            Trigger trigger{Trigger::None};
            std::string prefix;
            std::size_t trigger_offset{};
        };

        [[nodiscard]] CompletionContext detect_context(dcc::sm::SourceFile const& file, dcc::sm::Offset cursor_offset)
        {
            CompletionContext ctx;
            auto text = file.text();
            if (cursor_offset == 0 || cursor_offset > static_cast<dcc::sm::Offset>(text.size()))
                return ctx;

            auto pos = static_cast<std::ptrdiff_t>(cursor_offset) - 1;
            bool have_partial = false;

            while (pos >= 0)
            {
                char c = text[static_cast<std::size_t>(pos)];
                if (c == '.')
                {
                    ctx.trigger = CompletionContext::Trigger::Dot;
                    ctx.trigger_offset = static_cast<std::size_t>(pos);
                    break;
                }
                if (c == ':')
                {
                    if (pos > 0 && text[static_cast<std::size_t>(pos) - 1] == ':')
                    {
                        ctx.trigger = CompletionContext::Trigger::ColonColon;
                        ctx.trigger_offset = static_cast<std::size_t>(pos) - 1;
                    }
                    break;
                }

                if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' || c == ';' ||
                    c == ',')
                {
                    ctx.prefix.clear();
                    return ctx;
                }

                if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                    have_partial = true;

                --pos;
            }

            if (ctx.trigger != CompletionContext::Trigger::None)
            {
                auto trigger_end = ctx.trigger_offset + ((ctx.trigger == CompletionContext::Trigger::ColonColon) ? 2 : 1);
                if (trigger_end < cursor_offset)
                {
                    auto prefix_len = cursor_offset - trigger_end;
                    ctx.prefix = std::string{text.substr(trigger_end, prefix_len)};
                }
            }
            else if (have_partial)
            {
                auto start_pos = static_cast<std::size_t>(pos) + 1;
                while (start_pos < cursor_offset)
                {
                    auto c = text[start_pos];
                    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                    {
                        auto end = start_pos;
                        while (end < cursor_offset)
                        {
                            auto ec = text[end];
                            if (!(ec == '_' || (ec >= 'a' && ec <= 'z') || (ec >= 'A' && ec <= 'Z') || (ec >= '0' && ec <= '9')))
                                break;

                            ++end;
                        }
                        ctx.prefix = std::string{text.substr(start_pos, end - start_pos)};
                        break;
                    }
                    ++start_pos;
                }
            }

            return ctx;
        }

        static void find_asm_node_in_tu(dcc::session::CompilerSession const& session, dcc::sm::FileId fid, dcc::sm::Offset cursor_offset,
                                        dcc::ast::AsmStmt const*& out_stmt, dcc::ast::AsmExpr const*& out_expr);

        [[nodiscard]] bool try_asm_operand_completion(std::vector<protocol::CompletionItem>& items, dcc::session::CompilerSession const& session,
                                                      dcc::sm::FileId fid, dcc::sm::Offset cursor_offset, dcc::sm::SourceFile const& file,
                                                      std::optional<dcc::query::NodeAtLocation> const& node, std::string_view prefix)
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

            for (auto const& span : *spans_owner)
            {
                if (span.kind == dcc::ast::AsmPlaceholderSpan::Kind::RegLiteral)
                    continue;

                auto span_start = content_base + static_cast<dcc::sm::Offset>(span.byte_offset);
                auto span_end = span_start + static_cast<dcc::sm::Offset>(span.byte_length);

                if (cursor_offset >= span_start && cursor_offset <= span_end)
                {
                    inside_placeholder = true;
                    auto after_bracket = span_start + 2;
                    if (cursor_offset > after_bracket)
                    {
                        auto len = static_cast<std::size_t>(cursor_offset - after_bracket);
                        if (after_bracket + static_cast<dcc::sm::Offset>(len) <= static_cast<dcc::sm::Offset>(file.text().size()))
                            partial_name = std::string{file.text().substr(after_bracket, len)};
                    }
                    break;
                }

                if (cursor_offset >= span_start && cursor_offset < span_start + 2)
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
                    auto text = file.text();
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

                add_item(items, std::string{op.placeholder}, protocol::CompletionItemKind::Variable, std::move(detail));
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
        {
            if (ctx.trigger == CompletionContext::Trigger::None && !ctx.prefix.empty())
                ;
            return result;
        }

        auto const* module = find_module_by_uri(*sema_ctx, sm, uri);
        if (!module)
            module = find_module_by_file_id(*sema_ctx, fid);

        if (!module)
        {
            std::println(std::cerr, "[dccd] compute_completions: no module for uri {}", uri);
            return result;
        }

        switch (ctx.trigger)
        {
            case CompletionContext::Trigger::Dot:
                handle_member_access_completion(result.items, session, module, ctx.prefix, fid, *loc_result, static_cast<dcc::sm::Offset>(ctx.trigger_offset));
                break;
            case CompletionContext::Trigger::ColonColon:
                handle_namespace_completion(result.items, session, module, ctx.prefix);
                break;
            case CompletionContext::Trigger::None: {
                auto node = dcc::query::find_node_at(session, fid, cursor);
                if (!node || !node->has_ast_node())
                    std::println(std::cerr, "[dccd] compute_completions: no AST node at cursor position");

                if (try_asm_operand_completion(result.items, session, fid, cursor_offset, *file, node, ctx.prefix))
                    break;

                if (module->own_scope)
                {
                    for (auto const& [name, binding] : module->own_scope->bindings())
                    {
                        if (name.empty() || (!ctx.prefix.empty() && !name.starts_with(ctx.prefix)))
                            continue;
                        if (binding.has_type)
                            add_item(result.items, name, symbol_kind_to_completion_kind(binding.type_sym.kind));
                        for (auto const& vs : binding.value_syms)
                            add_item(result.items, name, symbol_kind_to_completion_kind(vs.kind));
                        if (binding.has_namespace)
                            add_item(result.items, name, symbol_kind_to_completion_kind(binding.namespace_sym.kind));
                    }
                }
                break;
            }
        }

        dedup_and_sort(result.items);
        return result;
    }

} // namespace dccd::completion
