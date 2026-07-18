export module dcc.backend;

import std;
import dcc.ir;
import dcc.ir.pass;
import dcc.target;
import dcc.sm;

export namespace dcc::backend
{
    enum class ArtifactKind : std::uint8_t
    {
        LlvmIrText,
        MirText,
        AsmText,
        ObjectBytes,
        ExecutableBytes,
        ArchiveBytes,
    };

    enum class DebugFormat : std::uint8_t
    {
        Auto,
        None,
        Dwarf,
        Pdb,
    };

    struct BackendDiagnostic
    {
        sm::SourceRange where;
        std::string message;
    };

    struct BackendArtifact
    {
        std::optional<std::string> llvm_ir_text;
        std::optional<std::string> mir_text;
        std::optional<std::string> asm_text;
        std::optional<std::vector<std::byte>> object_bytes;
        std::optional<std::vector<std::byte>> executable_bytes;
        std::optional<std::vector<std::byte>> archive_bytes;
        std::vector<BackendDiagnostic> diagnostics;
    };

    struct BackendOptions
    {
        target::TargetConfig target;
        std::set<ArtifactKind> requested_artifacts;
        std::vector<std::string> additional_objects;
        std::vector<std::string> library_paths;
        std::vector<std::string> libraries;
        std::vector<std::string> linker_args;
        bool emit_debug_info{false};
        DebugFormat debug_format{DebugFormat::Auto};
        bool omit_frame_pointer{true};
        sm::SourceManager const* source_manager{};
        dcc::ir::pass::OptLevel opt_level{dcc::ir::pass::OptLevel::O0};
    };

    class Backend
    {
    public:
        Backend() = default;
        virtual ~Backend() = default;
        Backend(Backend const&) = delete;
        Backend& operator=(Backend const&) = delete;

        [[nodiscard]] virtual std::string_view name() const = 0;
        [[nodiscard]] virtual std::set<ArtifactKind> supported_artifacts() const = 0;

        [[nodiscard]] virtual BackendArtifact emit(ir::IrModule const& module, BackendOptions const& options) = 0;
    };

} // namespace dcc::backend
