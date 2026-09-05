module;

export module dcc.ctfe;

import std;
import dcc.ast;
import dcc.comptime;
import dcc.const_eval;
import dcc.ctfe.memory;
import dcc.lex.tokens;
import dcc.sm;
import dcc.types;

export namespace dcc::ctfe
{
    enum class Mode : std::uint8_t
    {
        Required,
        Opportunistic
    };

    enum class Flow : std::uint8_t
    {
        Normal,
        Return,
        Break,
        Continue,
        NotEvaluatable,
        Error
    };

    struct Context
    {
        sm::SourceRange call_site{};
        ast::FuncDecl const* function{};
        types::TypeContext* types{};
        std::size_t step_limit{100000};
        std::size_t recursion_limit{128};
        std::size_t memory_limit{1 << 20};
        std::function<bool(ast::FuncDecl const&)> prepare_function;
    };

    struct Call
    {
        ast::FuncDecl const* function{};
        sm::SourceRange call_site{};
    };

    struct Result
    {
        Flow flow{Flow::Normal};
        std::optional<comptime::Value> value;
        std::string message;
        std::vector<Call> calls;

        [[nodiscard]] bool failed() const { return flow == Flow::NotEvaluatable || flow == Flow::Error; }
    };

    class Evaluator
    {
        using Kind = comptime::Value::Kind;

        struct Frame
        {
            Call call;
            std::unordered_map<ast::Decl const*, comptime::ValuePtr> locals;
            std::vector<std::size_t> allocations;
        };

        Context m_context;
        Mode m_mode;
        std::size_t m_steps{};
        std::size_t m_cells{};
        std::vector<Frame> m_frames;
        std::unordered_map<comptime::Value const*, comptime::ValuePtr> m_constants;
        Heap m_heap;

        Result failure(std::string message, bool hard = false)
        {
            Result r{hard ? Flow::Error : Flow::NotEvaluatable, {}, std::move(message), {}};
            for (auto const& frame : m_frames)
                if (frame.call.function)
                    r.calls.push_back(frame.call);
            return r;
        }

        bool step() { return ++m_steps <= m_context.step_limit; }
        Result exhausted() { return failure("step limit exceeded", true); }
        Result expired() { return failure("reference to storage that has gone out of scope"); }
        Result unsupported() { return failure("operation is unavailable at compile time"); }

        Result folded(std::optional<comptime::Value> value)
        {
            if (!value)
                return unsupported();
            return {Flow::Normal, std::move(value), {}, {}};
        }

        static types::TypePtr type_of(ast::Expr const& expr) { return reinterpret_cast<types::TypePtr>(expr.sema.resolved_type); }
        static types::TypePtr type_of(ast::TypePtr type) { return type ? reinterpret_cast<types::TypePtr>(type->sema.canonical) : nullptr; }

        static types::TypePtr pointee_of(types::TypePtr type)
        {
            if (auto const* p = types::type_cast<types::PointerType>(type))
                return p->pointee;
            return nullptr;
        }

        static types::TypePtr element_of(types::TypePtr type)
        {
            if (auto const* a = types::type_cast<types::ArrayType>(type))
                return a->element;
            if (auto const* s = types::type_cast<types::SliceType>(type))
                return s->element;
            return pointee_of(type);
        }

        static std::span<ast::FieldDecl const> record_fields(types::TypePtr type)
        {
            if (auto const* s = types::type_cast<types::StructType>(type))
            {
                if (auto const* d = ast::node_cast<ast::StructDecl>(reinterpret_cast<ast::Decl const*>(s->decl)))
                    return d->fields;
            }
            else if (auto const* u = types::type_cast<types::UnionType>(type))
            {
                if (auto const* d = ast::node_cast<ast::UnionDecl>(reinterpret_cast<ast::Decl const*>(u->decl)))
                    return d->fields;
            }
            return {};
        }

        static bool is_byte_type(types::TypePtr type)
        {
            if (!type)
                return false;
            if (type->kind == types::TypeKind::Char)
                return true;
            auto const* i = types::type_cast<types::IntType>(type);
            return i && i->bits == 8;
        }

        static bool volatile_type(ast::TypePtr type)
        {
            while (auto* qualified = ast::node_cast<ast::QualifiedType>(type))
            {
                if (ast::has_qual(qualified->quals, ast::Qual::Volatile))
                    return true;
                type = qualified->inner;
            }
            return false;
        }

        static bool volatile_decl(ast::Decl const* decl)
        {
            auto* var = ast::node_cast<ast::VarDecl>(decl);
            return var && volatile_type(var->type);
        }

        comptime::Value default_object(types::TypePtr type)
        {
            if (m_cells >= m_context.memory_limit)
                return {};

            ++m_cells;
            if (auto const* array = types::type_cast<types::ArrayType>(type))
            {
                std::vector<comptime::Value> elements(static_cast<std::size_t>(array->count));
                for (auto& element : elements)
                    element = default_object(array->element);
                return comptime::Value::make_aggregate(std::move(elements), type);
            }

            auto fields = record_fields(type);
            if (!fields.empty())
            {
                std::vector<comptime::Value> elements;
                elements.reserve(fields.size());
                for (auto const& field : fields)
                    elements.push_back(default_object(type_of(field.type)));
                return comptime::Value::make_aggregate(std::move(elements), type);
            }
            return {};
        }

        comptime::ValuePtr const* local(ast::Decl const* decl) const
        {
            if (m_frames.empty() || !decl)
                return nullptr;
            auto it = m_frames.back().locals.find(decl);
            return it == m_frames.back().locals.end() ? nullptr : &it->second;
        }

        static comptime::Value const* constant_of(ast::Decl const* decl, ast::Expr const& expr)
        {
            if (expr.sema.const_value)
                return expr.sema.const_value;
            auto* var = ast::node_cast<ast::VarDecl>(decl);
            if (var && var->sema.is_immutable && var->init)
                return var->init->sema.const_value;
            return nullptr;
        }

        Result named_place(ast::Expr const& expr, comptime::ValuePtr& out)
        {
            auto* decl = expr.sema.resolved_decl;
            if (volatile_decl(decl))
                return failure("volatile access");
            if (auto const* slot = local(decl))
            {
                out = *slot;
                return {};
            }

            auto const* constant = constant_of(decl, expr);
            if (!constant)
                return failure("read of non-constant storage");
            if (auto it = m_constants.find(constant); it != m_constants.end())
            {
                out = it->second;
                return {};
            }

            auto value = attach(*constant);
            if (value.flow != Flow::Normal || !value.value)
                return value;
            if (m_cells++ >= m_context.memory_limit)
                return failure("memory limit exceeded", true);

            out = m_heap.allocate(std::move(*value.value), false);
            m_constants.emplace(constant, out);
            return {};
        }

