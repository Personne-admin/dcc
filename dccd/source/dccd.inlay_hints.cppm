export module dccd.inlay_hints;

import std;
import dcc.sm;
import dcc.ast;
import dcc.types;
import dcc.sema;
import dcc.sema.type_helpers;
import dccd.protocol;
import dcc.ast.visitor;

export namespace dccd::inlay_hints
{
    using TypeFormatter = std::function<std::string(dcc::types::Type const*)>;
    using CancelCheck = std::function<bool()>;

    [[nodiscard]] std::vector<protocol::InlayHint> collect_inlay_hints(dcc::sm::SourceManager const& sm, dcc::ast::TranslationUnit const* tu,
                                                                       dcc::sm::SourceRange request_range, TypeFormatter format_type,
                                                                       protocol::InlayHintOptions const& options = {}, CancelCheck const& cancel = {});

} // namespace dccd::inlay_hints

module :private;

namespace dccd::inlay_hints
{
    namespace
    {
        [[nodiscard]] bool range_overlaps(dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) noexcept
        {
            if (!a.valid() || !b.valid())
                return false;

            if (a.begin.fileId != b.begin.fileId)
                return false;

            return a.begin.offset < b.end.offset && b.begin.offset < a.end.offset;
        }

        [[nodiscard]] bool range_before(dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) noexcept
        {
            if (!a.valid() || !b.valid())
                return false;

            if (a.begin.fileId != b.begin.fileId)
                return false;

            return a.end.offset <= b.begin.offset;
        }

        [[nodiscard]] bool range_after(dcc::sm::SourceRange const& a, dcc::sm::SourceRange const& b) noexcept
        {
            if (!a.valid() || !b.valid())
                return false;

            if (a.begin.fileId != b.begin.fileId)
                return false;

            return a.begin.offset >= b.end.offset;
        }

        [[nodiscard]] std::optional<protocol::LspPosition> src_to_lsp(dcc::sm::SourceManager const& sm, dcc::sm::Location loc)
        {
            auto pos = sm.location_to_lsp_position(loc);
            if (!pos)
                return std::nullopt;

            protocol::LspPosition lsp;
            lsp.line = pos->line;
            lsp.character = pos->character;
            return lsp;
        }

        [[nodiscard]] bool is_simple_identifier_matching(dcc::ast::Expr const* expr, std::string_view name) noexcept
        {
            if (!expr)
                return false;

            if (auto const* id = dcc::ast::node_cast<dcc::ast::IdentExpr>(expr))
                return id->name == name;

            if (auto const* pe = dcc::ast::node_cast<dcc::ast::PathExpr>(expr))
                if (pe->explicit_enum_args.empty() && pe->path.is_simple())
                    return pe->path.simple_name() == name;

            return false;
        }

        struct TraversalCancelled
        {
        };

        struct Collector : dcc::ast::RecursiveAstVisitor
        {
            dcc::sm::SourceManager const& sm;
            dcc::sm::SourceRange request_range;
            TypeFormatter format_type;
            protocol::InlayHintOptions options;
            CancelCheck cancel;
            std::vector<protocol::InlayHint> hints;
            std::uint32_t visit_count{0};
            static constexpr std::uint32_t kCancelInterval = 256;

            Collector(dcc::sm::SourceManager const& s, dcc::sm::SourceRange req_range, TypeFormatter fmt, protocol::InlayHintOptions const& opts,
                      CancelCheck const& c)
                : sm{s}, request_range{req_range}, format_type{std::move(fmt)}, options{opts}, cancel{c}
            {
            }

            void tick()
            {
                if (!cancel)
                    return;
                if ((++visit_count & (kCancelInterval - 1)) == 0)
                    if (cancel())
                        throw TraversalCancelled{};
            }

            void emit_type_hint(dcc::sm::Location pos, std::string label)
            {
                if (!options.typeHints)
                    return;

                auto lsp_pos = src_to_lsp(sm, pos);
                if (!lsp_pos)
                    return;

                protocol::InlayHint h;
                h.position = *lsp_pos;
                h.label = std::move(label);
                h.kind = protocol::InlayHintKind::Type;
                h.paddingLeft = false;
                hints.push_back(std::move(h));
            }

            void emit_param_hint(dcc::sm::Location pos, std::string_view param_name)
            {
                if (!options.parameterHints)
                    return;

                auto lsp_pos = src_to_lsp(sm, pos);
                if (!lsp_pos)
                    return;

                protocol::InlayHint h;
                h.position = *lsp_pos;
                h.label = std::format("{}:", param_name);
                h.kind = protocol::InlayHintKind::Parameter;
                h.paddingRight = true;
                hints.push_back(std::move(h));
            }

