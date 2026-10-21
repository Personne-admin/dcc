export module dcc.backend.i8086.place;

import std;
import dcc.target;
import dcc.backend.x86.encode;
import dcc.backend.object.image;
import dcc.backend.i8086.link;

export namespace dcc::backend::i8086
{
    struct FlatLayoutOptions
    {
        std::optional<std::uint16_t> segment;
        std::uint16_t offset{};
        std::uint32_t stack_reserve{4096};
        target::CodeModel model{target::CodeModel::Small};
    };

    struct LayoutSection
    {
        std::string name;
        object::ImageSpace space{object::ImageSpace::Unknown};
        std::uint64_t alignment{1};
        std::uint64_t linear{};
        std::uint64_t memory_size{};
        std::uint64_t stored_size{};
        std::string reason;
    };

    struct LayoutFragment
    {
        object::FragmentId id{};
        object::ImageRole role{object::ImageRole::Unknown};
        object::ImageSpace space{object::ImageSpace::Unknown};
        std::uint64_t linear{};
        std::uint64_t offset_in_segment{};
        std::optional<std::uint64_t> file_offset;
        std::size_t section{};
        std::uint64_t stored_size{};
        std::uint64_t memory_size{};
        std::string reason;
    };

    struct LayoutSymbol
    {
        object::SymbolId id{};
        std::string name;
        object::ImageSpace space{object::ImageSpace::Absolute};
        std::uint64_t value{};
        std::uint64_t offset_in_segment{};
        bool absolute{};
        bool defined{};
    };

    struct LayoutFixup
    {
        object::FragmentId owner{};
        std::uint64_t offset{};
        std::uint64_t patch_linear{};
        std::optional<std::uint64_t> file_offset;
        std::uint64_t width{};
        std::int64_t value{};
        object::ImageExpression expression{object::ImageExpression::SymbolOffset};
        object::ImageSpace space{object::ImageSpace::Unknown};
    };

    struct LayoutGap
    {
        std::uint64_t linear{};
        std::uint64_t size{};
        std::string reason;
    };

    struct LayoutSegment
    {
        object::ImageSpace space{object::ImageSpace::Unknown};
        std::uint64_t paragraph{};
        std::uint64_t linear_base{};
        std::uint64_t memory_end{};
        bool known{};
    };

    struct FlatImageLayout
    {
        FlatLayoutOptions options;
        std::vector<LayoutSegment> segments;
        std::vector<LayoutSection> sections;
        std::vector<LayoutFragment> fragments;
        std::vector<LayoutSymbol> symbols;
        std::vector<LayoutFixup> fixups;
        std::vector<LayoutGap> gaps;
        std::uint64_t image_start{};
        std::uint64_t stored_end{};
        std::uint64_t memory_end{};
        std::uint64_t code_end{};
        std::uint64_t bss_start{};
        std::uint64_t bss_end{};
        std::uint64_t stack_bottom{};
        std::uint64_t stack_top{};
        std::uint64_t entry{};
        bool has_entry{};
        std::vector<std::uint8_t> bytes;

        [[nodiscard]] LayoutSymbol const* find_symbol(std::string_view name) const
        {
            for (auto const& symbol : symbols)
                if (symbol.name == name)
                    return &symbol;
            return nullptr;
        }
    };

    [[nodiscard]] std::expected<FlatImageLayout, std::string> layout_flat_image(object::ImageDescription const& image, FlatLayoutOptions const& options);

} // namespace dcc::backend::i8086

namespace dcc::backend::i8086
{
    namespace
    {
        using namespace dcc::backend::object;

        enum class Family : std::uint8_t
        {
            None,
            Start,
            Text,
            Rodata,
            Data,
            Bss,
        };

        [[nodiscard]] Family family_of(std::string_view section)
        {
            if (section == ".start" || section.starts_with(".start."))
                return Family::Start;
            if (section == ".text" || section.starts_with(".text."))
                return Family::Text;
            if (section == ".rodata" || section.starts_with(".rodata."))
                return Family::Rodata;
            if (section == ".data.rel.ro" || section.starts_with(".data.rel.ro.") || section == ".data" || section.starts_with(".data."))
                return Family::Data;
            if (section == ".bss" || section.starts_with(".bss."))
                return Family::Bss;
            return Family::None;
        }

