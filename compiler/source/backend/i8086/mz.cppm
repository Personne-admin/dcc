export module dcc.backend.i8086.mz;

import std;
import dcc.target;
import dcc.backend.object.image;
import dcc.backend.i8086.place;
import dcc.backend.i8086.link;

export namespace dcc::backend::i8086
{
    struct MzLayoutOptions
    {
        std::uint32_t stack_reserve{4096};
        std::string entry_symbol{"_start"};
        target::CodeModel model{target::CodeModel::Small};
    };

    struct MzRelocation
    {
        std::uint64_t patch_linear{};
        std::uint64_t file_offset{};
        std::uint16_t offset_field{};
        std::uint16_t segment_field{};
        std::uint64_t value{};
        object::ImageSpace space{object::ImageSpace::Unknown};
        std::string symbol;
    };

    struct MzImageLayout
    {
        MzLayoutOptions options;
        std::vector<LayoutSection> sections;
        std::vector<LayoutFragment> fragments;
        std::vector<LayoutSymbol> symbols;
        std::vector<MzRelocation> relocations;
        std::vector<LayoutGap> gaps;
        std::uint64_t code_end{};
        std::uint64_t dgroup_paragraph{};
        std::uint64_t data_end{};
        std::uint64_t bss_start{};
        std::uint64_t bss_end{};
        std::uint64_t stack_bottom{};
        std::uint64_t stack_top{};
        std::uint64_t load_bytes{};
        std::uint64_t memory_extent{};
        std::uint16_t entry_offset{};
        std::uint16_t initial_sp{};
        std::uint16_t min_alloc{};
        std::uint32_t header_size{};
        std::uint32_t file_size{};
        bool has_entry{};
        std::vector<std::uint8_t> header;
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> file;

        [[nodiscard]] LayoutSymbol const* find_symbol(std::string_view name) const
        {
            for (auto const& symbol : symbols)
                if (symbol.name == name)
                    return &symbol;
            return nullptr;
        }
    };

    [[nodiscard]] std::expected<MzImageLayout, std::string> layout_mz_image(object::ImageDescription const& image, MzLayoutOptions const& options);
}

namespace dcc::backend::i8086
{
    namespace
    {
        using namespace dcc::backend::object;

        enum class MzFamily : std::uint8_t
        {
            None,
            Start,
            Text,
            Rodata,
            Data,
            Bss,
        };

        [[nodiscard]] MzFamily mz_family_of(std::string_view section)
        {
            if (section == ".start" || section.starts_with(".start."))
                return MzFamily::Start;
            if (section == ".text" || section.starts_with(".text."))
                return MzFamily::Text;
            if (section == ".rodata" || section.starts_with(".rodata."))
                return MzFamily::Rodata;
            if (section == ".data.rel.ro" || section.starts_with(".data.rel.ro.") || section == ".data" || section.starts_with(".data."))
                return MzFamily::Data;
            if (section == ".bss" || section.starts_with(".bss."))
                return MzFamily::Bss;
            return MzFamily::None;
        }

        [[nodiscard]] std::string_view mz_family_name(MzFamily family)
        {
            switch (family)
            {
                case MzFamily::Start:
                    return ".start";
                case MzFamily::Text:
                    return ".text";
                case MzFamily::Rodata:
                    return ".rodata";
                case MzFamily::Data:
                    return ".data";
                case MzFamily::Bss:
                    return ".bss";
                case MzFamily::None:
                    break;
            }
            return "";
        }

        [[nodiscard]] bool mz_discarded_section(std::string_view section)
        {
            return section == ".note" || section.starts_with(".note.") || section == ".comment" || section == ".eh_frame" || section.starts_with(".eh_frame.");
        }

        [[nodiscard]] bool mz_power_of_two(std::uint64_t value) noexcept
        {
            return value != 0 && (value & (value - 1)) == 0;
        }

        [[nodiscard]] std::expected<std::uint64_t, std::string> mz_align_up(std::uint64_t value, std::uint64_t alignment)
        {
            if (!mz_power_of_two(alignment))
                return std::unexpected(std::format("invalid alignment {}", alignment));
            auto const mask = alignment - 1;
            if (value > std::numeric_limits<std::uint64_t>::max() - mask)
                return std::unexpected(std::format("aligning 0x{:X} to {} overflows the image address space", value, alignment));
            return (value + mask) & ~mask;
        }

        [[nodiscard]] std::string mz_owner_text(ImageFragment const& fragment)
        {
            if (fragment.asm_section)
                return std::format("module asm section `{}`", fragment.section);
            if (fragment.associated_function)
                return std::format("jump table `{}`", fragment.name);
            return std::format("fragment `{}`", fragment.name);
        }