            struct ParamLayout
            {
                std::size_t num_value_tparams{0};
                bool has_pack{false};
                std::size_t non_pack_count{0};
                std::size_t param_count{0};
            };

            [[nodiscard]] ParamLayout layout_of(dcc::ast::FuncDecl const* target, dcc::ast::CallExpr const* e) const
            {
                ParamLayout layout;
                layout.param_count = target->params.size();

                layout.non_pack_count = layout.param_count;
                if (!target->params.empty() && dcc::sema::is_func_param_sema_pack(target->params.back(), *target))
                {
                    layout.has_pack = true;
                    layout.non_pack_count = target->params.size() - 1;
                }

                dcc::ast::FuncDecl const* generic = target;
                if (generic->template_params.empty())
                    if (e->sema.resolved_decl && e->sema.resolved_decl->kind == dcc::ast::DeclKind::Func)
                        generic = static_cast<dcc::ast::FuncDecl const*>(e->sema.resolved_decl);

                bool explicit_instantiation = e->callee && e->callee->kind == dcc::ast::ExprKind::TemplateInst;
                if (!explicit_instantiation)
                    for (auto const& tp : generic->template_params)
                        if (tp.value_type)
                            ++layout.num_value_tparams;

                return layout;
            }

            void emit_call_param_hints(dcc::ast::CallExpr const* e, dcc::ast::FuncDecl const* target, bool is_ufcs)
            {
                auto layout = layout_of(target, e);
                std::size_t const ufcs_offset = is_ufcs ? 1u : 0u;

                for (std::size_t i = 0; i < e->args.size(); ++i)
                {
                    auto* arg = e->args[i];
                    if (!arg)
                        continue;

                    if (i < layout.num_value_tparams)
                        continue;

                    std::size_t param_idx = i - layout.num_value_tparams + ufcs_offset;
                    if (layout.has_pack && param_idx >= layout.non_pack_count)
                        param_idx = layout.non_pack_count;

                    if (param_idx >= layout.param_count)
                        continue;

                    auto const& fp = target->params[param_idx];
                    if (fp.name.empty())
                        continue;

                    if (options.suppressParameterNameMatches && is_simple_identifier_matching(arg, fp.name))
                        continue;

                    if (range_overlaps(arg->range, request_range))
                        emit_param_hint(arg->range.begin, fp.name);
                }
            }

            void visitDecl(dcc::ast::Decl const* decl) override;
            void visitStmt(dcc::ast::Stmt const* stmt) override;
            void visitExpr(dcc::ast::Expr const* expr) override;
            void visitBlock(dcc::ast::Block const& block) override;
            void visitMatchArm(dcc::ast::MatchArm const& arm) override;
        };

        void Collector::visitBlock(dcc::ast::Block const& block)
        {
            tick();

            if (!range_overlaps(block.range, request_range))
                return;

            dcc::ast::RecursiveAstVisitor::visitBlock(block);
        }

        void Collector::visitMatchArm(dcc::ast::MatchArm const& arm)
        {
            if (arm.body)
                visitExpr(arm.body);
        }

