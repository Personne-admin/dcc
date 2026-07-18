export module dcc.backend.em64t.objwriter;

import std;
import dcc.ir;
import dcc.backend.em64t.mir;
import dcc.backend.em64t.encode;
import dcc.target;

#define into_u8 static_cast<std::uint8_t>

namespace dcc::backend::em64t
{
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
        constexpr std::uint32_t STT_NOTYPE = 0;
        constexpr std::uint32_t STT_OBJECT = 1;
        constexpr std::uint32_t STT_FUNC = 2;

        constexpr std::uint32_t R_X86_64_64 = 1;
        constexpr std::uint32_t R_X86_64_PC32 = 2;
        constexpr std::uint32_t R_X86_64_PLT32 = 4;
        constexpr std::uint32_t R_X86_64_REX_GOTPCRELX = 42;

        void w8(std::vector<std::uint8_t>& b, std::uint8_t v)
        {
            b.push_back(v);
        }
        void w16(std::vector<std::uint8_t>& b, std::uint16_t v)
        {
            b.push_back(std::uint8_t(v));
            b.push_back(std::uint8_t(v >> 8));
        }
        void w32(std::vector<std::uint8_t>& b, std::uint32_t v)
        {
            b.push_back(std::uint8_t(v));
            b.push_back(std::uint8_t(v >> 8));
            b.push_back(std::uint8_t(v >> 16));
            b.push_back(std::uint8_t(v >> 24));
        }
        void w64(std::vector<std::uint8_t>& b, std::uint64_t v)
        {
            w32(b, std::uint32_t(v));
            w32(b, std::uint32_t(v >> 32));
        }

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

        constexpr std::uint16_t IMAGE_FILE_MACHINE_AMD64 = 0x8664;
        constexpr std::uint16_t IMAGE_FILE_LINE_NUMS_STRIPPED = 0x0004;
        constexpr std::uint16_t IMAGE_FILE_DEBUG_STRIPPED = 0x0200;

        constexpr std::uint32_t IMAGE_SCN_CNT_CODE = 0x00000020;
        constexpr std::uint32_t IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040;
        constexpr std::uint32_t IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080;
        constexpr std::uint32_t IMAGE_SCN_MEM_EXECUTE = 0x20000000;
        constexpr std::uint32_t IMAGE_SCN_MEM_READ = 0x40000000;
        constexpr std::uint32_t IMAGE_SCN_MEM_WRITE = 0x80000000;
        constexpr std::uint32_t IMAGE_SCN_ALIGN_16BYTES = 0x00500000;

        constexpr std::uint16_t IMAGE_REL_AMD64_REL32 = 0x0004;
        constexpr std::uint16_t IMAGE_REL_AMD64_ADDR64 = 0x0001;

        enum class DataSection
        {
            None,
            Rodata,
            RodataRelRO,
            Data,
            Bss
        };

        struct GlobalLayout
        {
            ir::IrGlobal const* g{};
            DataSection sec{DataSection::None};
            std::uint64_t offset{};
            std::uint64_t alignment{};
            std::uint32_t section_index{};
            std::string name_str;
        };

        [[nodiscard]] std::uint64_t align_up(std::uint64_t val, std::uint64_t alignment)
        {
            return (val + alignment - 1) / alignment * alignment;
        }

        [[nodiscard]] bool has_global_ref(ir::IrValue const* val)
        {
            if (!val)
                return false;

            if (val->kind == ir::IrNodeKind::GlobalRef)
                return true;

            if (val->kind == ir::IrNodeKind::Aggregate)
            {
                auto* agg = static_cast<ir::IrAggregateInst const*>(val);
                for (auto* fv : agg->values)
                    if (has_global_ref(fv))
                        return true;
            }

            return false;
        }

        [[nodiscard]] DataSection classify_global(ir::IrGlobal const* g)
        {
            if (g->linkage == ir::Linkage::External && !g->init)
                return DataSection::None;
            if (g->is_constant && g->init)
            {
                if (has_global_ref(g->init))
                    return DataSection::RodataRelRO;

                return DataSection::Rodata;
            }
            if (g->init)
                return DataSection::Data;

            return DataSection::Bss;
        }

