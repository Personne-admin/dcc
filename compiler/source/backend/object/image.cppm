export module dcc.backend.object.image;

import std;
import dcc.ir;
import dcc.sm;
import dcc.target;
import dcc.backend.x86.encode;
import dcc.backend.x86.modasm;

export namespace dcc::backend::object
{
    enum class ImageRole
    {
        Startup,
        Code,
        Rodata,
        Relro,
        Data,
        Bss,
        Stack,
        Metadata,
        Unknown
    };
    enum class ImageSpace
    {
        Unknown,
        Code,
        Dgroup,
        Absolute
    };
    enum class ImageExpression
    {
        SymbolOffset,
        CodeOffset,
        PcRelative,
        SegmentBase,
        LayoutSize
    };
    enum class ImageUnits
    {
        Bytes,
        Paragraphs
    };
    enum class ImageBinding
    {
        Local,
        Global,
        Weak
    };
    enum class ImageSymbolKind
    {
        Unknown,
        Code,
        Data,
        Absolute,
        SegmentValue
    };

    using FragmentId = std::size_t;
    using SymbolId = std::size_t;

    struct ImageFixup
    {
        std::uint64_t offset{};
        std::uint64_t width{};
        SymbolId target{};
        std::int64_t addend{};
        ImageExpression expression{ImageExpression::SymbolOffset};
        ImageUnits units{ImageUnits::Bytes};
        ImageSpace space{ImageSpace::Unknown};
        x86::Reloc::Kind encoding{x86::Reloc::Kind::Abs16};
        std::string legacy_name;
        ir::IrGlobalRef const* reference{};
        sm::SourceRange range{};
    };

    struct ImageFragment
    {
        FragmentId id{};
        std::string name;
        std::string section;
        ImageRole role{ImageRole::Unknown};
        target::CodeModel model{};
        std::uint64_t alignment{1};
        std::vector<std::uint8_t> bytes;
        std::uint64_t memory_size{};
        std::vector<ImageFixup> fixups;
        ir::IrFunction const* function{};
        ir::IrGlobal const* global{};
        std::optional<std::size_t> asm_section;
        std::optional<FragmentId> associated_function;
        std::uint32_t source_file_id{};
        std::int32_t source_line{};
        bool alloc{true};
        bool write{};
        bool exec{};
        bool nobits{};
        bool retain{};
        bool opaque{};
        bool requires_ds_ss{true};
        std::string required_cpu;
    };

    struct ImageSymbol
    {
        SymbolId id{};
        std::string name;
        std::optional<FragmentId> fragment;
        std::uint64_t offset{};
        std::uint64_t size{};
        ImageBinding binding{ImageBinding::Local};
        ImageSymbolKind kind{ImageSymbolKind::Unknown};
        ir::IrFunction const* function{};
        ir::IrGlobal const* global{};
        std::optional<std::uint32_t> block;
    };

    struct ImageDescription
    {
        std::vector<ImageFragment> fragments;
        std::vector<ImageSymbol> symbols;
        std::vector<FragmentId> functions;
        std::vector<FragmentId> assembly_sections;
        x86::ModuleAsmObject assembly;
        std::vector<ir::IrModuleAsm const*> assembly_sources;

        [[nodiscard]] ImageFragment const* initializer(ir::IrValue const* value, ir::IrType const* type) const
        {
            for (auto const& fragment : fragments)
                if (fragment.global && fragment.global->init == value && fragment.global->type == type)
                    return &fragment;
            return nullptr;
        }
    };
}