        [[nodiscard]] std::string_view family_name(Family family)
        {
            switch (family)
            {
                case Family::Start:
                    return ".start";
                case Family::Text:
                    return ".text";
                case Family::Rodata:
                    return ".rodata";
                case Family::Data:
                    return ".data";
                case Family::Bss:
                    return ".bss";
                case Family::None:
                    break;
            }
            return "";
        }

        [[nodiscard]] bool discarded_section(std::string_view section)
        {
            return section == ".note" || section.starts_with(".note.") || section == ".comment" || section == ".eh_frame" || section.starts_with(".eh_frame.");
        }

        [[nodiscard]] bool power_of_two(std::uint64_t value) noexcept
        {
            return value != 0 && (value & (value - 1)) == 0;
        }

        [[nodiscard]] std::expected<std::uint64_t, std::string> align_up(std::uint64_t value, std::uint64_t alignment)
        {
            if (!power_of_two(alignment))
                return std::unexpected(std::format("invalid alignment {}", alignment));
            auto const mask = alignment - 1;
            if (value > std::numeric_limits<std::uint64_t>::max() - mask)
                return std::unexpected(std::format("aligning 0x{:X} to {} overflows the image address space", value, alignment));
            return (value + mask) & ~mask;
        }

        [[nodiscard]] std::string owner_text(ImageFragment const& fragment)
        {
            if (fragment.asm_section)
                return std::format("module asm section `{}`", fragment.section);
            if (fragment.associated_function)
                return std::format("jump table `{}`", fragment.name);
            return std::format("fragment `{}`", fragment.name);
        }

        [[nodiscard]] std::string join(std::vector<std::string> const& messages)
        {
            std::string text;
            for (auto const& message : messages)
            {
                if (!text.empty())
                    text += '\n';
                text += message;
            }
            return text;
        }

        struct Member
        {
            std::size_t index{};
            std::uint64_t alignment{1};
            std::uint64_t size{};
            std::uint64_t offset{};
            bool stored{};
        };

        struct Unit
        {
            std::string name;
            Family family{Family::None};
            ImageSpace space{ImageSpace::Unknown};
            std::uint64_t alignment{1};
            std::vector<Member> members;
            std::uint64_t size{};
            bool stored{};
            std::uint64_t linear{};
        };

        [[nodiscard]] std::uint64_t unsigned_max(std::uint64_t width)
        {
            if (width >= 8)
                return std::numeric_limits<std::uint64_t>::max();
            return (std::uint64_t{1} << (width * 8)) - 1;
        }

        [[nodiscard]] std::int64_t signed_min(std::uint64_t width)
        {
            if (width >= 8)
                return std::numeric_limits<std::int64_t>::min();
            return -static_cast<std::int64_t>(std::uint64_t{1} << (width * 8 - 1));
        }

        [[nodiscard]] std::int64_t signed_max(std::uint64_t width)
        {
            if (width >= 8)
                return std::numeric_limits<std::int64_t>::max();
            return static_cast<std::int64_t>((std::uint64_t{1} << (width * 8 - 1)) - 1);
        }

    } // namespace

