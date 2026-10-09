module;

#include <cstdio>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

export module dcc.backend.i8086.link;

import std;
import dcc.target;

export namespace dcc::backend::i8086
{
    struct LinkBase
    {
        std::optional<std::uint16_t> segment;
        std::uint16_t offset{};
    };

    struct LinkOptions
    {
        LinkBase base;
        std::uint32_t stack_reserve{4096};
        target::CodeModel model{target::CodeModel::Default};
    };

    [[nodiscard]] std::string_view model_marker(target::CodeModel model) noexcept;

    inline constexpr std::string_view model_marker_section = ".dcc.i8086.model";

    [[nodiscard]] std::expected<LinkBase, std::string> parse_link_base(std::string_view text);

    [[nodiscard]] std::expected<std::uint32_t, std::string> parse_stack_reserve(std::string_view text);

    [[nodiscard]] std::string linker_script(LinkOptions const& options);

    [[nodiscard]] std::expected<void, std::string> link_flat_binary(std::vector<std::string> const& inputs, std::string const& output,
                                                                    LinkOptions const& options, std::vector<std::string> const& extra_args);
} // namespace dcc::backend::i8086

namespace dcc::backend::i8086
{
    namespace
    {
        [[nodiscard]] std::optional<std::uint32_t> parse_number(std::string_view text)
        {
            unsigned base = 16;
            if (text.starts_with("0x") || text.starts_with("0X"))
                text.remove_prefix(2);
            if (text.empty() || text.size() > 8)
                return std::nullopt;
            std::uint32_t value = 0;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, static_cast<int>(base));
            if (error != std::errc{} || end != text.data() + text.size())
                return std::nullopt;
            return value;
        }

        [[nodiscard]] std::string quote(std::string_view text)
        {
#ifdef _WIN32
            return std::format("\"{}\"", text);
#else
            std::string out{'\''};
            for (char c : text)
                out += c == '\'' ? std::string{"'\\''"} : std::string(1, c);
            out += '\'';
            return out;
#endif
        }

        [[nodiscard]] bool small_model(target::CodeModel model)
        {
            return model != target::CodeModel::Unreal && model != target::CodeModel::Unreal32;
        }

        [[nodiscard]] std::string_view model_name(target::CodeModel model)
        {
            return small_model(model) ? "small" : target::TargetConfig::code_model_name(model);
        }

        [[nodiscard]] std::string references_to(std::string const& output, std::string_view symbol)
        {
            std::string references;
            std::istringstream lines{output};
            std::string line;
            bool inside = false;
            while (std::getline(lines, line))
            {
                if (line.find("undefined symbol: ") != std::string::npos)
                    inside = line.ends_with(symbol);
                else if (inside && line.starts_with(">>>"))
                    references += line + "\n";
            }
            return references;
        }

        [[nodiscard]] std::expected<void, std::string> run_linker(std::string command)
        {
            command += " 2>&1";
            auto* pipe = popen(command.c_str(), "r");
            if (!pipe)
                return std::unexpected("cannot run ld.lld");
            std::string captured;
            std::array<char, 4096> buffer{};
            while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
                captured += buffer.data();
            if (pclose(pipe) != 0)
                return std::unexpected(captured);
            return {};
        }

        struct OutputSection
        {
            std::string name;
            std::uint32_t flags{};
            std::uint32_t type{};
            std::uint32_t address{};
            std::uint32_t size{};
        };

        [[nodiscard]] std::optional<std::vector<OutputSection>> read_sections(std::filesystem::path const& path)
        {
            std::ifstream in{path, std::ios::binary};
            std::vector<unsigned char> data{std::istreambuf_iterator<char>(in), {}};
            auto u16 = [&](std::size_t at) { return static_cast<std::uint32_t>(data[at] | (data[at + 1] << 8)); };
            auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
            if (data.size() < 52 || data[0] != 0x7F || data[4] != 1)
                return std::nullopt;
            std::size_t shoff = u32(32);
            std::size_t entsize = u16(46);
            std::size_t count = u16(48);
            std::size_t strndx = u16(50);
            if (entsize < 40 || strndx >= count || shoff + count * entsize > data.size())
                return std::nullopt;
            std::size_t strtab = u32(shoff + strndx * entsize + 16);
            std::vector<OutputSection> sections;
            for (std::size_t i = 1; i < count; ++i)
            {
                auto header = shoff + i * entsize;
                OutputSection section;
                for (auto at = strtab + u32(header); at < data.size() && data[at] != 0; ++at)
                    section.name += static_cast<char>(data[at]);
                section.type = u32(header + 4);
                section.flags = u32(header + 8);
                section.address = u32(header + 12);
                section.size = u32(header + 20);
                sections.push_back(std::move(section));
            }
            return sections;
        }

