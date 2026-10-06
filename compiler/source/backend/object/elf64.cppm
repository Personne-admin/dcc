export module dcc.backend.object.elf64;

import std;
import dcc.ir;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;
import dcc.target;
import dcc.backend.object.layout;

#define into_u8 static_cast<std::uint8_t>

namespace dcc::backend::em64t
{
    using namespace dcc::backend::object;

    namespace
    {
        constexpr std::uint16_t ET_REL = 1;
        constexpr std::uint16_t EM_X86_64 = 62;
        constexpr std::uint32_t EV_CURRENT = 1;

        constexpr std::uint32_t SHT_PROGBITS = 1;
        constexpr std::uint32_t SHT_NOBITS = 8;
        constexpr std::uint32_t SHT_SYMTAB = 2;
        constexpr std::uint32_t SHT_STRTAB = 3;
        constexpr std::uint32_t SHT_RELA = 4;

        constexpr std::uint64_t SHF_ALLOC = 0x2;
        constexpr std::uint64_t SHF_EXECINSTR = 0x4;
        constexpr std::uint64_t SHF_WRITE = 0x1;

        constexpr std::uint32_t STB_LOCAL = 0;
        constexpr std::uint32_t STB_GLOBAL = 1;
        constexpr std::uint32_t STB_WEAK = 2;
        constexpr std::uint32_t STT_NOTYPE = 0;
        constexpr std::uint32_t STT_OBJECT = 1;
        constexpr std::uint32_t STT_FUNC = 2;

        constexpr std::uint32_t R_X86_64_64 = 1;
        constexpr std::uint32_t R_X86_64_PC32 = 2;
        constexpr std::uint32_t R_X86_64_PLT32 = 4;
        constexpr std::uint32_t R_X86_64_REX_GOTPCRELX = 42;

        struct Elf64_Ehdr
        {
            std::array<std::uint8_t, 16> e_ident;
            std::uint16_t e_type;
            std::uint16_t e_machine;
            std::uint32_t e_version;
            std::uint64_t e_entry;
            std::uint64_t e_phoff;
            std::uint64_t e_shoff;
            std::uint32_t e_flags;
            std::uint16_t e_ehsize;
            std::uint16_t e_phentsize;
            std::uint16_t e_phnum;
            std::uint16_t e_shentsize;
            std::uint16_t e_shnum;
            std::uint16_t e_shstrndx;
        };

        static_assert(sizeof(Elf64_Ehdr) == 64, "ELF header size");

        struct Elf64_Shdr
        {
            std::uint32_t sh_name;
            std::uint32_t sh_type;
            std::uint64_t sh_flags;
            std::uint64_t sh_addr;
            std::uint64_t sh_offset;
            std::uint64_t sh_size;
            std::uint32_t sh_link;
            std::uint32_t sh_info;
            std::uint64_t sh_addralign;
            std::uint64_t sh_entsize;
        };

        struct Elf64_Sym
        {
            std::uint32_t st_name;
            std::uint8_t st_info;
            std::uint8_t st_other;
            std::uint16_t st_shndx;
            std::uint64_t st_value;
            std::uint64_t st_size;
        };

        struct Elf64_Rela
        {
            std::uint64_t r_offset;
            std::uint64_t r_info;
            std::int64_t r_addend;
        };

        [[nodiscard]] std::uint8_t elf_st_info(std::uint8_t bind, std::uint8_t type)
        {
            return into_u8((bind << 4) | (type & 0xF));
        }
        [[nodiscard]] std::uint64_t elf_r_info(std::uint32_t sym, std::uint32_t type)
        {
            return static_cast<std::uint64_t>(type) | (static_cast<std::uint64_t>(sym) << 32);
        }

        void serialize_ehdr(std::vector<std::uint8_t>& b, Elf64_Ehdr const& h)
        {
            for (auto c : h.e_ident)
                w8(b, c);
            w16(b, h.e_type);
            w16(b, h.e_machine);
            w32(b, h.e_version);
            w64(b, h.e_entry);
            w64(b, h.e_phoff);
            w64(b, h.e_shoff);
            w32(b, h.e_flags);
            w16(b, h.e_ehsize);
            w16(b, h.e_phentsize);
            w16(b, h.e_phnum);
            w16(b, h.e_shentsize);
            w16(b, h.e_shnum);
            w16(b, h.e_shstrndx);
        }

        void serialize_shdr(std::vector<std::uint8_t>& b, Elf64_Shdr const& s)
        {
            w32(b, s.sh_name);
            w32(b, s.sh_type);
            w64(b, s.sh_flags);
            w64(b, s.sh_addr);
            w64(b, s.sh_offset);
            w64(b, s.sh_size);
            w32(b, s.sh_link);
            w32(b, s.sh_info);
            w64(b, s.sh_addralign);
            w64(b, s.sh_entsize);
        }

        void serialize_sym(std::vector<std::uint8_t>& b, Elf64_Sym const& s)
        {
            w32(b, s.st_name);
            w8(b, s.st_info);
            w8(b, s.st_other);
            w16(b, s.st_shndx);
            w64(b, s.st_value);
            w64(b, s.st_size);
        }

        [[nodiscard]] std::uint32_t elf_reloc_type(Reloc::Kind kind)
        {
            switch (kind)
            {
                case Reloc::Kind::Rel32:
                    return R_X86_64_PC32;
                case Reloc::Kind::Rel32_Got:
                    return R_X86_64_REX_GOTPCRELX;
                case Reloc::Kind::Rel32_Call:
                    return R_X86_64_PLT32;
                case Reloc::Kind::Abs64:
                    return R_X86_64_64;
            }
            return R_X86_64_64;
        }

        struct ElfCustomSection
        {
            std::string name;
            std::uint32_t type{};
            std::uint64_t flags{};
            std::uint64_t alignment{1};
            std::vector<GlobalLayout*> globals;
            std::vector<std::uint8_t> data;
            std::vector<Elf64_Rela> relas;
            std::uint64_t bss_size{};
            std::uint32_t section_index{};
            std::uint32_t rela_section_index{};
            std::uint32_t sh_name{};
            std::uint32_t rela_sh_name{};
            std::uint64_t file_offset{};
            std::uint64_t rela_file_offset{};
        };