    std::expected<FlatImageLayout, std::string> layout_flat_image(ImageDescription const& image, FlatLayoutOptions const& options)
    {
        std::vector<std::string> errors;
        auto const small = options.model != target::CodeModel::Unreal && options.model != target::CodeModel::Unreal32;
        auto const unreal = !small;
        auto const model_name = [](target::CodeModel model) -> std::string_view {
            switch (model)
            {
                case target::CodeModel::Unreal:
                    return "unreal";
                case target::CodeModel::Unreal32:
                    return "unreal32";
                default:
                    return "small";
            }
        };
        if (options.model != target::CodeModel::Default && options.model != target::CodeModel::Small && options.model != target::CodeModel::Unreal &&
            options.model != target::CodeModel::Unreal32)
            errors.push_back(std::format("i8086 flat layout does not support -mcmodel={}", static_cast<int>(options.model)));
        if (options.stack_reserve > 0xFFFF)
            errors.push_back(std::format("invalid -fstack-reserve value {}", options.stack_reserve));

        std::vector<std::optional<Family>> families(image.fragments.size());
        std::vector<std::size_t> combined_text;
        std::vector<std::size_t> combined_rodata_globals;
        std::vector<std::size_t> combined_rodata_jumps;
        std::vector<std::size_t> combined_relro;
        std::vector<std::size_t> combined_data;
        std::vector<std::size_t> combined_bss;
        std::vector<std::pair<std::string, std::vector<std::size_t>>> custom;
        std::vector<std::size_t> asm_fragments;
        auto add_custom = [&](std::size_t index, std::string_view section) {
            std::size_t slot = custom.size();
            for (std::size_t at = 0; at < custom.size(); ++at)
                if (custom[at].first == section)
                    slot = at;
            if (slot == custom.size())
                custom.emplace_back(std::string{section}, std::vector<std::size_t>{});
            custom[slot].second.push_back(index);
        };

        for (std::size_t index = 0; index < image.fragments.size(); ++index)
        {
            auto const& fragment = image.fragments[index];
            auto const family = family_of(fragment.section);
            families[index] = family;
            if (!fragment.alloc)
                continue;
            if (family == Family::None)
            {
                if (discarded_section(fragment.section))
                    continue;
                errors.push_back(std::format("allocated section {} ({}) is not placed by the i8086 flat image policy, which accepts only "
                                             ".start*, .text*, .rodata*, .data.rel.ro*, .data*, .bss* and discards .note*, .comment "
                                             "and .eh_frame* input sections",
                                             fragment.section, owner_text(fragment)));
                continue;
            }
            if (!power_of_two(fragment.alignment))
            {
                errors.push_back(std::format("fragment `{}` has invalid alignment {}", fragment.name, fragment.alignment));
                continue;
            }
            bool const asm_section = fragment.asm_section.has_value();
            bool const jump_table = !asm_section && fragment.associated_function.has_value();
            bool const function = !asm_section && !jump_table && fragment.function != nullptr;
            bool const global = !asm_section && fragment.global != nullptr;
            if (asm_section)
                asm_fragments.push_back(index);
            else if (jump_table)
            {
                if (fragment.section == ".rodata")
                    combined_rodata_jumps.push_back(index);
                else
                    asm_fragments.push_back(index);
            }
            else if (function)
            {
                if (!fragment.explicit_section && fragment.section == ".text")
                    combined_text.push_back(index);
                else
                    asm_fragments.push_back(index);
            }
            else if (global)
            {
                bool const combined = !fragment.explicit_section;
                if (combined && fragment.section == ".rodata")
                    combined_rodata_globals.push_back(index);
                else if (combined && fragment.section == ".data.rel.ro")
                    combined_relro.push_back(index);
                else if (combined && fragment.section == ".data")
                    combined_data.push_back(index);
                else if (combined && fragment.section == ".bss")
                    combined_bss.push_back(index);
                else
                    add_custom(index, fragment.section);
            }
            else
                add_custom(index, fragment.section);
        }

        auto const marker = model_marker(options.model);
        for (auto const& fragment : image.fragments)
            for (auto const& fixup : fragment.fixups)
            {
                if (fixup.target >= image.symbols.size())
                {
                    errors.push_back(std::format("fragment `{}` has a fixup with an invalid symbol id", fragment.name));
                    continue;
                }
                auto const& target = image.symbols[fixup.target];
                if (target.name.starts_with("__dcc_i8086_model_") && target.name != marker)
                    errors.push_back(std::format("i8086 code model mismatch: an object needs {}, but this link is for -mcmodel={} and provides {}", target.name,
                                                 model_name(options.model), marker));
            }

        if (!errors.empty())
            return std::unexpected(join(errors));

        std::unordered_map<FragmentId, std::size_t> function_order;
        for (std::size_t index = 0; index < image.functions.size(); ++index)
            function_order.emplace(image.functions[index], index);

        struct AsmKey
        {
            int kind{};
            std::size_t function{};
            std::size_t sub{};
        };
        std::unordered_map<std::size_t, AsmKey> asm_keys;
        {
            std::unordered_map<std::size_t, std::size_t> jumps_per_function;
            for (auto index : asm_fragments)
            {
                auto const& fragment = image.fragments[index];
                if (fragment.asm_section)
                    asm_keys[index] = AsmKey{0, *fragment.asm_section, 0};
                else if (fragment.associated_function)
                {
                    auto const position = function_order.find(*fragment.associated_function);
                    auto const where = position == function_order.end() ? std::size_t{0} : position->second;
                    auto& slot = jumps_per_function[where];
                    asm_keys[index] = AsmKey{2, where, slot++};
                }
                else
                {
                    auto const position = function_order.find(fragment.id);
                    asm_keys[index] = AsmKey{1, position == function_order.end() ? std::size_t{0} : position->second, 0};
                }
            }
        }
        std::vector<std::size_t> asm_order = asm_fragments;
        std::ranges::stable_sort(asm_order, [&](std::size_t left, std::size_t right) {
            auto const& a = asm_keys[left];
            auto const& b = asm_keys[right];
            return std::tie(a.kind, a.function, a.sub) < std::tie(b.kind, b.function, b.sub);
        });

        auto fragment_stored = [&](std::size_t index) { return image.fragments[index].alloc && !image.fragments[index].nobits; };
        auto fragment_size = [&](std::size_t index) -> std::uint64_t {
            return fragment_stored(index) ? image.fragments[index].bytes.size() : image.fragments[index].memory_size;
        };

        std::vector<Unit> units;
        auto add_unit = [&](std::string name, Family family, ImageSpace space, std::vector<std::size_t> const& members) {
            Unit unit;
            unit.name = std::move(name);
            unit.family = family;
            unit.space = space;
            std::uint64_t cursor = 0;
            for (auto index : members)
            {
                Member member;
                member.index = index;
                member.alignment = image.fragments[index].alignment;
                member.size = fragment_size(index);
                member.stored = fragment_stored(index);
                auto aligned = align_up(cursor, member.alignment);
                if (!aligned)
                    errors.push_back(aligned.error());
                else
                {
                    member.offset = *aligned;
                    cursor = *aligned + member.size;
                }
                unit.alignment = std::max(unit.alignment, member.alignment);
                unit.stored = unit.stored || member.stored;
                unit.members.push_back(member);
            }
            unit.size = cursor;
            units.push_back(std::move(unit));
        };
        auto add_custom_units = [&](Family family, ImageSpace space) {
            for (auto const& [name, members] : custom)
                if (family_of(name) == family)
                    add_unit(name, family, space, members);
        };
        auto add_asm_units = [&](Family family, ImageSpace space) {
            for (auto index : asm_order)
                if (families[index] == family)
                    add_unit(std::format("{}#{}", image.fragments[index].section, index), family, space, {index});
        };

        add_custom_units(Family::Start, ImageSpace::Code);
        add_asm_units(Family::Start, ImageSpace::Code);
        if (!combined_text.empty())
            add_unit(".text", Family::Text, ImageSpace::Code, combined_text);
        add_custom_units(Family::Text, ImageSpace::Code);
        add_asm_units(Family::Text, ImageSpace::Code);
        {
            std::vector<std::size_t> rodata = combined_rodata_globals;
            rodata.insert(rodata.end(), combined_rodata_jumps.begin(), combined_rodata_jumps.end());
            if (!rodata.empty())
                add_unit(".rodata", Family::Rodata, ImageSpace::Dgroup, rodata);
        }
        add_custom_units(Family::Rodata, ImageSpace::Dgroup);
        add_asm_units(Family::Rodata, ImageSpace::Dgroup);
        if (!combined_relro.empty())
            add_unit(".data.rel.ro", Family::Data, ImageSpace::Dgroup, combined_relro);
        if (!combined_data.empty())
            add_unit(".data", Family::Data, ImageSpace::Dgroup, combined_data);
        add_custom_units(Family::Data, ImageSpace::Dgroup);
        add_asm_units(Family::Data, ImageSpace::Dgroup);
        if (!combined_bss.empty())
            add_unit(".bss", Family::Bss, ImageSpace::Dgroup, combined_bss);
        add_custom_units(Family::Bss, ImageSpace::Dgroup);
        add_asm_units(Family::Bss, ImageSpace::Dgroup);

        if (!errors.empty())
            return std::unexpected(join(errors));

        constexpr std::size_t unplaced = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> fragment_unit(image.fragments.size(), unplaced);
        std::vector<std::uint64_t> fragment_linear(image.fragments.size(), 0);
        for (std::size_t unit_index = 0; unit_index < units.size(); ++unit_index)
            for (auto const& member : units[unit_index].members)
                fragment_unit[member.index] = unit_index;

        FlatImageLayout result;
        result.options = options;
        std::uint64_t current = options.offset;
        std::uint64_t code_end = options.offset;
        std::vector<std::optional<std::size_t>> family_section(static_cast<std::size_t>(Family::Bss) + 1);

        std::size_t unit_index = 0;
        while (unit_index < units.size())
        {
            auto const family = units[unit_index].family;
            std::size_t end = unit_index;
            std::uint64_t total = 0;
            std::uint64_t out_alignment = 1;
            while (end < units.size() && units[end].family == family)
            {
                auto aligned = align_up(total, units[end].alignment);
                if (!aligned)
                {
                    errors.push_back(aligned.error());
                    return std::unexpected(join(errors));
                }
                total = *aligned + units[end].size;
                out_alignment = std::max(out_alignment, units[end].alignment);
                ++end;
            }
            if (total == 0)
            {
                LayoutSection empty;
                empty.name = std::string{family_name(family)};
                empty.space = family == Family::Start || family == Family::Text ? ImageSpace::Code : ImageSpace::Dgroup;
                empty.linear = current;
                empty.reason = "dropped empty section";
                for (std::size_t index = unit_index; index < end; ++index)
                {
                    for (auto const& member : units[index].members)
                        fragment_linear[member.index] = current + member.offset;
                    units[index].linear = current;
                }
                family_section[static_cast<std::size_t>(family)] = result.sections.size();
                result.sections.push_back(std::move(empty));
                unit_index = end;
                continue;
            }
            auto aligned = align_up(current, out_alignment);
            if (!aligned)
            {
                errors.push_back(aligned.error());
                return std::unexpected(join(errors));
            }
            std::uint64_t address = *aligned;
            std::string reason = address > current ? "aligned" : "contiguous";
            if (unreal && (family == Family::Rodata || family == Family::Data || family == Family::Bss) && current < 0x10000)
            {
                if (address + total > 0x10000 - options.stack_reserve)
                {
                    address = 0x10000;
                    reason = "unreal stack band";
                }
            }
            if (address > current)
                result.gaps.push_back(LayoutGap{current, address - current, address == 0x10000 ? "unreal stack band" : "alignment"});
            LayoutSection section;
            section.name = std::string{family_name(family)};
            section.space = family == Family::Start || family == Family::Text ? ImageSpace::Code : ImageSpace::Dgroup;
            section.alignment = out_alignment;
            section.linear = address;
            section.memory_size = total;
            section.reason = reason;
            std::uint64_t cursor = address;
            std::uint64_t section_stored_end = address;
            for (std::size_t index = unit_index; index < end; ++index)
            {
                auto unit_address = align_up(cursor, units[index].alignment);
                if (!unit_address)
                {
                    errors.push_back(unit_address.error());
                    return std::unexpected(join(errors));
                }
                if (*unit_address > cursor)
                    result.gaps.push_back(LayoutGap{cursor, *unit_address - cursor, "unit alignment"});
                units[index].linear = *unit_address;
                std::uint64_t member_cursor = *unit_address;
                for (auto const& member : units[index].members)
                {
                    auto member_address = align_up(member_cursor, member.alignment);
                    if (!member_address)
                    {
                        errors.push_back(member_address.error());
                        return std::unexpected(join(errors));
                    }
                    if (*member_address > member_cursor)
                        result.gaps.push_back(LayoutGap{member_cursor, *member_address - member_cursor, "fragment alignment"});
                    fragment_linear[member.index] = *member_address;
                    member_cursor = *member_address + member.size;
                    if (member.stored)
                        section_stored_end = std::max(section_stored_end, member_cursor);
                }
                cursor = *unit_address + units[index].size;
            }
            section.stored_size = section_stored_end - address;
            family_section[static_cast<std::size_t>(family)] = result.sections.size();
            result.sections.push_back(std::move(section));
            current = cursor;
            if (family == Family::Start || family == Family::Text)
                code_end = current;
            unit_index = end;
        }

        result.code_end = code_end;
        result.memory_end = current;
        if (auto bss = family_section[static_cast<std::size_t>(Family::Bss)])
        {
            result.bss_start = result.sections[*bss].linear;
            result.bss_end = result.bss_start + result.sections[*bss].memory_size;
        }
        else
            result.bss_start = result.bss_end = result.memory_end;
        result.stack_top = 0x10000;
        result.stack_bottom = 0x10000 - options.stack_reserve;
        for (auto space : {ImageSpace::Code, ImageSpace::Dgroup})
            result.segments.push_back(LayoutSegment{.space = space,
                                                    .paragraph = options.segment.value_or(0),
                                                    .linear_base = 0,
                                                    .memory_end = space == ImageSpace::Code ? result.code_end : result.memory_end,
                                                    .known = options.segment.has_value()});

        if (result.code_end > 0x10000)
            errors.push_back("i8086 code (.start and .text) does not fit below offset 0x10000 of its 64 KiB code segment");
        if (small && result.memory_end > 0x10000 - options.stack_reserve)
            errors.push_back(std::format("i8086 small model: the image plus the {}-byte stack reserve (-fstack-reserve) does not fit in one 64 KiB segment",
                                         options.stack_reserve));
        if (!errors.empty())
            return std::unexpected(join(errors));

        result.image_start = std::numeric_limits<std::uint64_t>::max();
        result.stored_end = 0;
        for (std::size_t index = 0; index < image.fragments.size(); ++index)
        {
            if (fragment_unit[index] == unplaced || !fragment_stored(index) || image.fragments[index].bytes.empty())
                continue;
            auto const begin = fragment_linear[index];
            auto const end = begin + image.fragments[index].bytes.size();
            result.image_start = std::min(result.image_start, begin);
            result.stored_end = std::max(result.stored_end, end);
        }
        if (result.image_start == std::numeric_limits<std::uint64_t>::max())
            result.image_start = result.stored_end = 0;
        result.bytes.assign(static_cast<std::size_t>(result.stored_end - result.image_start), std::uint8_t{0});
        for (std::size_t index = 0; index < image.fragments.size(); ++index)
        {
            if (fragment_unit[index] == unplaced || !fragment_stored(index) || image.fragments[index].bytes.empty())
                continue;
            auto const at = static_cast<std::size_t>(fragment_linear[index] - result.image_start);
            std::copy(image.fragments[index].bytes.begin(), image.fragments[index].bytes.end(), result.bytes.begin() + static_cast<std::ptrdiff_t>(at));
        }
        result.has_entry = false;
        for (std::size_t index = 0; index < image.fragments.size(); ++index)
        {
            if (fragment_unit[index] == unplaced || image.fragments[index].bytes.empty())
                continue;
            auto const family = families[index];
            if (!family || (*family != Family::Start && *family != Family::Text))
                continue;
            if (!result.has_entry || fragment_linear[index] < result.entry)
            {
                result.entry = fragment_linear[index];
                result.has_entry = true;
            }
        }

        for (std::size_t index = 0; index < image.fragments.size(); ++index)
        {
            if (fragment_unit[index] == unplaced)
                continue;
            LayoutFragment placed;
            placed.id = index;
            placed.role = image.fragments[index].role;
            placed.space = *families[index] == Family::Start || *families[index] == Family::Text ? ImageSpace::Code : ImageSpace::Dgroup;
            placed.linear = fragment_linear[index];
            placed.offset_in_segment = placed.linear;
            placed.section = *family_section[static_cast<std::size_t>(*families[index])];
            placed.stored_size = fragment_stored(index) ? image.fragments[index].bytes.size() : 0;
            placed.memory_size = fragment_size(index);
            if (placed.stored_size != 0)
                placed.file_offset = placed.linear - result.image_start;
            placed.reason = std::format("placed in {}", result.sections[placed.section].name);
            result.fragments.push_back(std::move(placed));
        }

        std::vector<std::optional<LayoutSymbol>> resolved(image.symbols.size());
        auto make_symbol = [](ImageSymbol const& source, ImageSpace space, std::uint64_t value) {
            LayoutSymbol symbol;
            symbol.id = source.id;
            symbol.name = source.name;
            symbol.space = space;
            symbol.value = value;
            symbol.offset_in_segment = value;
            symbol.defined = true;
            return symbol;
        };
        for (std::size_t index = 0; index < image.symbols.size(); ++index)
        {
            auto const& source = image.symbols[index];
            if (source.fragment && *source.fragment < image.fragments.size() && fragment_unit[*source.fragment] != unplaced)
            {
                auto const family = families[*source.fragment];
                auto const value = fragment_linear[*source.fragment] + source.offset;
                resolved[index] =
                    make_symbol(source, family && (*family == Family::Start || *family == Family::Text) ? ImageSpace::Code : ImageSpace::Dgroup, value);
            }
            else if (source.name == "__dcc_code_end")
                resolved[index] = make_symbol(source, ImageSpace::Dgroup, result.code_end);
            else if (source.name == "__bss_start")
                resolved[index] = make_symbol(source, ImageSpace::Dgroup, result.bss_start);
            else if (source.name == "__bss_end")
                resolved[index] = make_symbol(source, ImageSpace::Dgroup, result.bss_end);
            else if (source.name == "__image_end")
                resolved[index] = make_symbol(source, ImageSpace::Dgroup, result.memory_end);
            else if (source.name.starts_with("__dcc_i8086_model_"))
            {
                if (source.name == marker)
                {
                    auto symbol = make_symbol(source, ImageSpace::Absolute, 0);
                    symbol.absolute = true;
                    resolved[index] = symbol;
                }
            }
            else if (source.name == "__dcc_dgroup_segment" || source.name == "__dcc_code_segment")
            {
                if (options.segment)
                {
                    auto symbol = make_symbol(source, ImageSpace::Absolute, *options.segment);
                    symbol.absolute = true;
                    resolved[index] = symbol;
                }
            }
        }
        for (auto const& symbol : resolved)
            if (symbol)
                result.symbols.push_back(*symbol);
        {
            std::array<std::pair<std::string_view, std::uint64_t>, 4> const generated{
                {{"__dcc_code_end", result.code_end}, {"__bss_start", result.bss_start}, {"__bss_end", result.bss_end}, {"__image_end", result.memory_end}}};
            for (auto const& [name, value] : generated)
            {
                if (result.find_symbol(name))
                    continue;
                LayoutSymbol symbol;
                symbol.id = image.symbols.size() + result.symbols.size();
                symbol.name = std::string{name};
                symbol.space = ImageSpace::Dgroup;
                symbol.value = value;
                symbol.offset_in_segment = value;
                symbol.defined = true;
                result.symbols.push_back(std::move(symbol));
            }
        }

        for (std::size_t index = 0; index < image.fragments.size(); ++index)
        {
            if (fragment_unit[index] == unplaced || !fragment_stored(index) || image.fragments[index].bytes.empty())
                continue;
            auto const& fragment = image.fragments[index];
            std::vector<std::pair<std::uint64_t, std::uint64_t>> patches;
            for (auto const& fixup : fragment.fixups)
            {
                if (fixup.width == 0 || fixup.width > 8)
                {
                    errors.push_back(std::format("fixup in {} at offset {} has invalid width {}", owner_text(fragment), fixup.offset, fixup.width));
                    continue;
                }
                if (fixup.offset > fragment.bytes.size() || fixup.width > fragment.bytes.size() - fixup.offset)
                {
                    errors.push_back(std::format("fixup in {} at offset {} with width {} reaches outside the fragment bytes", owner_text(fragment),
                                                 fixup.offset, fixup.width));
                    continue;
                }
                if (fixup.target >= image.symbols.size())
                {
                    errors.push_back(std::format("fixup in {} at offset {} has an invalid symbol id", owner_text(fragment), fixup.offset));
                    continue;
                }
                auto const& target = image.symbols[fixup.target];
                auto const patch_linear = fragment_linear[index] + fixup.offset;
                std::int64_t value = 0;
                switch (fixup.expression)
                {
                    case ImageExpression::PcRelative: {
                        if (!resolved[fixup.target] || !resolved[fixup.target]->defined)
                        {
                            errors.push_back(
                                std::format("undefined symbol `{}` referenced by {} at offset {}", target.name, owner_text(fragment), fixup.offset));
                            continue;
                        }
                        auto const symbol_value = resolved[fixup.target]->value;
                        if (symbol_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                            patch_linear > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                        {
                            errors.push_back(
                                std::format("PC-relative fixup in {} at offset {} overflows the address space", owner_text(fragment), fixup.offset));
                            continue;
                        }
                        value = static_cast<std::int64_t>(symbol_value) + fixup.addend - static_cast<std::int64_t>(patch_linear);
                        if (value < signed_min(fixup.width) || value > signed_max(fixup.width))
                        {
                            errors.push_back(std::format("PC-relative fixup in {} at offset {} resolves to {}, which does not fit in a signed {}-bit field",
                                                         owner_text(fragment), fixup.offset, value, fixup.width * 8));
                            continue;
                        }
                        break;
                    }
                    case ImageExpression::SymbolOffset:
                    case ImageExpression::CodeOffset: {
                        if (!resolved[fixup.target] || !resolved[fixup.target]->defined)
                        {
                            errors.push_back(
                                std::format("undefined symbol `{}` referenced by {} at offset {}", target.name, owner_text(fragment), fixup.offset));
                            continue;
                        }
                        if (fixup.units != ImageUnits::Bytes)
                        {
                            errors.push_back(
                                std::format("fixup in {} at offset {} uses paragraph units on an offset expression", owner_text(fragment), fixup.offset));
                            continue;
                        }
                        auto const symbol_value = resolved[fixup.target]->value;
                        if (symbol_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                        {
                            errors.push_back(std::format("fixup in {} at offset {} overflows the address space", owner_text(fragment), fixup.offset));
                            continue;
                        }
                        value = static_cast<std::int64_t>(symbol_value) + fixup.addend;
                        if (value < 0 || static_cast<std::uint64_t>(value) > unsigned_max(fixup.width))
                        {
                            errors.push_back(std::format("fixup in {} at offset {} resolves to 0x{:X}, which does not fit in an unsigned {}-bit field",
                                                         owner_text(fragment), fixup.offset, value, fixup.width * 8));
                            continue;
                        }
                        break;
                    }
                    case ImageExpression::SegmentBase: {
                        if (!options.segment)
                        {
                            errors.push_back(std::format("the image segment is unknown (-fbase=?:{:04X}), but a static far pointer needs it; pass "
                                                         "-fbase=SEG:OFF with a known segment\n>>> referenced by {}",
                                                         options.offset, owner_text(fragment)));
                            continue;
                        }
                        value = static_cast<std::int64_t>(*options.segment) + fixup.addend;
                        if (value < 0 || static_cast<std::uint64_t>(value) > unsigned_max(fixup.width))
                        {
                            errors.push_back(std::format("segment fixup in {} at offset {} resolves to 0x{:X}, which does not fit in an unsigned {}-bit field",
                                                         owner_text(fragment), fixup.offset, value, fixup.width * 8));
                            continue;
                        }
                        break;
                    }
                    case ImageExpression::LayoutSize:
                        errors.push_back(std::format("layout size expression in {} at offset {} is not supported by the flat image policy",
                                                     owner_text(fragment), fixup.offset));
                        continue;
                }
                bool overlaps = false;
                for (auto const& [start, end] : patches)
                    if (fixup.offset < end && start < fixup.offset + fixup.width)
                        overlaps = true;
                if (overlaps)
                {
                    errors.push_back(std::format("overlapping fixups in {} at offset {}", owner_text(fragment), fixup.offset));
                    continue;
                }
                patches.emplace_back(fixup.offset, fixup.offset + fixup.width);
                LayoutFixup placed;
                placed.owner = index;
                placed.offset = fixup.offset;
                placed.patch_linear = patch_linear;
                placed.file_offset = patch_linear - result.image_start;
                placed.width = fixup.width;
                placed.value = value;
                placed.expression = fixup.expression;
                placed.space = fixup.space;
                auto const at = static_cast<std::size_t>(patch_linear - result.image_start);
                auto bits = static_cast<std::uint64_t>(value);
                for (std::uint64_t byte = 0; byte < fixup.width; ++byte)
                    result.bytes[at + static_cast<std::size_t>(byte)] = static_cast<std::uint8_t>((bits >> (byte * 8)) & 0xFF);
                result.fixups.push_back(std::move(placed));
            }
        }

        if (!errors.empty())
            return std::unexpected(join(errors));
        return result;
    }

} // namespace dcc::backend::i8086
