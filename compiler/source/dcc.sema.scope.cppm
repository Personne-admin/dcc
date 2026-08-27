export module dcc.sema.scope;

import std;
import dcc.ast;
import dcc.sm;
import dcc.si;

export namespace dcc::sema
{
    struct ModuleInfo;
    class Scope;
    struct NameBinding;

    class ModulePath
    {
    public:
        ModulePath() = default;

        explicit ModulePath(std::vector<std::string_view> segs) : m_segments{std::move(segs)} {}

        static ModulePath from_ast(ast::Path const& p)
        {
            std::vector<std::string_view> segs;
            segs.reserve(p.segments.size());
            for (auto const& s : p.segments)
                segs.push_back(s.name);

            return ModulePath{std::move(segs)};
        }

        [[nodiscard]] std::span<std::string_view const> segments() const noexcept { return m_segments; }
        [[nodiscard]] bool empty() const noexcept { return m_segments.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return m_segments.size(); }

        [[nodiscard]] std::string str() const
        {
            std::string s;
            for (std::size_t i = 0; i < m_segments.size(); ++i)
            {
                if (i > 0)
                    s += "::";

                s += m_segments[i];
            }
            return s;
        }

        [[nodiscard]] ModulePath strip_common_prefix(ModulePath const& other) const
        {
            std::size_t i = 0;
            auto const& a = m_segments;
            auto const& b = other.m_segments;
            while (i < a.size() && i < b.size() && a[i] == b[i])
                ++i;

            return ModulePath{std::vector<std::string_view>{a.begin() + static_cast<std::ptrdiff_t>(i), a.end()}};
        }

        [[nodiscard]] ModulePath parent() const
        {
            if (m_segments.empty())
                return {};

            return ModulePath{std::vector<std::string_view>{m_segments.begin(), m_segments.end() - 1}};
        }

        bool operator==(ModulePath const& other) const noexcept
        {
            if (m_segments.size() != other.m_segments.size())
                return false;
            for (std::size_t i = 0; i < m_segments.size(); ++i)
            {
                auto const& a = m_segments[i];
                auto const& b = other.m_segments[i];

                if (a.empty() || b.empty())
                {
                    if (a.size() != 0 || b.size() != 0)
                        return false;
                    continue;
                }
                if (a.data() != b.data())
                    return false;
            }
            return true;
        }

    private:
        std::vector<std::string_view> m_segments;
    };

    struct ModulePathHash
    {
        std::size_t operator()(ModulePath const& p) const noexcept
        {
            std::size_t h = 0;
            dcc::si::InternedStringHash hf;
            for (auto const& s : p.segments())
                h = (h * 1099511628211ULL) ^ hf(s);

            return h;
        }
    };

    enum class ModuleState : std::uint8_t
    {
        Discovered,
        Parsed,
        Collected,
        UsingResolved,
        TypesResolved,
        Validated,
        BodiesAnalyzed
    };

    enum class NameSpace : std::uint8_t
    {
        Type,
        Value,
        Namespace,
    };

    enum class SymbolKind : std::uint8_t
    {
        Struct,
        Union,
        Enum,
        TypeAlias,
        TemplateParam,

        Function,
        Variable,
        EnumVariant,

        Module,
        UsingGroup,
    };

    [[nodiscard]] constexpr NameSpace namespace_of(SymbolKind k) noexcept
    {
        switch (k)
        {
            case SymbolKind::Struct:
            case SymbolKind::Union:
            case SymbolKind::Enum:
            case SymbolKind::TypeAlias:
            case SymbolKind::TemplateParam:
                return NameSpace::Type;
            case SymbolKind::Function:
            case SymbolKind::Variable:
            case SymbolKind::EnumVariant:
                return NameSpace::Value;
            case SymbolKind::Module:
            case SymbolKind::UsingGroup:
                return NameSpace::Namespace;
        }
        return NameSpace::Value;
    }

    struct Symbol
    {
        std::string_view name;
        SymbolKind kind{};

        ast::Decl const* decl{};

        ModuleInfo const* module{};

        ast::UsingDecl const* via_using{};

        bool is_spilled : 1 {};
        bool is_exported : 1 {};
        bool is_ambiguous : 1 {};

        sm::SourceRange definition_range;
    };

    struct NameBinding
    {
        using Allocator = std::pmr::polymorphic_allocator<>;

        Symbol type_sym{};
        bool has_type{};

        std::pmr::vector<Symbol> value_syms;
        bool has_value_variable{};

