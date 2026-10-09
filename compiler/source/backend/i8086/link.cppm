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

        [[nodiscard]] std::string rewrite_link_errors(std::string const& output, LinkOptions const& options)
        {
            if (output.find("undefined symbol: __dcc_dgroup_segment") == std::string::npos)
                return output;
            std::string references;
            std::istringstream lines{output};
            std::string line;
            while (std::getline(lines, line))
                if (line.find(">>> referenced by") != std::string::npos)
                    references += line + "\n";
            return std::format(
                "the image segment is unknown (-fbase=?:{:04X}), but a static far pointer needs it; pass -fbase=SEG:OFF with a known segment\n{}",
                options.base.offset, references);
        }
    } // namespace

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
                                         "  . = 0x{:X};\n"
                                         "  .start : {{ KEEP(*(.start .start.*)) }}\n"
                                         "  .text : {{ *(.text .text.*) }}\n"
                                         "  __dcc_code_end = .;\n"
                                         "  .rodata : {{ *(.rodata .rodata.*) }}\n"
                                         "  .data : {{ *(.data.rel.ro .data.rel.ro.* .data .data.*) }}\n"
                                         "  .bss (NOLOAD) : {{ __bss_start = .; *(.bss .bss.*) *(COMMON) __bss_end = .; }}\n"
                                         "  __image_end = .;\n"
                                         "  /DISCARD/ : {{ *(.note .note.* .comment .eh_frame .eh_frame.*) }}\n"
                                         "}}\n",
                                         options.base.offset);
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
        std::string command = "ld.lld -m elf_i386 --fatal-warnings";
        if (!user_script)
        {
            auto script = dir / "i8086.ld";
            std::ofstream{script} << linker_script(options);
            command += " -T " + quote(script.string());
        }
        command += " -o " + quote(output);
        for (auto const& input : inputs)
            command += " " + quote(input);
        for (auto const& arg : extra_args)
            command += " " + quote(arg);
        command += " 2>&1";

        auto* pipe = popen(command.c_str(), "r");
        if (!pipe)
            return std::unexpected("cannot run ld.lld");
        std::string captured;
        std::array<char, 4096> buffer{};
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
            captured += buffer.data();
        if (pclose(pipe) != 0)
            return std::unexpected(rewrite_link_errors(captured, options));
        return {};
    }
} // namespace dcc::backend::i8086
