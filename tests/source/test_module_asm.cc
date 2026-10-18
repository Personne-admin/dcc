import std;
import dcc.ir;
import dcc.sm;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;
import dcc.backend.x86.prefix;
import dcc.backend.x86.modasm;

#include "harness.hh"

using namespace dcc::backend::x86;

namespace
{
    std::string hex(std::span<std::uint8_t const> bytes)
    {
        std::string out;
        for (auto b : bytes)
            out += std::format("{:02x} ", b);
        return out;
    }

    std::optional<std::vector<std::uint8_t>> nasm(unsigned bits, std::string const& text)
    {
        static unsigned counter = 0;
        auto dir =
            std::filesystem::temp_directory_path() / std::format("dcc-modasm-{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), counter++);
        std::filesystem::create_directories(dir);
        auto source = dir / "case.asm";
        auto output = dir / "case.bin";
        {
            std::ofstream file{source};
            file << "bits " << bits << "\n" << text << "\n";
        }
        auto command = std::format("nasm -f bin -o '{}' '{}' > '{}' 2>&1", output.string(), source.string(), (dir / "log").string());
        std::optional<std::vector<std::uint8_t>> result;
        if (std::system(command.c_str()) == 0)
        {
            std::ifstream in{output, std::ios::binary};
            result = std::vector<std::uint8_t>{std::istreambuf_iterator<char>{in}, {}};
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return result;
    }

    std::expected<ModuleAsmObject, ModuleAsmError> assemble(std::string const& text, EncodeMode mode, bool intel = false,
                                                            ModuleAsmFormat format = ModuleAsmFormat::Elf)
    {
        static std::pmr::unsynchronized_pool_resource pool;
        std::pmr::polymorphic_allocator<> alloc{&pool};
        auto source = intel ? ".intel_syntax noprefix\n" + text + "\n.att_syntax prefix" : text;
        auto* block = alloc.new_object<dcc::ir::IrModuleAsm>(std::pmr::string{source, alloc}, dcc::ir::IrAsmDialect::Att, dcc::sm::SourceRange{}, alloc);
        std::array<dcc::ir::IrModuleAsm*, 1> blocks{block};
        return assemble_module_asm(blocks, mode, format);
    }

    ModuleAsmSection const* find_section(ModuleAsmObject const& object, std::string_view name)
    {
        for (auto const& s : object.sections)
            if (s.name == name)
                return &s;
        return nullptr;
    }

    ModuleAsmSymbol const* find_symbol(ModuleAsmObject const& object, std::string_view name)
    {
        for (auto const& s : object.symbols)
            if (s.name == name)
                return &s;
        return nullptr;
    }

    struct OracleCase
    {
        std::string source;
        std::string nasm;
        bool intel;
    };

    void check_against_nasm(EncodeMode mode, unsigned bits, std::vector<OracleCase> const& cases)
    {
        for (auto const& c : cases)
        {
            auto object = assemble(".text\n" + c.source, mode, c.intel);
            if (!object)
            {
                CHECK(object.has_value());
                std::println(std::cerr, "      {}: {}", c.source, object.error().message);
                continue;
            }
            auto const* text = find_section(*object, ".text");
            auto expected = nasm(bits, c.nasm);
            REQUIRE(expected.has_value());
            if (!text || text->data != *expected || !text->relocations.empty())
            {
                CHECK(false);
                std::println(std::cerr, "      {}: dcc [{}] nasm [{}]", c.source, text ? hex(text->data) : std::string{"<none>"}, hex(*expected));
            }
            else
                CHECK(true);
        }
    }

} // namespace

SECTION("module asm");

TEST_CASE("16-bit module asm instructions match nasm byte for byte")
{
    check_against_nasm(EncodeMode::Real16, 16,
                       {
                           {"cld", "cld", true},
                           {"std", "std", true},
                           {"cli", "cli", true},
                           {"sti", "sti", true},
                           {"hlt", "hlt", true},
                           {"int 0x10", "int 0x10", true},
                           {"in al, dx", "in al, dx", true},
                           {"in ax, dx", "in ax, dx", true},
                           {"in eax, dx", "in eax, dx", true},
                           {"in al, 0x60", "in al, 0x60", true},
                           {"in ax, 0x60", "in ax, 0x60", true},
                           {"out dx, al", "out dx, al", true},
                           {"out dx, ax", "out dx, ax", true},
                           {"out dx, eax", "out dx, eax", true},
                           {"out 0x80, al", "out 0x80, al", true},
                           {"out 0x80, eax", "out 0x80, eax", true},
                           {"mov ax, 0x1234", "mov ax, 0x1234", true},
                           {"mov eax, 0x12345678", "mov eax, 0x12345678", true},
                           {"mov ds, ax", "mov ds, ax", true},
                           {"mov ax, es", "mov ax, es", true},
                           {"mov es, word ptr [bx + 2]", "mov es, [bx+2]", true},
                           {"mov al, byte ptr [bp + 4]", "mov al, [bp+4]", true},
                           {"mov word ptr [si], 5", "mov word [si], 5", true},
                           {"mov dword ptr [ebx + 8], eax", "mov [ebx+8], eax", true},
                           {"mov ax, word ptr es:[di]", "mov ax, [es:di]", true},
                           {"mov ax, word ptr [fs:bx]", "mov ax, [fs:bx]", true},
                           {"add ax, cx", "add ax, cx", true},
                           {"add eax, 100000", "add eax, 100000", true},
                           {"sub cx, 1", "sub cx, 1", true},
                           {"xor eax, eax", "xor eax, eax", true},
                           {"cmp ax, word ptr [bx]", "cmp ax, [bx]", true},
                           {"test al, 0x20", "test al, 0x20", true},
                           {"shl ax, 1", "shl ax, 1", true},
                           {"sar ecx, 3", "sar ecx, 3", true},
                           {"shr dx, cl", "shr dx, cl", true},
                           {"push ax", "push ax", true},
                           {"pop eax", "pop eax", true},
                           {"push es", "push es", true},
                           {"pop ds", "pop ds", true},
                           {"push 0x1234", "push 0x1234", true},
                           {"lea si, [bx + di + 4]", "lea si, [bx+di+4]", true},
                           {"addr32 rep stosb", "a32 rep stosb", true},
                           {"rep movsw", "rep movsw", true},
                           {"rep stosb", "rep stosb", true},
                           {"movzx eax, al", "movzx eax, al", true},
                           {"movsx ax, byte ptr [bx]", "movsx ax, byte [bx]", true},
                           {"inc cx", "inc cx", true},
                           {"neg ax", "neg ax", true},
                           {"not al", "not al", true},
                           {"mul cx", "mul cx", true},
                           {"div ecx", "div ecx", true},
                           {"imul ax, cx, 10", "imul ax, cx, 10", true},
                           {"ret", "ret", true},
                           {"movw %ax, %bx", "mov bx, ax", false},
                           {"outb %al, %dx", "out dx, al", false},
                           {"inb $0x60, %al", "in al, 0x60", false},
                           {"movl $0x12345678, %eax", "mov eax, 0x12345678", false},
                           {"movb 4(%bp), %al", "mov al, [bp+4]", false},
                           {"movw %es:(%di), %ax", "mov ax, [es:di]", false},
                           {"pushw %es", "push es", false},
                           {"int $0x10", "int 0x10", false},
                           {"addr32 rep stosb", "a32 rep stosb", false},
                       });
}

TEST_CASE("64-bit module asm instructions match nasm byte for byte")
{
    check_against_nasm(EncodeMode::Long64, 64,
                       {
                           {"mov rax, rdi", "mov rax, rdi", true},
                           {"mov eax, 5", "mov eax, 5", true},
                           {"mov rax, qword ptr [rdi + 8]", "mov rax, [rdi+8]", true},
                           {"mov qword ptr [rsp + 16], rcx", "mov [rsp+16], rcx", true},
                           {"add rax, rbx", "add rax, rbx", true},
                           {"xor eax, eax", "xor eax, eax", true},
                           {"lea rax, [rdi + rsi*4 + 8]", "lea rax, [rdi+rsi*4+8]", true},
                           {"push rbx", "push rbx", true},
                           {"pop rbx", "pop rbx", true},
                           {"ret", "ret", true},
                           {"hlt", "hlt", true},
                           {"cli", "cli", true},
                           {"sti", "sti", true},
                           {"cld", "cld", true},
                           {"in al, dx", "in al, dx", true},
                           {"in ax, dx", "in ax, dx", true},
                           {"out dx, eax", "out dx, eax", true},
                           {"out 0x80, al", "out 0x80, al", true},
                           {"out 0x80, ax", "out 0x80, ax", true},
                           {"int 0x80", "int 0x80", true},
                           {"test rdi, rdi", "test rdi, rdi", true},
                           {"neg rax", "neg rax", true},
                           {"imul rax, rbx", "imul rax, rbx", true},
                           {"mov rax, qword ptr fs:[0x28]", "mov rax, [fs:0x28]", true},
                           {"movq %rdi, %rax", "mov rax, rdi", false},
                           {"leaq (%rdi,%rsi), %rax", "lea rax, [rdi+rsi]", false},
                           {"addq %rcx, %rax", "add rax, rcx", false},
                           {"movq 8(%rsp), %rax", "mov rax, [rsp+8]", false},
                           {"pushq %rbx", "push rbx", false},
                           {"call *%rax", "call rax", false},
                           {"jmp *%rax", "jmp rax", false},
                           {"outb %al, $0x80", "out 0x80, al", false},
                       });
}

TEST_CASE("module asm resolves local branches and relocates external references")
{
    auto object = assemble(".code16\n.text\nstart:\n    jmp 1f\n    call external\n1:\n    jz start\n    ret\n", EncodeMode::Real16, true);
    REQUIRE(object.has_value());
    auto const* text = find_section(*object, ".text");
    REQUIRE(text != nullptr);
    CHECK_EQ(hex(text->data), std::string{"e9 03 00 e8 00 00 0f 84 f6 ff c3 "});
    REQUIRE(text->relocations.size() == 1);
    CHECK_EQ(text->relocations[0].symbol, std::string{"external"});
    CHECK(text->relocations[0].kind == Reloc::Kind::Rel16);
    CHECK_EQ(text->relocations[0].offset, std::uint64_t{4});
    CHECK_EQ(text->relocations[0].addend, std::int64_t{-2});
    auto const* start = find_symbol(*object, "start");
    REQUIRE(start != nullptr);
    CHECK(start->binding == ModuleAsmBinding::Local);
    CHECK(find_symbol(*object, ".Ldcc.num.1.0") == nullptr);
}

TEST_CASE("module asm relocates symbolic immediates and memory operands")
{
    auto object = assemble(".text\n    mov edi, offset table + 4\n    mov ax, word ptr [table]\n    push offset table\n.data\ntable:\n    .word 1\n",
                           EncodeMode::Real16, true);
    REQUIRE(object.has_value());
    auto const* text = find_section(*object, ".text");
    REQUIRE(text != nullptr);
    CHECK_EQ(hex(text->data), std::string{"66 bf 00 00 00 00 a1 00 00 68 00 00 "});
    REQUIRE(text->relocations.size() == 3);
    CHECK(text->relocations[0].kind == Reloc::Kind::Abs32);
    CHECK_EQ(text->relocations[0].offset, std::uint64_t{2});
    CHECK_EQ(text->relocations[0].addend, std::int64_t{4});
    CHECK(text->relocations[1].kind == Reloc::Kind::Abs16);
    CHECK_EQ(text->relocations[1].offset, std::uint64_t{7});
    CHECK(text->relocations[2].kind == Reloc::Kind::Abs16);
    CHECK_EQ(text->relocations[2].offset, std::uint64_t{10});
    CHECK_EQ(text->relocations[2].symbol, std::string{"table"});

    auto wide = assemble(".text\n    lea rax, [rip + value + 8]\n    mov rcx, qword ptr [rip + value]\n    movabs_target:\n    mov rdx, offset value\n",
                         EncodeMode::Long64, true);
    REQUIRE(wide.has_value());
    auto const* code = find_section(*wide, ".text");
    REQUIRE(code != nullptr);
    REQUIRE(code->relocations.size() == 3);
    CHECK(code->relocations[0].kind == Reloc::Kind::Rel32);
    CHECK_EQ(code->relocations[0].addend, std::int64_t{4});
    CHECK(code->relocations[1].kind == Reloc::Kind::Rel32);
    CHECK_EQ(code->relocations[1].addend, std::int64_t{-4});
    CHECK(code->relocations[2].kind == Reloc::Kind::Abs64);
    CHECK_EQ(code->relocations[2].offset, code->data.size() - 8);
}

TEST_CASE("module asm data directives, sections and symbols")
{
    auto object = assemble(".section .start, \"ax\", @progbits\n.globl _start\n.type _start, @function\n_start:\n    nop\n    .p2align 2\n"
                           ".section .rodata\nfirst:\n    .byte 1, 'A', -1\n    .ascii \"a\\tb\\n\"\n    .asciz \"z\"\nsecond:\n    .short second - first\n"
                           "    .long 0x11223344\n    .quad external - 8\n    .zero 3, 0xAA\n.equ SIZE, 4 * 2 + 1\n    .byte SIZE\n"
                           ".bss\nbuffer:\n    .zero 32\n.section .unknown\n.word 1\n",
                           EncodeMode::Long64);
    REQUIRE(object.has_value());
    auto const* start = find_section(*object, ".start");
    REQUIRE(start != nullptr);
    CHECK(start->alloc && start->exec && !start->write && !start->nobits);
    CHECK_EQ(hex(start->data), std::string{"90 90 90 90 "});
    CHECK_EQ(start->alignment, std::uint64_t{4});
    auto const* rodata = find_section(*object, ".rodata");
    REQUIRE(rodata != nullptr);
    CHECK(rodata->alloc && !rodata->exec && !rodata->write);
    CHECK_EQ(hex(rodata->data), std::string{"01 41 ff 61 09 62 0a 7a 00 09 00 44 33 22 11 00 00 00 00 00 00 00 00 aa aa aa 09 "});
    REQUIRE(rodata->relocations.size() == 1);
    CHECK(rodata->relocations[0].kind == Reloc::Kind::Abs64);
    CHECK_EQ(rodata->relocations[0].offset, std::uint64_t{15});
    CHECK_EQ(rodata->relocations[0].addend, std::int64_t{-8});
    auto const* bss = find_section(*object, ".bss");
    REQUIRE(bss != nullptr);
    CHECK(bss->nobits && bss->write);
    CHECK_EQ(bss->size, std::uint64_t{32});
    CHECK(bss->data.empty());
    auto const* unknown = find_section(*object, ".unknown");
    REQUIRE(unknown != nullptr);
    CHECK(!unknown->alloc && !unknown->write && !unknown->exec);
    auto const* entry = find_symbol(*object, "_start");
    REQUIRE(entry != nullptr);
    CHECK(entry->binding == ModuleAsmBinding::Global);
    CHECK(entry->type == ModuleAsmSymbolType::Function);
    CHECK(find_symbol(*object, "buffer") != nullptr);
    CHECK_EQ(module_asm_external_references(*object), std::vector<std::string>{"external"});
}

TEST_CASE("module asm listings reassemble with nasm")
{
    auto object =
        assemble(".section .start, \"ax\", @progbits\n.globl _start\n_start:\n    call dcc_main\n    jmp .Ldone\n    mov edi, offset __bss_start\n.Ldone:\n"
                 "    jmp .Ldone\n.data\nvalue:\n    .word 7, value\n",
                 EncodeMode::Real16, true);
    REQUIRE(object.has_value());
    auto text = module_asm_intel_text(*object, ModuleAsmFormat::Elf);
    CHECK(text.find("dw dcc_main - 2 - $") != std::string::npos);
    CHECK(text.find("dd __bss_start") != std::string::npos);
    CHECK(text.find("dw value") != std::string::npos);
    CHECK(text.find("    ; call dcc_main") != std::string::npos);
    auto dir = std::filesystem::temp_directory_path() / std::format("dcc-modasm-listing-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    {
        std::ofstream file{dir / "listing.asm"};
        file << "bits 16\nextern dcc_main\nextern __bss_start\n" << text;
    }
    auto command = std::format("nasm -f elf32 -o '{0}/listing.o' '{0}/listing.asm' > '{0}/log' 2>&1", dir.string());
    CHECK_EQ(std::system(command.c_str()), 0);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("module asm reports unsupported input with its line")
{
    struct Bad
    {
        std::string text;
        EncodeMode mode;
        bool intel;
        ModuleAsmFormat format;
        std::string message;
    };
    std::vector<Bad> const cases{
        {".text\n    nop\n    cpuid", EncodeMode::Long64, true, ModuleAsmFormat::Elf,
         "module asm line 4: instruction 'cpuid' is not supported by the custom backend"},
        {".text\n    jmp .Lmissing", EncodeMode::Long64, true, ModuleAsmFormat::Elf, "undefined local label '.Lmissing'"},
        {".code32", EncodeMode::Real16, false, ModuleAsmFormat::Elf, "'.code32' does not match the target's code mode"},
        {".data\n    nop", EncodeMode::Long64, false, ModuleAsmFormat::Elf, "instruction in non-executable section '.data'"},
        {".text\n    mov rax, qword ptr [value]", EncodeMode::Long64, true, ModuleAsmFormat::Elf,
         "absolute symbol addresses are not supported in 64-bit module asm; use rip-relative addressing"},
        {".type f, @function", EncodeMode::Long64, false, ModuleAsmFormat::Coff, "'.type' is only supported for ELF targets"},
        {".text\n    mov ah, 1", EncodeMode::Real16, true, ModuleAsmFormat::Elf, "registers ah, bh, ch and dh are not supported by the custom backend"},
        {".text\n    mov rax, rbx", EncodeMode::Real16, true, ModuleAsmFormat::Elf, "64-bit registers are not available in 16-bit code"},
        {".weak missing", EncodeMode::Long64, false, ModuleAsmFormat::Elf, "weak symbol 'missing' must be defined in module asm"},
        {".text\nlabel:\nlabel:", EncodeMode::Long64, false, ModuleAsmFormat::Elf, "symbol 'label' is already defined"},
        {".byte 300", EncodeMode::Long64, false, ModuleAsmFormat::Elf, "value 300 does not fit 1 bytes"},
        {".hidden x", EncodeMode::Long64, false, ModuleAsmFormat::Elf, "directive '.hidden' is not supported by the custom backend"},
        {".text\n    lock add rax, rbx", EncodeMode::Long64, true, ModuleAsmFormat::Elf, "prefix 'lock' is not supported by the custom backend"},
    };
    for (auto const& c : cases)
    {
        auto object = assemble(c.text, c.mode, c.intel, c.format);
        CHECK(!object.has_value());
        if (!object && object.error().message.find(c.message) == std::string::npos)
        {
            CHECK(false);
            std::println(std::cerr, "      expected '{}' in '{}'", c.message, object.error().message);
        }
    }
}

TEST_CASE("module asm encodes alu rip-relative memory sources")
{
    auto object = assemble(".text\n    add rax, qword ptr [rip + value]", EncodeMode::Long64, true);
    REQUIRE(object.has_value());
    auto const* text = find_section(*object, ".text");
    REQUIRE(text != nullptr);
    CHECK_EQ(hex(text->data), std::string{"48 03 05 00 00 00 00 "});
    REQUIRE(text->relocations.size() == 1);
    CHECK_EQ(text->relocations[0].symbol, std::string{"value"});
    CHECK(text->relocations[0].kind == Reloc::Kind::Rel32);
    CHECK_EQ(text->relocations[0].addend, std::int64_t{-4});
}