        [[nodiscard]] std::map<std::string, std::vector<std::string>> map_inputs(std::filesystem::path const& path)
        {
            std::map<std::string, std::vector<std::string>> inputs;
            std::ifstream in{path};
            std::string line;
            std::string current;
            while (std::getline(in, line))
            {
                std::size_t at = 0;
                for (int field = 0; field < 4; ++field)
                {
                    while (at < line.size() && line[at] == ' ')
                        ++at;
                    while (at < line.size() && line[at] != ' ')
                        ++at;
                }
                auto text = line.substr(std::min(at, line.size()));
                auto indent = text.find_first_not_of(' ');
                if (indent == std::string::npos)
                    continue;
                if (indent == 1)
                    current = text.substr(1);
                else if (indent == 9 && !current.empty())
                    inputs[current].push_back(text.substr(9));
            }
            return inputs;
        }

        [[nodiscard]] std::expected<void, std::string> check_placement(std::filesystem::path const& image, std::filesystem::path const& map, LinkOptions const& options)
        {
            constexpr std::uint32_t alloc = 2;
            static constexpr std::array<std::string_view, 5> expected{".start", ".text", ".rodata", ".data", ".bss"};
            auto sections = read_sections(image);
            if (!sections)
                return std::unexpected("cannot read the intermediate i8086 image");
            auto inputs = map_inputs(map);
            std::string errors;
            for (auto const& section : *sections)
            {
                if (!(section.flags & alloc) || section.size == 0)
                    continue;
                if (std::ranges::find(expected, section.name) == expected.end())
                {
                    std::string from;
                    for (auto const& input : inputs[section.name])
                        from += (from.empty() ? "" : ", ") + input;
                    errors += std::format("allocated section {} ({}) is not placed by the i8086 linker script, which accepts only .start*, .text*, "
                                          ".rodata*, .data*, .bss* and COMMON input sections\n",
                                          section.name, from.empty() ? std::string{"unknown input"} : from);
                    continue;
                }
                if (section.address < options.base.offset)
                    errors += std::format("section {} starts at offset 0x{:X}, below the -fbase offset 0x{:X}\n", section.name, section.address, options.base.offset);
                if ((section.name == ".start" || section.name == ".text") && std::uint64_t{section.address} + section.size > 0x10000)
                    errors += std::format("section {} ends above offset 0x10000 of the 64 KiB code segment\n", section.name);
            }
            if (!errors.empty())
                return std::unexpected(errors);
            return {};
        }