        Scope* namespace_scope{};
        Symbol namespace_sym{};
        bool has_namespace{};

        explicit NameBinding(Allocator a) : value_syms(a) {}

        [[nodiscard]] bool empty() const noexcept { return !has_type && value_syms.empty() && !has_namespace; }
    };

    enum class ScopeKind : std::uint8_t
    {
        Module,
        Function,
        Block,
        Template,
        Struct,
        Enum,
        UsingGroup,
    };

    enum class DefineResult : std::uint8_t
    {
        Ok,
        Conflict,
        Redefinition,
    };

    class Scope
    {
    public:
        using Allocator = std::pmr::polymorphic_allocator<>;

        Scope(ScopeKind kind, Scope const* parent, Allocator a) : m_kind{kind}, m_parent{parent}, m_alloc{a}, m_bindings{a} {}

        [[nodiscard]] ScopeKind kind() const noexcept { return m_kind; }
        [[nodiscard]] Scope const* parent() const noexcept { return m_parent; }
        [[nodiscard]] Allocator allocator() const noexcept { return m_alloc; }

        DefineResult define_type(Symbol s, Symbol const** existing = nullptr)
        {
            auto& b = ensure_binding(s.name);
            if (b.has_type)
            {
                if (existing)
                    *existing = &b.type_sym;

                return same_decl(b.type_sym, s) ? DefineResult::Redefinition : DefineResult::Conflict;
            }

            b.type_sym = s;
            b.has_type = true;
            return DefineResult::Ok;
        }

        DefineResult define_variable(Symbol s, Symbol const** existing = nullptr)
        {
            auto& b = ensure_binding(s.name);
            if (!b.value_syms.empty())
            {
                if (existing)
                    *existing = &b.value_syms.front();

                return DefineResult::Conflict;
            }

            b.value_syms.push_back(s);
            b.has_value_variable = true;
            return DefineResult::Ok;
        }

        DefineResult add_function_overload(Symbol s, Symbol const** existing = nullptr)
        {
            auto& b = ensure_binding(s.name);
            if (b.has_value_variable)
            {
                if (existing)
                    *existing = &b.value_syms.front();

                return DefineResult::Conflict;
            }

            b.value_syms.push_back(s);
            return DefineResult::Ok;
        }

        Scope* ensure_namespace(std::string_view name, Symbol anchor, ScopeKind inner_kind = ScopeKind::UsingGroup)
        {
            auto& b = ensure_binding(name);
            if (!b.has_namespace)
            {
                auto* p = m_alloc.allocate_object<Scope>();
                b.namespace_scope = std::construct_at(p, inner_kind, this, m_alloc);
                b.namespace_sym = anchor;
                b.has_namespace = true;
            }
            else
            {
                if (anchor.is_exported)
                    b.namespace_sym.is_exported = true;
                if (anchor.is_spilled)
                    b.namespace_sym.is_spilled = true;
                if (anchor.via_using && !b.namespace_sym.via_using)
                    b.namespace_sym.via_using = anchor.via_using;
            }
            return b.namespace_scope;
        }

        DefineResult bind_namespace(std::string_view name, Scope* scope, Symbol anchor, Symbol const** existing = nullptr)
        {
            auto& b = ensure_binding(name);
            if (b.has_namespace)
            {
                if (existing)
                    *existing = &b.namespace_sym;

                return b.namespace_scope == scope ? DefineResult::Redefinition : DefineResult::Conflict;
            }

            b.namespace_scope = scope;
            b.namespace_sym = anchor;
            b.has_namespace = true;
            return DefineResult::Ok;
        }

        [[nodiscard]] NameBinding* find_binding_local(std::string_view name)
        {
            auto it = m_bindings.find(name);
            return it == m_bindings.end() ? nullptr : &it->second;
        }

        [[nodiscard]] NameBinding const* find_binding_local(std::string_view name) const
        {
            auto it = m_bindings.find(name);
            return it == m_bindings.end() ? nullptr : &it->second;
        }

        [[nodiscard]] Symbol const* lookup_type_local(std::string_view name) const
        {
            auto const* b = find_binding_local(name);
            return (b && b->has_type) ? &b->type_sym : nullptr;
        }

        [[nodiscard]] std::span<Symbol const> lookup_values_local(std::string_view name) const
        {
            auto const* b = find_binding_local(name);
            if (!b || b->value_syms.empty())
                return {};

            return {b->value_syms.data(), b->value_syms.size()};
        }

