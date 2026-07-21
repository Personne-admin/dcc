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

    enum class PhysRegClass : std::uint8_t
    {
        GPR,
        XMM,
        Seg,
        Flags,
        ST,
    };

    struct PhysReg
    {
        std::string_view name;
        PhysRegClass cls;
        std::uint16_t width;
        bool reserved;
    };

    namespace detail
    {
        using namespace std::string_view_literals;

        inline constexpr PhysReg x86_64_regs[] = {
            {"rax"sv, PhysRegClass::GPR, 64, false},     {"rbx"sv, PhysRegClass::GPR, 64, false},     {"rcx"sv, PhysRegClass::GPR, 64, false},
            {"rdx"sv, PhysRegClass::GPR, 64, false},     {"rsi"sv, PhysRegClass::GPR, 64, false},     {"rdi"sv, PhysRegClass::GPR, 64, false},
            {"rbp"sv, PhysRegClass::GPR, 64, true},      {"rsp"sv, PhysRegClass::GPR, 64, true},      {"r8"sv, PhysRegClass::GPR, 64, false},
            {"r9"sv, PhysRegClass::GPR, 64, false},      {"r10"sv, PhysRegClass::GPR, 64, false},     {"r11"sv, PhysRegClass::GPR, 64, false},
            {"r12"sv, PhysRegClass::GPR, 64, false},     {"r13"sv, PhysRegClass::GPR, 64, false},     {"r14"sv, PhysRegClass::GPR, 64, false},
            {"r15"sv, PhysRegClass::GPR, 64, false},

            {"eax"sv, PhysRegClass::GPR, 32, false},     {"ebx"sv, PhysRegClass::GPR, 32, false},     {"ecx"sv, PhysRegClass::GPR, 32, false},
            {"edx"sv, PhysRegClass::GPR, 32, false},     {"esi"sv, PhysRegClass::GPR, 32, false},     {"edi"sv, PhysRegClass::GPR, 32, false},
            {"ebp"sv, PhysRegClass::GPR, 32, true},      {"esp"sv, PhysRegClass::GPR, 32, true},      {"r8d"sv, PhysRegClass::GPR, 32, false},
            {"r9d"sv, PhysRegClass::GPR, 32, false},     {"r10d"sv, PhysRegClass::GPR, 32, false},    {"r11d"sv, PhysRegClass::GPR, 32, false},
            {"r12d"sv, PhysRegClass::GPR, 32, false},    {"r13d"sv, PhysRegClass::GPR, 32, false},    {"r14d"sv, PhysRegClass::GPR, 32, false},
            {"r15d"sv, PhysRegClass::GPR, 32, false},

            {"ax"sv, PhysRegClass::GPR, 16, false},      {"bx"sv, PhysRegClass::GPR, 16, false},      {"cx"sv, PhysRegClass::GPR, 16, false},
            {"dx"sv, PhysRegClass::GPR, 16, false},      {"si"sv, PhysRegClass::GPR, 16, false},      {"di"sv, PhysRegClass::GPR, 16, false},
            {"bp"sv, PhysRegClass::GPR, 16, true},       {"sp"sv, PhysRegClass::GPR, 16, true},       {"r8w"sv, PhysRegClass::GPR, 16, false},
            {"r9w"sv, PhysRegClass::GPR, 16, false},     {"r10w"sv, PhysRegClass::GPR, 16, false},    {"r11w"sv, PhysRegClass::GPR, 16, false},
            {"r12w"sv, PhysRegClass::GPR, 16, false},    {"r13w"sv, PhysRegClass::GPR, 16, false},    {"r14w"sv, PhysRegClass::GPR, 16, false},
            {"r15w"sv, PhysRegClass::GPR, 16, false},

            {"al"sv, PhysRegClass::GPR, 8, false},       {"bl"sv, PhysRegClass::GPR, 8, false},       {"cl"sv, PhysRegClass::GPR, 8, false},
            {"dl"sv, PhysRegClass::GPR, 8, false},       {"sil"sv, PhysRegClass::GPR, 8, false},      {"dil"sv, PhysRegClass::GPR, 8, false},
            {"bpl"sv, PhysRegClass::GPR, 8, true},       {"spl"sv, PhysRegClass::GPR, 8, true},       {"r8b"sv, PhysRegClass::GPR, 8, false},
            {"r9b"sv, PhysRegClass::GPR, 8, false},      {"r10b"sv, PhysRegClass::GPR, 8, false},     {"r11b"sv, PhysRegClass::GPR, 8, false},
            {"r12b"sv, PhysRegClass::GPR, 8, false},     {"r13b"sv, PhysRegClass::GPR, 8, false},     {"r14b"sv, PhysRegClass::GPR, 8, false},
            {"r15b"sv, PhysRegClass::GPR, 8, false},

            {"ah"sv, PhysRegClass::GPR, 8, false},       {"bh"sv, PhysRegClass::GPR, 8, false},       {"ch"sv, PhysRegClass::GPR, 8, false},
            {"dh"sv, PhysRegClass::GPR, 8, false},

            {"xmm0"sv, PhysRegClass::XMM, 128, false},   {"xmm1"sv, PhysRegClass::XMM, 128, false},   {"xmm2"sv, PhysRegClass::XMM, 128, false},
            {"xmm3"sv, PhysRegClass::XMM, 128, false},   {"xmm4"sv, PhysRegClass::XMM, 128, false},   {"xmm5"sv, PhysRegClass::XMM, 128, false},
            {"xmm6"sv, PhysRegClass::XMM, 128, false},   {"xmm7"sv, PhysRegClass::XMM, 128, false},   {"xmm8"sv, PhysRegClass::XMM, 128, false},
            {"xmm9"sv, PhysRegClass::XMM, 128, false},   {"xmm10"sv, PhysRegClass::XMM, 128, false},  {"xmm11"sv, PhysRegClass::XMM, 128, false},
            {"xmm12"sv, PhysRegClass::XMM, 128, false},  {"xmm13"sv, PhysRegClass::XMM, 128, false},  {"xmm14"sv, PhysRegClass::XMM, 128, false},
            {"xmm15"sv, PhysRegClass::XMM, 128, false},

            {"ymm0"sv, PhysRegClass::XMM, 256, false},   {"ymm1"sv, PhysRegClass::XMM, 256, false},   {"ymm2"sv, PhysRegClass::XMM, 256, false},
            {"ymm3"sv, PhysRegClass::XMM, 256, false},   {"ymm4"sv, PhysRegClass::XMM, 256, false},   {"ymm5"sv, PhysRegClass::XMM, 256, false},
            {"ymm6"sv, PhysRegClass::XMM, 256, false},   {"ymm7"sv, PhysRegClass::XMM, 256, false},   {"ymm8"sv, PhysRegClass::XMM, 256, false},
            {"ymm9"sv, PhysRegClass::XMM, 256, false},   {"ymm10"sv, PhysRegClass::XMM, 256, false},  {"ymm11"sv, PhysRegClass::XMM, 256, false},
            {"ymm12"sv, PhysRegClass::XMM, 256, false},  {"ymm13"sv, PhysRegClass::XMM, 256, false},  {"ymm14"sv, PhysRegClass::XMM, 256, false},
            {"ymm15"sv, PhysRegClass::XMM, 256, false},

            {"zmm0"sv, PhysRegClass::XMM, 512, false},   {"zmm1"sv, PhysRegClass::XMM, 512, false},   {"zmm2"sv, PhysRegClass::XMM, 512, false},
            {"zmm3"sv, PhysRegClass::XMM, 512, false},   {"zmm4"sv, PhysRegClass::XMM, 512, false},   {"zmm5"sv, PhysRegClass::XMM, 512, false},
            {"zmm6"sv, PhysRegClass::XMM, 512, false},   {"zmm7"sv, PhysRegClass::XMM, 512, false},   {"zmm8"sv, PhysRegClass::XMM, 512, false},
            {"zmm9"sv, PhysRegClass::XMM, 512, false},   {"zmm10"sv, PhysRegClass::XMM, 512, false},  {"zmm11"sv, PhysRegClass::XMM, 512, false},
            {"zmm12"sv, PhysRegClass::XMM, 512, false},  {"zmm13"sv, PhysRegClass::XMM, 512, false},  {"zmm14"sv, PhysRegClass::XMM, 512, false},
            {"zmm15"sv, PhysRegClass::XMM, 512, false},

            {"st(0)"sv, PhysRegClass::ST, 80, false},    {"st(1)"sv, PhysRegClass::ST, 80, false},    {"st(2)"sv, PhysRegClass::ST, 80, false},
            {"st(3)"sv, PhysRegClass::ST, 80, false},    {"st(4)"sv, PhysRegClass::ST, 80, false},    {"st(5)"sv, PhysRegClass::ST, 80, false},
            {"st(6)"sv, PhysRegClass::ST, 80, false},    {"st(7)"sv, PhysRegClass::ST, 80, false},

            {"cs"sv, PhysRegClass::Seg, 16, true},       {"ds"sv, PhysRegClass::Seg, 16, true},       {"es"sv, PhysRegClass::Seg, 16, true},
            {"fs"sv, PhysRegClass::Seg, 16, true},       {"gs"sv, PhysRegClass::Seg, 16, true},       {"ss"sv, PhysRegClass::Seg, 16, true},

            {"eflags"sv, PhysRegClass::Flags, 32, true}, {"rflags"sv, PhysRegClass::Flags, 64, true},
        };

        inline constexpr PhysReg x86_regs[] = {
            {"eax"sv, PhysRegClass::GPR, 32, false},     {"ebx"sv, PhysRegClass::GPR, 32, false},   {"ecx"sv, PhysRegClass::GPR, 32, false},
            {"edx"sv, PhysRegClass::GPR, 32, false},     {"esi"sv, PhysRegClass::GPR, 32, false},   {"edi"sv, PhysRegClass::GPR, 32, false},
            {"ebp"sv, PhysRegClass::GPR, 32, true},      {"esp"sv, PhysRegClass::GPR, 32, true},

            {"ax"sv, PhysRegClass::GPR, 16, false},      {"bx"sv, PhysRegClass::GPR, 16, false},    {"cx"sv, PhysRegClass::GPR, 16, false},
            {"dx"sv, PhysRegClass::GPR, 16, false},      {"si"sv, PhysRegClass::GPR, 16, false},    {"di"sv, PhysRegClass::GPR, 16, false},
            {"bp"sv, PhysRegClass::GPR, 16, true},       {"sp"sv, PhysRegClass::GPR, 16, true},

            {"al"sv, PhysRegClass::GPR, 8, false},       {"bl"sv, PhysRegClass::GPR, 8, false},     {"cl"sv, PhysRegClass::GPR, 8, false},
            {"dl"sv, PhysRegClass::GPR, 8, false},

            {"ah"sv, PhysRegClass::GPR, 8, false},       {"bh"sv, PhysRegClass::GPR, 8, false},     {"ch"sv, PhysRegClass::GPR, 8, false},
            {"dh"sv, PhysRegClass::GPR, 8, false},

            {"xmm0"sv, PhysRegClass::XMM, 128, false},   {"xmm1"sv, PhysRegClass::XMM, 128, false}, {"xmm2"sv, PhysRegClass::XMM, 128, false},
            {"xmm3"sv, PhysRegClass::XMM, 128, false},   {"xmm4"sv, PhysRegClass::XMM, 128, false}, {"xmm5"sv, PhysRegClass::XMM, 128, false},
            {"xmm6"sv, PhysRegClass::XMM, 128, false},   {"xmm7"sv, PhysRegClass::XMM, 128, false},

            {"ymm0"sv, PhysRegClass::XMM, 256, false},   {"ymm1"sv, PhysRegClass::XMM, 256, false}, {"ymm2"sv, PhysRegClass::XMM, 256, false},
            {"ymm3"sv, PhysRegClass::XMM, 256, false},   {"ymm4"sv, PhysRegClass::XMM, 256, false}, {"ymm5"sv, PhysRegClass::XMM, 256, false},
            {"ymm6"sv, PhysRegClass::XMM, 256, false},   {"ymm7"sv, PhysRegClass::XMM, 256, false},

            {"zmm0"sv, PhysRegClass::XMM, 512, false},   {"zmm1"sv, PhysRegClass::XMM, 512, false}, {"zmm2"sv, PhysRegClass::XMM, 512, false},
            {"zmm3"sv, PhysRegClass::XMM, 512, false},   {"zmm4"sv, PhysRegClass::XMM, 512, false}, {"zmm5"sv, PhysRegClass::XMM, 512, false},
            {"zmm6"sv, PhysRegClass::XMM, 512, false},   {"zmm7"sv, PhysRegClass::XMM, 512, false},

            {"st(0)"sv, PhysRegClass::ST, 80, false},    {"st(1)"sv, PhysRegClass::ST, 80, false},  {"st(2)"sv, PhysRegClass::ST, 80, false},
            {"st(3)"sv, PhysRegClass::ST, 80, false},    {"st(4)"sv, PhysRegClass::ST, 80, false},  {"st(5)"sv, PhysRegClass::ST, 80, false},
            {"st(6)"sv, PhysRegClass::ST, 80, false},    {"st(7)"sv, PhysRegClass::ST, 80, false},

            {"cs"sv, PhysRegClass::Seg, 16, true},       {"ds"sv, PhysRegClass::Seg, 16, true},     {"es"sv, PhysRegClass::Seg, 16, true},
            {"fs"sv, PhysRegClass::Seg, 16, true},       {"gs"sv, PhysRegClass::Seg, 16, true},     {"ss"sv, PhysRegClass::Seg, 16, true},

            {"eflags"sv, PhysRegClass::Flags, 32, true},
        };

    } // namespace detail

    [[nodiscard]] std::span<PhysReg const> register_table(Arch arch) noexcept
    {
        using namespace detail;
        switch (arch)
        {
            case Arch::X86_64:
                return x86_64_regs;
            case Arch::X86:
                return x86_regs;
        }

        return {};
    }

    [[nodiscard]] PhysReg const* lookup_register(Arch arch, std::string_view name) noexcept
    {
        auto const table = register_table(arch);
        for (auto const& reg : table)
            if (reg.name == name)
                return &reg;

        return nullptr;
    }

} // namespace dcc::target