        [[nodiscard]] std::pair<std::uint32_t, std::uint64_t> elf_section_traits(DataSection section)
        {
            switch (section)
            {
                case DataSection::Rodata:
                    return {SHT_PROGBITS, SHF_ALLOC};
                case DataSection::RodataRelRO:
                case DataSection::Data:
                    return {SHT_PROGBITS, SHF_ALLOC | SHF_WRITE};
                case DataSection::Bss:
                    return {SHT_NOBITS, SHF_ALLOC | SHF_WRITE};
                case DataSection::None:
                    break;
            }
            return {0, 0};
        }

        void serialize_init_value(std::vector<std::uint8_t>& data, ir::IrValue const* val, ir::IrType const* expected_type, std::vector<Elf64_Rela>& relas,
                                  std::unordered_map<std::string, std::uint32_t>& sym_name_to_idx, std::uint64_t base_offset)
        {
            auto size = init_type_size(expected_type);
            if (size == 0 && val && val->type)
                size = init_type_size(val->type);
            std::vector<std::uint8_t> image(size, 0);
            std::vector<InitReloc> init_relocs;
            serialize_init_memory(image, init_relocs, val, expected_type, 0);
            if (data.size() < base_offset)
                data.resize(base_offset, 0);
            data.resize(base_offset + size, 0);
            std::ranges::copy(image, data.begin() + static_cast<std::ptrdiff_t>(base_offset));
            for (auto const& init_reloc : init_relocs)
            {
                auto it = sym_name_to_idx.find(init_reloc.name);
                if (it == sym_name_to_idx.end())
                    continue;
                Elf64_Rela rela{};
                rela.r_offset = base_offset + init_reloc.offset;
                rela.r_addend = init_reloc.addend;
                rela.r_info = elf_r_info(it->second, R_X86_64_64);
                relas.push_back(rela);
            }
        }

        void serialize_custom_section_global(ElfCustomSection& section, GlobalLayout& gl, std::unordered_map<std::string, std::uint32_t>& sym_name_to_idx)
        {
            if (section.type == SHT_NOBITS)
            {
                auto pad = align_up(section.bss_size, gl.alignment);
                gl.offset = pad;
                section.bss_size = pad + (gl.g->type ? gl.g->type->byte_size : 0);
                return;
            }

            auto pad = align_up(section.data.size(), gl.alignment);
            while (section.data.size() < pad)
                section.data.push_back(0);

            gl.offset = section.data.size();
            if (gl.g->init)
                serialize_init_value(section.data, gl.g->init, gl.g->type, section.relas, sym_name_to_idx, section.data.size());
            else
                section.data.resize(section.data.size() + (gl.g->type ? gl.g->type->byte_size : 0), 0);
        }

    }
}