        Result bind(ast::Decl const* decl, comptime::Value object)
        {
            if (m_cells++ >= m_context.memory_limit)
                return failure("memory limit exceeded", true);

            auto& frame = m_frames.back();
            if (auto it = frame.locals.find(decl); it != frame.locals.end())
            {
                if (auto* slot = m_heap.write_target(it->second))
                {
                    *slot = std::move(object);
                    return {};
                }
            }

            auto ptr = m_heap.allocate(std::move(object), true);
            frame.allocations.push_back(ptr.allocation);
            frame.locals.insert_or_assign(decl, std::move(ptr));
            return {};
        }

        Result read(comptime::ValuePtr const& ptr)
        {
            auto const* value = m_heap.read(ptr);
            if (!value)
                return m_heap.is_live(ptr) ? failure("access outside the bounds of a compile-time object") : expired();
            if (!value->type)
                return failure("read of uninitialized storage");
            return folded(*value);
        }

        Result pointer_of(comptime::Value const& value, comptime::ValuePtr& out)
        {
            if (value.kind() == Kind::Null)
                return failure("null pointer dereference", true);
            if (value.kind() != Kind::Pointer)
                return failure("dereference of a value that is not a pointer");
            if (value.is_null_ptr())
                return failure("null pointer dereference", true);
            out = value.get_pointer();
            return {};
        }

        Result base_place(ast::Expr const& expr, comptime::ValuePtr& out)
        {
            auto type = type_of(expr);
            if (!types::type_cast<types::PointerType>(type))
                return place(expr, out);

            auto r = expression(expr);
            if (r.flow != Flow::Normal || !r.value)
                return r;

            auto p = pointer_of(*r.value, out);
            if (p.flow != Flow::Normal)
                return p;

            for (type = pointee_of(type); types::type_cast<types::PointerType>(type); type = pointee_of(type))
            {
                auto indirect = read(out);
                if (indirect.flow != Flow::Normal || !indirect.value)
                    return indirect;
                p = pointer_of(*indirect.value, out);
                if (p.flow != Flow::Normal)
                    return p;
            }
            return {};
        }

        Result field_place(ast::FieldAccessExpr const& expr, comptime::ValuePtr& out)
        {
            comptime::ValuePtr base;
            auto r = base_place(*expr.object, base);
            if (r.flow != Flow::Normal)
                return r;

            auto type = type_of(*expr.object);
            while (auto pointee = pointee_of(type))
                type = pointee;

            auto fields = record_fields(type);
            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                if (fields[i].name != expr.field)
                    continue;
                if (volatile_type(fields[i].type))
                    return failure("volatile access");

                auto member = m_heap.subobject(base, static_cast<std::uint32_t>(i));
                if (!member)
                    return failure("member has no compile-time storage");
                out = std::move(*member);
                return {};
            }
            return failure("member has no compile-time storage");
        }

        Result index_of(ast::Expr const& expr, std::int64_t& out)
        {
            auto r = expression(expr);
            if (r.flow != Flow::Normal || !r.value)
                return r;
            auto index = r.value->const_to_int();
            if (!index)
                return failure("index has no compile-time value");
            out = *index;
            return {};
        }

        Result element_place(ast::IndexExpr const& expr, comptime::ValuePtr& out)
        {
            std::int64_t index{};
            auto r = index_of(*expr.index, index);
            if (r.flow != Flow::Normal)
                return r;
            if (index < 0)
                return failure("negative index", true);

            auto type = type_of(*expr.object);
            if (types::type_cast<types::ArrayType>(type))
            {
                comptime::ValuePtr base;
                r = place(*expr.object, base);
                if (r.flow != Flow::Normal)
                    return r;
                auto element = m_heap.subobject(base, static_cast<std::uint32_t>(index));
                if (!element)
                    return failure("index out of bounds", true);
                out = std::move(*element);
                return {};
            }

            r = expression(*expr.object);
            if (r.flow != Flow::Normal || !r.value)
                return r;

            if (r.value->kind() == Kind::Slice)
            {
                if (static_cast<std::size_t>(index) >= r.value->slice_length())
                    return failure("index out of bounds", true);
                auto element = m_heap.offset(r.value->slice_base(), index);
                if (!element)
                    return failure("index has no compile-time storage");
                out = std::move(*element);
                return {};
            }

            comptime::ValuePtr base;
            auto p = pointer_of(*r.value, base);
            if (p.flow != Flow::Normal)
                return p;
            auto element = m_heap.offset(base, index);
            if (!element)
                return failure("index out of bounds", true);
            out = std::move(*element);
            return {};
        }

        Result place(ast::Expr const& expr, comptime::ValuePtr& out)
        {
            if (!step())
                return exhausted();

            switch (expr.kind)
            {
                case ast::ExprKind::Ident:
                case ast::ExprKind::PathExpr:
                    return named_place(expr, out);
                case ast::ExprKind::Unary: {
                    auto const& e = static_cast<ast::UnaryExpr const&>(expr);
                    if (e.op != lex::TokenKind::Star)
                        break;
                    auto r = expression(*e.operand);
                    if (r.flow != Flow::Normal || !r.value)
                        return r;
                    return pointer_of(*r.value, out);
                }
                case ast::ExprKind::FieldAccess:
                    return field_place(static_cast<ast::FieldAccessExpr const&>(expr), out);
                case ast::ExprKind::Index: {
                    auto const& e = static_cast<ast::IndexExpr const&>(expr);
                    if (e.index->kind == ast::ExprKind::Range)
                        break;
                    return element_place(e, out);
                }
                default:
                    break;
            }
            return failure("expression has no compile-time storage");
        }

        Result store(ast::Expr const& target, comptime::Value value)
        {
            comptime::ValuePtr ptr;
            auto r = place(target, ptr);
            if (r.flow != Flow::Normal)
                return r;

            auto* slot = m_heap.write_target(ptr);
            if (!slot)
                return m_heap.is_mutable(ptr) ? failure("write outside the bounds of a compile-time object")
                                              : failure("write to read-only storage");
            *slot = value;
            return folded(std::move(value));
        }

        static bool is_wide(types::TypePtr element)
        {
            auto const* i = types::type_cast<types::IntType>(element);
            return i && i->bits == 16;
        }

        static comptime::Value unit_value(std::string const& bytes, std::size_t index, types::TypePtr element)
        {
            std::uint32_t unit = 0;
            if (is_wide(element))
            {
                if ((index + 1) * sizeof(char16_t) <= bytes.size())
                    std::memcpy(&unit, bytes.data() + index * sizeof(char16_t), sizeof(char16_t));
            }
            else if (index < bytes.size())
                unit = static_cast<unsigned char>(bytes[index]);

            if (element->kind == types::TypeKind::Char)
                return comptime::Value::make_char(unit, element);
            return comptime::Value::make_int(unit, element);
        }

