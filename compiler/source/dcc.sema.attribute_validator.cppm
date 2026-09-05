export module dcc.sema.attribute_validator;

import std;
import dcc.ast;
import dcc.ast.visitor;
import dcc.diag;
import dcc.sm;
import dcc.sema.scope;

[[nodiscard]] constexpr bool is_power_of_two(std::uint64_t n) noexcept
{
    return n > 0 && (n & (n - 1)) == 0;
}

namespace
{
    [[nodiscard]] bool known_calling_conv(std::string_view name) noexcept
    {
        return name == "Cdecl" || name == "cdecl" || name == "Stdcall" || name == "stdcall" || name == "Fastcall" || name == "fastcall" ||
               name == "Vectorcall" || name == "vectorcall" || name == "SystemV" || name == "systemv" || name == "sysv" || name == "Win64" || name == "win64";
    }

    [[nodiscard]] std::int64_t get_align_value(dcc::ast::Attribute const& attr)
    {
        if (attr.args.empty())
            return 0;

        if (auto* lit = dcc::ast::node_cast<dcc::ast::IntLiteralExpr>(attr.args[0]))
            return lit->value;

        return 0;
    }

    [[nodiscard]] std::string_view get_string_value(dcc::ast::Attribute const& attr)
    {
        if (attr.args.empty())
            return {};

        if (auto* sl = dcc::ast::node_cast<dcc::ast::StringLiteralExpr>(attr.args[0]))
            return sl->value;

        return {};
    }

    void validate_attr_args(dcc::ast::Attribute const& attr, dcc::diag::DiagnosticEngine& diag)
    {
        if (attr.name == "align" && attr.args.size() == 1)
        {
            auto val = get_align_value(attr);
            if (val <= 0)
                diag.emit(dcc::diag::Diagnostic{dcc::diag::Severity::Error, "alignment must be a positive integer"}.primary(attr.range));
            else if (!is_power_of_two(static_cast<std::uint64_t>(val)))
                diag.emit(
                    dcc::diag::Diagnostic{dcc::diag::Severity::Error, std::format("alignment value `{}` is not a power of two", val)}.primary(attr.range));
        }

        if (attr.name == "section" && attr.args.size() == 1)
        {
            auto val = get_string_value(attr);
            if (val.empty())
                diag.emit(dcc::diag::Diagnostic{dcc::diag::Severity::Error, "section name must be a non-empty string"}.primary(attr.range));
        }

        if (attr.name == "calling_conv" && attr.args.size() == 1)
        {
            auto val = get_string_value(attr);
            if (!val.empty() && !known_calling_conv(val))
                diag.emit(dcc::diag::Diagnostic{dcc::diag::Severity::Error, std::format("unknown calling convention `{}`", val)}.primary(attr.range));
        }
    }

    void propagate_decl_attrs(dcc::ast::Decl& d, std::span<dcc::ast::Attribute const> attrs)
    {
        for (auto const& attr : attrs)
        {
            if (attr.name == "inline")
                d.sema.is_inline = true;
            else if (attr.name == "intrinsic")
                d.sema.is_intrinsic = true;
            else if (attr.name == "noinline")
                d.sema.is_noinline = true;
            else if (attr.name == "nominal")
                d.sema.is_nominal = true;
            else if (attr.name == "nomangle")
                d.sema.is_nomangle = true;
            else if (attr.name == "import")
                d.sema.is_dll_import = true;
            else if (attr.name == "export")
                d.sema.is_dll_export = true;
            else if (attr.name == "runtime")
                d.sema.is_runtime = true;
            else if (attr.name == "section")
                d.sema.section = get_string_value(attr);
            else if (attr.name == "calling_conv")
                d.sema.calling_conv = get_string_value(attr);
            else if (attr.name == "align")
            {
                auto val = get_align_value(attr);
                if (val > 0)
                    d.sema.alignment = static_cast<std::uint32_t>(val);
            }
        }

        if (d.sema.is_intrinsic)
            if (auto const* f = dcc::ast::node_cast<dcc::ast::FuncDecl>(&d); f && f->name == "source_location")
                d.sema.intrinsic_kind = dcc::ast::IntrinsicKind::SourceLocation;
    }