export namespace dcc::backend::em64t
{
    [[nodiscard]] std::vector<std::uint8_t> write_elf64(ir::IrModule const& ir_mod, MModule const& mod, std::vector<EncodeResult> const& encoded,
                                                        target::TargetConfig const& target)
    {
        (void)target;

        std::vector<std::string> func_names;
        func_names.reserve(mod.functions.size());
        std::vector<ir::Linkage> func_linkages;
        func_linkages.reserve(mod.functions.size());
        for (auto const& mf : mod.functions)
        {
            if (!mf.owned_name.empty())
                func_names.push_back(mf.owned_name);
            else
                func_names.push_back("<unnamed>");
            func_linkages.push_back(mf.linkage);
        }

        std::unordered_set<std::string> defined_names;
        for (auto const& fn : func_names)
            defined_names.insert(fn);

        for (auto const& mf : mod.functions)
            for (auto const& jt : mf.jump_tables)
                defined_names.insert(jt.symbol);

        for (auto* g : ir_mod.globals)
        {
            if (!g)
                continue;
            if (classify_global(g) != DataSection::None)
                defined_names.insert(std::string{g->name});
        }

        std::unordered_set<std::string> ext_sym_set;
        for (auto const& er : encoded)
            for (auto const& r : er.relocs)
                if (!defined_names.contains(std::string{r.symbol}))
                    ext_sym_set.insert(std::string{r.symbol});

        std::vector<std::string> ext_syms(ext_sym_set.begin(), ext_sym_set.end());

        std::vector<std::vector<std::uint8_t>> func_codes;
        std::vector<std::vector<Reloc>> func_relocs;
        func_codes.reserve(encoded.size());
        func_relocs.reserve(encoded.size());
        for (auto const& er : encoded)
        {
            func_codes.push_back(er.bytes);
            func_relocs.push_back(er.relocs);
        }

        std::vector<std::uint8_t> out;
        std::string shstrtab;
        auto add_str = [&](std::string& tab, std::string_view s) -> std::uint32_t {
            auto off = static_cast<std::uint32_t>(tab.size());
            tab += s;
            tab += '\0';
            return off;
        };

        std::string strtab;
        add_str(strtab, "");

        add_str(shstrtab, "");
        std::uint32_t sh_name_text = add_str(shstrtab, ".text");
        std::uint32_t sh_name_rela_text = add_str(shstrtab, ".rela.text");
        std::uint32_t sh_name_rodata = add_str(shstrtab, ".rodata");
        std::uint32_t sh_name_rela_rodata = add_str(shstrtab, ".rela.rodata");
        std::uint32_t sh_name_data_rel_ro = add_str(shstrtab, ".data.rel.ro");
        std::uint32_t sh_name_rela_data_rel_ro = add_str(shstrtab, ".rela.data.rel.ro");
        std::uint32_t sh_name_data = add_str(shstrtab, ".data");
        std::uint32_t sh_name_rela_data = add_str(shstrtab, ".rela.data");
        std::uint32_t sh_name_bss = add_str(shstrtab, ".bss");
        std::uint32_t sh_name_symtab = add_str(shstrtab, ".symtab");
        std::uint32_t sh_name_strtab = add_str(shstrtab, ".strtab");
        std::uint32_t sh_name_shstr = add_str(shstrtab, ".shstrtab");

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

        std::vector<GlobalLayout*> rodata_globals, rodata_relro_globals, data_globals, bss_globals;
        std::vector<ElfCustomSection> custom_sections;
        std::unordered_map<std::string, std::size_t> custom_section_index;
        for (auto& gl : globals)
        {
            if (!gl.g->section.empty())
            {
                std::string sname{gl.g->section};
                auto it = custom_section_index.find(sname);
                if (it == custom_section_index.end())
                {
                    ElfCustomSection cs;
                    cs.name = sname;
                    cs.type = SHT_NOBITS;
                    custom_section_index[sname] = custom_sections.size();
                    custom_sections.push_back(std::move(cs));
                }
                auto& cs = custom_sections[custom_section_index[sname]];
                auto [ty, fl] = elf_section_traits(gl.sec);
                if (ty == SHT_PROGBITS)
                    cs.type = SHT_PROGBITS;
                cs.flags |= fl;
                cs.alignment = std::max(cs.alignment, gl.alignment);
                cs.globals.push_back(&gl);
                continue;
            }
            if (gl.sec == DataSection::Rodata)
                rodata_globals.push_back(&gl);
            else if (gl.sec == DataSection::RodataRelRO)
                rodata_relro_globals.push_back(&gl);
            else if (gl.sec == DataSection::Data)
                data_globals.push_back(&gl);
            else if (gl.sec == DataSection::Bss)
                bss_globals.push_back(&gl);
        }

        std::uint64_t max_rodata_align = max_global_alignment(rodata_globals);
        std::uint64_t max_rodata_relro_align = max_global_alignment(rodata_relro_globals);
        std::uint64_t max_data_align = max_global_alignment(data_globals);
        std::uint64_t max_bss_align = max_global_alignment(bss_globals);

        for (auto& cs : custom_sections)
        {
            cs.sh_name = add_str(shstrtab, cs.name);
            cs.rela_sh_name = add_str(shstrtab, ".rela" + cs.name);
        }

        std::vector<std::uint8_t> rodata_data;
        std::vector<Elf64_Rela> rodata_relas;
        std::unordered_map<std::string, std::uint32_t> empty_sym_map;
        for (auto* glp : rodata_globals)
        {
            auto pad = align_up(rodata_data.size(), glp->alignment);
            while (rodata_data.size() < pad)
                rodata_data.push_back(0);

            glp->offset = rodata_data.size();
            if (glp->g->init)
                serialize_init_value(rodata_data, glp->g->init, glp->g->type, rodata_relas, empty_sym_map, rodata_data.size());
        }

        for (auto const& mf : mod.functions)
        {
            for (auto const& jt : mf.jump_tables)
            {
                while (rodata_data.size() % 4 != 0)
                    rodata_data.push_back(0);

                for (std::size_t ei = 0; ei < jt.targets.size(); ++ei)
                {
                    for (int k = 0; k < 4; ++k)
                        rodata_data.push_back(0);
                }
            }
        }

        std::vector<std::uint8_t> data_rel_ro_data;
        std::vector<Elf64_Rela> data_rel_ro_relas;
        for (auto* glp : rodata_relro_globals)
        {
            auto pad = align_up(data_rel_ro_data.size(), glp->alignment);
            while (data_rel_ro_data.size() < pad)
                data_rel_ro_data.push_back(0);

            glp->offset = data_rel_ro_data.size();
            if (glp->g->init)
                serialize_init_value(data_rel_ro_data, glp->g->init, glp->g->type, data_rel_ro_relas, empty_sym_map, data_rel_ro_data.size());
        }

        std::vector<std::uint8_t> data_data;
        std::vector<Elf64_Rela> data_relas;
        for (auto* glp : data_globals)
        {
            auto pad = align_up(data_data.size(), glp->alignment);
            while (data_data.size() < pad)
                data_data.push_back(0);

            glp->offset = data_data.size();
            if (glp->g->init)
            {
                serialize_init_value(data_data, glp->g->init, glp->g->type, data_relas, empty_sym_map, data_data.size());
            }
        }

        std::uint64_t bss_size = 0;
        for (auto* glp : bss_globals)
        {
            auto pad = align_up(bss_size, glp->alignment);
            glp->offset = pad;
            bss_size = pad + (glp->g->type ? glp->g->type->byte_size : 0);
        }

        for (auto& cs : custom_sections)
            for (auto* glp : cs.globals)
                serialize_custom_section_global(cs, *glp, empty_sym_map);

        struct BlockSymInfo
        {
            std::uint32_t func_index;
            std::uint32_t block_id;
            std::string sym_name;
            std::uint32_t func_offset;
            std::uint32_t block_offset_within_func;
        };
        std::vector<BlockSymInfo> block_syms;
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

                    block_syms.push_back(BlockSymInfo{
                        .func_index = static_cast<std::uint32_t>(fi),
                        .block_id = tgt,
                        .sym_name = sym_name,
                        .func_offset = 0,
                        .block_offset_within_func = it->second,
                    });
                }
            }
        }

        std::vector<std::uint64_t> func_offsets;
        func_offsets.reserve(func_codes.size());
        {
            std::uint64_t cur = 0;
            for (std::size_t i = 0; i < func_codes.size(); ++i)
            {
                func_offsets.push_back(cur);
                cur += func_codes[i].size();
                cur = align_up(cur, 16);
            }
        }

        for (auto& bs : block_syms)
            bs.func_offset = static_cast<std::uint32_t>(func_offsets[bs.func_index]);

        std::vector<std::uint8_t> text_data;
        for (std::size_t i = 0; i < func_codes.size(); ++i)
        {
            while (text_data.size() < func_offsets[i])
                text_data.push_back(0x90);

            auto const& code = func_codes[i];
            text_data.insert(text_data.end(), code.begin(), code.end());
            while (text_data.size() % 16 != 0)
                text_data.push_back(0x90);
        }

        std::vector<Elf64_Rela> text_relas;
        for (std::size_t fi = 0; fi < func_relocs.size(); ++fi)
        {
            auto func_off = func_offsets[fi];
            for (auto const& r : func_relocs[fi])
            {
                Elf64_Rela rela{};
                rela.r_offset = func_off + r.offset;
                rela.r_addend = r.addend;
                text_relas.push_back(rela);
            }
        }

        std::unordered_set<std::string> data_ref_names;
        for (auto* glp : rodata_globals)
            if (glp->g->init)
                collect_ref_names(glp->g->init, data_ref_names);
        for (auto* glp : rodata_relro_globals)
            if (glp->g->init)
                collect_ref_names(glp->g->init, data_ref_names);
        for (auto* glp : data_globals)
            if (glp->g->init)
                collect_ref_names(glp->g->init, data_ref_names);
        for (auto& cs : custom_sections)
            for (auto* glp : cs.globals)
                if (glp->g->init)
                    collect_ref_names(glp->g->init, data_ref_names);

        for (auto const& n : data_ref_names)
            if (!defined_names.contains(n))
                ext_sym_set.insert(n);

        ext_syms.assign(ext_sym_set.begin(), ext_sym_set.end());
        std::ranges::sort(ext_syms);

        std::vector<std::string> undef_globals;
        for (auto* g : ir_mod.globals)
        {
            if (!g)
                continue;
            if (g->is_declaration)
            {
                auto ns = std::string{g->name};
                if (!defined_names.contains(ns) && std::ranges::find(ext_syms, ns) == ext_syms.end())
                    undef_globals.push_back(ns);
            }
        }
        ext_syms.insert(ext_syms.end(), undef_globals.begin(), undef_globals.end());

        std::vector<Elf64_Sym> syms;
        std::unordered_map<std::string, std::uint32_t> name_to_sym_idx;

        syms.push_back({});

        bool has_jump_tables = false;
        for (auto const& mf : mod.functions)
            if (!mf.jump_tables.empty())
            {
                has_jump_tables = true;
                break;
            }

        bool has_rodata = !rodata_globals.empty() || has_jump_tables;
        bool has_rodata_relro = !rodata_relro_globals.empty();
        bool has_data = !data_globals.empty();
        bool has_bss = !bss_globals.empty();
        auto section_has_refs = [](std::vector<GlobalLayout*> const& gs) {
            for (auto* glp : gs)
                if (glp->g->init && has_global_ref(glp->g->init))
                    return true;
            return false;
        };
        bool has_rodata_rela = has_rodata && (has_jump_tables || section_has_refs(rodata_globals));
        bool has_rodata_relro_rela = has_rodata_relro && section_has_refs(rodata_relro_globals);
        bool has_data_rela = has_data && section_has_refs(data_globals);
        bool has_text_rela = !text_relas.empty();

        std::uint32_t sec_text = 1;
        std::uint32_t sec_rela_text = 2;
        std::uint32_t sec_rodata = 3;
        std::uint32_t sec_rela_rodata = 4;
        std::uint32_t sec_data_rel_ro = 5;
        std::uint32_t sec_rela_data_rel_ro = 6;
        std::uint32_t sec_data = 7;
        std::uint32_t sec_rela_data = 8;
        std::uint32_t sec_bss = 9;

        std::uint32_t next_sec = 2;
        if (has_text_rela)
            next_sec++;
        sec_rodata = next_sec;
        if (has_rodata)
            next_sec++;
        sec_rela_rodata = next_sec;
        if (has_rodata_rela)
            next_sec++;
        sec_data_rel_ro = next_sec;
        if (has_rodata_relro)
            next_sec++;
        sec_rela_data_rel_ro = next_sec;
        if (has_rodata_relro_rela)
            next_sec++;
        sec_data = next_sec;
        if (has_data)
            next_sec++;
        sec_rela_data = next_sec;
        if (has_data_rela)
            next_sec++;
        sec_bss = next_sec;
        if (has_bss)
            next_sec++;

        for (auto& cs : custom_sections)
        {
            cs.section_index = next_sec++;
            if (section_has_refs(cs.globals))
                cs.rela_section_index = next_sec++;
        }

        std::uint32_t sec_symtab = next_sec++;
        std::uint32_t sec_strtab = next_sec++;
        std::uint32_t sec_shstrtab = next_sec++;
        std::uint32_t total_sec = next_sec;

        for (auto& gl : globals)
        {
            if (!gl.g->section.empty())
                continue;
            if (gl.sec == DataSection::Rodata)
                gl.section_index = sec_rodata;
            else if (gl.sec == DataSection::RodataRelRO)
                gl.section_index = sec_data_rel_ro;
            else if (gl.sec == DataSection::Data)
                gl.section_index = sec_data;
            else if (gl.sec == DataSection::Bss)
                gl.section_index = sec_bss;
        }

        for (auto& cs : custom_sections)
            for (auto* glp : cs.globals)
                glp->section_index = cs.section_index;

        for (auto* glp : rodata_globals)
        {
            if (glp->g->linkage == ir::Linkage::Internal)
            {
                Elf64_Sym s{};
                s.st_name = add_str(strtab, glp->name_str);
                s.st_info = elf_st_info(STB_LOCAL, STT_OBJECT);
                s.st_shndx = static_cast<std::uint16_t>(glp->section_index);
                s.st_value = glp->offset;
                s.st_size = glp->g->type ? glp->g->type->byte_size : 0;
                name_to_sym_idx[std::string{glp->g->name}] = static_cast<std::uint32_t>(syms.size());
                syms.push_back(s);
            }
        }
        for (auto* glp : rodata_relro_globals)
        {
            if (glp->g->linkage == ir::Linkage::Internal)
            {
                Elf64_Sym s{};
                s.st_name = add_str(strtab, glp->name_str);
                s.st_info = elf_st_info(STB_LOCAL, STT_OBJECT);
                s.st_shndx = static_cast<std::uint16_t>(glp->section_index);
                s.st_value = glp->offset;
                s.st_size = glp->g->type ? glp->g->type->byte_size : 0;
                name_to_sym_idx[std::string{glp->g->name}] = static_cast<std::uint32_t>(syms.size());
                syms.push_back(s);
            }
        }
        for (auto* glp : data_globals)
        {
            if (glp->g->linkage == ir::Linkage::Internal)
            {
                Elf64_Sym s{};
                s.st_name = add_str(strtab, glp->name_str);
                s.st_info = elf_st_info(STB_LOCAL, STT_OBJECT);
                s.st_shndx = static_cast<std::uint16_t>(glp->section_index);
                s.st_value = glp->offset;
                s.st_size = glp->g->type ? glp->g->type->byte_size : 0;
                name_to_sym_idx[std::string{glp->g->name}] = static_cast<std::uint32_t>(syms.size());
                syms.push_back(s);
            }
        }
        for (auto* glp : bss_globals)
        {
            if (glp->g->linkage == ir::Linkage::Internal)
            {
                Elf64_Sym s{};
                s.st_name = add_str(strtab, glp->name_str);
                s.st_info = elf_st_info(STB_LOCAL, STT_OBJECT);
                s.st_shndx = static_cast<std::uint16_t>(glp->section_index);
                s.st_value = glp->offset;
                s.st_size = glp->g->type ? glp->g->type->byte_size : 0;
                name_to_sym_idx[std::string{glp->g->name}] = static_cast<std::uint32_t>(syms.size());
                syms.push_back(s);
            }
        }
        for (auto& cs : custom_sections)
        {
            for (auto* glp : cs.globals)
            {
                if (glp->g->linkage == ir::Linkage::Internal)
                {
                    Elf64_Sym s{};
                    s.st_name = add_str(strtab, glp->name_str);
                    s.st_info = elf_st_info(STB_LOCAL, STT_OBJECT);
                    s.st_shndx = static_cast<std::uint16_t>(glp->section_index);
                    s.st_value = glp->offset;
                    s.st_size = glp->g->type ? glp->g->type->byte_size : 0;
                    name_to_sym_idx[std::string{glp->g->name}] = static_cast<std::uint32_t>(syms.size());
                    syms.push_back(s);
                }
            }
        }

        for (auto const& mf : mod.functions)
        {
            for (auto const& jt : mf.jump_tables)
            {
                if (!jt.symbol.empty() && !name_to_sym_idx.contains(jt.symbol))
                {
                    Elf64_Sym s{};
                    s.st_name = add_str(strtab, jt.symbol);
                    s.st_info = elf_st_info(STB_LOCAL, STT_OBJECT);
                    s.st_shndx = static_cast<std::uint16_t>(sec_rodata);
                    s.st_value = 0;
                    s.st_size = static_cast<std::uint64_t>(jt.targets.size()) * 4;
                    name_to_sym_idx[jt.symbol] = static_cast<std::uint32_t>(syms.size());
                    syms.push_back(s);
                }
            }
        }

        for (auto const& bs : block_syms)
        {
            if (name_to_sym_idx.contains(bs.sym_name))
                continue;
            Elf64_Sym s{};
            s.st_name = add_str(strtab, bs.sym_name);
            s.st_info = elf_st_info(STB_LOCAL, STT_NOTYPE);
            s.st_shndx = static_cast<std::uint16_t>(sec_text);
            s.st_value = static_cast<std::uint64_t>(bs.func_offset) + bs.block_offset_within_func;
            s.st_size = 0;
            name_to_sym_idx[bs.sym_name] = static_cast<std::uint32_t>(syms.size());
            syms.push_back(s);
        }

        for (auto const& ext : ext_syms)
        {
            if (name_to_sym_idx.contains(ext))
                continue;
            Elf64_Sym s{};
            s.st_name = add_str(strtab, ext);
            s.st_info = elf_st_info(STB_GLOBAL, STT_NOTYPE);
            s.st_shndx = 0;
            name_to_sym_idx[ext] = static_cast<std::uint32_t>(syms.size());
            syms.push_back(s);
        }

        for (std::size_t i = 0; i < func_names.size(); ++i)
        {
            auto const& fn = func_names[i];
            if (name_to_sym_idx.contains(fn))
                continue;
            std::uint8_t bind = STB_GLOBAL;
            if (i < func_linkages.size() &&
                (func_linkages[i] == ir::Linkage::LinkOnceODR || func_linkages[i] == ir::Linkage::WeakODR))
                bind = STB_WEAK;
            Elf64_Sym s{};
            s.st_name = add_str(strtab, fn);
            s.st_info = elf_st_info(bind, STT_FUNC);
            s.st_shndx = static_cast<std::uint16_t>(sec_text);
            s.st_value = func_offsets[i];
            s.st_size = func_codes[i].size();
            name_to_sym_idx[fn] = static_cast<std::uint32_t>(syms.size());
            syms.push_back(s);
        }

        for (auto* glp : rodata_globals)
        {
            if (glp->g->linkage == ir::Linkage::External)
            {
                if (name_to_sym_idx.contains(std::string{glp->g->name}))
                    continue;
                Elf64_Sym s{};
                s.st_name = add_str(strtab, glp->name_str);
                s.st_info = elf_st_info(STB_GLOBAL, STT_OBJECT);
                s.st_shndx = static_cast<std::uint16_t>(glp->section_index);
                s.st_value = glp->offset;
                s.st_size = glp->g->type ? glp->g->type->byte_size : 0;
                name_to_sym_idx[std::string{glp->g->name}] = static_cast<std::uint32_t>(syms.size());
                syms.push_back(s);
            }
        }
        for (auto* glp : rodata_relro_globals)
        {
            if (glp->g->linkage == ir::Linkage::External)
            {
                if (name_to_sym_idx.contains(std::string{glp->g->name}))
                    continue;
                Elf64_Sym s{};
                s.st_name = add_str(strtab, glp->name_str);
                s.st_info = elf_st_info(STB_GLOBAL, STT_OBJECT);
                s.st_shndx = static_cast<std::uint16_t>(glp->section_index);
                s.st_value = glp->offset;
                s.st_size = glp->g->type ? glp->g->type->byte_size : 0;
                name_to_sym_idx[std::string{glp->g->name}] = static_cast<std::uint32_t>(syms.size());
                syms.push_back(s);
            }
        }
        for (auto* glp : data_globals)
        {
            if (glp->g->linkage == ir::Linkage::External)
            {
                if (name_to_sym_idx.contains(std::string{glp->g->name}))
                    continue;
                Elf64_Sym s{};
                s.st_name = add_str(strtab, glp->name_str);
                s.st_info = elf_st_info(STB_GLOBAL, STT_OBJECT);
                s.st_shndx = static_cast<std::uint16_t>(glp->section_index);
                s.st_value = glp->offset;
                s.st_size = glp->g->type ? glp->g->type->byte_size : 0;
                name_to_sym_idx[std::string{glp->g->name}] = static_cast<std::uint32_t>(syms.size());
                syms.push_back(s);
            }
        }
        for (auto* glp : bss_globals)
        {
            if (glp->g->linkage == ir::Linkage::External)
            {
                if (name_to_sym_idx.contains(std::string{glp->g->name}))
                    continue;
                Elf64_Sym s{};
                s.st_name = add_str(strtab, glp->name_str);
                s.st_info = elf_st_info(STB_GLOBAL, STT_OBJECT);
                s.st_shndx = static_cast<std::uint16_t>(glp->section_index);
                s.st_value = glp->offset;
                s.st_size = glp->g->type ? glp->g->type->byte_size : 0;
                name_to_sym_idx[std::string{glp->g->name}] = static_cast<std::uint32_t>(syms.size());
                syms.push_back(s);
            }
        }
        for (auto& cs : custom_sections)
        {
            for (auto* glp : cs.globals)
            {
                if (glp->g->linkage == ir::Linkage::External)
                {
                    if (name_to_sym_idx.contains(std::string{glp->g->name}))
                        continue;
                    Elf64_Sym s{};
                    s.st_name = add_str(strtab, glp->name_str);
                    s.st_info = elf_st_info(STB_GLOBAL, STT_OBJECT);
                    s.st_shndx = static_cast<std::uint16_t>(glp->section_index);
                    s.st_value = glp->offset;
                    s.st_size = glp->g->type ? glp->g->type->byte_size : 0;
                    name_to_sym_idx[std::string{glp->g->name}] = static_cast<std::uint32_t>(syms.size());
                    syms.push_back(s);
                }
            }
        }

        rodata_data.clear();
        rodata_relas.clear();
        for (auto* glp : rodata_globals)
        {
            auto pad = align_up(rodata_data.size(), glp->alignment);
            while (rodata_data.size() < pad)
                rodata_data.push_back(0);

            glp->offset = rodata_data.size();
            if (glp->g->init)
                serialize_init_value(rodata_data, glp->g->init, glp->g->type, rodata_relas, name_to_sym_idx, rodata_data.size());
        }

        for (std::size_t fi = 0; fi < mod.functions.size(); ++fi)
        {
            auto const& mf = mod.functions[fi];
            for (auto const& jt : mf.jump_tables)
            {
                while (rodata_data.size() % 4 != 0)
                    rodata_data.push_back(0);

                auto jt_sym_it = name_to_sym_idx.find(jt.symbol);
                std::uint64_t jt_offset = rodata_data.size();

                for (std::size_t ei = 0; ei < jt.targets.size(); ++ei)
                {
                    std::uint32_t tgt_block = jt.targets[ei];
                    std::string blk_sym = mf.owned_name.empty() ? "anon" : mf.owned_name;
                    blk_sym += ".bb" + std::to_string(tgt_block);

                    auto blk_sym_it = name_to_sym_idx.find(blk_sym);
                    std::uint64_t entry_offset = rodata_data.size();
                    for (int k = 0; k < 4; ++k)
                        rodata_data.push_back(0);

                    if (blk_sym_it != name_to_sym_idx.end())
                    {
                        Elf64_Rela rela{};
                        rela.r_offset = entry_offset;
                        rela.r_addend = -4;
                        rela.r_info = elf_r_info(blk_sym_it->second, R_X86_64_PC32);
                        rodata_relas.push_back(rela);
                    }
                }

                if (jt_sym_it != name_to_sym_idx.end())
                    syms[jt_sym_it->second].st_value = jt_offset;
            }
        }

        data_rel_ro_data.clear();
        data_rel_ro_relas.clear();
        for (auto* glp : rodata_relro_globals)
        {
            auto pad = align_up(data_rel_ro_data.size(), glp->alignment);
            while (data_rel_ro_data.size() < pad)
                data_rel_ro_data.push_back(0);

            glp->offset = data_rel_ro_data.size();
            if (glp->g->init)
                serialize_init_value(data_rel_ro_data, glp->g->init, glp->g->type, data_rel_ro_relas, name_to_sym_idx, data_rel_ro_data.size());
        }

        data_data.clear();
        data_relas.clear();
        for (auto* glp : data_globals)
        {
            auto pad = align_up(data_data.size(), glp->alignment);
            while (data_data.size() < pad)
                data_data.push_back(0);

            glp->offset = data_data.size();
            if (glp->g->init)
            {
                serialize_init_value(data_data, glp->g->init, glp->g->type, data_relas, name_to_sym_idx, data_data.size());
            }
        }

        for (auto& cs : custom_sections)
        {
            cs.data.clear();
            cs.relas.clear();
            cs.bss_size = 0;
            for (auto* glp : cs.globals)
                serialize_custom_section_global(cs, *glp, name_to_sym_idx);
        }

        std::vector<Elf64_Rela> final_text_relas;
        for (std::size_t fi = 0; fi < func_relocs.size(); ++fi)
        {
            auto func_off = func_offsets[fi];
            for (auto const& r : func_relocs[fi])
            {
                Elf64_Rela rela{};
                rela.r_offset = func_off + r.offset;
                rela.r_addend = r.addend;

                auto it = name_to_sym_idx.find(std::string{r.symbol});
                if (it == name_to_sym_idx.end())
                    continue;

                std::uint32_t rtype = elf_reloc_type(r.kind);
                rela.r_info = elf_r_info(it->second, rtype);
                final_text_relas.push_back(rela);
            }
        }

        std::uint64_t shoff = 64;
        std::uint64_t sec_hdr_size = static_cast<std::uint64_t>(total_sec) * sizeof(Elf64_Shdr);
        std::uint64_t cur_offset = shoff + sec_hdr_size;

        auto text_off = cur_offset;
        auto text_size = text_data.size();
        cur_offset = align_up(text_off + text_size, 16);

        auto rela_text_off = cur_offset;
        auto rela_text_size = final_text_relas.size() * sizeof(Elf64_Rela);
        cur_offset = rela_text_off + rela_text_size;

        auto rodata_off = cur_offset;
        auto rodata_size = rodata_data.size();
        cur_offset = rodata_off + rodata_size;

        auto rela_rodata_off = cur_offset;
        auto rela_rodata_size = rodata_relas.size() * sizeof(Elf64_Rela);
        cur_offset = rela_rodata_off + rela_rodata_size;

        auto data_rel_ro_off = cur_offset;
        auto data_rel_ro_size = data_rel_ro_data.size();
        cur_offset = data_rel_ro_off + data_rel_ro_size;

        auto rela_data_rel_ro_off = cur_offset;
        auto rela_data_rel_ro_size = data_rel_ro_relas.size() * sizeof(Elf64_Rela);
        cur_offset = rela_data_rel_ro_off + rela_data_rel_ro_size;

        auto data_off = cur_offset;
        auto data_size = data_data.size();
        cur_offset = data_off + data_size;

        auto rela_data_off = cur_offset;
        auto rela_data_size = data_relas.size() * sizeof(Elf64_Rela);
        cur_offset = rela_data_off + rela_data_size;

        for (auto& cs : custom_sections)
        {
            if (cs.type == SHT_NOBITS)
                continue;
            cs.file_offset = cur_offset;
            cur_offset += cs.data.size();
        }
        for (auto& cs : custom_sections)
        {
            if (cs.rela_section_index == 0)
                continue;
            cs.rela_file_offset = cur_offset;
            cur_offset += cs.relas.size() * sizeof(Elf64_Rela);
        }

        auto symtab_off = cur_offset;
        auto symtab_size = static_cast<std::uint64_t>(syms.size()) * sizeof(Elf64_Sym);
        cur_offset = symtab_off + symtab_size;

        auto strtab_off = cur_offset;
        auto strtab_size = strtab.size();
        cur_offset = strtab_off + strtab_size;

        auto shstrtab_off = cur_offset;
        auto shstrtab_size = shstrtab.size();

        std::vector<Elf64_Shdr> shdrs(total_sec);

        shdrs[0] = {};

        shdrs[sec_text] = {};
        shdrs[sec_text].sh_name = sh_name_text;
        shdrs[sec_text].sh_type = SHT_PROGBITS;
        shdrs[sec_text].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
        shdrs[sec_text].sh_offset = text_off;
        shdrs[sec_text].sh_size = text_size;
        shdrs[sec_text].sh_addralign = 16;

        if (has_text_rela)
        {
            shdrs[sec_rela_text] = {};
            shdrs[sec_rela_text].sh_name = sh_name_rela_text;
            shdrs[sec_rela_text].sh_type = SHT_RELA;
            shdrs[sec_rela_text].sh_offset = rela_text_off;
            shdrs[sec_rela_text].sh_size = rela_text_size;
            shdrs[sec_rela_text].sh_link = sec_symtab;
            shdrs[sec_rela_text].sh_info = sec_text;
            shdrs[sec_rela_text].sh_addralign = 8;
            shdrs[sec_rela_text].sh_entsize = sizeof(Elf64_Rela);
        }

        if (has_rodata)
        {
            shdrs[sec_rodata] = {};
            shdrs[sec_rodata].sh_name = sh_name_rodata;
            shdrs[sec_rodata].sh_type = SHT_PROGBITS;
            shdrs[sec_rodata].sh_flags = SHF_ALLOC;
            shdrs[sec_rodata].sh_offset = rodata_off;
            shdrs[sec_rodata].sh_size = rodata_size;
            shdrs[sec_rodata].sh_addralign = std::max<std::uint64_t>(8, max_rodata_align);
        }

        if (has_rodata_rela)
        {
            shdrs[sec_rela_rodata] = {};
            shdrs[sec_rela_rodata].sh_name = sh_name_rela_rodata;
            shdrs[sec_rela_rodata].sh_type = SHT_RELA;
            shdrs[sec_rela_rodata].sh_offset = rela_rodata_off;
            shdrs[sec_rela_rodata].sh_size = rela_rodata_size;
            shdrs[sec_rela_rodata].sh_link = sec_symtab;
            shdrs[sec_rela_rodata].sh_info = sec_rodata;
            shdrs[sec_rela_rodata].sh_addralign = 8;
            shdrs[sec_rela_rodata].sh_entsize = sizeof(Elf64_Rela);
        }

        if (has_rodata_relro)
        {
            shdrs[sec_data_rel_ro] = {};
            shdrs[sec_data_rel_ro].sh_name = sh_name_data_rel_ro;
            shdrs[sec_data_rel_ro].sh_type = SHT_PROGBITS;
            shdrs[sec_data_rel_ro].sh_flags = SHF_ALLOC | SHF_WRITE;
            shdrs[sec_data_rel_ro].sh_offset = data_rel_ro_off;
            shdrs[sec_data_rel_ro].sh_size = data_rel_ro_size;
            shdrs[sec_data_rel_ro].sh_addralign = std::max<std::uint64_t>(8, max_rodata_relro_align);
        }

        if (has_rodata_relro_rela)
        {
            shdrs[sec_rela_data_rel_ro] = {};
            shdrs[sec_rela_data_rel_ro].sh_name = sh_name_rela_data_rel_ro;
            shdrs[sec_rela_data_rel_ro].sh_type = SHT_RELA;
            shdrs[sec_rela_data_rel_ro].sh_offset = rela_data_rel_ro_off;
            shdrs[sec_rela_data_rel_ro].sh_size = rela_data_rel_ro_size;
            shdrs[sec_rela_data_rel_ro].sh_link = sec_symtab;
            shdrs[sec_rela_data_rel_ro].sh_info = sec_data_rel_ro;
            shdrs[sec_rela_data_rel_ro].sh_addralign = 8;
            shdrs[sec_rela_data_rel_ro].sh_entsize = sizeof(Elf64_Rela);
        }

        if (has_data)
        {
            shdrs[sec_data] = {};
            shdrs[sec_data].sh_name = sh_name_data;
            shdrs[sec_data].sh_type = SHT_PROGBITS;
            shdrs[sec_data].sh_flags = SHF_ALLOC | SHF_WRITE;
            shdrs[sec_data].sh_offset = data_off;
            shdrs[sec_data].sh_size = data_size;
            shdrs[sec_data].sh_addralign = std::max<std::uint64_t>(8, max_data_align);
        }

        if (has_data_rela)
        {
            shdrs[sec_rela_data] = {};
            shdrs[sec_rela_data].sh_name = sh_name_rela_data;
            shdrs[sec_rela_data].sh_type = SHT_RELA;
            shdrs[sec_rela_data].sh_offset = rela_data_off;
            shdrs[sec_rela_data].sh_size = rela_data_size;
            shdrs[sec_rela_data].sh_link = sec_symtab;
            shdrs[sec_rela_data].sh_info = sec_data;
            shdrs[sec_rela_data].sh_addralign = 8;
            shdrs[sec_rela_data].sh_entsize = sizeof(Elf64_Rela);
        }

        if (has_bss)
        {
            shdrs[sec_bss] = {};
            shdrs[sec_bss].sh_name = sh_name_bss;
            shdrs[sec_bss].sh_type = SHT_NOBITS;
            shdrs[sec_bss].sh_flags = SHF_ALLOC | SHF_WRITE;
            shdrs[sec_bss].sh_offset = 0;
            shdrs[sec_bss].sh_size = bss_size;
            shdrs[sec_bss].sh_addralign = std::max<std::uint64_t>(8, max_bss_align);
        }

        for (auto& cs : custom_sections)
        {
            shdrs[cs.section_index] = {};
            shdrs[cs.section_index].sh_name = cs.sh_name;
            shdrs[cs.section_index].sh_type = cs.type;
            shdrs[cs.section_index].sh_flags = cs.flags;
            shdrs[cs.section_index].sh_offset = cs.type == SHT_NOBITS ? 0 : cs.file_offset;
            shdrs[cs.section_index].sh_size = cs.type == SHT_NOBITS ? cs.bss_size : cs.data.size();
            shdrs[cs.section_index].sh_addralign = cs.alignment;
            if (cs.rela_section_index != 0)
            {
                shdrs[cs.rela_section_index] = {};
                shdrs[cs.rela_section_index].sh_name = cs.rela_sh_name;
                shdrs[cs.rela_section_index].sh_type = SHT_RELA;
                shdrs[cs.rela_section_index].sh_offset = cs.rela_file_offset;
                shdrs[cs.rela_section_index].sh_size = cs.relas.size() * sizeof(Elf64_Rela);
                shdrs[cs.rela_section_index].sh_link = sec_symtab;
                shdrs[cs.rela_section_index].sh_info = cs.section_index;
                shdrs[cs.rela_section_index].sh_addralign = 8;
                shdrs[cs.rela_section_index].sh_entsize = sizeof(Elf64_Rela);
            }
        }

        shdrs[sec_symtab] = {};
        shdrs[sec_symtab].sh_name = sh_name_symtab;
        shdrs[sec_symtab].sh_type = SHT_SYMTAB;
        shdrs[sec_symtab].sh_offset = symtab_off;
        shdrs[sec_symtab].sh_size = symtab_size;
        shdrs[sec_symtab].sh_link = sec_strtab;
        {
            std::uint32_t last_local = 0;
            for (std::size_t si = 0; si < syms.size(); ++si)
                if ((syms[si].st_info & 0xF0) == 0)
                    last_local = static_cast<std::uint32_t>(si);

            shdrs[sec_symtab].sh_info = last_local + 1;
        }
        shdrs[sec_symtab].sh_addralign = 8;
        shdrs[sec_symtab].sh_entsize = sizeof(Elf64_Sym);

        shdrs[sec_strtab] = {};
        shdrs[sec_strtab].sh_name = sh_name_strtab;
        shdrs[sec_strtab].sh_type = SHT_STRTAB;
        shdrs[sec_strtab].sh_offset = strtab_off;
        shdrs[sec_strtab].sh_size = strtab_size;
        shdrs[sec_strtab].sh_addralign = 1;

        shdrs[sec_shstrtab] = {};
        shdrs[sec_shstrtab].sh_name = sh_name_shstr;
        shdrs[sec_shstrtab].sh_type = SHT_STRTAB;
        shdrs[sec_shstrtab].sh_offset = shstrtab_off;
        shdrs[sec_shstrtab].sh_size = shstrtab_size;
        shdrs[sec_shstrtab].sh_addralign = 1;

        Elf64_Ehdr ehdr{};
        ehdr.e_ident = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
        ehdr.e_type = ET_REL;
        ehdr.e_machine = EM_X86_64;
        ehdr.e_version = EV_CURRENT;
        ehdr.e_shoff = shoff;
        ehdr.e_ehsize = 64;
        ehdr.e_shentsize = sizeof(Elf64_Shdr);
        ehdr.e_shnum = static_cast<std::uint16_t>(total_sec);
        ehdr.e_shstrndx = static_cast<std::uint16_t>(sec_shstrtab);
        serialize_ehdr(out, ehdr);

        for (auto const& sh : shdrs)
            serialize_shdr(out, sh);

        out.insert(out.end(), text_data.begin(), text_data.end());

        while (out.size() < rela_text_off)
            out.push_back(0);

        for (auto const& rel : final_text_relas)
        {
            w64(out, rel.r_offset);
            w64(out, rel.r_info);
            w64(out, static_cast<std::uint64_t>(rel.r_addend));
        }

        out.insert(out.end(), rodata_data.begin(), rodata_data.end());

        for (auto const& rel : rodata_relas)
        {
            w64(out, rel.r_offset);
            w64(out, rel.r_info);
            w64(out, static_cast<std::uint64_t>(rel.r_addend));
        }

        out.insert(out.end(), data_rel_ro_data.begin(), data_rel_ro_data.end());

        for (auto const& rel : data_rel_ro_relas)
        {
            w64(out, rel.r_offset);
            w64(out, rel.r_info);
            w64(out, static_cast<std::uint64_t>(rel.r_addend));
        }

        out.insert(out.end(), data_data.begin(), data_data.end());

        for (auto const& rel : data_relas)
        {
            w64(out, rel.r_offset);
            w64(out, rel.r_info);
            w64(out, static_cast<std::uint64_t>(rel.r_addend));
        }

        for (auto& cs : custom_sections)
        {
            if (cs.type == SHT_NOBITS)
                continue;
            while (out.size() < cs.file_offset)
                out.push_back(0);
            out.insert(out.end(), cs.data.begin(), cs.data.end());
        }
        for (auto& cs : custom_sections)
        {
            if (cs.rela_section_index == 0)
                continue;
            while (out.size() < cs.rela_file_offset)
                out.push_back(0);
            for (auto const& rel : cs.relas)
            {
                w64(out, rel.r_offset);
                w64(out, rel.r_info);
                w64(out, static_cast<std::uint64_t>(rel.r_addend));
            }
        }

        for (auto const& sym : syms)
            serialize_sym(out, sym);

        out.insert(out.end(), strtab.begin(), strtab.end());

        out.insert(out.end(), shstrtab.begin(), shstrtab.end());

        return out;
    }

}