        [[nodiscard]] Scope const* lookup_namespace_local(std::string_view name) const
        {
            auto const* b = find_binding_local(name);
            return (b && b->has_namespace) ? b->namespace_scope : nullptr;
        }

        [[nodiscard]] Symbol const* lookup_type(std::string_view name) const
        {
            for (auto const* s = this; s; s = s->m_parent)
                if (auto const* sym = s->lookup_type_local(name))
                    return sym;

            return nullptr;
        }

        [[nodiscard]] std::span<Symbol const> lookup_values(std::string_view name) const
        {
            for (auto const* s = this; s; s = s->m_parent)
            {
                auto vs = s->lookup_values_local(name);
                if (!vs.empty())
                    return vs;
            }
            return {};
        }

        [[nodiscard]] Scope const* lookup_namespace(std::string_view name) const
        {
            for (auto const* s = this; s; s = s->m_parent)
                if (auto const* ns = s->lookup_namespace_local(name))
                    return ns;

            return nullptr;
        }

        [[nodiscard]] auto const& bindings() const noexcept { return m_bindings; }

    private:
        ScopeKind m_kind;
        Scope const* m_parent;
        Allocator m_alloc;
        dcc::si::InternedPmrHashMap<NameBinding> m_bindings;

        NameBinding& ensure_binding(std::string_view name)
        {
            auto it = m_bindings.find(name);
            if (it == m_bindings.end())
                it = m_bindings.try_emplace(name, NameBinding{m_alloc}).first;

            return it->second;
        }

        static bool same_decl(Symbol const& a, Symbol const& b) noexcept { return a.decl == b.decl && a.decl != nullptr; }
    };

    namespace detail
    {
        [[nodiscard]] Scope const* walk_prefix(Scope const& start, std::span<ast::PathSegment const> segs)
        {
            if (segs.empty())
                return &start;

            Scope const* cur = start.lookup_namespace(segs.front().name);
            if (!cur)
                return nullptr;

            for (std::size_t i = 1; i < segs.size(); ++i)
            {
                cur = cur->lookup_namespace_local(segs[i].name);
                if (!cur)
                    return nullptr;
            }

            return cur;
        }

    } // namespace detail

    [[nodiscard]] Symbol const* resolve_type_path(Scope const& start, ast::Path const& path) noexcept
    {
        if (path.is_empty())
            return nullptr;

        auto const& segs = path.segments;
        if (segs.size() == 1)
            return start.lookup_type(segs.front().name);

        auto const* prefix = detail::walk_prefix(start, std::span{segs}.first(segs.size() - 1));
        return prefix ? prefix->lookup_type_local(segs.back().name) : nullptr;
    }

    [[nodiscard]] std::span<Symbol const> resolve_value_overloads(Scope const& start, ast::Path const& path) noexcept
    {
        if (path.is_empty())
            return {};

        auto const& segs = path.segments;
        if (segs.size() == 1)
            return start.lookup_values(segs.front().name);

        auto const* prefix = detail::walk_prefix(start, std::span{segs}.first(segs.size() - 1));
        return prefix ? prefix->lookup_values_local(segs.back().name) : std::span<Symbol const>{};
    }

    [[nodiscard]] Symbol const* resolve_value_path(Scope const& start, ast::Path const& path) noexcept
    {
        auto vs = resolve_value_overloads(start, path);
        return vs.empty() ? nullptr : &vs.front();
    }

    [[nodiscard]] Scope const* resolve_namespace_path(Scope const& start, ast::Path const& path) noexcept
    {
        if (path.is_empty())
            return nullptr;

        return detail::walk_prefix(start, path.segments);
    }

    struct ModuleInfo
    {
        ModulePath canonical_path;
        std::filesystem::path file_path;
        sm::FileId file_id{sm::FileId::Invalid};

        ast::TranslationUnit* tu{};

        Scope* own_scope{};
        Scope* export_scope{};
        Scope* ufcs_scope{};

        std::vector<ast::UsingDecl*> using_worklist;

        struct ImportBinding
        {
            ast::ImportDecl const* decl{};
            ModuleInfo const* target{};
            ModulePath alias_prefix;
        };
        std::vector<ImportBinding> imports;

        ModuleState state{ModuleState::Discovered};
        bool has_errors{};

        std::vector<ast::FuncDecl*> lambda_funcs;
        std::uint32_t lambda_counter{};
    };

} // namespace dcc::sema