        Result intern_string(std::string const& bytes, types::TypePtr element, comptime::ValuePtr& out)
        {
            if (!m_context.types)
                return failure("string has no compile-time storage");
            if (!element)
                element = m_context.types->m_chart();

            auto units = is_wide(element) ? bytes.size() / sizeof(char16_t) : bytes.size();
            std::vector<comptime::Value> elements;
            elements.reserve(units + 1);
            for (std::size_t i = 0; i <= units; ++i)
                elements.push_back(unit_value(bytes, i, element));

            auto array = m_context.types->array_t(element, units + 1);
            auto key = std::format("{}:{}", static_cast<void const*>(array), bytes);
            auto base = m_heap.intern(key, comptime::Value::make_aggregate(std::move(elements), array));
            auto first = m_heap.subobject(base, 0);
            if (!first)
                return failure("string has no compile-time storage");
            out = std::move(*first);
            return {};
        }

        Result attach(comptime::Value const& value)
        {
            auto type = value.type;
            auto element = element_of(type);

            if (value.kind() == Kind::String)
            {
                if (auto const* array = types::type_cast<types::ArrayType>(type))
                {
                    std::vector<comptime::Value> elements;
                    elements.reserve(static_cast<std::size_t>(array->count));
                    for (std::uint64_t i = 0; i < array->count; ++i)
                        elements.push_back(unit_value(value.get_string(), static_cast<std::size_t>(i), element));
                    return folded(comptime::Value::make_aggregate(std::move(elements), type));
                }

                bool to_pointer = types::type_cast<types::PointerType>(type) != nullptr;
                if (!to_pointer && !types::type_cast<types::SliceType>(type))
                    return folded(value);

                comptime::ValuePtr base;
                auto r = intern_string(value.get_string(), element, base);
                if (r.flow != Flow::Normal)
                    return r;
                if (to_pointer)
                    return folded(comptime::Value::make_pointer_to(std::move(base), type));

                auto units = is_wide(element) ? value.get_string().size() / sizeof(char16_t) : value.get_string().size();
                return folded(comptime::Value::make_slice_ref(std::move(base), units, type));
            }

            if (value.kind() != Kind::Aggregate && value.kind() != Kind::Slice)
                return folded(value);
            if (value.kind() == Kind::Slice && value.slice_is_ref())
                return folded(value);

            std::vector<comptime::Value> elements;
            elements.reserve(value.size());
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                auto r = attach(value.at(i));
                if (r.flow != Flow::Normal || !r.value)
                    return r;
                elements.push_back(std::move(*r.value));
            }

            if (value.kind() == Kind::Aggregate)
                return folded(comptime::Value::make_aggregate(std::move(elements), type));
            if (elements.empty() || !element || !m_context.types)
                return folded(comptime::Value::make_slice(std::move(elements), type));

