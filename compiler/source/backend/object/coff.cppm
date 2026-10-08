export module dcc.backend.object.coff;

import std;
import dcc.ir;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;
import dcc.target;
import dcc.backend.object.layout;

#define into_u8 static_cast<std::uint8_t>

export namespace dcc::backend::object
{
    struct CoffArchPolicy
    {
        std::uint16_t machine;
        std::uint16_t reloc_rel32;
        std::uint16_t reloc_abs;
        std::uint64_t address_width;
    };

    inline constexpr CoffArchPolicy coff_x86_64_policy{0x8664, 0x0004, 0x0001, 8};

} // namespace dcc::backend::object

namespace dcc::backend::object
{
    using namespace dcc::backend::x86;

    namespace
    {
        constexpr std::uint16_t IMAGE_FILE_LINE_NUMS_STRIPPED = 0x0004;
        constexpr std::uint16_t IMAGE_FILE_DEBUG_STRIPPED = 0x0200;

        constexpr std::uint32_t IMAGE_SCN_CNT_CODE = 0x00000020;
        constexpr std::uint32_t IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040;
        constexpr std::uint32_t IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080;
        constexpr std::uint32_t IMAGE_SCN_MEM_EXECUTE = 0x20000000;
        constexpr std::uint32_t IMAGE_SCN_MEM_READ = 0x40000000;
        constexpr std::uint32_t IMAGE_SCN_MEM_WRITE = 0x80000000;
        constexpr std::uint32_t IMAGE_SCN_ALIGN_16BYTES = 0x00500000;
        constexpr std::uint32_t IMAGE_SCN_ALIGN_4BYTES = 0x00300000;
        constexpr std::uint32_t IMAGE_SCN_LNK_COMDAT = 0x00001000;
        constexpr std::uint32_t IMAGE_SCN_LNK_INFO = 0x00000200;
        constexpr std::uint32_t IMAGE_SCN_LNK_REMOVE = 0x00000800;
        constexpr std::uint8_t IMAGE_COMDAT_SELECT_ANY = 2;
        constexpr std::uint8_t IMAGE_COMDAT_SELECT_ASSOCIATIVE = 5;
        constexpr std::size_t no_coff_comdat = std::numeric_limits<std::size_t>::max();

        [[nodiscard]] bool is_link_once(ir::Linkage linkage)
        {
            return linkage == ir::Linkage::LinkOnceODR || linkage == ir::Linkage::WeakODR;
        }

        [[nodiscard]] std::uint32_t coff_comdat_checksum(std::span<std::uint8_t const> data)
        {
            std::uint32_t crc = 0;
            for (auto byte : data)
            {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
            }
            return crc;
        }

        [[nodiscard]] std::uint32_t coff_align_bits(std::uint64_t align)
        {
            if (align > 8192)
                align = 8192;
            std::uint64_t log2_align = 0;
            while ((std::uint64_t{1} << log2_align) < align)
                ++log2_align;
            return static_cast<std::uint32_t>((log2_align + 1) << 20);
        }

    } // namespace

} // namespace dcc::backend::object

