export module dcc.backend.object.archive;

import std;

#define into_u8 static_cast<std::uint8_t>

namespace dcc::backend::em64t
{
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

}

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

}