        void serialize_init_value(std::vector<std::uint8_t>& data, ir::IrValue const* val, ir::IrType const* expected_type, std::vector<Elf64_Rela>& relas,
                                  std::unordered_map<std::string, std::uint32_t>& sym_name_to_idx, std::uint64_t base_offset)
        {
            if (!val || !expected_type)
                return;

            switch (val->kind)
            {
                case ir::IrNodeKind::IntConstant: {
                    auto* ic = static_cast<ir::IrIntConstant const*>(val);
                    auto sz = expected_type->byte_size;
                    std::uint64_t uv = static_cast<std::uint64_t>(ic->value);
                    for (std::uint64_t i = 0; i < sz; ++i)
                    {
                        data.push_back(into_u8(uv & 0xFF));
                        uv >>= 8;
                    }
                    break;
                }
                case ir::IrNodeKind::FloatConstant: {
                    auto* fc = static_cast<ir::IrFloatConstant const*>(val);
                    auto bits =
                        expected_type ? (expected_type->kind == ir::IrTypeKind::Float ? static_cast<ir::IrFloatType const*>(expected_type)->bits : 64) : 64;
                    if (bits == 32)
                    {
                        float f = static_cast<float>(fc->value);
                        std::uint32_t raw;
                        std::memcpy(&raw, &f, 4);
                        w32(data, raw);
                    }
                    else
                    {
                        double d = fc->value;
                        std::uint64_t raw;
                        std::memcpy(&raw, &d, 8);
                        w64(data, raw);
                    }
                    break;
                }
                case ir::IrNodeKind::BoolConstant: {
                    auto* bc = static_cast<ir::IrBoolConstant const*>(val);
                    data.push_back(bc->value ? 1 : 0);

                    for (std::uint64_t i = 1; i < expected_type->byte_size; ++i)
                        data.push_back(0);
                    break;
                }
                case ir::IrNodeKind::NullConstant: {
                    for (std::uint64_t i = 0; i < expected_type->byte_size; ++i)
                        data.push_back(0);
                    break;
                }
                case ir::IrNodeKind::StringConstant: {
                    auto* sc = static_cast<ir::IrStringConstant const*>(val);
                    data.insert(data.end(), sc->value.begin(), sc->value.end());

                    data.push_back(0);
                    break;
                }
                case ir::IrNodeKind::Aggregate: {
                    auto* agg = static_cast<ir::IrAggregateInst const*>(val);
                    auto const* agg_type = agg->type ? agg->type : expected_type;

                    if (!agg_type)
                        break;

                    if (agg_type->kind == ir::IrTypeKind::Aggregate)
                    {
                        auto* at = static_cast<ir::IrAggregateType const*>(agg_type);
                        auto num_fields = std::min(agg->values.size(), at->members.size());
                        for (std::size_t i = 0; i < num_fields; ++i)
                        {
                            auto* fval = agg->values[i];
                            if (!fval)
                                continue;

                            auto field_off = (i < at->member_offsets.size()) ? at->member_offsets[i] : 0;
                            auto* field_type = (i < at->members.size()) ? at->members[i] : nullptr;

                            auto target_pos = base_offset + field_off;
                            if (target_pos > data.size())
                                data.resize(target_pos, 0);
                            serialize_init_value(data, fval, field_type, relas, sym_name_to_idx, data.size());
                        }
                    }
                    else if (agg_type->kind == ir::IrTypeKind::Array)
                    {
                        auto* at = static_cast<ir::IrArrayType const*>(agg_type);
                        auto elem_size = at->element ? at->element->byte_size : 1;
                        for (std::size_t i = 0; i < agg->values.size(); ++i)
                        {
                            auto* fval = agg->values[i];
                            if (!fval)
                            {
                                for (std::uint64_t j = 0; j < elem_size; ++j)
                                    data.push_back(0);

                                continue;
                            }
                            serialize_init_value(data, fval, at->element, relas, sym_name_to_idx, data.size());
                        }
                    }
                    break;
                }
                case ir::IrNodeKind::GlobalRef: {
                    auto* gr = static_cast<ir::IrGlobalRef const*>(val);
                    auto sz = expected_type ? expected_type->byte_size : 8;

                    auto pos = data.size();
                    for (std::uint64_t i = 0; i < sz; ++i)
                        data.push_back(0);

                    auto it = sym_name_to_idx.find(std::string{gr->name});
                    if (it != sym_name_to_idx.end())
                    {
                        Elf64_Rela rela{};
                        rela.r_offset = base_offset + pos;
                        rela.r_addend = 0;
                        rela.r_info = elf_r_info(it->second, R_X86_64_64);
                        relas.push_back(rela);
                    }
                    break;
                }
                default:
                    for (std::uint64_t i = 0; i < expected_type->byte_size; ++i)
                        data.push_back(0);
                    break;
            }
        }

        void collect_ref_names(ir::IrValue const* val, std::unordered_set<std::string>& out)
        {
            if (!val)
                return;

            if (val->kind == ir::IrNodeKind::GlobalRef)
            {
                auto* gr = static_cast<ir::IrGlobalRef const*>(val);
                out.insert(std::string{gr->name});
            }

            else if (val->kind == ir::IrNodeKind::Aggregate)
            {
                auto* agg = static_cast<ir::IrAggregateInst const*>(val);
                for (auto* fv : agg->values)
                    collect_ref_names(fv, out);
            }
        }

    } // anonymous namespace

} // namespace dcc::backend::em64t