    void validate_combinations(dcc::ast::Decl const& d, std::span<dcc::ast::Attribute const> attrs, dcc::diag::DiagnosticEngine& diag)
    {
        bool has_inline = false;
        bool has_noinline = false;
        bool has_import = false;
        bool has_export = false;

        for (auto const& attr : attrs)
        {
            if (attr.name == "inline")
                has_inline = true;
            else if (attr.name == "noinline")
                has_noinline = true;
            else if (attr.name == "import")
                has_import = true;
            else if (attr.name == "export")
                has_export = true;
        }

        if (has_inline && has_noinline)
            for (auto const& attr : attrs)
            {
                if (attr.name == "inline" || attr.name == "noinline")
                    diag.emit(
                        dcc::diag::Diagnostic{dcc::diag::Severity::Error, "attributes `@inline` and `@noinline` are mutually exclusive"}.primary(attr.range));
            }

        if (has_import && has_export)
            for (auto const& attr : attrs)
            {
                if (attr.name == "import" || attr.name == "export")
                    diag.emit(
                        dcc::diag::Diagnostic{dcc::diag::Severity::Error, "attributes `@import` and `@export` are mutually exclusive"}.primary(attr.range));
            }

        if (has_import && !d.is_extern)
            for (auto const& attr : attrs)
                if (attr.name == "import")
                    diag.emit(dcc::diag::Diagnostic{dcc::diag::Severity::Error, "attribute `@import` requires 'extern' storage"}.primary(attr.range));

        if (has_export && d.is_extern)
            for (auto const& attr : attrs)
                if (attr.name == "export")
                    diag.emit(dcc::diag::Diagnostic{dcc::diag::Severity::Error, "attribute `@export` cannot be combined with 'extern'"}.primary(attr.range));
    }

} // anonymous namespace

export namespace dcc::sema
{
    enum class AttributeTarget : std::uint32_t
    {
        None = 0,
        EnumVariant = 1 << 0,
        Function = 1 << 1,
        Variable = 1 << 2,
        LocalVariable = 1 << 3,
        Struct_ = 1 << 4,
        Union = 1 << 5,
        EnumDecl = 1 << 6,
        UsingAlias = 1 << 7,
        Expression = 1 << 8,
    };

    [[nodiscard]] constexpr AttributeTarget operator|(AttributeTarget a, AttributeTarget b) noexcept
    {
        return static_cast<AttributeTarget>(std::to_underlying(a) | std::to_underlying(b));
    }

    [[nodiscard]] constexpr bool target_has(AttributeTarget mask, AttributeTarget t) noexcept
    {
        return (std::to_underlying(mask) & std::to_underlying(t)) != 0;
    }

    [[nodiscard]] constexpr bool is_power_of_two(std::uint64_t n) noexcept
    {
        return n > 0 && (n & (n - 1)) == 0;
    }

    class AttributeRegistry
    {
    public:
        AttributeRegistry()
        {
            register_attr("implicit_construction", AttributeTarget::EnumVariant);

            register_attr("intrinsic", AttributeTarget::Function);

            register_attr("deprecated", AttributeTarget::Function | AttributeTarget::EnumVariant | AttributeTarget::Variable | AttributeTarget::Struct_ |
                                            AttributeTarget::Union | AttributeTarget::EnumDecl);

            register_attr("packed", AttributeTarget::Struct_ | AttributeTarget::Union);
            register_attr("align", AttributeTarget::Struct_ | AttributeTarget::Union | AttributeTarget::EnumDecl | AttributeTarget::Variable |
                                       AttributeTarget::LocalVariable | AttributeTarget::Function);

            register_attr("import", AttributeTarget::Function | AttributeTarget::Variable);
            register_attr("export", AttributeTarget::Function | AttributeTarget::Variable);
            register_attr("nomangle", AttributeTarget::Function | AttributeTarget::Variable);

            register_attr("inline", AttributeTarget::Function);
            register_attr("noinline", AttributeTarget::Function);

            register_attr("runtime", AttributeTarget::Function | AttributeTarget::Expression);

            register_attr("nominal", AttributeTarget::UsingAlias);

            register_attr("section", AttributeTarget::Function | AttributeTarget::Variable);
            register_attr("calling_conv", AttributeTarget::Function);

            register_attr("output", AttributeTarget::None);
            register_attr("inputs", AttributeTarget::None);
            register_attr("inout", AttributeTarget::None);
            register_attr("clobbers", AttributeTarget::None);
            register_attr("volatile", AttributeTarget::None);
            register_attr("intel", AttributeTarget::None);
            register_attr("att", AttributeTarget::None);
            register_attr("alignstack", AttributeTarget::None);
            register_attr("arch", AttributeTarget::None);
        }

        void register_attr(std::string_view name, AttributeTarget targets) { m_registry[std::string{name}] = targets; }

        [[nodiscard]] AttributeTarget allowed_targets(std::string_view name) const
        {
            auto it = m_registry.find(std::string{name});
            if (it == m_registry.end())
                return AttributeTarget::None;

            return it->second;
        }

        [[nodiscard]] bool is_known(std::string_view name) const { return m_registry.contains(std::string{name}); }

    private:
        std::unordered_map<std::string, AttributeTarget> m_registry;
    };