            auto count = elements.size();
            auto array = m_context.types->array_t(element, count);
            auto base = m_heap.intern(std::format("{}", static_cast<void const*>(&value)), comptime::Value::make_aggregate(std::move(elements), array));
            auto first = m_heap.subobject(base, 0);
            if (!first)
                return failure("slice has no compile-time storage");
            return folded(comptime::Value::make_slice_ref(std::move(*first), count, type));
        }

        Result slice_bounds(ast::RangeExpr const& range, std::size_t length, std::size_t& start, std::size_t& end)
        {
            std::int64_t first = 0;
            std::int64_t last = static_cast<std::int64_t>(length);
            if (range.start)
            {
                auto r = index_of(*range.start, first);
                if (r.flow != Flow::Normal)
                    return r;
            }
            if (range.end)
            {
                auto r = index_of(*range.end, last);
                if (r.flow != Flow::Normal)
                    return r;
                if (range.inclusive)
                    ++last;
            }
            if (first < 0 || last < first || static_cast<std::size_t>(last) > length)
                return failure("slice bounds out of range", true);

            start = static_cast<std::size_t>(first);
            end = static_cast<std::size_t>(last);
            return {};
        }

        Result subslice(ast::IndexExpr const& expr)
        {
            auto const& range = static_cast<ast::RangeExpr const&>(*expr.index);
            auto type = type_of(*expr.object);

            comptime::ValuePtr base;
            std::size_t length = 0;
            if (auto const* array = types::type_cast<types::ArrayType>(type))
            {
                length = static_cast<std::size_t>(array->count);
                if (length != 0)
                {
                    comptime::ValuePtr object;
                    auto r = place(*expr.object, object);
                    if (r.flow != Flow::Normal)
                        return r;
                    auto first = m_heap.subobject(object, 0);
                    if (!first)
                        return failure("slice has no compile-time storage");
                    base = std::move(*first);
                }
            }
            else
            {
                auto r = expression(*expr.object);
                if (r.flow != Flow::Normal || !r.value)
                    return r;
                if (r.value->kind() != Kind::Slice)
                    return failure("slice has no compile-time storage");
                base = r.value->slice_base();
                length = r.value->slice_length();
            }

            std::size_t start = 0;
            std::size_t end = 0;
            auto bounds = slice_bounds(range, length, start, end);
            if (bounds.flow != Flow::Normal)
                return bounds;
            if (start == end)
                return folded(comptime::Value::make_slice({}, type_of(expr)));

            auto first = m_heap.offset(base, static_cast<std::int64_t>(start));
            if (!first)
                return failure("slice bounds out of range", true);
            return folded(comptime::Value::make_slice_ref(std::move(*first), end - start, type_of(expr)));
        }

        Result convert(ast::Expr const& expr, types::TypePtr target)
        {
            auto const* array = types::type_cast<types::ArrayType>(type_of(expr));
            bool to_slice = types::type_cast<types::SliceType>(target) != nullptr;
            bool to_pointer = types::type_cast<types::PointerType>(target) != nullptr;
            if (!array || (!to_slice && !to_pointer))
                return expression(expr);

            if (array->count == 0 && to_slice)
                return folded(comptime::Value::make_slice({}, target));

            comptime::ValuePtr object;
            auto r = place(expr, object);
            if (r.flow != Flow::Normal)
                return r;
            auto first = m_heap.subobject(object, 0);
            if (!first)
                return failure("array has no compile-time storage");

            if (to_pointer)
                return folded(comptime::Value::make_pointer_to(std::move(*first), target));
            return folded(comptime::Value::make_slice_ref(std::move(*first), static_cast<std::size_t>(array->count), target));
        }

        Result aggregate_literal(ast::StructLiteralExpr const& expr)
        {
            auto type = type_of(expr);
            switch (expr.sema.construction_kind)
            {
                case ast::ExprSema::ConstructionKind::Struct: {
                    auto fields = record_fields(type);
                    if (fields.empty() || expr.fields.size() != fields.size())
                        return failure("aggregate requires fully initialized resolved fields");

                    std::vector<comptime::Value> elements(fields.size());
                    for (auto const& field : expr.fields)
                    {
                        if (field.resolved_field_index >= elements.size())
                            return failure("aggregate field index is unresolved");
                        auto r = convert(*field.value, type_of(fields[field.resolved_field_index].type));
                        if (r.flow != Flow::Normal || !r.value)
                            return r;
                        elements[field.resolved_field_index] = std::move(*r.value);
                    }
                    return folded(comptime::Value::make_aggregate(std::move(elements), type));
                }
                case ast::ExprSema::ConstructionKind::Array:
                case ast::ExprSema::ConstructionKind::Slice: {
                    auto element = element_of(type);
                    std::vector<comptime::Value> elements;
                    elements.reserve(expr.fields.size());
                    for (auto const& field : expr.fields)
                    {
                        auto r = convert(*field.value, element);
                        if (r.flow != Flow::Normal || !r.value)
                            return r;
                        elements.push_back(std::move(*r.value));
                    }
                    if (expr.sema.construction_kind == ast::ExprSema::ConstructionKind::Slice)
                        return folded(comptime::Value::make_slice(std::move(elements), type));

                    auto const* array = types::type_cast<types::ArrayType>(type);
                    if (array)
                        elements.resize(static_cast<std::size_t>(array->count), default_object(array->element));
                    return folded(comptime::Value::make_aggregate(std::move(elements), type));
                }
                default:
                    break;
            }
            return unsupported();
        }

        Result enum_construction(ast::CallExpr const& call)
        {
            auto const* variant = call.sema.constructed_variant;
            auto type = type_of(call);
            auto const* enum_type = types::type_cast<types::EnumType>(type);
            if (!variant || !enum_type)
                return failure("enum construction has not been resolved");

            if (!enum_type->is_tagged || !enum_type->tagged_layout)
                return folded(comptime::Value::make_int(variant->discriminant, enum_type->backing));

            std::vector<comptime::Value> elements;
            elements.push_back(comptime::Value::make_int(variant->discriminant, enum_type->tagged_layout->discriminant_type));
            if (!call.args.empty() && !variant->payload.empty())
            {
                auto r = convert(*call.args.front(), type_of(variant->payload.front()));
                if (r.flow != Flow::Normal || !r.value)
                    return r;
                elements.push_back(std::move(*r.value));
            }
            return folded(comptime::Value::make_aggregate(std::move(elements), type));
        }

        Result subject_place(ast::Expr const& expr, comptime::ValuePtr& out)
        {
            auto r = place(expr, out);
            if (r.flow == Flow::Normal || r.flow == Flow::Error)
                return r;

            r = expression(expr);
            if (r.flow != Flow::Normal || !r.value)
                return r;
            if (m_cells++ >= m_context.memory_limit)
                return failure("memory limit exceeded", true);

            out = m_heap.allocate(std::move(*r.value), true);
            m_frames.back().allocations.push_back(out.allocation);
            return {};
        }

        Result discriminant_of(comptime::Value const& value, std::int64_t& out)
        {
            auto const* enum_type = types::type_cast<types::EnumType>(value.type);
            if (enum_type && enum_type->is_tagged)
            {
                if (value.kind() != Kind::Aggregate || value.empty())
                    return failure("enum value has no compile-time discriminant");
                auto tag = value.at(0).const_to_int();
                if (!tag)
                    return failure("enum value has no compile-time discriminant");
                out = *tag;
                return {};
            }

            auto tag = value.const_to_int();
            if (!tag)
                return failure("enum value has no compile-time discriminant");
            out = *tag;
            return {};
        }

        Result match_pattern(ast::Pattern const& pattern, comptime::ValuePtr const& subject, bool& matched)
        {
            if (!step())
                return exhausted();

            matched = false;
            switch (pattern.kind)
            {
                case ast::PatternKind::Wildcard:
                    matched = true;
                    return {};
                case ast::PatternKind::Ref:
                    return match_pattern(*static_cast<ast::RefPattern const&>(pattern).inner, subject, matched);
                case ast::PatternKind::Binding: {
                    auto const& binding = static_cast<ast::BindingPattern const&>(pattern);
                    if (!binding.synthetic_decl)
                        return failure("pattern binding has not been resolved");

                    matched = true;
                    if (binding.by_reference)
                    {
                        auto matched_type = reinterpret_cast<types::TypePtr>(pattern.matched_type);
                        if (!m_context.types || !matched_type)
                            return failure("pattern binding has not been resolved");
                        auto type = m_context.types->pointer_to(matched_type, types::Qual::None);
                        return bind(binding.synthetic_decl, comptime::Value::make_pointer_to(subject, type));
                    }

                    auto value = read(subject);
                    if (value.flow != Flow::Normal || !value.value)
                        return value;
                    return bind(binding.synthetic_decl, std::move(*value.value));
                }
                case ast::PatternKind::Literal: {
                    auto const* literal = static_cast<ast::LiteralPattern const&>(pattern).value;
                    if (!literal || !literal->sema.const_value)
                        return failure("literal pattern has not been resolved");
                    auto value = read(subject);
                    if (value.flow != Flow::Normal || !value.value)
                        return value;
                    matched = *value.value == *literal->sema.const_value;
                    return {};
                }
                case ast::PatternKind::Range: {
                    auto const& range = static_cast<ast::RangePattern const&>(pattern);
                    auto value = read(subject);
                    if (value.flow != Flow::Normal || !value.value)
                        return value;
                    auto scalar = value.value->const_to_int();
                    if (!scalar)
                        return failure("range pattern requires a compile-time integer");

                    std::int64_t low = std::numeric_limits<std::int64_t>::min();
                    std::int64_t high = std::numeric_limits<std::int64_t>::max();
                    if (range.start)
                    {
                        auto r = index_of(*range.start, low);
                        if (r.flow != Flow::Normal)
                            return r;
                    }
                    if (range.end)
                    {
                        auto r = index_of(*range.end, high);
                        if (r.flow != Flow::Normal)
                            return r;
                        if (!range.inclusive)
                            --high;
                    }
                    matched = *scalar >= low && *scalar <= high;
                    return {};
                }
                case ast::PatternKind::Or: {
                    for (auto* alternative : static_cast<ast::OrPattern const&>(pattern).alternatives)
                    {
                        auto r = match_pattern(*alternative, subject, matched);
                        if (r.flow != Flow::Normal || matched)
                            return r;
                    }
                    return {};
                }
                case ast::PatternKind::EnumDestructure: {
                    auto const& destructure = static_cast<ast::EnumDestructurePattern const&>(pattern);
                    if (!destructure.resolved_variant)
                        return failure("enum pattern has not been resolved");

                    auto value = read(subject);
                    if (value.flow != Flow::Normal || !value.value)
                        return value;

                    std::int64_t tag{};
                    auto r = discriminant_of(*value.value, tag);
                    if (r.flow != Flow::Normal)
                        return r;
                    if (tag != destructure.resolved_variant->discriminant)
                        return {};
                    if (destructure.payload.empty())
                    {
                        matched = true;
                        return {};
                    }

                    auto payload = m_heap.subobject(subject, 1);
                    if (!payload)
                        return failure("enum payload has no compile-time storage");
                    return match_pattern(*destructure.payload.front(), *payload, matched);
                }
                case ast::PatternKind::StructDestructure: {
                    auto const& destructure = static_cast<ast::StructDestructurePattern const&>(pattern);
                    for (auto const& field : destructure.fields)
                    {
                        auto member = m_heap.subobject(subject, field.resolved_field_index);
                        if (!member)
                            return failure("member has no compile-time storage");
                        auto r = match_pattern(*field.pattern, *member, matched);
                        if (r.flow != Flow::Normal || !matched)
                            return r;
                    }
                    matched = true;
                    return {};
                }
            }
            return unsupported();
        }

        Result match_value(ast::MatchExpr const& expr)
        {
            comptime::ValuePtr subject;
            auto r = subject_place(*expr.operand, subject);
            if (r.flow != Flow::Normal)
                return r;

            for (auto const& arm : expr.arms)
            {
                if (!arm.pattern || !arm.body)
                    return failure("match arm has not been resolved");

                bool matched = false;
                r = match_pattern(*arm.pattern, subject, matched);
                if (r.flow != Flow::Normal)
                    return r;
                if (!matched)
                    continue;

                if (arm.guard)
                {
                    auto guard = expression(*arm.guard);
                    if (guard.flow != Flow::Normal)
                        return guard;
                    auto truth = guard.value ? guard.value->const_to_bool() : std::nullopt;
                    if (!truth)
                        return failure("match guard has no compile-time value");
                    if (!*truth)
                        continue;
                }
                return expression(*arm.body);
            }
            return failure("no match arm applies", true);
        }

        Result member_value(ast::FieldAccessExpr const& expr)
        {
            if (types::type_cast<types::SliceType>(type_of(*expr.object)))
            {
                auto object = expression(*expr.object);
                if (object.flow != Flow::Normal || !object.value || object.value->kind() != Kind::Slice)
                    return object;
                if (expr.field == "len")
                    return folded(comptime::Value::make_int(static_cast<std::int64_t>(object.value->slice_length()), type_of(expr)));
                if (expr.field == "ptr" && object.value->slice_is_ref())
                    return folded(comptime::Value::make_pointer_to(object.value->slice_base(), type_of(expr)));
                return failure("slice field has no compile-time value");
            }

            comptime::ValuePtr member;
            auto r = field_place(expr, member);
            if (r.flow != Flow::Normal)
                return r;
            return read(member);
        }

        Result element_value(ast::IndexExpr const& expr)
        {
            if (expr.index->kind == ast::ExprKind::Range)
                return subslice(expr);

            comptime::ValuePtr element;
            auto r = element_place(expr, element);
            if (r.flow != Flow::Normal)
                return r;
            return read(element);
        }

        Result offset_pointer(comptime::Value const& pointer, std::int64_t delta)
        {
            if (pointer.is_null_ptr())
                return failure("arithmetic on a null pointer", true);

            auto moved = m_heap.offset(pointer.get_pointer(), delta);
            if (!moved)
                return failure("pointer arithmetic leaves the bounds of a compile-time object", true);
            return folded(comptime::Value::make_pointer_to(std::move(*moved), pointer.type));
        }

        static bool share_container(comptime::ValuePtr const& a, comptime::ValuePtr const& b)
        {
            if (a.is_null || b.is_null || a.allocation != b.allocation || a.path.size() != b.path.size() || a.path.empty())
                return false;
            return std::equal(a.path.begin(), a.path.end() - 1, b.path.begin());
        }

        Result pointer_binary(lex::TokenKind op, comptime::Value const& lhs, comptime::Value const& rhs, types::TypePtr out_type)
        {
            using K = lex::TokenKind;
            bool lhs_pointer = lhs.kind() == Kind::Pointer;
            bool rhs_pointer = rhs.kind() == Kind::Pointer;
            bool equality = op == K::EqEq || op == K::BangEq;

            if (equality && (lhs.kind() == Kind::Null || rhs.kind() == Kind::Null))
            {
                auto const& pointer = lhs_pointer ? lhs : rhs;
                if (pointer.kind() != Kind::Pointer)
                    return folded(comptime::Value::make_bool(op == K::EqEq, out_type));
                return folded(comptime::Value::make_bool(pointer.is_null_ptr() == (op == K::EqEq), out_type));
            }

            if (lhs_pointer && rhs_pointer && equality)
                return folded(comptime::Value::make_bool((lhs.get_pointer() == rhs.get_pointer()) == (op == K::EqEq), out_type));

            if (lhs_pointer && rhs.kind() == Kind::Int && (op == K::Plus || op == K::Minus))
                return offset_pointer(lhs, op == K::Plus ? rhs.get_int() : -rhs.get_int());

            if (rhs_pointer && lhs.kind() == Kind::Int && op == K::Plus)
                return offset_pointer(rhs, lhs.get_int());

            if (lhs_pointer && rhs_pointer)
            {
                auto const& a = lhs.get_pointer();
                auto const& b = rhs.get_pointer();
                if (!share_container(a, b))
                    return failure(op == K::Minus ? "difference of unrelated compile-time pointers" : "comparison of unrelated compile-time pointers", true);

                auto first = static_cast<std::int64_t>(a.path.back());
                auto second = static_cast<std::int64_t>(b.path.back());
                if (op == K::Minus)
                    return folded(comptime::Value::make_int(first - second, out_type));
                return folded(const_eval::fold_int_cmp(op, first, second, out_type));
            }
            return unsupported();
        }

        Result adjust(ast::Expr const& target, bool increment, bool prefix)
        {
            comptime::ValuePtr ptr;
            auto r = place(target, ptr);
            if (r.flow != Flow::Normal)
                return r;

            auto old = read(ptr);
            if (old.flow != Flow::Normal || !old.value)
                return old;

            std::int64_t delta = increment ? 1 : -1;
            std::optional<comptime::Value> next;
            switch (old.value->kind())
            {
                case Kind::Int:
                    next = comptime::Value::fold_int_binary(increment ? comptime::BinaryOp::Add : comptime::BinaryOp::Sub, old.value->get_int(), 1,
                                                            old.value->type);
                    if (!next)
                        return failure("integer overflow", true);
                    break;
                case Kind::Float:
                    next = comptime::Value::make_float(old.value->get_float() + static_cast<double>(delta), old.value->type);
                    break;
                case Kind::Pointer: {
                    auto moved = offset_pointer(*old.value, delta);
                    if (moved.flow != Flow::Normal)
                        return moved;
                    next = std::move(moved.value);
                    break;
                }
                default:
                    return unsupported();
            }

            auto* slot = m_heap.write_target(ptr);
            if (!slot)
                return failure("write to read-only storage");
            *slot = *next;
            return prefix ? folded(std::move(next)) : folded(std::move(old.value));
        }

        static lex::TokenKind compound_base(lex::TokenKind op)
        {
            using K = lex::TokenKind;
            switch (op)
            {
                case K::PlusEq:
                    return K::Plus;
                case K::MinusEq:
                    return K::Minus;
                case K::StarEq:
                    return K::Star;
                case K::SlashEq:
                    return K::Slash;
                case K::PercentEq:
                    return K::Percent;
                case K::AmpEq:
                    return K::Amp;
                case K::PipeEq:
                    return K::Pipe;
                case K::CaretEq:
                    return K::Caret;
                case K::LtLtEq:
                    return K::LtLt;
                case K::GtGtEq:
                    return K::GtGt;
                default:
                    return op;
            }
        }

        Result binary(ast::BinaryExpr const& expr)
        {
            using K = lex::TokenKind;
            if (expr.op == K::Eq)
            {
                auto rhs = convert(*expr.rhs, type_of(*expr.lhs));
                if (rhs.flow != Flow::Normal || !rhs.value)
                    return rhs;
                return store(*expr.lhs, std::move(*rhs.value));
            }

            auto lhs = expression(*expr.lhs);
            if (lhs.flow != Flow::Normal || !lhs.value)
                return lhs;

            if (expr.op == K::AmpAmp || expr.op == K::PipePipe)
            {
                auto truth = lhs.value->const_to_bool();
                if (!truth)
                    return failure("condition has no compile-time value");
                if (*truth == (expr.op == K::PipePipe))
                    return folded(comptime::Value::make_bool(*truth, type_of(expr)));
                return expression(*expr.rhs);
            }

            auto rhs = expression(*expr.rhs);
            if (rhs.flow != Flow::Normal || !rhs.value)
                return rhs;

            auto op = compound_base(expr.op);
            auto out_type = op == expr.op ? type_of(expr) : type_of(*expr.lhs);

            Result r{};
            if (lhs.value->kind() == Kind::Pointer || rhs.value->kind() == Kind::Pointer)
                r = pointer_binary(op, *lhs.value, *rhs.value, out_type);
            else
            {
                r = folded(const_eval::fold_binary(op, *lhs.value, *rhs.value, out_type));
                if (r.failed() && lhs.value->kind() == Kind::Int && rhs.value->kind() == Kind::Int)
                {
                    if ((op == K::Slash || op == K::Percent) && rhs.value->get_int() == 0)
                        return failure("division by zero", true);
                    if (op == K::Plus || op == K::Minus || op == K::Star || op == K::Slash || op == K::Percent)
                        return failure("integer overflow", true);
                    if (op == K::LtLt || op == K::GtGt)
                        return failure("shift amount out of range", true);
                }
            }

            if (op != expr.op && r.value)
                return store(*expr.lhs, std::move(*r.value));
            return r;
        }

        Result callee_arguments(ast::CallExpr const& call, ast::FuncDecl const& fn, std::vector<comptime::Value>& args)
        {
            if (call.sema.ufcs_callee)
            {
                auto* field = ast::node_cast<ast::FieldAccessExpr>(call.callee);
                if (!field)
                    return failure("UFCS receiver has not been resolved");

                Result receiver{};
                if (fn.params.empty())
                    return failure("UFCS receiver has not been resolved");

                if (field->object->sema.implicit_addr_of)
                {
                    comptime::ValuePtr object;
                    receiver = place(*field->object, object);
                    if (receiver.flow != Flow::Normal)
                        return receiver;
                    receiver = folded(comptime::Value::make_pointer_to(std::move(object), type_of(fn.params.front().type)));
                }
                else if (field->object->sema.implicit_deref)
                {
                    comptime::ValuePtr object;
                    receiver = base_place(*field->object, object);
                    if (receiver.flow != Flow::Normal)
                        return receiver;
                    receiver = read(object);
                }
                else
                    receiver = convert(*field->object, type_of(fn.params.front().type));

                if (receiver.flow != Flow::Normal || !receiver.value)
                    return receiver;
                args.push_back(std::move(*receiver.value));
            }

            for (std::size_t i = call.sema.call_argument_offset; i < call.args.size(); ++i)
            {
                auto index = args.size();
                auto target = index < fn.params.size() ? type_of(fn.params[index].type) : nullptr;
                auto r = convert(*call.args[i], target);
                if (r.flow != Flow::Normal)
                    return r;
                if (!r.value)
                    return failure("call argument has no compile-time value");
                args.push_back(std::move(*r.value));
            }
            return {};
        }

        Result call(ast::CallExpr const& call)
        {
            if (call.sema.construction_kind == ast::ExprSema::ConstructionKind::Enum)
                return enum_construction(call);

            if (call.sema.construction_kind == ast::ExprSema::ConstructionKind::Struct)
            {
                std::vector<comptime::Value> fields;
                for (auto* arg : call.args)
                {
                    auto r = expression(*arg);
                    if (r.flow != Flow::Normal || !r.value)
                        return r;
                    fields.push_back(std::move(*r.value));
                }
                return folded(comptime::Value::make_aggregate(std::move(fields), type_of(call)));
            }

            if (!call.callee || (call.callee->kind != ast::ExprKind::Ident && call.callee->kind != ast::ExprKind::PathExpr &&
                                 call.callee->kind != ast::ExprKind::TemplateInst && !call.sema.ufcs_callee))
                return failure("indirect call");

            auto* fn = call.sema.resolved_specialization;
            if (!fn)
                fn = call.callee->sema.resolved_specialization;
            if (!fn)
                fn = ast::node_cast<ast::FuncDecl>(call.sema.resolved_decl);
            if (!fn)
                fn = ast::node_cast<ast::FuncDecl>(call.callee->sema.resolved_decl);
            if (!fn)
                fn = ast::node_cast<ast::FuncDecl>(call.sema.ufcs_callee);
            if (!fn || fn->is_extern || fn->sema.is_intrinsic || !fn->body)
                return failure("external, indirect or runtime-only call");
            if (m_frames.size() > m_context.recursion_limit)
                return failure("recursion limit exceeded", true);
            if (call.sema.call_argument_offset > call.args.size())
                return failure("call argument mapping is unresolved");

            if (m_context.prepare_function && !m_context.prepare_function(*fn))
                return failure("function body is not available at compile time");

            std::vector<comptime::Value> args;
            auto r = callee_arguments(call, *fn, args);
            if (r.flow != Flow::Normal)
                return r;

            if (fn->sema.storage == ast::StorageClass::Unresolved || !fn->template_params.empty() || args.size() != fn->params.size())
                return failure("call requires a resolved function and materialized arguments");

            m_frames.push_back(Frame{Call{fn, call.range}, {}, {}});
            auto result = enter(*fn, std::move(args));
            for (auto allocation : m_frames.back().allocations)
                m_heap.end_lifetime(allocation);
            m_frames.pop_back();
            return result;
        }

        Result enter(ast::FuncDecl const& fn, std::vector<comptime::Value> args)
        {
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                if (volatile_decl(fn.params[i].synthetic_decl))
                    return failure("volatile access");
                auto r = bind(fn.params[i].synthetic_decl, std::move(args[i]));
                if (r.flow != Flow::Normal)
                    return r;
            }

            auto result = block(*fn.body);
            if (result.flow == Flow::Return)
                result.flow = Flow::Normal;
            else if (result.flow == Flow::Break || result.flow == Flow::Continue)
                return failure("control flow escaping a compile-time function", true);
            return result;
        }

        Result loop(ast::Expr const* condition, ast::Block const& body, ast::Expr const* update, bool first)
        {
            for (;;)
            {
                if (!step())
                    return exhausted();
                if (!first && condition)
                {
                    auto r = expression(*condition);
                    if (r.flow != Flow::Normal)
                        return r;
                    auto truth = r.value ? r.value->const_to_bool() : std::nullopt;
                    if (!truth)
                        return failure("loop condition has no compile-time value");
                    if (!*truth)
                        return {};
                }
                first = false;

                auto r = block(body);
                if (r.flow == Flow::Break)
                    return {};
                if (r.flow != Flow::Normal && r.flow != Flow::Continue)
                    return r;
                if (update)
                {
                    r = expression(*update);
                    if (r.flow != Flow::Normal)
                        return r;
                }
            }
        }

        Result declaration(ast::Decl const* decl)
        {
            auto* var = ast::node_cast<ast::VarDecl>(decl);
            if (!var)
                return failure("unsupported compile-time declaration");
            if (volatile_decl(var))
                return failure("volatile access");

            auto type = type_of(var->type);
            if (!var->init)
                return bind(var, default_object(type));

            auto r = convert(*var->init, type);
            if (r.flow != Flow::Normal || !r.value)
                return r;
            return bind(var, std::move(*r.value));
        }

        Result statement(ast::Stmt const& stmt)
        {
            if (!step())
                return exhausted();

            switch (stmt.kind)
            {
                case ast::StmtKind::Expr:
                    return expression(*static_cast<ast::ExprStmt const&>(stmt).expr);
                case ast::StmtKind::DeclStmt:
                    return declaration(static_cast<ast::DeclStmt const&>(stmt).decl);
                case ast::StmtKind::Return: {
                    auto* value = static_cast<ast::ReturnStmt const&>(stmt).value;
                    auto* fn = m_frames.back().call.function;
                    auto r = value ? convert(*value, fn && fn->return_type ? type_of(fn->return_type) : nullptr) : Result{};
                    if (r.flow == Flow::Normal)
                        r.flow = Flow::Return;
                    return r;
                }
                case ast::StmtKind::Break:
                    return {Flow::Break, {}, {}, {}};
                case ast::StmtKind::Continue:
                    return {Flow::Continue, {}, {}, {}};
                case ast::StmtKind::While: {
                    auto const& s = static_cast<ast::WhileStmt const&>(stmt);
                    return loop(s.condition, s.body, nullptr, false);
                }
                case ast::StmtKind::DoWhile: {
                    auto const& s = static_cast<ast::DoWhileStmt const&>(stmt);
                    return loop(s.condition, s.body, nullptr, true);
                }
                case ast::StmtKind::For: {
                    auto const& s = static_cast<ast::ForStmt const&>(stmt);
                    auto r = s.init ? statement(*s.init) : Result{};
                    if (r.flow != Flow::Normal)
                        return r;
                    return loop(s.cond, s.body, s.update, false);
                }
                case ast::StmtKind::StaticIf: {
                    auto const& s = static_cast<ast::StaticIfStmt const&>(stmt);
                    if (!s.is_type_if && (!s.condition || !s.condition->sema.const_value))
                        return failure("static if condition has not been resolved");
                    if (s.taken_branch == 0)
                        return block(s.then_block);
                    if (s.taken_branch == 1)
                        return s.else_branch ? statement(*s.else_branch) : Result{};
                    return failure("static if has not been resolved");
                }
                default:
                    return failure("statement is unavailable at compile time");
            }
        }

        Result block(ast::Block const& body)
        {
            for (auto* stmt : body.stmts)
            {
                auto r = statement(*stmt);
                if (r.flow != Flow::Normal)
                    return r;
            }
            return body.tail ? expression(*body.tail) : Result{};
        }

        Result expression(ast::Expr const& expr)
        {
            if (!step())
                return exhausted();
            if (auto type = type_of(expr); type && type->kind == types::TypeKind::Error)
                return failure("expression has no resolved type");

            switch (expr.kind)
            {
                case ast::ExprKind::Ident:
                case ast::ExprKind::PathExpr: {
                    comptime::ValuePtr object;
                    auto r = named_place(expr, object);
                    if (r.flow != Flow::Normal)
                        return r;
                    return read(object);
                }
                case ast::ExprKind::Call:
                    return call(static_cast<ast::CallExpr const&>(expr));
                case ast::ExprKind::Binary:
                    return binary(static_cast<ast::BinaryExpr const&>(expr));
                case ast::ExprKind::Postfix: {
                    auto const& e = static_cast<ast::PostfixExpr const&>(expr);
                    if (e.op != lex::TokenKind::Increment && e.op != lex::TokenKind::Decrement)
                        return unsupported();
                    return adjust(*e.operand, e.op == lex::TokenKind::Increment, false);
                }
                case ast::ExprKind::Unary: {
                    auto const& e = static_cast<ast::UnaryExpr const&>(expr);
                    if (e.op == lex::TokenKind::Increment || e.op == lex::TokenKind::Decrement)
                        return adjust(*e.operand, e.op == lex::TokenKind::Increment, true);
                    if (e.op == lex::TokenKind::Amp)
                    {
                        comptime::ValuePtr object;
                        auto r = place(*e.operand, object);
                        if (r.flow != Flow::Normal)
                            return r;
                        return folded(comptime::Value::make_pointer_to(std::move(object), type_of(expr)));
                    }
                    if (e.op == lex::TokenKind::Star)
                    {
                        comptime::ValuePtr object;
                        auto r = place(expr, object);
                        if (r.flow != Flow::Normal)
                            return r;
                        return read(object);
                    }
                    auto r = expression(*e.operand);
                    if (r.flow != Flow::Normal || !r.value)
                        return r;
                    return folded(const_eval::fold_unary(e.op, *r.value, type_of(expr)));
                }
                case ast::ExprKind::Cast: {
                    auto const& e = static_cast<ast::CastExpr const&>(expr);
                    auto r = convert(*e.operand, type_of(expr));
                    if (r.flow != Flow::Normal || !r.value)
                        return r;
                    if (r.value->type == type_of(expr))
                        return r;
                    return folded(const_eval::fold_cast(*r.value, type_of(expr)));
                }
                case ast::ExprKind::Block:
                    return block(static_cast<ast::BlockExpr const&>(expr).body);
                case ast::ExprKind::If: {
                    auto const& e = static_cast<ast::IfExpr const&>(expr);
                    auto r = expression(*e.condition);
                    if (r.flow != Flow::Normal)
                        return r;
                    auto truth = r.value ? r.value->const_to_bool() : std::nullopt;
                    if (!truth)
                        return failure("condition has no compile-time value");
                    return *truth ? block(e.then_block) : e.else_branch ? expression(*e.else_branch) : Result{};
                }
                case ast::ExprKind::StructLiteral:
                    return aggregate_literal(static_cast<ast::StructLiteralExpr const&>(expr));
                case ast::ExprKind::FieldAccess:
                    return member_value(static_cast<ast::FieldAccessExpr const&>(expr));
                case ast::ExprKind::Index:
                    return element_value(static_cast<ast::IndexExpr const&>(expr));
                case ast::ExprKind::Match:
                    return match_value(static_cast<ast::MatchExpr const&>(expr));
                case ast::ExprKind::IntLiteral:
                case ast::ExprKind::FloatLiteral:
                case ast::ExprKind::BoolLiteral:
                case ast::ExprKind::CharLiteral:
                case ast::ExprKind::U16CharLiteral:
                case ast::ExprKind::NullLiteral:
                case ast::ExprKind::Sizeof:
                case ast::ExprKind::Alignof:
                case ast::ExprKind::Offsetof:
                case ast::ExprKind::SizeofPack:
                case ast::ExprKind::Compiles:
                case ast::ExprKind::StringLiteral:
                case ast::ExprKind::U16StringLiteral:
                    if (expr.sema.const_value)
                        return attach(*expr.sema.const_value);
                    return failure("expression has no resolved compile-time value");
                default:
                    return unsupported();
            }
        }

        std::optional<std::string> read_text(comptime::ValuePtr const& base, std::optional<std::size_t> length) const
        {
            std::string text;
            for (std::size_t i = 0; !length || i < *length; ++i)
            {
                auto element = m_heap.offset(base, static_cast<std::int64_t>(i));
                if (!element)
                    return std::nullopt;
                auto const* value = m_heap.read(*element);
                if (!value)
                    return std::nullopt;
                auto unit = value->const_to_int();
                if (!unit)
                    return std::nullopt;
                if (!length && *unit == 0)
                    break;
                text.push_back(static_cast<char>(*unit));
            }
            return text;
        }

        Result detach(comptime::Value value)
        {
            switch (value.kind())
            {
                case Kind::Pointer: {
                    if (value.is_null_ptr())
                        return folded(std::move(value));
                    if (!m_heap.is_live(value.get_pointer()))
                        return expired();
                    if (!is_byte_type(pointee_of(value.type)))
                        return failure("compile-time pointer cannot be used as a constant");
                    auto text = read_text(value.get_pointer(), std::nullopt);
                    if (!text)
                        return failure("compile-time pointer cannot be used as a constant");
                    return folded(comptime::Value::make_string(std::move(*text), value.type));
                }
                case Kind::Slice: {
                    if (!value.slice_is_ref())
                        break;
                    auto length = value.slice_length();
                    auto base = value.slice_base();
                    if (!m_heap.is_live(base))
                        return expired();
                    if (is_byte_type(element_of(value.type)))
                    {
                        auto text = read_text(base, length);
                        if (!text)
                            return failure("compile-time slice cannot be used as a constant");
                        return folded(comptime::Value::make_string(std::move(*text), value.type));
                    }

                    std::vector<comptime::Value> elements;
                    elements.reserve(length);
                    for (std::size_t i = 0; i < length; ++i)
                    {
                        auto element = m_heap.offset(base, static_cast<std::int64_t>(i));
                        auto const* stored = element ? m_heap.read(*element) : nullptr;
                        if (!stored)
                            return failure("compile-time slice cannot be used as a constant");
                        auto r = detach(*stored);
                        if (r.flow != Flow::Normal || !r.value)
                            return r;
                        elements.push_back(std::move(*r.value));
                    }
                    return folded(comptime::Value::make_slice(std::move(elements), value.type));
                }
                default:
                    break;
            }

            if (value.kind() == Kind::Aggregate || value.kind() == Kind::Slice)
                for (std::size_t i = 0; i < value.size(); ++i)
                {
                    auto r = detach(value.at(i));
                    if (r.flow != Flow::Normal || !r.value)
                        return r;
                    value.at(i) = std::move(*r.value);
                }
            return folded(std::move(value));
        }

    public:
        explicit Evaluator(Context context = {}, Mode mode = Mode::Required) : m_context(std::move(context)), m_mode(mode) {}

        Result evaluate(ast::Expr const& expr)
        {
            m_steps = 0;
            m_cells = 0;
            m_frames.clear();
            m_frames.push_back(Frame{Call{m_context.function, m_context.call_site}, {}, {}});

            auto r = expression(expr);
            if (r.flow == Flow::Normal && r.value)
                r = detach(std::move(*r.value));
            if (m_mode == Mode::Opportunistic && r.failed())
                r.flow = Flow::NotEvaluatable;
            return r;
        }
    };
}