export namespace dcc::backend::em64t
{
    [[nodiscard]] std::vector<std::uint8_t> write_elf64(ir::IrModule const& ir_mod, MModule const& mod, std::vector<EncodeResult> const& encoded,
                                                        target::TargetConfig const& target)
    {
        (void)target;

        std::vector<std::string> func_names;
        func_names.reserve(mod.functions.size());
        for (auto const& mf : mod.functions)
            if (!mf.owned_name.empty())
                func_names.push_back(mf.owned_name);
            else
                func_names.push_back("<unnamed>");

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
        for (auto& gl : globals)
        {
            if (gl.sec == DataSection::Rodata)
                rodata_globals.push_back(&gl);
            else if (gl.sec == DataSection::RodataRelRO)
                rodata_relro_globals.push_back(&gl);
            else if (gl.sec == DataSection::Data)
                data_globals.push_back(&gl);
            else if (gl.sec == DataSection::Bss)
                bss_globals.push_back(&gl);
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

        for (auto* glp : bss_globals)
        {
            auto pad = align_up(0, glp->alignment);
            glp->offset = pad;
        }

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
            if (g->linkage == ir::Linkage::External && !g->init)
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
        bool has_rodata_rela = has_rodata && (!rodata_relas.empty() || has_jump_tables);
        bool has_rodata_relro_rela = has_rodata_relro && !data_rel_ro_relas.empty();
        bool has_data_rela = has_data && !data_relas.empty();
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

        std::uint32_t sec_symtab = next_sec++;
        std::uint32_t sec_strtab = next_sec++;
        std::uint32_t sec_shstrtab = next_sec++;
        std::uint32_t total_sec = next_sec;

        for (auto& gl : globals)
        {
            if (gl.sec == DataSection::Rodata)
                gl.section_index = sec_rodata;
            else if (gl.sec == DataSection::RodataRelRO)
                gl.section_index = sec_data_rel_ro;
            else if (gl.sec == DataSection::Data)
                gl.section_index = sec_data;
            else if (gl.sec == DataSection::Bss)
                gl.section_index = sec_bss;
        }

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
            Elf64_Sym s{};
            s.st_name = add_str(strtab, fn);
            s.st_info = elf_st_info(STB_GLOBAL, STT_FUNC);
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

        std::uint64_t bss_size = 0;
        for (auto* glp : bss_globals)
        {
            auto pad = align_up(bss_size, glp->alignment);
            glp->offset = pad;
            bss_size = pad + (glp->g->type ? glp->g->type->byte_size : 0);
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
            shdrs[sec_rodata].sh_addralign = 8;
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
            shdrs[sec_data_rel_ro].sh_addralign = 8;
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
            shdrs[sec_data].sh_addralign = 8;
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
            shdrs[sec_bss].sh_addralign = 8;
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

        for (auto const& sym : syms)
            serialize_sym(out, sym);

        out.insert(out.end(), strtab.begin(), strtab.end());

        out.insert(out.end(), shstrtab.begin(), shstrtab.end());

        return out;
    }

    [[nodiscard]] std::vector<std::uint8_t> write_coff(ir::IrModule const& ir_mod, MModule const& mod, std::vector<EncodeResult> const& encoded,
                                                       target::TargetConfig const& target)
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

        std::vector<GlobalLayout*> rodata_globals, data_globals, bss_globals;
        for (auto& gl : globals)
        {
            if (gl.sec == DataSection::Rodata)
                rodata_globals.push_back(&gl);
            else if (gl.sec == DataSection::Data)
                data_globals.push_back(&gl);
            else if (gl.sec == DataSection::Bss)
                bss_globals.push_back(&gl);
        }

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
            std::uint32_t sec_idx;
            std::uint64_t value;
            std::uint64_t size;
        };
        std::vector<CoffSym> coff_syms;
        std::unordered_map<std::string, std::uint32_t> sym_name_to_idx;

        coff_syms.push_back({.name = "", .str_off = 0, .is_func = false, .is_object = false, .sec_idx = 0, .value = 0, .size = 0});

        auto text_sec_str = add_str(".text");
        coff_syms.push_back({.name = ".text", .str_off = text_sec_str, .is_func = false, .is_object = false, .sec_idx = 1, .value = 0, .size = 0});

        std::uint32_t sec_rdata = 0, sec_data = 0, sec_bss = 0;
        bool has_coff_jt = false;
        for (auto const& mf : mod.functions)
            if (!mf.jump_tables.empty())
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
            coff_syms.push_back(
                {.name = ".rdata", .str_off = rdata_sec_str, .is_func = false, .is_object = false, .sec_idx = sec_rdata, .value = 0, .size = 0});
        }
        if (has_data_sec)
        {
            data_sec_str = add_str(".data");
            sec_data = has_rdata ? 3 : 2;
            coff_syms.push_back({.name = ".data", .str_off = data_sec_str, .is_func = false, .is_object = false, .sec_idx = sec_data, .value = 0, .size = 0});
        }
        if (has_bss_sec)
        {
            bss_sec_str = add_str(".bss");
            sec_bss = (has_rdata ? 1 : 0) + (has_data_sec ? 1 : 0) + 2;
            coff_syms.push_back({.name = ".bss", .str_off = bss_sec_str, .is_func = false, .is_object = false, .sec_idx = sec_bss, .value = 0, .size = 0});
        }

        for (auto& gl : globals)
        {
            if (gl.sec == DataSection::Rodata)
                gl.section_index = sec_rdata;
            else if (gl.sec == DataSection::Data)
                gl.section_index = sec_data;
            else if (gl.sec == DataSection::Bss)
                gl.section_index = sec_bss;
        }

        for (auto const& ext : ext_syms)
        {
            auto so = add_str(ext);
            coff_syms.push_back({ext, so, false, false, 0, 0, 0});
            sym_name_to_idx[ext] = static_cast<std::uint32_t>(coff_syms.size() - 1);
        }

        for (std::size_t i = 0; i < func_names.size(); ++i)
        {
            auto so = add_str(func_names[i]);
            coff_syms.push_back({func_names[i], so, true, false, 1, 0, func_codes[i].size()});
            sym_name_to_idx[func_names[i]] = static_cast<std::uint32_t>(coff_syms.size() - 1);
        }

        for (auto& gl : globals)
        {
            auto so = add_str(gl.name_str);
            coff_syms.push_back({gl.name_str, so, false, true, gl.section_index, gl.offset, gl.g->type ? gl.g->type->byte_size : 0});
            sym_name_to_idx[std::string{gl.g->name}] = static_cast<std::uint32_t>(coff_syms.size() - 1);
        }

