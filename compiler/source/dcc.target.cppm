export module dcc.target;

import std;

export namespace dcc::target
{
    enum class Arch : std::uint8_t
    {
        X86_64,
        X86,
    };

    enum class Os : std::uint8_t
    {
        Linux,
        Windows,
        Freestanding,
    };

    enum class ObjectFormat : std::uint8_t
    {
        Elf,
        Coff,
    };

    enum class CodeModel : std::uint8_t
    {
        Default,
        Small,
        Kernel,
        Medium,
        Large,
    };

    struct Layout
    {
        std::uint64_t size{};
        std::uint64_t align{};
    };

    struct TargetConfig
    {
        std::string triple;
        Arch arch{Arch::X86_64};
        Os os{Os::Linux};
        ObjectFormat object_format{ObjectFormat::Elf};
        std::uint8_t pointer_bits{64};
        std::uint8_t pointer_align{8};
        bool little_endian{true};

        bool no_red_zone{false};
        bool no_simd{false};
        bool no_x87{false};
        bool no_stack_protector{false};
        bool no_stack_probe{false};
        bool position_independent_code{false};
        CodeModel code_model{CodeModel::Default};
        std::string cpu;

        [[nodiscard]] static std::optional<CodeModel> parse_code_model(std::string_view s)
        {
            if (s == "default")
                return CodeModel::Default;
            if (s == "small")
                return CodeModel::Small;
            if (s == "kernel")
                return CodeModel::Kernel;
            if (s == "medium")
                return CodeModel::Medium;
            if (s == "large")
                return CodeModel::Large;
            return std::nullopt;
        }

        [[nodiscard]] static bool is_x86_cpu_allowed(std::string_view cpu)
        {
            using namespace std::string_view_literals;
            static constexpr std::string_view allowed[] = {
                "generic"sv,    "i386"sv,     "i486"sv,     "i586"sv,     "pentium"sv, "pentium-mmx"sv, "i686"sv,
                "pentiumpro"sv, "pentium2"sv, "pentium3"sv, "pentium4"sv, "x86-64"sv,  "native"sv,
            };

            for (auto a : allowed)
                if (a == cpu)
                    return true;

            return false;
        }

        [[nodiscard]] static bool cpu_is_pre_i686(std::string_view cpu)
        {
            using namespace std::string_view_literals;
            return cpu == "i386"sv || cpu == "i486"sv || cpu == "i586"sv || cpu == "pentium"sv || cpu == "pentium-mmx"sv;
        }

        [[nodiscard]] Layout int_layout(std::uint8_t bits) const { return Layout{static_cast<std::uint64_t>(bits / 8), static_cast<std::uint64_t>(bits / 8)}; }

        [[nodiscard]] Layout float_layout(std::uint8_t bits) const
        {
            return Layout{static_cast<std::uint64_t>(bits / 8), static_cast<std::uint64_t>(bits / 8)};
        }

        [[nodiscard]] Layout pointer_layout() const { return Layout{static_cast<std::uint64_t>(pointer_bits / 8), static_cast<std::uint64_t>(pointer_align)}; }

        [[nodiscard]] Layout slice_layout() const
        {
            if (pointer_bits == 64)
                return Layout{16, 8};
            else
                return Layout{8, 4};
        }

        [[nodiscard]] static TargetConfig host_default()
        {
            TargetConfig cfg;
            cfg.triple = "x86_64-elf";
            cfg.arch = Arch::X86_64;
            cfg.os = Os::Linux;
            cfg.object_format = ObjectFormat::Elf;
            cfg.pointer_bits = 64;
            cfg.pointer_align = 8;
            cfg.little_endian = true;
            return cfg;
        }

        [[nodiscard]] static std::optional<TargetConfig> parse_triple(std::string_view triple)
        {
            TargetConfig cfg;
            cfg.triple = std::string{triple};

            if (triple == "x86_64-elf")
            {
                cfg.arch = Arch::X86_64;
                cfg.os = Os::Linux;
                cfg.object_format = ObjectFormat::Elf;
                cfg.pointer_bits = 64;
                cfg.pointer_align = 8;
                cfg.little_endian = true;
                return cfg;
            }

            if (triple == "x86-elf")
            {
                cfg.arch = Arch::X86;
                cfg.os = Os::Linux;
                cfg.object_format = ObjectFormat::Elf;
                cfg.pointer_bits = 32;
                cfg.pointer_align = 4;
                cfg.little_endian = true;
                return cfg;
            }

            if (triple == "x86_64-coff")
            {
                cfg.arch = Arch::X86_64;
                cfg.os = Os::Windows;
                cfg.object_format = ObjectFormat::Coff;
                cfg.pointer_bits = 64;
                cfg.pointer_align = 8;
                cfg.little_endian = true;
                return cfg;
            }

            if (triple == "x86-coff")
            {
                cfg.arch = Arch::X86;
                cfg.os = Os::Windows;
                cfg.object_format = ObjectFormat::Coff;
                cfg.pointer_bits = 32;
                cfg.pointer_align = 4;
                cfg.little_endian = true;
                return cfg;
            }

            return std::nullopt;
        }
    };

} // namespace dcc::target