    void validate_local_var_attrs(AttributeRegistry const& registry, diag::DiagnosticEngine& diag, ast::Stmt const* stmt);
    void validate_local_var_attrs_in_expr(AttributeRegistry const& registry, diag::DiagnosticEngine& diag, ast::Expr const* expr);

    void validate_decl_attrs(AttributeRegistry const& registry, diag::DiagnosticEngine& diag, std::span<ast::Attribute const> attrs, AttributeTarget target)
    {
        for (std::size_t i = 0; i < attrs.size(); ++i)
        {
            auto const& attr = attrs[i];
            if (attr.name.empty())
                continue;

            if (!registry.is_known(attr.name))
            {
                diag.emit(diag::Diagnostic{diag::Severity::Error, std::format("unknown attribute `@{}`", attr.name)}.primary(attr.range));
                continue;
            }

            for (std::size_t j = 0; j < i; ++j)
            {
                if (attrs[j].name == attr.name)
                {
                    diag.emit(diag::Diagnostic{diag::Severity::Error, std::format("duplicate attribute `@{}`", attr.name)}.primary(attr.range));
                    break;
                }
            }

            if (target == AttributeTarget::None || !target_has(registry.allowed_targets(attr.name), target))
                diag.emit(
                    diag::Diagnostic{diag::Severity::Error, std::format("attribute `@{}` is not valid on this declaration", attr.name)}.primary(attr.range));

            validate_attr_args(attr, diag);
        }
    }

    void validate_expr_attrs(AttributeRegistry const& registry, diag::DiagnosticEngine& diag, ast::Expr const& expr)
    {
        for (std::size_t i = 0; i < expr.attrs.size(); ++i)
        {
            auto const& attr = expr.attrs[i];
            if (attr.name.empty())
                continue;

            if (!registry.is_known(attr.name))
            {
                diag.emit(diag::Diagnostic{diag::Severity::Error, std::format("unknown attribute `@{}`", attr.name)}.primary(attr.range));
                continue;
            }

            for (std::size_t j = 0; j < i; ++j)
            {
                if (expr.attrs[j].name == attr.name)
                {
                    diag.emit(diag::Diagnostic{diag::Severity::Error, std::format("duplicate attribute `@{}`", attr.name)}.primary(attr.range));
                    break;
                }
            }

            if (!target_has(registry.allowed_targets(attr.name), AttributeTarget::Expression))
            {
                diag.emit(diag::Diagnostic{diag::Severity::Error, std::format("attribute `@{}` is not valid on expressions", attr.name)}.primary(attr.range));
                continue;
            }

            if (attr.name == "runtime" && expr.kind != ast::ExprKind::Call)
                diag.emit(diag::Diagnostic{diag::Severity::Error, "attribute `@runtime` is only valid on call expressions"}.primary(attr.range));

            validate_attr_args(attr, diag);
        }
    }

    struct ExprAttrVisitor : ast::RecursiveAstVisitor
    {
        AttributeRegistry const& registry;
        diag::DiagnosticEngine& diag;

        ExprAttrVisitor(AttributeRegistry const& r, diag::DiagnosticEngine& d) : registry(r), diag(d) {}

        void visitExpr(ast::Expr const* expr) override
        {
            if (!expr)
                return;

            if (!expr->attrs.empty())
                validate_expr_attrs(registry, diag, *expr);

            ast::RecursiveAstVisitor::visitExpr(expr);
        }
    };

    void validate_attributes(std::span<std::unique_ptr<ModuleInfo> const> modules, diag::DiagnosticEngine& diag, std::pmr::polymorphic_allocator<> a)
    {
        std::ignore = a;
        AttributeRegistry registry;
        ExprAttrVisitor expr_visitor{registry, diag};

        for (auto const& m : modules)
        {
            if (!m->tu)
                continue;

            for (auto* d : m->tu->decls)
            {
                expr_visitor.visitDecl(d);

                AttributeTarget target = AttributeTarget::None;
                switch (d->kind)
                {
                    case ast::DeclKind::Func:
                        target = AttributeTarget::Function;
                        break;
                    case ast::DeclKind::Var:
                        target = AttributeTarget::Variable;
                        break;
                    case ast::DeclKind::Struct:
                        target = AttributeTarget::Struct_;
                        break;
                    case ast::DeclKind::Union:
                        target = AttributeTarget::Union;
                        break;
                    case ast::DeclKind::Enum: {
                        target = AttributeTarget::EnumDecl;
                        if (auto const* ed = ast::node_cast<ast::EnumDecl>(d))
                            for (auto const& v : ed->variants)
                            {
                                validate_decl_attrs(registry, diag, v.attrs, AttributeTarget::EnumVariant);
                                for (auto const& va : v.attrs)
                                    validate_attr_args(va, diag);
                            }
                        break;
                    }
                    case ast::DeclKind::Using: {
                        auto const* ud = static_cast<ast::UsingDecl const*>(d);
                        if (ud->using_kind == ast::UsingKind::Alias)
                            target = AttributeTarget::UsingAlias;
                        break;
                    }
                    default:
                        break;
                }

                validate_decl_attrs(registry, diag, d->attrs, target);
                validate_combinations(*d, d->attrs, diag);
                propagate_decl_attrs(*d, d->attrs);

                if (d->sema.is_intrinsic)
                {
                    auto segs = m->canonical_path.segments();
                    if (segs.empty() || segs[0] != "core")
                    {
                        for (auto const& attr : d->attrs)
                            if (attr.name == "intrinsic")
                            {
                                diag.emit(
                                    diag::Diagnostic{diag::Severity::Error, "attribute `@intrinsic` is only valid in the 'core' module"}.primary(attr.range));
                            }
                    }
                }

                if (auto const* fd = ast::node_cast<ast::FuncDecl>(d))
                {
                    if (fd->body)
                    {
                        for (auto* s : fd->body->stmts)
                            validate_local_var_attrs(registry, diag, s);

                        if (fd->body->tail)
                            validate_local_var_attrs_in_expr(registry, diag, fd->body->tail);
                    }
                }
            }
        }
    }