        void Collector::visitExpr(dcc::ast::Expr const* expr)
        {
            tick();

            if (!expr)
                return;

            if (range_before(expr->range, request_range) || range_after(expr->range, request_range))
                return;

            switch (expr->kind)
            {
                case dcc::ast::ExprKind::Call: {
                    auto* e = static_cast<dcc::ast::CallExpr const*>(expr);

                    dcc::ast::FuncDecl const* target = nullptr;
                    bool is_ufcs = e->sema.ufcs_callee != nullptr;

                    if (e->sema.resolved_specialization)
                        target = e->sema.resolved_specialization;
                    else if (e->sema.ufcs_callee && e->sema.ufcs_callee->kind == dcc::ast::DeclKind::Func)
                        target = static_cast<dcc::ast::FuncDecl const*>(e->sema.ufcs_callee);
                    else if (e->sema.resolved_decl && e->sema.resolved_decl->kind == dcc::ast::DeclKind::Func)
                        target = static_cast<dcc::ast::FuncDecl const*>(e->sema.resolved_decl);

                    if (!target && e->callee)
                    {
                        if (e->callee->sema.resolved_specialization)
                            target = e->callee->sema.resolved_specialization;
                        else if (e->callee->sema.ufcs_callee && e->callee->sema.ufcs_callee->kind == dcc::ast::DeclKind::Func)
                            target = static_cast<dcc::ast::FuncDecl const*>(e->callee->sema.ufcs_callee);
                        else if (e->callee->sema.resolved_decl && e->callee->sema.resolved_decl->kind == dcc::ast::DeclKind::Func)
                            target = static_cast<dcc::ast::FuncDecl const*>(e->callee->sema.resolved_decl);
                    }

                    if (target)
                        emit_call_param_hints(e, target, is_ufcs);

                    if (e->callee)
                        visitExpr(e->callee);

                    for (auto* a : e->args)
                        if (a)
                            visitExpr(a);
                    break;
                }
                case dcc::ast::ExprKind::Match: {
                    auto* e2 = static_cast<dcc::ast::MatchExpr const*>(expr);
                    if (e2->operand)
                        visitExpr(e2->operand);
                    for (auto const& arm : e2->arms)
                        visitMatchArm(arm);
                    break;
                }
                case dcc::ast::ExprKind::StructLiteral: {
                    auto* e2 = static_cast<dcc::ast::StructLiteralExpr const*>(expr);
                    for (auto const& f : e2->fields)
                        if (f.value)
                            visitExpr(f.value);
                    break;
                }
                case dcc::ast::ExprKind::Lambda: {
                    auto* e = static_cast<dcc::ast::LambdaExpr const*>(expr);
                    if (e->synthesized_func)
                    {
                        auto const& synth = *e->synthesized_func;
                        for (std::size_t i = 0; i < e->params.size() && i < synth.params.size(); ++i)
                        {
                            auto const& lp = e->params[i];
                            if (lp.type != nullptr)
                                continue;

                            auto const& sp = synth.params[i];
                            if (!sp.type || !sp.type->sema.canonical)
                                continue;

                            auto const* canon = dcc::sema::get_canonical(sp.type->sema);
                            if (!canon || canon->kind == dcc::types::TypeKind::Error)
                                continue;

                            // Untyped lambda params are a bare name token.
                            auto const name_range = lp.range;
                            if (!name_range.valid() || !range_overlaps(name_range, request_range))
                                continue;

                            auto type_str = format_type(canon);
                            if (!type_str.empty())
                                emit_type_hint(name_range.end, std::format(": {}", type_str));
                        }
                    }
                    if (e->body)
                        visitExpr(e->body);
                    break;
                }
                default:
                    dcc::ast::RecursiveAstVisitor::visitExpr(expr);
                    break;
            }
        }

        void Collector::visitStmt(dcc::ast::Stmt const* stmt)
        {
            tick();

            if (!stmt)
                return;

            if (range_before(stmt->range, request_range) || range_after(stmt->range, request_range))
                return;

            switch (stmt->kind)
            {
                case dcc::ast::StmtKind::StaticMatch: {
                    auto* s = static_cast<dcc::ast::StaticMatchStmt const*>(stmt);
                    if (s->operand)
                        visitExpr(s->operand);
                    for (auto const& arm : s->arms)
                        visitMatchArm(arm);
                    break;
                }
                case dcc::ast::StmtKind::ForIn: {
                    auto* s = static_cast<dcc::ast::ForInStmt const*>(stmt);

                    if (!s->item_type && s->name_range.valid() && range_overlaps(s->name_range, request_range))
                    {
                        dcc::types::Type const* item_ty = reinterpret_cast<dcc::types::Type const*>(s->resolved_item_type);
                        dcc::types::Qual item_quals = dcc::types::Qual::None;
                        if (!item_ty && s->iterable)
                        {
                            auto* iterable_ty = dcc::sema::get_resolved_type(s->iterable->sema);
                            if (iterable_ty)
                            {
                                switch (iterable_ty->kind)
                                {
                                    case dcc::types::TypeKind::Array:
                                        item_ty = static_cast<dcc::types::ArrayType const*>(iterable_ty)->element;
                                        break;
                                    case dcc::types::TypeKind::RuntimeArray:
                                        item_ty = static_cast<dcc::types::RuntimeArrayType const*>(iterable_ty)->element;
                                        break;
                                    case dcc::types::TypeKind::Slice:
                                        item_ty = static_cast<dcc::types::SliceType const*>(iterable_ty)->element;
                                        item_quals = static_cast<dcc::types::SliceType const*>(iterable_ty)->element_quals;
                                        break;
                                    case dcc::types::TypeKind::Range:
                                        item_ty = static_cast<dcc::types::RangeType const*>(iterable_ty)->element;
                                        break;
                                    case dcc::types::TypeKind::RangeInclusive:
                                        item_ty = static_cast<dcc::types::RangeInclusiveType const*>(iterable_ty)->element;
                                        break;
                                    default:
                                        break;
                                }
                            }
                        }

                        if (s->by_reference && item_ty && item_ty->kind != dcc::types::TypeKind::Error)
                        {
                            std::string quals;
                            if (dcc::types::has_qual(item_quals, dcc::types::Qual::Const))
                                quals += "const ";
                            if (dcc::types::has_qual(item_quals, dcc::types::Qual::Volatile))
                                quals += "volatile ";
                            if (dcc::types::has_qual(item_quals, dcc::types::Qual::Restrict))
                                quals += "restrict ";

                            auto elem_str = format_type(item_ty);
                            if (!elem_str.empty())
                                emit_type_hint(s->name_range.end, std::format(": {}{}*", quals, elem_str));
                        }

                        else if (item_ty && item_ty->kind != dcc::types::TypeKind::Error)
                        {
                            auto type_str = format_type(item_ty);
                            if (!type_str.empty())
                                emit_type_hint(s->name_range.end, std::format(": {}", type_str));
                        }
                    }

                    if (s->iterable)
                        visitExpr(s->iterable);
                    visitBlock(s->body);
                    break;
                }
                default:
                    dcc::ast::RecursiveAstVisitor::visitStmt(stmt);
                    break;
            }
        }