        std::vector<std::uint8_t> text_data;
        std::vector<std::size_t> func_starts;
        for (std::size_t i = 0; i < func_codes.size(); ++i)
        {
            func_starts.push_back(text_data.size());
            auto const& code = func_codes[i];
            text_data.insert(text_data.end(), code.begin(), code.end());
            while (text_data.size() % 16 != 0)
                text_data.push_back(0x90);
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
                    coff_syms.push_back({sym_name, so, false, false, 1, block_offset, 0});
                    sym_name_to_idx[sym_name] = static_cast<std::uint32_t>(coff_syms.size() - 1);
                }
            }
        }

        struct CoffReloc
        {
            std::uint32_t virt_addr;
            std::uint32_t sym_idx;
            std::uint16_t type;
        };

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
                    std::function<void(ir::IrValue const*, ir::IrType const*)> walker;
                    walker = [&](ir::IrValue const* val, ir::IrType const* exp_type) {
                        if (!val || !exp_type)
                            return;

                        if (val->kind == ir::IrNodeKind::GlobalRef)
                        {
                            auto* gr = static_cast<ir::IrGlobalRef const*>(val);
                            auto pos = sec_data.size();
                            auto sz = exp_type->byte_size;
                            for (std::uint64_t i = 0; i < sz; ++i)
                                sec_data.push_back(0);

                            auto it = sym_name_to_idx.find(std::string{gr->name});
                            if (it != sym_name_to_idx.end())
                            {
                                CoffReloc rel{};
                                rel.virt_addr = static_cast<std::uint32_t>(pos);
                                rel.sym_idx = it->second;
                                rel.type = IMAGE_REL_AMD64_ADDR64;
                                rels.push_back(rel);
                            }
                        }
                        else if (val->kind == ir::IrNodeKind::Aggregate)
                        {
                            auto* agg = static_cast<ir::IrAggregateInst const*>(val);
                            auto const* agg_type = agg->type ? agg->type : exp_type;
                            if (!agg_type)
                                return;

                            if (agg_type->kind == ir::IrTypeKind::Aggregate)
                            {
                                auto* at = static_cast<ir::IrAggregateType const*>(agg_type);
                                auto nf = std::min(agg->values.size(), at->members.size());
                                for (std::size_t i = 0; i < nf; ++i)
                                {
                                    auto* fv = agg->values[i];
                                    if (!fv)
                                        continue;

                                    auto field_off = (i < at->member_offsets.size()) ? at->member_offsets[i] : 0;
                                    auto* ft = (i < at->members.size()) ? at->members[i] : nullptr;
                                    auto target_pos = field_off;
                                    while (sec_data.size() - glp->offset < target_pos)
                                        sec_data.push_back(0);

                                    walker(fv, ft);
                                }
                            }
                            else if (agg_type->kind == ir::IrTypeKind::Array)
                            {
                                auto* at = static_cast<ir::IrArrayType const*>(agg_type);
                                for (auto* fv : agg->values)
                                {
                                    if (!fv)
                                    {
                                        for (std::uint64_t i = 0; i < (at->element ? at->element->byte_size : 1); ++i)
                                            sec_data.push_back(0);
                                        continue;
                                    }
                                    walker(fv, at->element);
                                }
                            }
                        }
                        else
                        {
                            ir::IrType const* effective_type = exp_type;
                            if (val->type && val->type->byte_size > 0)
                                effective_type = val->type;

                            if (auto* ic = ir::ir_cast<ir::IrIntConstant>(val))
                            {
                                auto sz = effective_type->byte_size;
                                std::uint64_t uv = static_cast<std::uint64_t>(ic->value);
                                for (std::uint64_t i = 0; i < sz; ++i)
                                {
                                    sec_data.push_back(into_u8(uv & 0xFF));
                                    uv >>= 8;
                                }
                            }
                            else if (auto* fc = ir::ir_cast<ir::IrFloatConstant>(val))
                            {
                                auto bits = effective_type->kind == ir::IrTypeKind::Float ? static_cast<ir::IrFloatType const*>(effective_type)->bits : 64;
                                if (bits == 32)
                                {
                                    float f = static_cast<float>(fc->value);
                                    std::uint32_t raw;
                                    std::memcpy(&raw, &f, 4);
                                    for (int i = 0; i < 4; ++i)
                                        sec_data.push_back(into_u8((raw >> (i * 8)) & 0xFF));
                                }
                                else
                                {
                                    double d = fc->value;
                                    std::uint64_t raw;
                                    std::memcpy(&raw, &d, 8);
                                    for (int i = 0; i < 8; ++i)
                                        sec_data.push_back(into_u8((raw >> (i * 8)) & 0xFF));
                                }
                            }
                            else if (auto* bc = ir::ir_cast<ir::IrBoolConstant>(val))
                            {
                                sec_data.push_back(bc->value ? 1 : 0);
                                for (std::uint64_t i = 1; i < effective_type->byte_size; ++i)
                                    sec_data.push_back(0);
                            }
                            else if (ir::ir_cast<ir::IrNullConstant>(val))
                                for (std::uint64_t i = 0; i < effective_type->byte_size; ++i)
                                    sec_data.push_back(0);
                            else
                                for (std::uint64_t i = 0; i < effective_type->byte_size; ++i)
                                    sec_data.push_back(0);
                        }
                    };

                    walker(glp->g->init, glp->g->type);
                }
            }
            return {std::move(sec_data), std::move(rels)};
        };

        auto [rdata_data, rdata_rels] = build_coff_init_data(rodata_globals, sec_rdata);

        for (auto const& mf : mod.functions)
        {
            for (auto const& jt : mf.jump_tables)
            {
                while (rdata_data.size() % 4 != 0)
                    rdata_data.push_back(0);

                for (std::size_t ei = 0; ei < jt.targets.size(); ++ei)
                {
                    std::uint32_t tgt_block = jt.targets[ei];
                    std::string blk_sym = mf.owned_name.empty() ? "anon" : mf.owned_name;
                    blk_sym += ".bb" + std::to_string(tgt_block);

                    auto blk_sym_it = sym_name_to_idx.find(blk_sym);
                    std::uint64_t entry_va_offset = rdata_data.size();
                    w32(rdata_data, 0);

                    if (blk_sym_it != sym_name_to_idx.end())
                    {
                        CoffReloc rel{};
                        rel.virt_addr = static_cast<std::uint32_t>(entry_va_offset);
                        rel.sym_idx = blk_sym_it->second;
                        rel.type = IMAGE_REL_AMD64_REL32;
                        rdata_rels.push_back(rel);
                    }
                }
            }
        }

        auto [data_sec_data, data_rels] = build_coff_init_data(data_globals, sec_data);

        std::uint64_t bss_sec_size = 0;
        for (auto* glp : bss_globals)
        {
            auto pad = align_up(bss_sec_size, glp->alignment);
            glp->offset = pad;
            bss_sec_size = pad + (glp->g->type ? glp->g->type->byte_size : 0);
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
                        rtype = IMAGE_REL_AMD64_REL32;
                        break;
                    case Reloc::Kind::Abs64:
                        rtype = IMAGE_REL_AMD64_ADDR64;
                        break;
                    default:
                        rtype = IMAGE_REL_AMD64_REL32;
                        break;
                }
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

        struct CoffSecReloc
        {
            std::uint32_t raw_start;
            std::vector<CoffReloc> rels;
        };
        std::vector<CoffSecReloc> sec_relocs;
        sec_relocs.push_back({text_raw_start, std::move(text_rels)});
        if (has_rdata)
            sec_relocs.push_back({rdata_raw_start, std::move(rdata_rels)});
        if (has_data_sec)
            sec_relocs.push_back({data_raw_start, std::move(data_rels)});
        if (has_bss_sec)
            sec_relocs.push_back({0, {}});

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
            if (cs.is_func)
                num_func_aux++;

        std::uint32_t num_syms = static_cast<std::uint32_t>(coff_syms.size()) + num_func_aux;
        std::uint32_t str_size = 4 + static_cast<std::uint32_t>(strtab.size());

        std::vector<std::uint8_t> out;

        w16(out, IMAGE_FILE_MACHINE_AMD64);
        w16(out, static_cast<std::uint16_t>(num_sec));
        w32(out, 0);
        w32(out, sym_start);
        w32(out, num_syms);
        w16(out, 0);
        w16(out, IMAGE_FILE_LINE_NUMS_STRIPPED | IMAGE_FILE_DEBUG_STRIPPED);

        auto write_sec_hdr = [&](std::string_view name8, std::uint32_t raw_size, std::uint32_t raw_ptr, std::uint32_t reloc_ptr, std::uint16_t reloc_count,
                                 std::uint32_t characteristics) {
            std::array<std::uint8_t, 8> name_buf = {0};
            for (std::size_t i = 0; i < name8.size() && i < 8; ++i)
                name_buf[i] = into_u8(name8[i]);

            for (auto c : name_buf)
                w8(out, c);

            w32(out, raw_size);
            w32(out, raw_size);
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
            write_sec_hdr(".text", static_cast<std::uint32_t>(text_data.size()), text_raw_start, reloc_ptr, static_cast<std::uint16_t>(text_rels.size()),
                          IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_16BYTES);
        }

        if (has_rdata)
        {
            std::size_t ri = 1;
            auto reloc_ptr = rdata_rels.empty() ? 0U : sec_relocs[ri].raw_start;
            write_sec_hdr(".rdata", static_cast<std::uint32_t>(rdata_data.size()), rdata_raw_start, reloc_ptr, static_cast<std::uint16_t>(rdata_rels.size()),
                          IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ);
        }

        if (has_data_sec)
        {
            std::size_t ri = (has_rdata ? std::size_t{2} : std::size_t{1});
            auto reloc_ptr = data_rels.empty() ? 0U : sec_relocs[ri].raw_start;
            write_sec_hdr(".data", static_cast<std::uint32_t>(data_sec_data.size()), data_raw_start, reloc_ptr, static_cast<std::uint16_t>(data_rels.size()),
                          IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);
        }

        if (has_bss_sec)
        {
            write_sec_hdr(".bss", static_cast<std::uint32_t>(bss_sec_size), 0, 0, 0,
                          IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);
        }

        out.insert(out.end(), text_data.begin(), text_data.end());
        while (out.size() < sec_relocs[0].raw_start)
            w8(out, 0);

        for (auto const& r : sec_relocs[0].rels)
        {
            w32(out, r.virt_addr);
            w32(out, r.sym_idx);
            w16(out, r.type);
        }

        if (has_rdata)
        {
            out.insert(out.end(), rdata_data.begin(), rdata_data.end());
            while (out.size() < sec_relocs[1].raw_start)
                w8(out, 0);

            for (auto const& r : sec_relocs[1].rels)
            {
                w32(out, r.virt_addr);
                w32(out, r.sym_idx);
                w16(out, r.type);
            }
        }

        if (has_data_sec)
        {
            std::size_t di = has_rdata ? std::size_t{2} : std::size_t{1};
            out.insert(out.end(), data_sec_data.begin(), data_sec_data.end());
            while (out.size() < sec_relocs[di].raw_start)
                w8(out, 0);

            for (auto const& r : sec_relocs[di].rels)
            {
                w32(out, r.virt_addr);
                w32(out, r.sym_idx);
                w16(out, r.type);
            }
        }

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
            else
            {
                w16(out, 0);
                w8(out, cs.name.empty() ? 0 : 2);
                w8(out, 0);
            }
        }

        w32(out, str_size);
        out.insert(out.end(), strtab.begin(), strtab.end());

        return out;
    }

    [[nodiscard]] std::uint64_t rd_elf64_le(std::span<std::uint8_t const> data, std::size_t off)
    {
        if (off + 8 > data.size())
            return 0;

        std::uint64_t v = 0;
        for (unsigned i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(data[off + i]) << (i * 8);

        return v;
    }

    [[nodiscard]] std::uint32_t rd_elf32_le(std::span<std::uint8_t const> data, std::size_t off)
    {
        if (off + 4 > data.size())
            return 0;

        std::uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(data[off + i]) << (i * 8);

        return v;
    }

    [[nodiscard]] std::uint16_t rd_elf16_le(std::span<std::uint8_t const> data, std::size_t off)
    {
        if (off + 2 > data.size())
            return 0;

        return static_cast<std::uint16_t>(static_cast<unsigned>(data[off]) | (static_cast<unsigned>(data[off + 1]) << 8));
    }

    struct ArSymbol
    {
        std::string name;
        std::uint64_t member_offset;
    };

    [[nodiscard]] std::vector<ArSymbol> parse_elf64_syms(std::span<std::uint8_t const> obj, std::uint64_t member_offset)
    {
        std::vector<ArSymbol> out;
        if (obj.size() < 64)
            return out;

        if (obj[0] != 0x7F || obj[1] != 'E' || obj[2] != 'L' || obj[3] != 'F')
            return out;
        if (obj[4] != 2)
            return out;
        if (obj[5] != 1)
            return out;

        std::uint64_t e_shoff = rd_elf64_le(obj, 40);
        std::uint16_t e_shentsize = rd_elf16_le(obj, 58);
        std::uint16_t e_shnum = rd_elf16_le(obj, 60);
        if (e_shoff == 0 || e_shentsize < 64 || e_shnum < 2)
            return out;

        std::size_t symtab_off = 0;
        std::size_t symtab_size = 0;
        std::size_t symtab_entsize = 0;
        std::size_t symtab_link = 0;
        std::size_t strtab_off = 0;
        std::size_t strtab_size = 0;

        for (std::uint16_t i = 0; i < e_shnum; ++i)
        {
            std::uint64_t shdr_off = e_shoff + (static_cast<std::uint64_t>(i) * e_shentsize);
            if (shdr_off + 64 > obj.size())
                break;

            std::uint32_t sh_type = rd_elf32_le(obj, static_cast<std::size_t>(shdr_off + 4));
            std::uint64_t sh_offset = rd_elf64_le(obj, static_cast<std::size_t>(shdr_off + 24));
            std::uint64_t sh_size = rd_elf64_le(obj, static_cast<std::size_t>(shdr_off + 32));
            std::uint32_t sh_link = rd_elf32_le(obj, static_cast<std::size_t>(shdr_off + 40));
            std::uint64_t sh_entsize = rd_elf64_le(obj, static_cast<std::size_t>(shdr_off + 56));

            if (sh_type == 2)
            {
                symtab_off = static_cast<std::size_t>(sh_offset);
                symtab_size = static_cast<std::size_t>(sh_size);
                symtab_entsize = static_cast<std::size_t>(sh_entsize);
                symtab_link = static_cast<std::size_t>(sh_link);
            }
        }

        if (symtab_off == 0 || symtab_entsize < 24)
            return out;

        if (symtab_link < static_cast<std::size_t>(e_shnum))
        {
            std::uint64_t str_shdr_off = e_shoff + (static_cast<std::uint64_t>(symtab_link) * e_shentsize);
            strtab_off = static_cast<std::size_t>(rd_elf64_le(obj, static_cast<std::size_t>(str_shdr_off + 24)));
            strtab_size = static_cast<std::size_t>(rd_elf64_le(obj, static_cast<std::size_t>(str_shdr_off + 32)));
        }

        if (strtab_off == 0 || strtab_size == 0)
            return out;

        std::size_t num_syms = symtab_size / symtab_entsize;
        for (std::size_t si = 0; si < num_syms; ++si)
        {
            std::size_t sym_off = symtab_off + (si * symtab_entsize);
            if (sym_off + 24 > obj.size())
                break;

            std::uint32_t st_name = rd_elf32_le(obj, sym_off);
            std::uint8_t st_info = (sym_off + 4 < obj.size()) ? obj[sym_off + 4] : 0;
            std::uint16_t st_shndx = rd_elf16_le(obj, sym_off + 6);

            std::uint8_t st_bind = st_info >> 4;

            if (st_shndx == 0)
                continue;

            if (st_bind != 1 && st_bind != 2)
                continue;

            std::uint8_t st_type = st_info & 0xF;
            if (st_type == 3 || st_type == 4)
                continue;

            std::string sym_name;
            if (st_name > 0 && static_cast<std::size_t>(st_name) < strtab_size)
            {
                std::size_t pos = strtab_off + static_cast<std::size_t>(st_name);
                while (pos < obj.size() && obj[pos] != 0)
                {
                    sym_name += static_cast<char>(obj[pos]);
                    ++pos;
                }
            }

            if (!sym_name.empty())
                out.push_back({.name = std::move(sym_name), .member_offset = member_offset});
        }

        return out;
    }

    [[nodiscard]] std::vector<ArSymbol> parse_elf32_syms(std::span<std::uint8_t const> obj, std::uint64_t member_offset)
    {
        std::vector<ArSymbol> out;
        if (obj.size() < 52)
            return out;

        if (obj[0] != 0x7F || obj[1] != 'E' || obj[2] != 'L' || obj[3] != 'F')
            return out;
        if (obj[4] != 1)
            return out;
        if (obj[5] != 1)
            return out;

        std::uint32_t e_shoff = rd_elf32_le(obj, 32);
        std::uint16_t e_shentsize = rd_elf16_le(obj, 46);
        std::uint16_t e_shnum = rd_elf16_le(obj, 48);
        if (e_shoff == 0 || e_shentsize < 40 || e_shnum < 2)
            return out;

        std::size_t symtab_off = 0;
        std::size_t symtab_size = 0;
        std::size_t symtab_entsize = 0;
        std::size_t symtab_link = 0;
        std::size_t strtab_off = 0;
        std::size_t strtab_size = 0;

        for (std::uint16_t i = 0; i < e_shnum; ++i)
        {
            std::uint64_t shdr_off = static_cast<std::uint64_t>(e_shoff) + (static_cast<std::uint64_t>(i) * e_shentsize);
            if (shdr_off + 40 > obj.size())
                break;

            std::uint32_t sh_type = rd_elf32_le(obj, static_cast<std::size_t>(shdr_off + 4));
            std::uint32_t sh_offset = rd_elf32_le(obj, static_cast<std::size_t>(shdr_off + 16));
            std::uint32_t sh_size = rd_elf32_le(obj, static_cast<std::size_t>(shdr_off + 20));
            std::uint32_t sh_link = rd_elf32_le(obj, static_cast<std::size_t>(shdr_off + 24));
            std::uint32_t sh_entsize = rd_elf32_le(obj, static_cast<std::size_t>(shdr_off + 36));

            if (sh_type == 2)
            {
                symtab_off = static_cast<std::size_t>(sh_offset);
                symtab_size = static_cast<std::size_t>(sh_size);
                symtab_entsize = static_cast<std::size_t>(sh_entsize);
                symtab_link = static_cast<std::size_t>(sh_link);
            }
        }

        if (symtab_off == 0 || symtab_entsize < 16)
            return out;

        if (symtab_link < static_cast<std::size_t>(e_shnum))
        {
            std::uint64_t str_shdr_off = static_cast<std::uint64_t>(e_shoff) + (static_cast<std::uint64_t>(symtab_link) * e_shentsize);
            strtab_off = static_cast<std::size_t>(rd_elf32_le(obj, static_cast<std::size_t>(str_shdr_off + 16)));
            strtab_size = static_cast<std::size_t>(rd_elf32_le(obj, static_cast<std::size_t>(str_shdr_off + 20)));
        }

        if (strtab_off == 0 || strtab_size == 0)
            return out;

        std::size_t num_syms = symtab_size / symtab_entsize;
        for (std::size_t si = 0; si < num_syms; ++si)
        {
            std::size_t sym_off = symtab_off + (si * symtab_entsize);
            if (sym_off + 16 > obj.size())
                break;

            std::uint32_t st_name = rd_elf32_le(obj, sym_off);
            std::uint8_t st_info = (sym_off + 12 < obj.size()) ? obj[sym_off + 12] : 0;
            std::uint16_t st_shndx = rd_elf16_le(obj, sym_off + 14);

            std::uint8_t st_bind = st_info >> 4;

            if (st_shndx == 0)
                continue;

            if (st_bind != 1 && st_bind != 2)
                continue;

            std::uint8_t st_type = st_info & 0xF;
            if (st_type == 3 || st_type == 4)
                continue;

            std::string sym_name;
            if (st_name > 0 && static_cast<std::size_t>(st_name) < strtab_size)
            {
                std::size_t pos = strtab_off + static_cast<std::size_t>(st_name);
                while (pos < obj.size() && obj[pos] != 0)
                {
                    sym_name += static_cast<char>(obj[pos]);
                    ++pos;
                }
            }

            if (!sym_name.empty())
                out.push_back({.name = std::move(sym_name), .member_offset = member_offset});
        }

        return out;
    }

    [[nodiscard]] std::string ar_dec_field(std::uint64_t val, std::size_t width)
    {
        auto s = std::to_string(val);
        if (s.size() >= width)
            return s;

        return s + std::string(width - s.size(), ' ');
    }

    [[nodiscard]] std::string ar_oct_field(std::uint64_t val, std::size_t width)
    {
        std::string s;
        if (val == 0)
            s = "0";
        else
        {
            while (val > 0)
            {
                s.insert(s.begin(), static_cast<char>('0' + (val & 7)));
                val >>= 3;
            }
        }
        if (s.size() >= width)
            return s;

        return std::string(width - s.size(), ' ') + s;
    }

    void ar_w32be(std::vector<std::uint8_t>& b, std::uint32_t v)
    {
        b.push_back(into_u8((v >> 24) & 0xFF));
        b.push_back(into_u8((v >> 16) & 0xFF));
        b.push_back(into_u8((v >> 8) & 0xFF));
        b.push_back(into_u8(v & 0xFF));
    }

    void ar_write_hdr(std::vector<std::uint8_t>& out, std::string const& name, std::uint64_t size)
    {
        std::string nam16;
        bool is_special = (name == "/" || name == "//" || (name.size() > 1 && name[0] == '/' && name[1] >= '0' && name[1] <= '9'));

        if (is_special)
        {
            nam16 = name;
            if (nam16.size() < 16)
                nam16 += std::string(16 - nam16.size(), ' ');
        }
        else if (name.size() <= 15)
        {
            nam16 = name + "/";
            if (nam16.size() < 16)
                nam16 += std::string(16 - nam16.size(), ' ');
        }
        else
        {
            nam16 = name;
            if (nam16.size() < 16)
                nam16 += std::string(16 - nam16.size(), ' ');
        }

        for (std::size_t i = 0; i < 16; ++i)
            out.push_back(into_u8(i < nam16.size() ? nam16[i] : ' '));

        auto ts = ar_dec_field(0, 12);
        for (auto c : ts)
            out.push_back(into_u8(c));

        auto owner = ar_dec_field(0, 6);
        for (auto c : owner)
            out.push_back(into_u8(c));

        auto group = ar_dec_field(0, 6);
        for (auto c : group)
            out.push_back(into_u8(c));

        auto mode = std::string("100644  ");
        for (auto c : mode)
            out.push_back(into_u8(c));

        auto sz = ar_dec_field(size, 10);
        for (auto c : sz)
            out.push_back(into_u8(c));

        out.push_back(into_u8(0x60));
        out.push_back(into_u8('\n'));
    }

    void ar_maybe_pad(std::vector<std::uint8_t>& out, std::uint64_t data_size)
    {
        if (data_size % 2 != 0)
            out.push_back(into_u8('\n'));
    }

    void ar_write_global_hdr(std::vector<std::uint8_t>& out)
    {
        out.push_back(into_u8('!'));
        out.push_back(into_u8('<'));
        out.push_back(into_u8('a'));
        out.push_back(into_u8('r'));
        out.push_back(into_u8('c'));
        out.push_back(into_u8('h'));
        out.push_back(into_u8('>'));
        out.push_back(into_u8('\n'));
    }

} // namespace dcc::backend::em64t