    void validate_local_var_attrs_in_expr(AttributeRegistry const& registry, diag::DiagnosticEngine& diag, ast::Expr const* expr)
    {
        if (!expr)
            return;

        switch (expr->kind)
        {
            case ast::ExprKind::Block: {
                auto const& be = static_cast<ast::BlockExpr const&>(*expr);
                for (auto* s : be.body.stmts)
                    validate_local_var_attrs(registry, diag, s);

                if (be.body.tail)
                    validate_local_var_attrs_in_expr(registry, diag, be.body.tail);

                break;
            }
            case ast::ExprKind::If: {
                auto const& ie = static_cast<ast::IfExpr const&>(*expr);
                for (auto* s : ie.then_block.stmts)
                    validate_local_var_attrs(registry, diag, s);

                if (ie.then_block.tail)
                    validate_local_var_attrs_in_expr(registry, diag, ie.then_block.tail);

                if (ie.else_branch)
                    validate_local_var_attrs_in_expr(registry, diag, ie.else_branch);

                break;
            }
            default:
                break;
        }
    }

    void validate_local_var_attrs(AttributeRegistry const& registry, diag::DiagnosticEngine& diag, ast::Stmt const* stmt)
    {
        if (!stmt)
            return;

        auto walk_block = [&](ast::Block const& b) {
            for (auto* s : b.stmts)
                validate_local_var_attrs(registry, diag, s);

            if (b.tail)
                validate_local_var_attrs_in_expr(registry, diag, b.tail);
        };

        switch (stmt->kind)
        {
            case ast::StmtKind::DeclStmt: {
                auto const& ds = static_cast<ast::DeclStmt const&>(*stmt);
                if (ds.decl && ds.decl->kind == ast::DeclKind::Var)
                {
                    validate_decl_attrs(registry, diag, ds.decl->attrs, AttributeTarget::LocalVariable);
                    validate_combinations(*ds.decl, ds.decl->attrs, diag);
                    propagate_decl_attrs(*ds.decl, ds.decl->attrs);
                }
                break;
            }
            case ast::StmtKind::While: {
                auto const& w = static_cast<ast::WhileStmt const&>(*stmt);
                walk_block(w.body);
                break;
            }
            case ast::StmtKind::DoWhile: {
                auto const& dw = static_cast<ast::DoWhileStmt const&>(*stmt);
                walk_block(dw.body);
                break;
            }
            case ast::StmtKind::For: {
                auto const& f = static_cast<ast::ForStmt const&>(*stmt);
                validate_local_var_attrs(registry, diag, f.init);
                walk_block(f.body);
                break;
            }
            case ast::StmtKind::ForIn: {
                auto const& fi = static_cast<ast::ForInStmt const&>(*stmt);
                walk_block(fi.body);
                break;
            }
            case ast::StmtKind::Defer: {
                auto const& d = static_cast<ast::DeferStmt const&>(*stmt);
                validate_local_var_attrs(registry, diag, d.body);
                break;
            }
            case ast::StmtKind::StaticIf: {
                auto const& si = static_cast<ast::StaticIfStmt const&>(*stmt);
                walk_block(si.then_block);
                validate_local_var_attrs(registry, diag, si.else_branch);
                break;
            }
            case ast::StmtKind::StaticMatch: {
                auto const& sm = static_cast<ast::StaticMatchStmt const&>(*stmt);
                for (auto const& arm : sm.arms)
                    if (arm.body)
                        validate_local_var_attrs_in_expr(registry, diag, arm.body);

                break;
            }
            default:
                break;
        }
    }

} // namespace dcc::sema