export namespace dcc::backend::object
{
    [[nodiscard]] std::vector<std::uint8_t> write_coff(ir::IrModule const& ir_mod, MModule const& mod, std::vector<EncodeResult> const& encoded,
                                                       target::TargetConfig const& target, CoffArchPolicy const& arch)
    {
        (void)target;

        std::vector<std::string> func_names;
        func_names.reserve(mod.functions.size());
        for (auto const& mf : mod.functions)
        {
            if (!mf.owned_name.empty())
                func_names.push_back(mf.owned_name);
            else
                func_names.push_back("<unnamed>");
        }

        std::unordered_set<std::string> defined_names;
        for (auto const& fn : func_names)
            defined_names.insert(fn);

        for (auto* g : ir_mod.globals)
        {
            if (!g)
                continue;
            if (classify_global(g) != DataSection::None)
                defined_names.insert(std::string{g->name});
        }

        for (auto const& mf : mod.functions)
            for (auto const& jt : mf.jump_tables)
                defined_names.insert(jt.symbol);

        std::unordered_set<std::string> ext_sym_set;
        for (auto const& er : encoded)
            for (auto const& r : er.relocs)
                if (!defined_names.contains(std::string{r.symbol}))
                    ext_sym_set.insert(std::string{r.symbol});

        std::vector<std::string> ext_syms(ext_sym_set.begin(), ext_sym_set.end());
        std::ranges::sort(ext_syms);

        std::vector<std::vector<std::uint8_t>> func_codes;
        std::vector<std::vector<Reloc>> func_relocs;
        func_codes.reserve(encoded.size());
        func_relocs.reserve(encoded.size());
        for (auto const& er : encoded)
        {
            func_codes.push_back(er.bytes);
            func_relocs.push_back(er.relocs);
        }

        std::vector<GlobalLayout> globals;
        for (auto* g : ir_mod.globals)
        {
            if (!g)
                continue;

            GlobalLayout gl;
            gl.g = g;
            gl.sec = classify_global(g);
            if (gl.sec == DataSection::None)
                continue;

            gl.name_str = std::string{g->name};
            gl.alignment = g->alignment;
            if (gl.alignment == 0)
            {
                gl.alignment = g->type ? g->type->byte_align : 1;
                if (gl.alignment == 0)
                {
                    auto const* agg = dcc::ir::ir_type_cast<dcc::ir::IrAggregateType>(g->type);
                    if (agg && !agg->members.empty())
                    {
                        std::uint64_t max_align = 1;
                        for (auto const* m : agg->members)
                            if (m->byte_align > max_align)
                                max_align = m->byte_align;
                        gl.alignment = max_align;
                    }
                    else
                        gl.alignment = 1;
                }
            }

            globals.push_back(std::move(gl));
        }

        struct CoffReloc
        {
            std::uint32_t virt_addr;
            std::uint32_t sym_idx;
            std::uint16_t type;
            std::uint32_t addend{};
        };

        struct CoffComdat
        {
            std::size_t func{};
            std::uint32_t text_index{};
            std::uint32_t rdata_index{};
            std::vector<std::uint8_t> rdata;
            std::vector<CoffReloc> text_rels;
            std::vector<CoffReloc> rdata_rels;
            std::uint32_t text_raw{};
            std::uint32_t rdata_raw{};
            std::size_t text_reloc_block{};
            std::size_t rdata_reloc_block{};
        };

        struct CoffCustomSection
        {
            std::string name;
            std::uint32_t str_off{};
            std::uint32_t characteristics{};
            bool is_bss{true};
            std::vector<GlobalLayout*> globals;
            std::vector<std::uint8_t> data;
            std::vector<CoffReloc> rels;
            std::uint64_t bss_size{};
            std::uint32_t section_index{};
            std::uint32_t raw_start{};
            std::uint32_t reloc_block{};
        };

        std::vector<GlobalLayout*> rodata_globals, data_globals, bss_globals;
        std::vector<CoffCustomSection> custom_sections;
        std::unordered_map<std::string, std::size_t> custom_section_index;
        for (auto& gl : globals)
        {
            if (!gl.g->section.empty())
            {
                std::string sname{gl.g->section};
                auto it = custom_section_index.find(sname);
                if (it == custom_section_index.end())
                {
                    CoffCustomSection cs;
                    cs.name = sname;
                    custom_section_index[sname] = custom_sections.size();
                    custom_sections.push_back(std::move(cs));
                }
                auto& cs = custom_sections[custom_section_index[sname]];
                if (gl.sec == DataSection::Rodata)
                    cs.characteristics |= IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
                else if (gl.sec == DataSection::RodataRelRO || gl.sec == DataSection::Data)
                    cs.characteristics |= IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
                else if (gl.sec == DataSection::Bss)
                    cs.characteristics |= IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
                if (sname == ".drectve")
                    cs.characteristics = IMAGE_SCN_LNK_INFO | IMAGE_SCN_LNK_REMOVE;
                if (gl.sec != DataSection::Bss)
                    cs.is_bss = false;
                cs.globals.push_back(&gl);
                continue;
            }
            if (gl.sec == DataSection::Rodata || gl.sec == DataSection::RodataRelRO)
                rodata_globals.push_back(&gl);
            else if (gl.sec == DataSection::Data)
                data_globals.push_back(&gl);
            else if (gl.sec == DataSection::Bss)
                bss_globals.push_back(&gl);
        }

        std::uint64_t rodata_align = max_global_alignment(rodata_globals);
        std::uint64_t data_align = max_global_alignment(data_globals);
        std::uint64_t bss_align = max_global_alignment(bss_globals);
        for (auto& cs : custom_sections)
            cs.characteristics |= coff_align_bits(max_global_alignment(cs.globals));

        std::string strtab;
        auto add_str = [&](std::string_view s) -> std::uint32_t {
            auto off = static_cast<std::uint32_t>(strtab.size());
            strtab += s;
            strtab += '\0';
            return off;
        };

        struct CoffSym
        {
            std::string name;
            std::uint32_t str_off{};
            bool is_func : 1 {};
            bool is_object : 1 {};
            bool is_sec : 1 {};
            bool is_local : 1 {};
            std::uint32_t sec_idx;
            std::uint64_t value;
            std::uint64_t size;
            std::size_t comdat{no_coff_comdat};
            bool comdat_rdata{};
        };
        std::vector<CoffSym> coff_syms;
        std::unordered_map<std::string, std::uint32_t> sym_name_to_idx;

        auto text_sec_str = add_str(".text");
        coff_syms.push_back(
            {.name = ".text", .str_off = text_sec_str, .is_func = false, .is_object = false, .is_sec = true, .sec_idx = 1, .value = 0, .size = 0});

        std::uint32_t sec_rdata = 0, sec_data = 0, sec_bss = 0;
        bool has_coff_jt = false;
        for (auto const& mf : mod.functions)
            if (!mf.jump_tables.empty() && !is_link_once(mf.linkage))
            {
                has_coff_jt = true;
                break;
            }
        bool has_rdata = !rodata_globals.empty() || has_coff_jt;
        bool has_data_sec = !data_globals.empty();
        bool has_bss_sec = !bss_globals.empty();

        std::uint32_t rdata_sec_str = 0, data_sec_str = 0, bss_sec_str = 0;
        if (has_rdata)
        {
            rdata_sec_str = add_str(".rdata");
            sec_rdata = 2;
            coff_syms.push_back({.name = ".rdata",
                                 .str_off = rdata_sec_str,
                                 .is_func = false,
                                 .is_object = false,
                                 .is_sec = true,
                                 .sec_idx = sec_rdata,
                                 .value = 0,
                                 .size = 0});
        }
        if (has_data_sec)
        {
            data_sec_str = add_str(".data");
            sec_data = has_rdata ? 3 : 2;
            coff_syms.push_back(
                {.name = ".data", .str_off = data_sec_str, .is_func = false, .is_object = false, .is_sec = true, .sec_idx = sec_data, .value = 0, .size = 0});
        }
        if (has_bss_sec)
        {
            bss_sec_str = add_str(".bss");
            sec_bss = (has_rdata ? 1 : 0) + (has_data_sec ? 1 : 0) + 2;
            coff_syms.push_back(
                {.name = ".bss", .str_off = bss_sec_str, .is_func = false, .is_object = false, .is_sec = true, .sec_idx = sec_bss, .value = 0, .size = 0});
        }

        std::uint32_t num_std_sec = 1 + (has_rdata ? 1U : 0U) + (has_data_sec ? 1U : 0U) + (has_bss_sec ? 1U : 0U);
        std::uint32_t next_sec = num_std_sec + 1;
        for (auto& cs : custom_sections)
        {
            cs.section_index = next_sec++;
            cs.str_off = add_str(cs.name);
            coff_syms.push_back({.name = cs.name,
                                 .str_off = cs.str_off,
                                 .is_func = false,
                                 .is_object = false,
                                 .is_sec = true,
                                 .sec_idx = cs.section_index,
                                 .value = 0,
                                 .size = 0});
        }

        std::vector<CoffComdat> comdats;
        std::vector<std::size_t> func_comdat(mod.functions.size(), no_coff_comdat);
        for (std::size_t fi = 0; fi < mod.functions.size(); ++fi)
        {
            if (!is_link_once(mod.functions[fi].linkage))
                continue;
            CoffComdat comdat;
            comdat.func = fi;
            comdat.text_index = next_sec++;
            if (!mod.functions[fi].jump_tables.empty())
                comdat.rdata_index = next_sec++;
            func_comdat[fi] = comdats.size();
            comdats.push_back(std::move(comdat));
        }

        for (auto& gl : globals)
        {
            if (!gl.g->section.empty())
                continue;
            if (gl.sec == DataSection::Rodata || gl.sec == DataSection::RodataRelRO)
                gl.section_index = sec_rdata;
            else if (gl.sec == DataSection::Data)
                gl.section_index = sec_data;
            else if (gl.sec == DataSection::Bss)
                gl.section_index = sec_bss;
        }
        for (auto& cs : custom_sections)
            for (auto* glp : cs.globals)
                glp->section_index = cs.section_index;

        for (auto const& ext : ext_syms)
        {
            auto so = add_str(ext);
            coff_syms.push_back({.name = ext, .str_off = so, .is_func = false, .is_object = false, .sec_idx = 0, .value = 0, .size = 0});
            sym_name_to_idx[ext] = static_cast<std::uint32_t>(coff_syms.size() - 1);
        }

        std::vector<std::uint8_t> text_data;
        std::vector<std::size_t> func_starts;
        func_starts.reserve(func_codes.size());
        for (std::size_t fi = 0; fi < func_codes.size(); ++fi)
        {
            auto const& code = func_codes[fi];
            if (func_comdat[fi] != no_coff_comdat)
            {
                func_starts.push_back(0);
                continue;
            }
            func_starts.push_back(text_data.size());
            text_data.insert(text_data.end(), code.begin(), code.end());
            while (text_data.size() % 16 != 0)
                text_data.push_back(0x90);
        }

        for (std::size_t i = 0; i < func_names.size(); ++i)
        {
            auto so = add_str(func_names[i]);
            std::uint32_t func_sec = 1;
            if (func_comdat[i] != no_coff_comdat)
            {
                func_sec = comdats[func_comdat[i]].text_index;
                coff_syms.push_back({.name = ".text",
                                     .str_off = text_sec_str,
                                     .is_func = false,
                                     .is_object = false,
                                     .is_sec = true,
                                     .sec_idx = func_sec,
                                     .value = 0,
                                     .size = 0,
                                     .comdat = func_comdat[i]});
            }
            coff_syms.push_back({.name = func_names[i],
                                 .str_off = so,
                                 .is_func = true,
                                 .is_object = false,
                                 .sec_idx = func_sec,
                                 .value = func_starts[i],
                                 .size = func_codes[i].size()});
            sym_name_to_idx[func_names[i]] = static_cast<std::uint32_t>(coff_syms.size() - 1);
            if (func_comdat[i] != no_coff_comdat && comdats[func_comdat[i]].rdata_index != 0)
                coff_syms.push_back({.name = ".rdata",
                                     .str_off = 0,
                                     .is_func = false,
                                     .is_object = false,
                                     .is_sec = true,
                                     .sec_idx = comdats[func_comdat[i]].rdata_index,
                                     .value = 0,
                                     .size = 0,
                                     .comdat = func_comdat[i],
                                     .comdat_rdata = true});
        }

        std::uint64_t bss_sec_size = 0;
        for (auto* glp : bss_globals)
        {
            auto pad = align_up(bss_sec_size, glp->alignment);
            glp->offset = pad;
            bss_sec_size = pad + (glp->g->type ? glp->g->type->byte_size : 0);
        }

        for (auto& gl : globals)
        {
            auto so = add_str(gl.name_str);
            coff_syms.push_back({.name = gl.name_str,
                                 .str_off = so,
                                 .is_func = false,
                                 .is_object = true,
                                 .sec_idx = gl.section_index,
                                 .value = gl.offset,
                                 .size = gl.g->type ? gl.g->type->byte_size : 0});
            sym_name_to_idx[std::string{gl.g->name}] = static_cast<std::uint32_t>(coff_syms.size() - 1);
        }

        for (std::size_t fi = 0; fi < mod.functions.size(); ++fi)
        {
            auto const& mf = mod.functions[fi];
            auto const& er = encoded[fi];
            for (auto const& jt : mf.jump_tables)
            {
                for (auto tgt : jt.targets)
                {
                    auto it = er.block_offsets.find(tgt);
                    if (it == er.block_offsets.end())
                        continue;

                    std::string sym_name = mf.owned_name.empty() ? std::string{"anon"} : mf.owned_name;
                    sym_name += ".bb" + std::to_string(tgt);

                    if (sym_name_to_idx.contains(sym_name))
                        continue;

                    std::uint64_t block_offset = func_starts[fi] + it->second;
                    auto so = add_str(sym_name);
                    coff_syms.push_back({.name = sym_name,
                                         .str_off = so,
                                         .is_func = false,
                                         .is_object = false,
                                         .is_local = true,
                                         .sec_idx = func_comdat[fi] == no_coff_comdat ? 1U : comdats[func_comdat[fi]].text_index,
                                         .value = block_offset,
                                         .size = 0});
                    sym_name_to_idx[sym_name] = static_cast<std::uint32_t>(coff_syms.size() - 1);
                }
            }
        }

        std::unordered_map<std::string, std::size_t> jt_sym_slot;
        for (std::size_t fi = 0; fi < mod.functions.size(); ++fi)
        {
            for (auto const& jt : mod.functions[fi].jump_tables)
            {
                if (sym_name_to_idx.contains(jt.symbol))
                    continue;

                auto so = add_str(jt.symbol);
                auto jt_sec = func_comdat[fi] == no_coff_comdat ? sec_rdata : comdats[func_comdat[fi]].rdata_index;
                coff_syms.push_back(
                    {.name = jt.symbol, .str_off = so, .is_func = false, .is_object = false, .is_local = true, .sec_idx = jt_sec, .value = 0, .size = 0});

                sym_name_to_idx[jt.symbol] = static_cast<std::uint32_t>(coff_syms.size() - 1);
                jt_sym_slot[jt.symbol] = coff_syms.size() - 1;
            }
        }

        {
            std::uint32_t aux_so_far = 0;
            for (std::size_t ci = 0; ci < coff_syms.size(); ++ci)
            {
                auto& cs = coff_syms[ci];
                if (!cs.name.empty())
                {
                    auto it = sym_name_to_idx.find(cs.name);
                    if (it != sym_name_to_idx.end() && it->second == static_cast<std::uint32_t>(ci))
                        it->second = static_cast<std::uint32_t>(ci) + aux_so_far;
                }
                if (cs.is_func || cs.comdat != no_coff_comdat)
                    ++aux_so_far;
            }
        }

        auto build_coff_init_data = [&](std::vector<GlobalLayout*>& gvec, std::uint32_t) -> std::pair<std::vector<std::uint8_t>, std::vector<CoffReloc>> {
            std::vector<std::uint8_t> sec_data;
            std::vector<CoffReloc> rels;

            for (auto* glp : gvec)
            {
                auto pad = align_up(sec_data.size(), glp->alignment);
                while (sec_data.size() < pad)
                    sec_data.push_back(0);

                glp->offset = sec_data.size();

                if (glp->g->init)
                {
                    auto size = init_type_size(glp->g->type);
                    if (size == 0 && glp->g->init->type)
                        size = init_type_size(glp->g->init->type);
                    std::vector<std::uint8_t> image(size, 0);
                    std::vector<InitReloc> init_relocs;
                    serialize_init_memory(image, init_relocs, glp->g->init, glp->g->type, 0, arch.address_width);
                    sec_data.insert(sec_data.end(), image.begin(), image.end());
                    for (auto const& init_reloc : init_relocs)
                    {
                        auto it = sym_name_to_idx.find(init_reloc.name);
                        if (it == sym_name_to_idx.end())
                            continue;
                        CoffReloc rel{};
                        rel.virt_addr = static_cast<std::uint32_t>(glp->offset + init_reloc.offset);
                        rel.sym_idx = it->second;
                        rel.type = arch.reloc_abs;
                        rel.addend = static_cast<std::uint32_t>(static_cast<std::int32_t>(init_reloc.addend));
                        rels.push_back(rel);
                    }
                }
            }
            return {std::move(sec_data), std::move(rels)};
        };

        auto [rdata_data, rdata_rels] = build_coff_init_data(rodata_globals, sec_rdata);

        auto emit_jump_tables = [&](MFunction const& mf, std::vector<std::uint8_t>& section, std::vector<CoffReloc>& rels) {
            for (auto const& jt : mf.jump_tables)
            {
                while (section.size() % 4 != 0)
                    section.push_back(0);

                if (auto slot = jt_sym_slot.find(jt.symbol); slot != jt_sym_slot.end())
                    coff_syms[slot->second].value = section.size();

                for (std::size_t ei = 0; ei < jt.targets.size(); ++ei)
                {
                    std::uint32_t tgt_block = jt.targets[ei];
                    std::string blk_sym = mf.owned_name.empty() ? "anon" : mf.owned_name;
                    blk_sym += ".bb" + std::to_string(tgt_block);

                    auto blk_sym_it = sym_name_to_idx.find(blk_sym);
                    std::uint64_t entry_va_offset = section.size();
                    w32(section, 0);

                    if (blk_sym_it != sym_name_to_idx.end())
                    {
                        CoffReloc rel{};
                        rel.virt_addr = static_cast<std::uint32_t>(entry_va_offset);
                        rel.sym_idx = blk_sym_it->second;
                        rel.type = arch.reloc_rel32;
                        rels.push_back(rel);
                    }
                }
            }
        };

        for (std::size_t fi = 0; fi < mod.functions.size(); ++fi)
            if (func_comdat[fi] == no_coff_comdat)
                emit_jump_tables(mod.functions[fi], rdata_data, rdata_rels);
        for (auto& comdat : comdats)
            if (comdat.rdata_index != 0)
                emit_jump_tables(mod.functions[comdat.func], comdat.rdata, comdat.rdata_rels);

        auto [data_sec_data, data_rels] = build_coff_init_data(data_globals, sec_data);

        for (auto& cs : custom_sections)
        {
            if (cs.is_bss)
            {
                for (auto* glp : cs.globals)
                {
                    auto pad = align_up(cs.bss_size, glp->alignment);
                    glp->offset = pad;
                    cs.bss_size = pad + (glp->g->type ? glp->g->type->byte_size : 0);
                }
                continue;
            }

            for (auto* glp : cs.globals)
            {
                auto pad = align_up(cs.data.size(), glp->alignment);
                while (cs.data.size() < pad)
                    cs.data.push_back(0);

                glp->offset = cs.data.size();

                if (glp->g->init)
                {
                    auto size = init_type_size(glp->g->type);
                    if (size == 0 && glp->g->init->type)
                        size = init_type_size(glp->g->init->type);
                    std::vector<std::uint8_t> image(size, 0);
                    std::vector<InitReloc> init_relocs;
                    serialize_init_memory(image, init_relocs, glp->g->init, glp->g->type, 0, arch.address_width);
                    cs.data.insert(cs.data.end(), image.begin(), image.end());
                    for (auto const& init_reloc : init_relocs)
                    {
                        auto it = sym_name_to_idx.find(init_reloc.name);
                        if (it == sym_name_to_idx.end())
                            continue;
                        CoffReloc rel{};
                        rel.virt_addr = static_cast<std::uint32_t>(glp->offset + init_reloc.offset);
                        rel.sym_idx = it->second;
                        rel.type = arch.reloc_abs;
                        rel.addend = static_cast<std::uint32_t>(static_cast<std::int32_t>(init_reloc.addend));
                        cs.rels.push_back(rel);
                    }
                }
                else
                    cs.data.resize(cs.data.size() + (glp->g->type ? glp->g->type->byte_size : 0), 0);
            }
        }

        for (auto& cs : coff_syms)
        {
            if (!cs.is_object)
                continue;
            for (auto const& gl : globals)
            {
                if (gl.name_str == cs.name)
                {
                    cs.value = gl.offset;
                    break;
                }
            }
        }

        for (auto& cs : coff_syms)
        {
            if (cs.name == ".text")
                cs.size = text_data.size();
            if (cs.name == ".rdata")
                cs.size = rdata_data.size();
            if (cs.name == ".data")
                cs.size = data_sec_data.size();
            if (cs.name == ".bss")
                cs.size = bss_sec_size;
        }

        std::vector<CoffReloc> text_rels;
        for (std::size_t fi = 0; fi < func_relocs.size(); ++fi)
        {
            auto func_off = func_starts[fi];
            for (auto const& r : func_relocs[fi])
            {
                auto it = sym_name_to_idx.find(std::string{r.symbol});
                if (it == sym_name_to_idx.end())
                    continue;

                std::uint16_t rtype;
                switch (r.kind)
                {
                    case Reloc::Kind::Rel32:
                    case Reloc::Kind::Rel32_Got:
                    case Reloc::Kind::Rel32_Call:
                        rtype = arch.reloc_rel32;
                        break;
                    case Reloc::Kind::Abs64:
                        rtype = arch.reloc_abs;
                        break;
                    case Reloc::Kind::Abs16:
                    case Reloc::Kind::Abs32:
                    case Reloc::Kind::Rel16:
                        std::println(std::cerr, "coff objwriter: 16/32-bit x86 relocation in a 64-bit object; refusing to emit malformed object");
                        std::abort();
                    default:
                        rtype = arch.reloc_rel32;
                        break;
                }
                if (func_comdat[fi] != no_coff_comdat)
                    comdats[func_comdat[fi]].text_rels.push_back({.virt_addr = static_cast<std::uint32_t>(r.offset), .sym_idx = it->second, .type = rtype});
                else
                    text_rels.push_back({.virt_addr = static_cast<std::uint32_t>(func_off + r.offset), .sym_idx = it->second, .type = rtype});
            }
        }

        std::uint32_t num_sec = 1;
        if (has_rdata)
            num_sec++;
        if (has_data_sec)
            num_sec++;
        if (has_bss_sec)
            num_sec++;
        num_sec += static_cast<std::uint32_t>(custom_sections.size());
        for (auto const& comdat : comdats)
            num_sec += comdat.rdata_index != 0 ? 2U : 1U;

        std::uint32_t hdr_size = 20;
        std::uint32_t sec_hdr_size = 40;
        std::uint32_t sec_hdr_start = hdr_size;
        std::uint32_t sec_hdr_total = num_sec * sec_hdr_size;
        std::uint32_t sec_hdr_end = sec_hdr_start + sec_hdr_total;

        std::uint32_t cur_raw = sec_hdr_end;

        std::uint32_t text_raw_start = cur_raw;
        std::uint32_t text_raw_end = text_raw_start + static_cast<std::uint32_t>(text_data.size());
        cur_raw = text_raw_end;
        if (cur_raw % 4 != 0)
            cur_raw += 4 - (cur_raw % 4);

        std::uint32_t rdata_raw_start = cur_raw;
        std::uint32_t rdata_raw_end = rdata_raw_start + static_cast<std::uint32_t>(rdata_data.size());
        cur_raw = rdata_raw_end;
        if (cur_raw % 4 != 0)
            cur_raw += 4 - (cur_raw % 4);

        std::uint32_t data_raw_start = cur_raw;
        std::uint32_t data_raw_end = data_raw_start + static_cast<std::uint32_t>(data_sec_data.size());
        cur_raw = data_raw_end;
        if (cur_raw % 4 != 0)
            cur_raw += 4 - (cur_raw % 4);

        for (auto& cs : custom_sections)
        {
            if (cs.is_bss)
                continue;
            cs.raw_start = cur_raw;
            cur_raw += static_cast<std::uint32_t>(cs.data.size());
            if (cur_raw % 4 != 0)
                cur_raw += 4 - (cur_raw % 4);
        }

        for (auto& comdat : comdats)
        {
            comdat.text_raw = cur_raw;
            cur_raw += static_cast<std::uint32_t>(func_codes[comdat.func].size());
            if (cur_raw % 4 != 0)
                cur_raw += 4 - (cur_raw % 4);
            if (comdat.rdata_index == 0)
                continue;
            comdat.rdata_raw = cur_raw;
            cur_raw += static_cast<std::uint32_t>(comdat.rdata.size());
            if (cur_raw % 4 != 0)
                cur_raw += 4 - (cur_raw % 4);
        }

        struct CoffSecReloc
        {
            std::uint32_t raw_start;
            std::vector<CoffReloc> rels;
        };
        std::vector<CoffSecReloc> sec_relocs;
        sec_relocs.push_back({text_raw_start, text_rels});
        if (has_rdata)
            sec_relocs.push_back({rdata_raw_start, rdata_rels});
        if (has_data_sec)
            sec_relocs.push_back({data_raw_start, data_rels});
        if (has_bss_sec)
            sec_relocs.push_back({0, {}});
        for (auto& cs : custom_sections)
        {
            if (cs.is_bss)
                sec_relocs.push_back({0, {}});
            else
                sec_relocs.push_back({cs.raw_start, cs.rels});
            cs.reloc_block = static_cast<std::uint32_t>(sec_relocs.size() - 1);
        }
        for (auto& comdat : comdats)
        {
            sec_relocs.push_back({comdat.text_raw, comdat.text_rels});
            comdat.text_reloc_block = sec_relocs.size() - 1;
            if (comdat.rdata_index == 0)
                continue;
            sec_relocs.push_back({comdat.rdata_raw, comdat.rdata_rels});
            comdat.rdata_reloc_block = sec_relocs.size() - 1;
        }

        for (auto& sr : sec_relocs)
        {
            if (!sr.rels.empty())
            {
                sr.raw_start = cur_raw;
                cur_raw += static_cast<std::uint32_t>(sr.rels.size() * 10);
            }
        }

        std::uint32_t sym_start = cur_raw;
        std::uint32_t num_func_aux = 0;
        for (auto const& cs : coff_syms)
            if (cs.is_func || cs.comdat != no_coff_comdat)
                num_func_aux++;

        std::uint32_t num_syms = static_cast<std::uint32_t>(coff_syms.size()) + num_func_aux;
        std::uint32_t str_size = 4 + static_cast<std::uint32_t>(strtab.size());

        std::vector<std::uint8_t> out;

        w16(out, arch.machine);
        w16(out, static_cast<std::uint16_t>(num_sec));
        w32(out, 0);
        w32(out, sym_start);
        w32(out, num_syms);
        w16(out, 0);
        w16(out, IMAGE_FILE_LINE_NUMS_STRIPPED | IMAGE_FILE_DEBUG_STRIPPED);

        auto write_sec_hdr = [&](std::string_view name, std::uint32_t name_str_off, std::uint32_t raw_size, std::uint32_t raw_ptr, std::uint32_t reloc_ptr,
                                 std::uint16_t reloc_count, std::uint32_t characteristics) {
            std::array<std::uint8_t, 8> name_buf = {0};
            if (name.size() <= 8)
            {
                for (std::size_t i = 0; i < name.size(); ++i)
                    name_buf[i] = into_u8(name[i]);
            }
            else
            {
                auto offset_str = "/" + std::to_string(name_str_off + 4);
                for (std::size_t i = 0; i < offset_str.size() && i < 8; ++i)
                    name_buf[i] = into_u8(offset_str[i]);
            }

            for (auto c : name_buf)
                w8(out, c);

            w32(out, 0);
            w32(out, 0);
            w32(out, raw_size);
            w32(out, raw_ptr);
            w32(out, reloc_ptr);
            w32(out, 0);
            w16(out, reloc_count);
            w16(out, 0);
            w32(out, characteristics);
        };

        {
            auto reloc_ptr = text_rels.empty() ? 0 : sec_relocs[0].raw_start;
            write_sec_hdr(".text", 0, static_cast<std::uint32_t>(text_data.size()), text_raw_start, reloc_ptr, static_cast<std::uint16_t>(text_rels.size()),
                          IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_16BYTES);
        }

        if (has_rdata)
        {
            std::size_t ri = 1;
            auto reloc_ptr = rdata_rels.empty() ? 0U : sec_relocs[ri].raw_start;
            write_sec_hdr(".rdata", 0, static_cast<std::uint32_t>(rdata_data.size()), rdata_raw_start, reloc_ptr, static_cast<std::uint16_t>(rdata_rels.size()),
                          IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | coff_align_bits(rodata_align));
        }

        if (has_data_sec)
        {
            std::size_t ri = (has_rdata ? std::size_t{2} : std::size_t{1});
            auto reloc_ptr = data_rels.empty() ? 0U : sec_relocs[ri].raw_start;
            write_sec_hdr(".data", 0, static_cast<std::uint32_t>(data_sec_data.size()), data_raw_start, reloc_ptr, static_cast<std::uint16_t>(data_rels.size()),
                          IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | coff_align_bits(data_align));
        }

        if (has_bss_sec)
        {
            write_sec_hdr(".bss", 0, static_cast<std::uint32_t>(bss_sec_size), 0, 0, 0,
                          IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | coff_align_bits(bss_align));
        }

        for (auto& cs : custom_sections)
        {
            auto reloc_ptr = cs.rels.empty() ? 0U : sec_relocs[cs.reloc_block].raw_start;
            if (cs.is_bss)
                write_sec_hdr(cs.name, cs.str_off, static_cast<std::uint32_t>(cs.bss_size), 0, 0, 0, cs.characteristics);
            else
                write_sec_hdr(cs.name, cs.str_off, static_cast<std::uint32_t>(cs.data.size()), cs.raw_start, reloc_ptr,
                              static_cast<std::uint16_t>(cs.rels.size()), cs.characteristics);
        }

        for (auto const& comdat : comdats)
        {
            auto text_reloc_ptr = comdat.text_rels.empty() ? 0U : sec_relocs[comdat.text_reloc_block].raw_start;
            write_sec_hdr(".text", 0, static_cast<std::uint32_t>(func_codes[comdat.func].size()), comdat.text_raw, text_reloc_ptr,
                          static_cast<std::uint16_t>(comdat.text_rels.size()),
                          IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_16BYTES | IMAGE_SCN_LNK_COMDAT);
            if (comdat.rdata_index == 0)
                continue;
            auto rdata_reloc_ptr = comdat.rdata_rels.empty() ? 0U : sec_relocs[comdat.rdata_reloc_block].raw_start;
            write_sec_hdr(".rdata", 0, static_cast<std::uint32_t>(comdat.rdata.size()), comdat.rdata_raw, rdata_reloc_ptr,
                          static_cast<std::uint16_t>(comdat.rdata_rels.size()),
                          IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_4BYTES | IMAGE_SCN_LNK_COMDAT);
        }

        while (out.size() < text_raw_start)
            w8(out, 0);
        out.insert(out.end(), text_data.begin(), text_data.end());

        if (has_rdata)
        {
            while (out.size() < rdata_raw_start)
                w8(out, 0);
            out.insert(out.end(), rdata_data.begin(), rdata_data.end());
        }

        if (has_data_sec)
        {
            while (out.size() < data_raw_start)
                w8(out, 0);
            out.insert(out.end(), data_sec_data.begin(), data_sec_data.end());
        }

        for (auto& cs : custom_sections)
        {
            if (cs.is_bss)
                continue;
            while (out.size() < cs.raw_start)
                w8(out, 0);
            out.insert(out.end(), cs.data.begin(), cs.data.end());
        }

        for (auto const& comdat : comdats)
        {
            while (out.size() < comdat.text_raw)
                w8(out, 0);
            out.insert(out.end(), func_codes[comdat.func].begin(), func_codes[comdat.func].end());
            if (comdat.rdata_index == 0)
                continue;
            while (out.size() < comdat.rdata_raw)
                w8(out, 0);
            out.insert(out.end(), comdat.rdata.begin(), comdat.rdata.end());
        }

        for (auto const& sr : sec_relocs)
        {
            if (sr.rels.empty())
                continue;
            while (out.size() < sr.raw_start)
                w8(out, 0);
            for (auto const& r : sr.rels)
            {
                w32(out, r.virt_addr);
                w32(out, r.sym_idx);
                w16(out, r.type);
            }
        }

        while (out.size() < sym_start)
            w8(out, 0);

        for (auto const& cs : coff_syms)
        {
            if (cs.name.size() <= 8)
            {
                std::array<std::uint8_t, 8> name_buf = {0};
                for (std::size_t i = 0; i < cs.name.size(); ++i)
                    name_buf[i] = into_u8(cs.name[i]);
                for (auto c : name_buf)
                    w8(out, c);
            }
            else
            {
                w32(out, 0);
                w32(out, cs.str_off + 4);
            }
            w32(out, static_cast<std::uint32_t>(cs.value));
            w16(out, static_cast<std::uint16_t>(cs.sec_idx));
            if (cs.is_func)
            {
                w16(out, 0x20);
                w8(out, 0x02);
                w8(out, 1);
                w32(out, 0);
                w32(out, static_cast<std::uint32_t>(cs.size));
                w32(out, 0);
                w32(out, 0);
                w16(out, 0);
            }
            else if (cs.is_object)
            {
                bool is_local = false;
                for (auto const& gl : globals)
                    if (gl.name_str == cs.name && gl.g->linkage == ir::Linkage::Internal)
                        is_local = true;
                w16(out, 0);
                w8(out, is_local ? 3 : 2);
                w8(out, 0);
            }
            else if (cs.comdat != no_coff_comdat)
            {
                auto const& comdat = comdats[cs.comdat];
                auto const& data = cs.comdat_rdata ? comdat.rdata : func_codes[comdat.func];
                auto const& rels = cs.comdat_rdata ? comdat.rdata_rels : comdat.text_rels;
                w16(out, 0);
                w8(out, 3);
                w8(out, 1);
                w32(out, static_cast<std::uint32_t>(data.size()));
                w16(out, static_cast<std::uint16_t>(rels.size()));
                w16(out, 0);
                w32(out, coff_comdat_checksum(data));
                w16(out, static_cast<std::uint16_t>(comdat.text_index));
                w8(out, cs.comdat_rdata ? IMAGE_COMDAT_SELECT_ASSOCIATIVE : IMAGE_COMDAT_SELECT_ANY);
                w8(out, 0);
                w16(out, 0);
            }
            else
            {
                w16(out, 0);
                if (cs.is_sec || cs.is_local)
                    w8(out, 3);
                else
                    w8(out, cs.name.empty() ? 0 : 2);
                w8(out, 0);
            }
        }

        w32(out, str_size);
        out.insert(out.end(), strtab.begin(), strtab.end());

        return out;
    }

} // namespace dcc::backend::object
