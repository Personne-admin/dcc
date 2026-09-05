module;

export module dcc.ctfe.memory;

import std;
import dcc.comptime;
import dcc.types;

export namespace dcc::ctfe
{
    struct Allocation
    {
        std::vector<comptime::Value> elements;
        types::TypePtr type{};
        bool is_mutable{true};
        bool live{true};
    };

    class Heap
    {
        std::deque<Allocation> m_allocations;
        std::unordered_map<std::string, std::size_t> m_literals;

        template <typename Self> static auto* locate(Self& self, comptime::ValuePtr const& p)
        {
            using Result = std::conditional_t<std::is_const_v<Self>, comptime::Value const, comptime::Value>;
            if (p.is_null || p.allocation >= self.m_allocations.size() || p.path.empty())
                return static_cast<Result*>(nullptr);

            auto& a = self.m_allocations[p.allocation];
            if (!a.live || p.path.front() >= a.elements.size())
                return static_cast<Result*>(nullptr);

            Result* value = &a.elements[p.path.front()];
            for (auto index : std::span{p.path}.subspan(1))
            {
                if (!indexable(*value) || index >= value->size())
                    return static_cast<Result*>(nullptr);
                value = &value->at(index);
            }
            return value;
        }

        [[nodiscard]] static bool indexable(comptime::Value const& v)
        {
            if (v.kind() == comptime::Value::Kind::Aggregate)
                return true;
            return v.kind() == comptime::Value::Kind::Slice && !v.slice_is_ref();
        }

    public:
        [[nodiscard]] std::size_t size() const noexcept { return m_allocations.size(); }

        [[nodiscard]] comptime::ValuePtr allocate(comptime::Value object, types::TypePtr type, bool is_mutable)
        {
            m_allocations.push_back(Allocation{{std::move(object)}, type, is_mutable, true});
            return comptime::ValuePtr{false, m_allocations.size() - 1, {0}};
        }

        [[nodiscard]] comptime::ValuePtr intern(std::string_view key, comptime::Value object, types::TypePtr type)
        {
            std::string cache_key{key};
            if (auto it = m_literals.find(cache_key); it != m_literals.end())
                return comptime::ValuePtr{false, it->second, {0}};

            auto base = allocate(std::move(object), type, false);
            m_literals.emplace(std::move(cache_key), base.allocation);
            return base;
        }

        void end_lifetime(std::size_t allocation)
        {
            if (allocation < m_allocations.size())
                m_allocations[allocation].live = false;
        }

        [[nodiscard]] bool is_live(comptime::ValuePtr const& p) const
        {
            return !p.is_null && p.allocation < m_allocations.size() && m_allocations[p.allocation].live;
        }

        [[nodiscard]] bool is_mutable(comptime::ValuePtr const& p) const
        {
            return !p.is_null && p.allocation < m_allocations.size() && m_allocations[p.allocation].is_mutable;
        }

        [[nodiscard]] comptime::Value const* read(comptime::ValuePtr const& p) const { return locate(*this, p); }

        [[nodiscard]] comptime::Value* write_target(comptime::ValuePtr const& p) { return is_mutable(p) ? locate(*this, p) : nullptr; }

        [[nodiscard]] std::optional<std::size_t> container_length(comptime::ValuePtr const& p) const
        {
            if (p.is_null || p.allocation >= m_allocations.size() || p.path.empty())
                return std::nullopt;

            auto const& a = m_allocations[p.allocation];
            if (!a.live)
                return std::nullopt;
            if (p.path.size() == 1)
                return a.elements.size();

            auto parent = p;
            parent.path.pop_back();
            auto const* value = read(parent);
            if (!value || !indexable(*value))
                return std::nullopt;
            return value->size();
        }

        [[nodiscard]] std::optional<comptime::ValuePtr> offset(comptime::ValuePtr const& p, std::int64_t delta) const
        {
            auto bound = container_length(p);
            if (!bound)
                return std::nullopt;

            auto index = static_cast<std::int64_t>(p.path.back()) + delta;
            if (index < 0 || static_cast<std::size_t>(index) > *bound)
                return std::nullopt;

            auto out = p;
            out.path.back() = static_cast<std::uint32_t>(index);
            return out;
        }

        [[nodiscard]] std::optional<comptime::ValuePtr> subobject(comptime::ValuePtr const& p, std::uint32_t index) const
        {
            auto const* value = read(p);
            if (!value || !indexable(*value) || index >= value->size())
                return std::nullopt;

            auto out = p;
            out.path.push_back(index);
            return out;
        }
    };
}