        [[nodiscard]] std::string mz_join(std::vector<std::string> const& messages)
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

        [[nodiscard]] std::uint64_t mz_unsigned_max(std::uint64_t width)
        {
            if (width >= 8)
                return std::numeric_limits<std::uint64_t>::max();
            return (std::uint64_t{1} << (width * 8)) - 1;
        }

        [[nodiscard]] std::int64_t mz_signed_min(std::uint64_t width)
        {
            if (width >= 8)
                return std::numeric_limits<std::int64_t>::min();
            return -static_cast<std::int64_t>(std::uint64_t{1} << (width * 8 - 1));
        }

        [[nodiscard]] std::int64_t mz_signed_max(std::uint64_t width)
        {
            if (width >= 8)
                return std::numeric_limits<std::int64_t>::max();
            return static_cast<std::int64_t>((std::uint64_t{1} << (width * 8 - 1)) - 1);
        }

        struct MzMember
        {
            std::size_t index{};
            std::uint64_t alignment{1};
            std::uint64_t size{};
            std::uint64_t offset{};
            bool stored{};
        };

        struct MzUnit
        {
            std::string name;
            MzFamily family{MzFamily::None};
            ImageSpace space{ImageSpace::Unknown};
            std::uint64_t alignment{1};
            std::vector<MzMember> members;
            std::uint64_t size{};
            bool stored{};
            std::uint64_t domain{};
        };

        void mz_write_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
        {
            bytes[offset] = static_cast<std::uint8_t>(value & 0xFF);
            bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        }

    } // namespace