        void Collector::visitDecl(dcc::ast::Decl const* decl)
        {
            tick();

            if (!decl)
                return;

            if (range_before(decl->range, request_range) || range_after(decl->range, request_range))
                return;

            switch (decl->kind)
            {
                case dcc::ast::DeclKind::Var: {
                    auto* d = static_cast<dcc::ast::VarDecl const*>(decl);

                    if (d->type == nullptr)
                    {
                        dcc::types::Type const* inferred_type = nullptr;
                        if (d->init)
                            inferred_type = dcc::sema::get_resolved_type(d->init->sema);

                        if (inferred_type && d->name_range.valid() && range_overlaps(d->name_range, request_range))
                        {
                            auto type_str = format_type(inferred_type);
                            if (!type_str.empty())
                                emit_type_hint(d->name_range.end, std::format(": {}", type_str));
                        }
                    }

                    if (d->init)
                        visitExpr(d->init);
                    break;
                }
                case dcc::ast::DeclKind::Func: {
                    auto* d = static_cast<dcc::ast::FuncDecl const*>(decl);
                    if (d->body.has_value())
                        visitBlock(*d->body);
                    if (d->constraint)
                        visitExpr(d->constraint);
                    break;
                }
                case dcc::ast::DeclKind::Using: {
                    auto* d = static_cast<dcc::ast::UsingDecl const*>(decl);
                    if (d->target_expr)
                        visitExpr(d->target_expr);
                    break;
                }
                default:
                    break;
            }
        }

        void sort_and_dedupe(std::vector<protocol::InlayHint>& hints)
        {
            std::ranges::stable_sort(hints, [](protocol::InlayHint const& a, protocol::InlayHint const& b) {
                if (a.position.line != b.position.line)
                    return a.position.line < b.position.line;
                if (a.position.character != b.position.character)
                    return a.position.character < b.position.character;

                auto ak = a.kind.value_or(-1);
                auto bk = b.kind.value_or(-1);
                if (ak != bk)
                    return ak < bk;
                return a.label < b.label;
            });

            std::vector<protocol::InlayHint> unique;
            unique.reserve(hints.size());
            for (auto const& h : hints)
            {
                if (!unique.empty())
                {
                    auto const& last = unique.back();
                    if (last.position.line == h.position.line && last.position.character == h.position.character && last.kind == h.kind &&
                        last.label == h.label)
                        continue;
                }
                unique.push_back(h);
            }
            hints = std::move(unique);
        }

    } // anonymous namespace

    std::vector<protocol::InlayHint> collect_inlay_hints(dcc::sm::SourceManager const& sm, dcc::ast::TranslationUnit const* tu,
                                                         dcc::sm::SourceRange request_range, TypeFormatter format_type,
                                                         protocol::InlayHintOptions const& options, CancelCheck const& cancel)
    {
        std::vector<protocol::InlayHint> result;
        if (!tu)
            return result;

        Collector c{sm, request_range, std::move(format_type), options, cancel};

        try
        {
            if (tu->module_decl)
                c.visitDecl(tu->module_decl);

            for (auto* d : tu->imports)
                if (d)
                    c.visitDecl(d);

            for (auto* d : tu->decls)
                if (d)
                    c.visitDecl(d);
        }
        catch (TraversalCancelled const&)
        {
            return {};
        }

        result = std::move(c.hints);
        sort_and_dedupe(result);
        return result;
    }

} // namespace dccd::inlay_hints