export namespace dcc::backend::em64t
{
    [[nodiscard]] std::vector<std::uint8_t> write_archive_elf(std::vector<std::pair<std::string, std::vector<std::uint8_t>>> const& members)
    {
        struct MemberInfo
        {
            std::string name;
            std::vector<std::uint8_t> data;
            std::vector<ArSymbol> syms;
            std::uint64_t hdr_offset;
            std::uint64_t long_name_offset{0};
        };

        std::vector<MemberInfo> infos;
        infos.reserve(members.size());
        for (auto const& [name, data] : members)
            infos.push_back({.name = name, .data = data, .syms = {}, .hdr_offset = 0});

        std::uint64_t total_syms = 0;
        std::uint64_t total_name_len = 0;
        for (auto& info : infos)
        {
            std::vector<ArSymbol> syms;
            if (info.data.size() > 4 && info.data[4] == 1)
                syms = parse_elf32_syms(info.data, 0);
            else
                syms = parse_elf64_syms(info.data, 0);
            for (auto& s : syms)
                info.syms.push_back(std::move(s));

            total_syms += info.syms.size();
            for (auto const& s : info.syms)
                total_name_len += s.name.size() + 1;
        }

        std::uint64_t sym_data_size = 4 + (4 * total_syms) + total_name_len;
        std::uint64_t sym_pad = (sym_data_size % 2 != 0) ? 1ULL : 0ULL;

        bool need_long = false;
        for (auto const& info : infos)
            if (info.name.size() > 15)
                need_long = true;

        std::string long_names;
        if (need_long)
        {
            for (auto& info : infos)
            {
                if (info.name.size() > 15)
                {
                    info.long_name_offset = long_names.size();
                    long_names += info.name;
                    long_names += "/\n";
                }
            }
        }
        std::uint64_t long_pad = (need_long && !long_names.empty() && long_names.size() % 2 != 0) ? 1ULL : 0ULL;
        std::uint64_t cur = 8;

        cur += 60 + sym_data_size + sym_pad;

        if (need_long && !long_names.empty())
            cur += 60 + long_names.size() + long_pad;

        for (auto& info : infos)
        {
            info.hdr_offset = cur;
            cur += 60 + info.data.size();
            if (info.data.size() % 2 != 0)
                cur += 1;
        }

        struct FinalSym
        {
            std::string name;
            std::uint64_t member_offset;
        };
        std::vector<FinalSym> final_syms;
        for (auto const& info : infos)
            for (auto const& s : info.syms)
                final_syms.push_back({.name = s.name, .member_offset = info.hdr_offset});

        std::ranges::sort(final_syms, [](auto const& a, auto const& b) { return a.name < b.name; });

        std::vector<std::uint8_t> sym_data;
        ar_w32be(sym_data, static_cast<std::uint32_t>(final_syms.size()));
        for (auto const& fs : final_syms)
            ar_w32be(sym_data, static_cast<std::uint32_t>(fs.member_offset));

        for (auto const& fs : final_syms)
        {
            for (auto c : fs.name)
                sym_data.push_back(into_u8(c));
            sym_data.push_back(0);
        }

        std::vector<std::uint8_t> out;
        ar_write_global_hdr(out);

        ar_write_hdr(out, "/", sym_data_size);
        out.insert(out.end(), sym_data.begin(), sym_data.end());
        ar_maybe_pad(out, sym_data_size);

        if (need_long && !long_names.empty())
        {
            ar_write_hdr(out, "//", long_names.size());
            for (auto c : long_names)
                out.push_back(into_u8(c));
            ar_maybe_pad(out, long_names.size());
        }

        for (auto const& info : infos)
        {
            std::string mname = info.name;
            if (mname.size() > 15)
                mname = "/" + std::to_string(info.long_name_offset);

            ar_write_hdr(out, mname, info.data.size());
            out.insert(out.end(), info.data.begin(), info.data.end());
            ar_maybe_pad(out, info.data.size());
        }

        return out;
    }

} // namespace dcc::backend::em64t