    std::expected<MzImageLayout, std::string> layout_mz_image(ImageDescription const& image, MzLayoutOptions const& options)
    {
        std::vector<std::string> errors;
        if (options.model != target::CodeModel::Default && options.model != target::CodeModel::Small)
            errors.push_back(std::format("i8086 MZ layout supports only the small model, not -mcmodel={}", static_cast<int>(options.model)));
        if (options.stack_reserve == 0)
            errors.push_back("i8086 MZ requires a nonzero stack reservation");
        else if (options.stack_reserve > 0xFFFE)
            errors.push_back(std::format("invalid -fstack-reserve value {}", options.stack_reserve));
        auto const stack_size = (static_cast<std::uint64_t>(options.stack_reserve) + 1) & ~std::uint64_t{1};

        std::vector<std::optional<MzFamily>> families(image.fragments.size());
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
            auto const family = mz_family_of(fragment.section);
            families[index] = family;
            if (!fragment.alloc)
                continue;
            if (family == MzFamily::None)
            {
                if (mz_discarded_section(fragment.section))
                    continue;
                errors.push_back(std::format("allocated section {} ({}) is not placed by the i8086 MZ image policy, which accepts only "
                                             ".start*, .text*, .rodata*, .data.rel.ro*, .data*, .bss* and discards .note*, .comment "
                                             "and .eh_frame* input sections",
                                             fragment.section, mz_owner_text(fragment)));
                continue;
            }
            if (!mz_power_of_two(fragment.alignment))
            {
                errors.push_back(std::format("fragment `{}` has invalid alignment {}", fragment.name, fragment.alignment));
                continue;
            }
            if (fragment.alignment > 16)
            {
                errors.push_back(std::format("fragment `{}` requests alignment {}, which an arbitrary paragraph load base cannot guarantee",
                                             fragment.name, fragment.alignment));
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
                    errors.push_back(std::format("i8086 code model mismatch: an object needs {}, but this link is for -mcmodel={} and provides {}",
                                                 target.name, options.model == target::CodeModel::Small ? "small" : "default", marker));
            }

        if (!errors.empty())
            return std::unexpected(mz_join(errors));

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

        std::vector<MzUnit> units;
        auto add_unit = [&](std::string name, MzFamily family, ImageSpace space, std::vector<std::size_t> const& members) {
            MzUnit unit;
            unit.name = std::move(name);
            unit.family = family;
            unit.space = space;
            std::uint64_t cursor = 0;
            for (auto index : members)
            {
                MzMember member;
                member.index = index;
                member.alignment = image.fragments[index].alignment;
                member.size = fragment_size(index);
                member.stored = fragment_stored(index);
                auto aligned = mz_align_up(cursor, member.alignment);
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
        auto add_custom_units = [&](MzFamily family, ImageSpace space) {
            for (auto const& [name, members] : custom)
                if (mz_family_of(name) == family)
                    add_unit(name, family, space, members);
        };
        auto add_asm_units = [&](MzFamily family, ImageSpace space) {
            for (auto index : asm_order)
                if (families[index] == family)
                    add_unit(std::format("{}#{}", image.fragments[index].section, index), family, space, {index});
        };

        add_custom_units(MzFamily::Start, ImageSpace::Code);
        add_asm_units(MzFamily::Start, ImageSpace::Code);
        if (!combined_text.empty())
            add_unit(".text", MzFamily::Text, ImageSpace::Code, combined_text);
        add_custom_units(MzFamily::Text, ImageSpace::Code);
        add_asm_units(MzFamily::Text, ImageSpace::Code);
        {
            std::vector<std::size_t> rodata = combined_rodata_globals;
            rodata.insert(rodata.end(), combined_rodata_jumps.begin(), combined_rodata_jumps.end());
            if (!rodata.empty())
                add_unit(".rodata", MzFamily::Rodata, ImageSpace::Dgroup, rodata);
        }
        add_custom_units(MzFamily::Rodata, ImageSpace::Dgroup);
        add_asm_units(MzFamily::Rodata, ImageSpace::Dgroup);
        if (!combined_relro.empty())
            add_unit(".data.rel.ro", MzFamily::Data, ImageSpace::Dgroup, combined_relro);
        if (!combined_data.empty())
            add_unit(".data", MzFamily::Data, ImageSpace::Dgroup, combined_data);
        add_custom_units(MzFamily::Data, ImageSpace::Dgroup);
        add_asm_units(MzFamily::Data, ImageSpace::Dgroup);
        if (!combined_bss.empty())
            add_unit(".bss", MzFamily::Bss, ImageSpace::Dgroup, combined_bss);
        add_custom_units(MzFamily::Bss, ImageSpace::Dgroup);
        add_asm_units(MzFamily::Bss, ImageSpace::Dgroup);

        if (!errors.empty())
            return std::unexpected(mz_join(errors));

        constexpr std::size_t unplaced = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> fragment_unit(image.fragments.size(), unplaced);
        std::vector<std::uint64_t> fragment_domain(image.fragments.size(), 0);
        std::vector<std::uint64_t> fragment_linear(image.fragments.size(), 0);
        std::vector<std::optional<std::uint64_t>> fragment_file(image.fragments.size());
        for (std::size_t unit_index = 0; unit_index < units.size(); ++unit_index)
            for (auto const& member : units[unit_index].members)
                fragment_unit[member.index] = unit_index;

        MzImageLayout result;
        result.options = options;
        std::vector<LayoutSection> code_sections;
        std::vector<LayoutSection> data_sections;

        auto place_range = [&](std::size_t begin, std::size_t end, std::uint64_t start, std::uint64_t linear_base, bool code,
                               std::vector<LayoutSection>& sections) -> std::uint64_t {
            std::uint64_t current = start;
            std::size_t unit_index = begin;
            while (unit_index < end)
            {
                auto const family = units[unit_index].family;
                std::size_t last = unit_index;
                std::uint64_t total = 0;
                std::uint64_t out_alignment = 1;
                while (last < end && units[last].family == family)
                {
                    auto aligned = mz_align_up(total, units[last].alignment);
                    if (!aligned)
                    {
                        errors.push_back(aligned.error());
                        return current;
                    }
                    total = *aligned + units[last].size;
                    out_alignment = std::max(out_alignment, units[last].alignment);
                    ++last;
                }
                auto address = mz_align_up(current, out_alignment);
                if (!address)
                {
                    errors.push_back(address.error());
                    return current;
                }
                if (*address > current)
                    result.gaps.push_back(LayoutGap{current, *address - current, "alignment"});
                LayoutSection section;
                section.name = std::string{mz_family_name(family)};
                section.space = code ? ImageSpace::Code : ImageSpace::Dgroup;
                section.alignment = out_alignment;
                section.linear = linear_base + *address;
                section.memory_size = total;
                section.reason = "mz";
                std::uint64_t cursor = *address;
                std::uint64_t stored_end = *address;
                for (std::size_t index = unit_index; index < last; ++index)
                {
                    auto unit_address = mz_align_up(cursor, units[index].alignment);
                    if (!unit_address)
                    {
                        errors.push_back(unit_address.error());
                        return current;
                    }
                    if (*unit_address > cursor)
                        result.gaps.push_back(LayoutGap{cursor, *unit_address - cursor, "unit alignment"});
                    units[index].domain = *unit_address;
                    std::uint64_t member_cursor = *unit_address;
                    for (auto const& member : units[index].members)
                    {
                        auto member_address = mz_align_up(member_cursor, member.alignment);
                        if (!member_address)
                        {
                            errors.push_back(member_address.error());
                            return current;
                        }
                        if (*member_address > member_cursor)
                            result.gaps.push_back(LayoutGap{member_cursor, *member_address - member_cursor, "fragment alignment"});
                        fragment_domain[member.index] = *member_address;
                        fragment_linear[member.index] = linear_base + *member_address;
                        member_cursor = *member_address + member.size;
                        if (member.stored)
                            stored_end = std::max(stored_end, member_cursor);
                    }
                    cursor = *unit_address + units[index].size;
                }
                section.stored_size = stored_end - *address;
                sections.push_back(std::move(section));
                current = cursor;
                unit_index = last;
            }
            return current;
        };

        std::size_t first_data = 0;
        while (first_data < units.size() && (units[first_data].family == MzFamily::Start || units[first_data].family == MzFamily::Text))
            ++first_data;
        std::size_t first_bss = first_data;
        while (first_bss < units.size() && units[first_bss].family != MzFamily::Bss)
            ++first_bss;

        result.code_end = place_range(0, first_data, 0, 0, true, code_sections);
        if (!errors.empty())
            return std::unexpected(mz_join(errors));
        if (result.code_end > 0x10000)
            return std::unexpected("i8086 MZ code (.start and .text) does not fit below offset 0x10000 of its 64 KiB code segment");
        auto dgroup = mz_align_up(result.code_end, 16);
        if (!dgroup)
            return std::unexpected(dgroup.error());
        result.dgroup_paragraph = *dgroup / 16;

        result.data_end = place_range(first_data, first_bss, 0, result.dgroup_paragraph * 16, false, data_sections);
        if (!errors.empty())
            return std::unexpected(mz_join(errors));
        auto bss_start = mz_align_up(result.data_end, 16);
        if (!bss_start)
            return std::unexpected(bss_start.error());
        std::uint64_t bss_align = 1;
        for (std::size_t index = first_bss; index < units.size(); ++index)
            bss_align = std::max(bss_align, units[index].alignment);
        bss_start = mz_align_up(*bss_start, bss_align);
        if (!bss_start)
            return std::unexpected(bss_start.error());
        result.bss_start = *bss_start;
        result.bss_end = place_range(first_bss, units.size(), result.bss_start, result.dgroup_paragraph * 16, false, data_sections);
        if (!errors.empty())
            return std::unexpected(mz_join(errors));
        if (first_bss == units.size())
            result.bss_end = result.bss_start;
        result.stack_bottom = (result.bss_end + 1) & ~std::uint64_t{1};
        result.stack_top = result.stack_bottom + stack_size;
        if (result.stack_top > 0x10000)
            return std::unexpected("i8086 MZ DGROUP (rodata, data, BSS and stack) does not fit below offset 0x10000 of its 64 KiB data segment");
        result.load_bytes = result.dgroup_paragraph * 16 + result.bss_start;
        result.memory_extent = result.dgroup_paragraph * 16 + result.stack_top;
        auto const image_paragraphs = (result.load_bytes + 15) / 16;
        auto const memory_paragraphs = (result.memory_extent + 15) / 16;
        if (memory_paragraphs < image_paragraphs)
            return std::unexpected("i8086 MZ memory extent is smaller than the stored load module");
        auto const extra = memory_paragraphs - image_paragraphs;
        if (extra > 0xFFFF)
            return std::unexpected("i8086 MZ minimum allocation does not fit in a 16-bit header field");
        result.min_alloc = static_cast<std::uint16_t>(extra);

        result.sections = std::move(code_sections);
        result.sections.insert(result.sections.end(), data_sections.begin(), data_sections.end());

        for (auto parse = result.sections.begin(); parse != result.sections.end(); ++parse)
            for (auto later = std::next(parse); later != result.sections.end(); ++later)
                if (later->linear < parse->linear + parse->memory_size && parse->linear < later->linear + later->memory_size &&
                    parse->name == later->name)
                {
                    errors.push_back(std::format("internal layout overlap between {} and {}", parse->name, later->name));
                }
        if (!errors.empty())
            return std::unexpected(mz_join(errors));

        result.payload.assign(static_cast<std::size_t>(result.load_bytes), std::uint8_t{0});
        for (std::size_t index = 0; index < image.fragments.size(); ++index)
        {
            if (fragment_unit[index] == unplaced || !fragment_stored(index) || image.fragments[index].bytes.empty())
                continue;
            std::copy(image.fragments[index].bytes.begin(), image.fragments[index].bytes.end(),
                      result.payload.begin() + static_cast<std::ptrdiff_t>(fragment_linear[index]));
            fragment_file[index] = fragment_linear[index];
        }

        std::vector<std::optional<LayoutSymbol>> resolved(image.symbols.size());
        auto make_symbol = [](ImageSymbol const& source, ImageSpace space, std::uint64_t value, bool absolute = false) {
            LayoutSymbol symbol;
            symbol.id = source.id;
            symbol.name = source.name;
            symbol.space = space;
            symbol.value = value;
            symbol.offset_in_segment = value;
            symbol.absolute = absolute;
            symbol.defined = true;
            return symbol;
        };
        for (std::size_t index = 0; index < image.symbols.size(); ++index)
        {
            auto const& source = image.symbols[index];
            if (source.fragment && *source.fragment < image.fragments.size() && fragment_unit[*source.fragment] != unplaced)
            {
                auto const family = families[*source.fragment];
                auto const code = family && (*family == MzFamily::Start || *family == MzFamily::Text);
                resolved[index] = make_symbol(source, code ? ImageSpace::Code : ImageSpace::Dgroup,
                                              fragment_domain[*source.fragment] + source.offset);
            }
            else if (source.name == "__dcc_code_segment")
                resolved[index] = make_symbol(source, ImageSpace::Code, 0);
            else if (source.name == "__dcc_dgroup_segment")
                resolved[index] = make_symbol(source, ImageSpace::Dgroup, result.dgroup_paragraph);
            else if (source.name == "__dcc_code_end")
                resolved[index] = make_symbol(source, ImageSpace::Code, result.code_end);
            else if (source.name == "__bss_start")
                resolved[index] = make_symbol(source, ImageSpace::Dgroup, result.bss_start);
            else if (source.name == "__bss_end")
                resolved[index] = make_symbol(source, ImageSpace::Dgroup, result.bss_end);
            else if (source.name == "__image_end")
                resolved[index] = make_symbol(source, ImageSpace::Dgroup, result.bss_end);
            else if (source.name == "__dcc_stack_bottom")
                resolved[index] = make_symbol(source, ImageSpace::Dgroup, result.stack_bottom);
            else if (source.name == "__dcc_stack_top")
                resolved[index] = make_symbol(source, ImageSpace::Dgroup, result.stack_top);
            else if (source.name == "__dcc_initial_sp")
                resolved[index] = make_symbol(source, ImageSpace::Absolute, result.stack_top & 0xFFFF, true);
            else if (source.name == "__dcc_load_bytes")
                resolved[index] = make_symbol(source, ImageSpace::Absolute, result.load_bytes, true);
            else if (source.name == "__dcc_memory_paragraphs")
                resolved[index] = make_symbol(source, ImageSpace::Absolute, memory_paragraphs, true);
            else if (source.name.starts_with("__dcc_i8086_model_"))
            {
                if (source.name == marker)
                    resolved[index] = make_symbol(source, ImageSpace::Absolute, 0, true);
            }
        }
        for (auto& symbol : resolved)
            if (symbol)
                result.symbols.push_back(*symbol);
        {
            std::array<std::pair<std::string_view, std::pair<std::uint64_t, ImageSpace>>, 10> const generated{{
                {"__dcc_code_segment", {0, ImageSpace::Code}},
                {"__dcc_dgroup_segment", {result.dgroup_paragraph, ImageSpace::Dgroup}},
                {"__dcc_code_end", {result.code_end, ImageSpace::Code}},
                {"__bss_start", {result.bss_start, ImageSpace::Dgroup}},
                {"__bss_end", {result.bss_end, ImageSpace::Dgroup}},
                {"__image_end", {result.bss_end, ImageSpace::Dgroup}},
                {"__dcc_stack_bottom", {result.stack_bottom, ImageSpace::Dgroup}},
                {"__dcc_stack_top", {result.stack_top, ImageSpace::Dgroup}},
                {"__dcc_initial_sp", {result.stack_top & 0xFFFF, ImageSpace::Absolute}},
                {"__dcc_load_bytes", {result.load_bytes, ImageSpace::Absolute}},
            }};
            for (auto const& [name, value] : generated)
            {
                if (result.find_symbol(name))
                    continue;
                LayoutSymbol symbol;
                symbol.id = image.symbols.size() + result.symbols.size();
                symbol.name = std::string{name};
                symbol.space = value.second;
                symbol.value = value.first;
                symbol.offset_in_segment = value.first;
                symbol.absolute = value.second == ImageSpace::Absolute;
                symbol.defined = true;
                result.symbols.push_back(std::move(symbol));
            }
            if (!result.find_symbol("__dcc_memory_paragraphs"))
            {
                LayoutSymbol symbol;
                symbol.id = image.symbols.size() + result.symbols.size();
                symbol.name = "__dcc_memory_paragraphs";
                symbol.space = ImageSpace::Absolute;
                symbol.value = memory_paragraphs;
                symbol.offset_in_segment = symbol.value;
                symbol.absolute = true;
                symbol.defined = true;
                result.symbols.push_back(std::move(symbol));
            }
        }

        std::vector<std::pair<std::uint64_t, std::uint64_t>> global_patches;
        for (std::size_t index = 0; index < image.fragments.size(); ++index)
        {
            auto const& fragment = image.fragments[index];
            if (!fragment.alloc)
                continue;
            if (!fragment.fixups.empty() && (fragment_unit[index] == unplaced || !fragment_stored(index)))
            {
                errors.push_back(std::format("fixup in {} is outside stored image bytes", mz_owner_text(fragment)));
                continue;
            }
            if (fragment_unit[index] == unplaced)
                continue;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> patches;
            for (auto const& fixup : fragment.fixups)
            {
                if (fixup.width == 0 || fixup.width > 8)
                {
                    errors.push_back(std::format("fixup in {} at offset {} has invalid width {}", mz_owner_text(fragment), fixup.offset, fixup.width));
                    continue;
                }
                if (fixup.offset > fragment.bytes.size() || fixup.width > fragment.bytes.size() - fixup.offset)
                {
                    errors.push_back(std::format("fixup in {} at offset {} with width {} reaches outside the fragment bytes", mz_owner_text(fragment),
                                                 fixup.offset, fixup.width));
                    continue;
                }
                if (fixup.target >= image.symbols.size())
                {
                    errors.push_back(std::format("fixup in {} at offset {} has an invalid symbol id", mz_owner_text(fragment), fixup.offset));
                    continue;
                }
                auto const& target = image.symbols[fixup.target];
                auto const patch_linear = fragment_linear[index] + fixup.offset;
                std::int64_t value = 0;
                bool relocation = false;
                switch (fixup.expression)
                {
                    case ImageExpression::PcRelative: {
                        if (!resolved[fixup.target] || !resolved[fixup.target]->defined)
                        {
                            errors.push_back(
                                std::format("undefined symbol `{}` referenced by {} at offset {}", target.name, mz_owner_text(fragment), fixup.offset));
                            continue;
                        }
                        auto const symbol_value = resolved[fixup.target]->value;
                        if (symbol_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                            patch_linear > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                        {
                            errors.push_back(
                                std::format("PC-relative fixup in {} at offset {} overflows the address space", mz_owner_text(fragment), fixup.offset));
                            continue;
                        }
                        value = static_cast<std::int64_t>(symbol_value) + fixup.addend - static_cast<std::int64_t>(patch_linear);
                        if (value < mz_signed_min(fixup.width) || value > mz_signed_max(fixup.width))
                        {
                            errors.push_back(std::format("PC-relative fixup in {} at offset {} resolves to {}, which does not fit in a signed {}-bit field",
                                                         mz_owner_text(fragment), fixup.offset, value, fixup.width * 8));
                            continue;
                        }
                        break;
                    }
                    case ImageExpression::SymbolOffset:
                    case ImageExpression::CodeOffset: {
                        if (!resolved[fixup.target] || !resolved[fixup.target]->defined)
                        {
                            errors.push_back(
                                std::format("undefined symbol `{}` referenced by {} at offset {}", target.name, mz_owner_text(fragment), fixup.offset));
                            continue;
                        }
                        if (fixup.units != ImageUnits::Bytes)
                        {
                            errors.push_back(
                                std::format("fixup in {} at offset {} uses paragraph units on an offset expression", mz_owner_text(fragment), fixup.offset));
                            continue;
                        }
                        auto const symbol_value = resolved[fixup.target]->value;
                        if (symbol_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                        {
                            errors.push_back(std::format("fixup in {} at offset {} overflows the address space", mz_owner_text(fragment), fixup.offset));
                            continue;
                        }
                        value = static_cast<std::int64_t>(symbol_value) + fixup.addend;
                        if (value < 0 || static_cast<std::uint64_t>(value) > mz_unsigned_max(fixup.width))
                        {
                            errors.push_back(std::format("fixup in {} at offset {} resolves to 0x{:X}, which does not fit in an unsigned {}-bit field",
                                                         mz_owner_text(fragment), fixup.offset, value, fixup.width * 8));
                            continue;
                        }
                        break;
                    }
                    case ImageExpression::SegmentBase: {
                        if (fixup.width != 2)
                        {
                            errors.push_back(std::format("segment fixup in {} at offset {} has width {}; MZ relocations require a 16-bit segment word",
                                                         mz_owner_text(fragment), fixup.offset, fixup.width));
                            continue;
                        }
                        if (fixup.units != ImageUnits::Paragraphs)
                        {
                            errors.push_back(std::format("segment fixup in {} at offset {} does not use paragraph units", mz_owner_text(fragment), fixup.offset));
                            continue;
                        }
                        if (!resolved[fixup.target] || !resolved[fixup.target]->defined)
                        {
                            errors.push_back(
                                std::format("undefined symbol `{}` referenced by {} at offset {}", target.name, mz_owner_text(fragment), fixup.offset));
                            continue;
                        }
                        auto const space = resolved[fixup.target]->space;
                        if (space != ImageSpace::Code && space != ImageSpace::Dgroup)
                        {
                            errors.push_back(std::format("segment fixup in {} at offset {} refers to `{}`, which has no load-relative segment",
                                                         mz_owner_text(fragment), fixup.offset, target.name));
                            continue;
                        }
                        auto const base = space == ImageSpace::Code ? std::uint64_t{0} : result.dgroup_paragraph;
                        auto const segment = static_cast<std::int64_t>(base) + fixup.addend;
                        if (segment < 0 || static_cast<std::uint64_t>(segment) > 0xFFFF)
                        {
                            errors.push_back(std::format("segment fixup in {} at offset {} resolves to 0x{:X}, which does not fit in a 16-bit field",
                                                         mz_owner_text(fragment), fixup.offset, segment));
                            continue;
                        }
                        value = segment;
                        relocation = true;
                        break;
                    }
                    case ImageExpression::LayoutSize:
                        errors.push_back(std::format("layout size expression in {} at offset {} is not supported by the i8086 MZ image policy",
                                                     mz_owner_text(fragment), fixup.offset));
                        continue;
                }
                bool overlaps = false;
                for (auto const& [start, end] : patches)
                    if (fixup.offset < end && start < fixup.offset + fixup.width)
                        overlaps = true;
                if (overlaps)
                {
                    errors.push_back(std::format("overlapping fixups in {} at offset {}", mz_owner_text(fragment), fixup.offset));
                    continue;
                }
                patches.emplace_back(fixup.offset, fixup.offset + fixup.width);
                global_patches.emplace_back(patch_linear, patch_linear + fixup.width);
                auto bits = static_cast<std::uint64_t>(value);
                for (std::uint64_t byte = 0; byte < fixup.width; ++byte)
                    result.payload[static_cast<std::size_t>(patch_linear) + static_cast<std::size_t>(byte)] =
                        static_cast<std::uint8_t>((bits >> (byte * 8)) & 0xFF);
                if (relocation)
                {
                    MzRelocation entry;
                    entry.patch_linear = patch_linear;
                    entry.file_offset = patch_linear;
                    entry.offset_field = static_cast<std::uint16_t>(patch_linear & 15);
                    entry.segment_field = static_cast<std::uint16_t>(patch_linear >> 4);
                    entry.value = static_cast<std::uint64_t>(value);
                    entry.space = fixup.space;
                    entry.symbol = target.name;
                    result.relocations.push_back(std::move(entry));
                }
            }
        }
        if (!errors.empty())
            return std::unexpected(mz_join(errors));

        std::ranges::sort(result.relocations, [](MzRelocation const& a, MzRelocation const& b) { return a.patch_linear < b.patch_linear; });
        for (std::size_t index = 0; index < result.relocations.size(); ++index)
        {
            auto const& entry = result.relocations[index];
            if (entry.patch_linear + 2 > result.load_bytes)
                errors.push_back(std::format("MZ relocation site 0x{:X} is outside the stored load module", entry.patch_linear));
            if (index != 0 && entry.patch_linear < result.relocations[index - 1].patch_linear + 2)
                errors.push_back(std::format("duplicate or overlapping MZ relocation sites at 0x{:X}", entry.patch_linear));
        }
        if (!errors.empty())
            return std::unexpected(mz_join(errors));

        auto const& entry_name = options.entry_symbol.empty() ? std::string{"_start"} : options.entry_symbol;
        auto const* entry = result.find_symbol(entry_name);
        if (!entry || !entry->defined || entry->absolute || entry->space != ImageSpace::Code)
            return std::unexpected(std::format("entry symbol `{}` is not defined executable code for target 'i8086-mz'", entry_name));
        if (entry->value > 0xFFFF)
            return std::unexpected(std::format("entry symbol `{}` does not fit in the 16-bit code segment", entry_name));
        result.entry_offset = static_cast<std::uint16_t>(entry->value);
        result.initial_sp = static_cast<std::uint16_t>(result.stack_top & 0xFFFF);
        result.has_entry = true;

        auto const row_count = static_cast<std::uint64_t>(result.relocations.size());
        auto header_size = mz_align_up(28 + 4 * row_count, 16);
        if (!header_size)
            return std::unexpected(header_size.error());
        if (*header_size / 16 > 0xFFFF)
            return std::unexpected("i8086 MZ header exceeds the e_cparhdr field");
        result.header_size = static_cast<std::uint32_t>(*header_size);
        auto const file_size = static_cast<std::uint64_t>(result.header_size) + result.load_bytes;
        if (file_size > 0xFFFFFFFF)
            return std::unexpected("i8086 MZ file exceeds the DOS file size");
        auto const pages = (file_size + 511) / 512;
        if (pages > 0xFFFF)
            return std::unexpected("i8086 MZ file exceeds the e_cp field");
        auto const header_paragraphs = result.header_size / 16;
        if (header_paragraphs > 0xFFFF)
            return std::unexpected("i8086 MZ header exceeds the e_cparhdr field");
        result.file_size = static_cast<std::uint32_t>(file_size);
        result.header.assign(result.header_size, 0);
        auto const cblp = static_cast<std::uint16_t>(file_size % 512);
        mz_write_u16(result.header, 0x00, 0x5A4D);
        mz_write_u16(result.header, 0x02, cblp);
        mz_write_u16(result.header, 0x04, static_cast<std::uint16_t>(pages));
        mz_write_u16(result.header, 0x06, static_cast<std::uint16_t>(row_count));
        mz_write_u16(result.header, 0x08, static_cast<std::uint16_t>(header_paragraphs));
        mz_write_u16(result.header, 0x0A, result.min_alloc);
        mz_write_u16(result.header, 0x0C, result.min_alloc);
        mz_write_u16(result.header, 0x0E, static_cast<std::uint16_t>(result.dgroup_paragraph));
        mz_write_u16(result.header, 0x10, result.initial_sp);
        mz_write_u16(result.header, 0x12, 0);
        mz_write_u16(result.header, 0x14, result.entry_offset);
        mz_write_u16(result.header, 0x16, 0);
        mz_write_u16(result.header, 0x18, 0x1C);
        mz_write_u16(result.header, 0x1A, 0);
        for (std::size_t index = 0; index < result.relocations.size(); ++index)
        {
            auto const& relocation = result.relocations[index];
            mz_write_u16(result.header, 28 + index * 4, relocation.offset_field);
            mz_write_u16(result.header, 30 + index * 4, relocation.segment_field);
        }
        result.file = result.header;
        result.file.insert(result.file.end(), result.payload.begin(), result.payload.end());

        for (std::size_t index = 0; index < image.fragments.size(); ++index)
        {
            if (fragment_unit[index] == unplaced)
                continue;
            LayoutFragment placed;
            placed.id = index;
            placed.role = image.fragments[index].role;
            auto const family = families[index];
            placed.space = family && (*family == MzFamily::Start || *family == MzFamily::Text) ? ImageSpace::Code : ImageSpace::Dgroup;
            placed.linear = fragment_linear[index];
            placed.offset_in_segment = fragment_domain[index];
            placed.section = 0;
            for (std::size_t section = 0; section < result.sections.size(); ++section)
            {
                auto const& candidate = result.sections[section];
                if (placed.linear >= candidate.linear && placed.linear < candidate.linear + std::max<std::uint64_t>(candidate.memory_size, 1))
                    placed.section = section;
            }
            placed.stored_size = fragment_stored(index) ? image.fragments[index].bytes.size() : 0;
            placed.memory_size = fragment_size(index);
            if (placed.stored_size != 0)
                placed.file_offset = result.header_size + fragment_linear[index];
            placed.reason = std::format("placed in {}", result.sections[placed.section].name);
            result.fragments.push_back(std::move(placed));
        }

        return result;
    }

} // namespace dcc::backend::i8086