        [[nodiscard]] std::string rewrite_link_errors(std::string const& output, LinkOptions const& options)
        {
            for (auto model : {target::CodeModel::Small, target::CodeModel::Unreal, target::CodeModel::Unreal32})
            {
                auto marker = model_marker(model);
                if (output.find(std::format("undefined symbol: {}\n", marker)) == std::string::npos || marker == model_marker(options.model))
                    continue;

                return std::format("i8086 code model mismatch: an object needs {}, but this link is for -mcmodel={} and provides {}", marker,
                                   model_name(options.model), model_marker(options.model));
            }
            if (output.find("undefined symbol: __dcc_dgroup_segment") == std::string::npos)
                return output;

            auto references = references_to(output, "__dcc_dgroup_segment");
            return std::format(
                "the image segment is unknown (-fbase=?:{:04X}), but a static far pointer needs it; pass -fbase=SEG:OFF with a known segment\n{}",
                options.base.offset, references);
        }

    } // namespace

    std::string_view model_marker(target::CodeModel model) noexcept
    {
        switch (model)
        {
            case target::CodeModel::Unreal:
                return "__dcc_i8086_model_unreal";
            case target::CodeModel::Unreal32:
                return "__dcc_i8086_model_unreal32";
            default:
                return "__dcc_i8086_model_small";
        }
    }

    std::expected<LinkBase, std::string> parse_link_base(std::string_view text)
    {
        auto colon = text.find(':');
        auto invalid = [&] {
            return std::unexpected(std::format("invalid -fbase value '{}'; expected SEG:OFF in hexadecimal, with SEG '?' if unknown", text));
        };
        if (colon == std::string_view::npos)
            return invalid();
        LinkBase base;
        auto segment = text.substr(0, colon);
        if (segment != "?")
        {
            auto value = parse_number(segment);
            if (!value || *value > 0xFFFF)
                return invalid();
            base.segment = static_cast<std::uint16_t>(*value);
        }
        auto offset = parse_number(text.substr(colon + 1));
        if (!offset || *offset > 0xFFFF)
            return invalid();
        base.offset = static_cast<std::uint16_t>(*offset);
        return base;
    }

    std::expected<std::uint32_t, std::string> parse_stack_reserve(std::string_view text)
    {
        std::uint32_t value = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value > 0xFFFF)
            return std::unexpected(std::format("invalid -fstack-reserve value '{}'; expected a byte count from 0 to 65535", text));
        return value;
    }

    std::string linker_script(LinkOptions const& options)
    {
        std::string script = std::format("OUTPUT_FORMAT(binary)\n"
                                         "SECTIONS\n"
                                         "{{\n"
                                         "  . = 0x{2:X};\n"
                                         "  .start : {{ KEEP(*(.start .start.*)) }}\n"
                                         "  .text : {{ *(.text .text.*) }}\n"
                                         "  __dcc_code_end = .;\n"
                                         "  .rodata : {{ *(.rodata .rodata.*) }}\n"
                                         "  .data : {{ *(.data.rel.ro .data.rel.ro.* .data .data.*) }}\n"
                                         "  .bss (NOLOAD) : {{ __bss_start = .; *(.bss .bss.*) *(COMMON) __bss_end = .; }}\n"
                                         "  __image_end = .;\n"
                                         "  /DISCARD/ : {{ *(.note .note.* .comment .eh_frame .eh_frame.*) }}\n"
                                         "  {0} 0 (INFO) : {{ KEEP(*({0})) }}\n"
                                         "}}\n"
                                         "{1} = 0;\n",
                                         model_marker_section, model_marker(options.model), options.base.offset);
        if (options.base.segment)
            script += std::format("__dcc_dgroup_segment = 0x{:X};\n", *options.base.segment);
        script += "ASSERT(__dcc_code_end <= 0x10000, \"i8086 code (.start and .text) does not fit below offset 0x10000 of its 64 KiB code segment\")\n";
        if (small_model(options.model))
            script += std::format("ASSERT(__image_end <= 0x{:X}, \"i8086 small model: the image plus the {}-byte stack reserve (-fstack-reserve) does not fit "
                                  "in one 64 KiB segment\")\n",
                                  0x10000 - options.stack_reserve, options.stack_reserve);
        return script;
    }

    std::expected<void, std::string> link_flat_binary(std::vector<std::string> const& inputs, std::string const& output, LinkOptions const& options,
                                                      std::vector<std::string> const& extra_args)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto dir = fs::temp_directory_path(ec) / std::format("dcc-i8086-link-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        if (ec || !fs::create_directories(dir, ec))
            return std::unexpected("cannot create a temporary directory for linking");
        struct Cleanup
        {
            fs::path path;
            ~Cleanup()
            {
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
        } cleanup{dir};

        bool user_script = std::ranges::any_of(extra_args, [](std::string const& arg) { return arg.starts_with("--script=") || arg == "-T"; });
        std::string arguments;
        for (auto const& input : inputs)
            arguments += " " + quote(input);
        for (auto const& arg : extra_args)
            arguments += " " + quote(arg);

        if (!user_script)
        {
            auto script = linker_script(options);
            auto elf_script = dir / "i8086-elf.ld";
            std::ofstream{elf_script} << "OUTPUT_FORMAT(elf32-i386)" << script.substr(script.find('\n'));
            auto intermediate = dir / "image.elf";
            auto map = dir / "image.map";
            auto linked = run_linker(std::format("ld.lld -m elf_i386 --fatal-warnings -e 0 -T {} -o {}{} -Map={}", quote(elf_script.string()),
                                                 quote(intermediate.string()), arguments, quote(map.string())));
            if (!linked)
                return std::unexpected(rewrite_link_errors(linked.error(), options));
            if (auto placement = check_placement(intermediate, map, options); !placement)
                return placement;
        }

        std::string command = "ld.lld -m elf_i386 --fatal-warnings";
        if (!user_script)
        {
            auto script = dir / "i8086.ld";
            std::ofstream{script} << linker_script(options);
            command += " -T " + quote(script.string());
        }
        auto linked = run_linker(command + " -o " + quote(output) + arguments);
        if (!linked)
            return std::unexpected(rewrite_link_errors(linked.error(), options));
        return {};
    }
} // namespace dcc::backend::i8086
