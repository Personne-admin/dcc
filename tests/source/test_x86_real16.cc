import std;
import dcc.backend.x86.mir;
import dcc.backend.x86.encode;
import dcc.backend.x86.prefix;

#include "harness.hh"

using namespace dcc::backend::x86;

namespace
{
    MOp R(PhysReg r)
    {
        return MOp::from_reg(VReg::phys(r));
    }

    MOp I(std::int64_t v)
    {
        return MOp::from_imm(v);
    }

    MOp M(MMem m)
    {
        return MOp::from_mem(m);
    }

    MMem B(PhysReg base, std::int32_t disp = 0)
    {
        return MMem::make_base_disp(VReg::phys(base), disp);
    }

    MMem BI(PhysReg base, PhysReg index, std::uint8_t scale = 1, std::int32_t disp = 0)
    {
        return MMem::make_indexed(VReg::phys(base), VReg::phys(index), scale, disp);
    }

    MMem IX(PhysReg index, std::uint8_t scale, std::int32_t disp)
    {
        MMem m;
        m.index = VReg::phys(index);
        m.scale = scale;
        m.disp = disp;
        return m;
    }

    MMem D(std::int32_t disp)
    {
        MMem m;
        m.disp = disp;
        return m;
    }

    MMem A32(MMem m)
    {
        m.address_bits = 32;
        return m;
    }

    MMem Seg(MMem m, SegmentOverride s)
    {
        return m.with_segment(s);
    }

    MInstr X(MOpc opc, std::initializer_list<MOp> ops = {}, std::uint8_t defs = 0)
    {
        MInstr instr;
        instr.opc = opc;
        for (auto const& op : ops)
            instr.ops[instr.num_ops++] = op;
        instr.num_defs = defs;
        return instr;
    }

    std::string hex(std::span<std::uint8_t const> bytes)
    {
        std::string out;
        for (auto b : bytes)
            out += std::format("{:02x} ", b);
        return out;
    }

    std::optional<std::vector<std::uint8_t>> nasm16(std::string const& text)
    {
        static unsigned counter = 0;
        auto dir =
            std::filesystem::temp_directory_path() / std::format("dcc-real16-{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), counter++);
        std::filesystem::create_directories(dir);
        auto source = dir / "case.asm";
        auto output = dir / "case.bin";
        {
            std::ofstream file{source};
            file << "bits 16\n" << text << "\n";
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

    struct Case
    {
        std::string text;
        MInstr instr;
    };

    std::vector<Case> encodable_cases()
    {
        using P = PhysReg;
        using S = SegmentOverride;
        return {
            {"mov ax, bx", X(MOpc::MOV16rr, {R(P::RAX), R(P::RBX)}, 1)},
            {"mov eax, ebx", X(MOpc::MOV32rr, {R(P::RAX), R(P::RBX)}, 1)},
            {"mov al, bl", X(MOpc::MOV8rr, {R(P::RAX), R(P::RBX)}, 1)},
            {"mov eax, ebx", X(MOpc::COPY, {R(P::RAX), R(P::RBX)}, 1)},
            {"mov cx, 0x1234", X(MOpc::MOV16ri, {R(P::RCX), I(0x1234)}, 1)},
            {"mov edx, 0x12345678", X(MOpc::MOV32ri, {R(P::RDX), I(0x12345678)}, 1)},
            {"mov eax, -1", X(MOpc::MOV32ri, {R(P::RAX), I(-1)}, 1)},
            {"mov bl, 7", X(MOpc::MOV8ri, {R(P::RBX), I(7)}, 1)},
            {"mov ax, [bx+si+4]", X(MOpc::MOV16rm, {R(P::RAX), M(BI(P::RBX, P::RSI, 1, 4))}, 1)},
            {"mov ax, [si+bx]", X(MOpc::MOV16rm, {R(P::RAX), M(BI(P::RSI, P::RBX))}, 1)},
            {"mov cx, [bp]", X(MOpc::MOV16rm, {R(P::RCX), M(B(P::RBP))}, 1)},
            {"mov dx, [bp+di-2]", X(MOpc::MOV16rm, {R(P::RDX), M(BI(P::RBP, P::RDI, 1, -2))}, 1)},
            {"mov si, [di+0x300]", X(MOpc::MOV16rm, {R(P::RSI), M(B(P::RDI, 0x300))}, 1)},
            {"mov bx, [0x1234]", X(MOpc::MOV16rm, {R(P::RBX), M(D(0x1234))}, 1)},
            {"mov ax, [0x1234]", X(MOpc::MOV16rm, {R(P::RAX), M(D(0x1234))}, 1)},
            {"mov ax, [0xfff0]", X(MOpc::MOV16rm, {R(P::RAX), M(D(0xfff0))}, 1)},
            {"mov ax, fs:[0x10]", X(MOpc::MOV16rm, {R(P::RAX), M(Seg(D(0x10), S::FS))}, 1)},
            {"mov di, es:[bx]", X(MOpc::MOV16rm, {R(P::RDI), M(Seg(B(P::RBX), S::ES))}, 1)},
            {"mov cx, ss:[si]", X(MOpc::MOV16rm, {R(P::RCX), M(Seg(B(P::RSI), S::SS))}, 1)},
            {"mov ax, ds:[bp+2]", X(MOpc::MOV16rm, {R(P::RAX), M(Seg(B(P::RBP, 2), S::DS))}, 1)},
            {"mov cl, [bx+di]", X(MOpc::MOV8rm, {R(P::RCX), M(BI(P::RBX, P::RDI))}, 1)},
            {"mov eax, [bx]", X(MOpc::MOV32rm, {R(P::RAX), M(B(P::RBX))}, 1)},
            {"mov ecx, [bp+6]", X(MOpc::MOV32rm, {R(P::RCX), M(B(P::RBP, 6))}, 1)},
            {"mov edx, [si+0x1234]", X(MOpc::MOV32rm, {R(P::RDX), M(B(P::RSI, 0x1234))}, 1)},
            {"mov eax, [0x2000]", X(MOpc::MOV32rm, {R(P::RAX), M(D(0x2000))}, 1)},
            {"mov ebx, gs:[di]", X(MOpc::MOV32rm, {R(P::RBX), M(Seg(B(P::RDI), S::GS))}, 1)},
            {"mov eax, [ebx]", X(MOpc::MOV32rm, {R(P::RAX), M(A32(B(P::RBX)))}, 1)},
            {"mov ecx, [ebp+8]", X(MOpc::MOV32rm, {R(P::RCX), M(A32(B(P::RBP, 8)))}, 1)},
            {"mov edx, [esp+4]", X(MOpc::MOV32rm, {R(P::RDX), M(A32(B(P::RSP, 4)))}, 1)},
            {"mov ebx, [eax+ecx*4+0x10]", X(MOpc::MOV32rm, {R(P::RBX), M(A32(BI(P::RAX, P::RCX, 4, 0x10)))}, 1)},
            {"mov esi, [ecx*8+0x100]", X(MOpc::MOV32rm, {R(P::RSI), M(A32(IX(P::RCX, 8, 0x100)))}, 1)},
            {"mov eax, fs:[ecx]", X(MOpc::MOV32rm, {R(P::RAX), M(A32(Seg(B(P::RCX), S::FS)))}, 1)},
            {"mov ecx, [dword 0x12345]", X(MOpc::MOV32rm, {R(P::RCX), M(A32(D(0x12345)))}, 1)},
            {"mov eax, [dword 0x12345]", X(MOpc::MOV32rm, {R(P::RAX), M(A32(D(0x12345)))}, 1)},
            {"mov ax, [ebx+0x100000]", X(MOpc::MOV16rm, {R(P::RAX), M(A32(B(P::RBX, 0x100000)))}, 1)},
            {"mov [bx], eax", X(MOpc::MOV32mr, {M(B(P::RBX)), R(P::RAX)})},
            {"mov [bp-4], ecx", X(MOpc::MOV32mr, {M(B(P::RBP, -4)), R(P::RCX)})},
            {"mov [0x2000], eax", X(MOpc::MOV32mr, {M(D(0x2000)), R(P::RAX)})},
            {"mov [0x2000], edx", X(MOpc::MOV32mr, {M(D(0x2000)), R(P::RDX)})},
            {"mov [ebx+4], edx", X(MOpc::MOV32mr, {M(A32(B(P::RBX, 4))), R(P::RDX)})},
            {"mov gs:[esi], eax", X(MOpc::MOV32mr, {M(A32(Seg(B(P::RSI), S::GS))), R(P::RAX)})},
            {"mov [edi+eax*2-8], ebx", X(MOpc::MOV32mr, {M(A32(BI(P::RDI, P::RAX, 2, -8))), R(P::RBX)})},
            {"mov [si], ax", X(MOpc::MOV16mr, {M(B(P::RSI)), R(P::RAX)})},
            {"mov [bx+si+0x200], dx", X(MOpc::MOV16mr, {M(BI(P::RBX, P::RSI, 1, 0x200)), R(P::RDX)})},
            {"mov [si], al", X(MOpc::MOV8mr, {M(B(P::RSI)), R(P::RAX)})},
            {"mov [0x40], al", X(MOpc::MOV8mr, {M(D(0x40)), R(P::RAX)})},
            {"mov word [bx], 0x1234", X(MOpc::MOV16mi, {M(B(P::RBX)), I(0x1234)})},
            {"mov dword [bp-8], 0x12345678", X(MOpc::MOV32mi, {M(B(P::RBP, -8)), I(0x12345678)})},
            {"mov dword [ecx], 7", X(MOpc::MOV32mi, {M(A32(B(P::RCX))), I(7)})},
            {"mov byte [di], 5", X(MOpc::MOV8mi, {M(B(P::RDI)), I(5)})},
            {"movzx ax, bl", X(MOpc::MOVZX16_8rr, {R(P::RAX), R(P::RBX)}, 1)},
            {"movzx eax, bl", X(MOpc::MOVZX32_8rr, {R(P::RAX), R(P::RBX)}, 1)},
            {"movzx eax, cl", X(MOpc::MOVZX32rr8, {R(P::RAX), R(P::RCX)}, 1)},
            {"movzx eax, bx", X(MOpc::MOVZX32_16rr, {R(P::RAX), R(P::RBX)}, 1)},
            {"movsx ax, bl", X(MOpc::MOVSX16_8rr, {R(P::RAX), R(P::RBX)}, 1)},
            {"movsx eax, cl", X(MOpc::MOVSX32_8rr, {R(P::RAX), R(P::RCX)}, 1)},
            {"movsx eax, dx", X(MOpc::MOVSX32_16rr, {R(P::RAX), R(P::RDX)}, 1)},
            {"movzx ax, byte [bx]", X(MOpc::MOVZX16rm8, {R(P::RAX), M(B(P::RBX))}, 1)},
            {"movzx eax, byte [si]", X(MOpc::MOVZX32rm8, {R(P::RAX), M(B(P::RSI))}, 1)},
            {"movzx eax, word [bp+2]", X(MOpc::MOVZX32rm16, {R(P::RAX), M(B(P::RBP, 2))}, 1)},
            {"movsx ax, byte [di]", X(MOpc::MOVSX16rm8, {R(P::RAX), M(B(P::RDI))}, 1)},
            {"movsx eax, byte [bx]", X(MOpc::MOVSX32rm8, {R(P::RAX), M(B(P::RBX))}, 1)},
            {"movsx eax, word [ebx]", X(MOpc::MOVSX32rm16, {R(P::RAX), M(A32(B(P::RBX)))}, 1)},
            {"add ax, bx", X(MOpc::ADD16rr, {R(P::RAX), R(P::RAX), R(P::RBX)}, 1)},
            {"add ax, bx", X(MOpc::ADD16rr, {R(P::RAX), R(P::RBX), R(P::RAX)}, 1)},
            {"mov ax, bx\nadd ax, cx", X(MOpc::ADD16rr, {R(P::RAX), R(P::RBX), R(P::RCX)}, 1)},
            {"mov ecx, edx\nsub ecx, ebx", X(MOpc::SUB32rr, {R(P::RCX), R(P::RDX), R(P::RBX)}, 1)},
            {"sub cx, dx", X(MOpc::SUB16rr, {R(P::RCX), R(P::RCX), R(P::RDX)}, 1)},
            {"and eax, ebx", X(MOpc::AND32rr, {R(P::RAX), R(P::RAX), R(P::RBX)}, 1)},
            {"or si, di", X(MOpc::OR16rr, {R(P::RSI), R(P::RSI), R(P::RDI)}, 1)},
            {"xor eax, eax", X(MOpc::XOR32rr, {R(P::RAX), R(P::RAX), R(P::RAX)}, 1)},
            {"adc dx, bx", X(MOpc::ADC16rr, {R(P::RDX), R(P::RDX), R(P::RBX)}, 1)},
            {"sbb edx, ecx", X(MOpc::SBB32rr, {R(P::RDX), R(P::RDX), R(P::RCX)}, 1)},
            {"add al, bl", X(MOpc::ADD8rr, {R(P::RAX), R(P::RAX), R(P::RBX)}, 1)},
            {"sub cl, dl", X(MOpc::SUB8rr, {R(P::RCX), R(P::RCX), R(P::RDX)}, 1)},
            {"add ax, bx", X(MOpc::ADD16rr, {R(P::RAX), R(P::RBX)}, 1)},
            {"cmp ax, bx", X(MOpc::CMP16rr, {R(P::RAX), R(P::RBX)})},
            {"cmp eax, ecx", X(MOpc::CMP32rr, {R(P::RAX), R(P::RCX)})},
            {"cmp bl, cl", X(MOpc::CMP8rr, {R(P::RBX), R(P::RCX)})},
            {"cmp al, 5", X(MOpc::CMP8ri, {R(P::RAX), I(5)})},
            {"cmp bl, 0x90", X(MOpc::CMP8ri, {R(P::RBX), I(0x90)})},
            {"cmp ax, 5", X(MOpc::CMP16ri, {R(P::RAX), I(5)})},
            {"cmp ax, 1000", X(MOpc::CMP16ri, {R(P::RAX), I(1000)})},
            {"cmp bx, 1000", X(MOpc::CMP16ri, {R(P::RBX), I(1000)})},
            {"cmp eax, 0x12345", X(MOpc::CMP32ri, {R(P::RAX), I(0x12345)})},
            {"add eax, 0x12345", X(MOpc::ADD32ri, {R(P::RAX), I(0x12345)}, 1)},
            {"add ebx, -1", X(MOpc::ADD32ri, {R(P::RBX), I(-1)}, 1)},
            {"add ax, 0xffff", X(MOpc::ADD16ri, {R(P::RAX), I(0xffff)}, 1)},
            {"and bx, 0xff00", X(MOpc::AND16ri, {R(P::RBX), I(0xff00)}, 1)},
            {"mov dx, cx\nand dx, 15", X(MOpc::AND16ri, {R(P::RDX), R(P::RCX), I(15)}, 1)},
            {"xor al, 0x80", X(MOpc::XOR8ri, {R(P::RAX), I(0x80)}, 1)},
            {"add al, 5", X(MOpc::ADD8ri, {R(P::RAX), I(5)}, 1)},
            {"or bl, 1", X(MOpc::OR8ri, {R(P::RBX), I(1)}, 1)},
            {"sub cl, 3", X(MOpc::SUB8ri, {R(P::RCX), I(3)}, 1)},
            {"and dl, 0x7f", X(MOpc::AND8ri, {R(P::RDX), I(0x7f)}, 1)},
            {"sbb ax, 0", X(MOpc::SBB16ri, {R(P::RAX), I(0)}, 1)},
            {"adc eax, 0x10000", X(MOpc::ADC32ri, {R(P::RAX), I(0x10000)}, 1)},
            {"adc si, 1", X(MOpc::ADC16ri, {R(P::RSI), I(1)}, 1)},
            {"sbb ecx, 200", X(MOpc::SBB32ri, {R(P::RCX), I(200)}, 1)},
            {"or eax, 0x80000000", X(MOpc::OR32ri, {R(P::RAX), I(0x80000000)}, 1)},
            {"xor di, 0x1234", X(MOpc::XOR16ri, {R(P::RDI), I(0x1234)}, 1)},
            {"sub sp, 16", X(MOpc::SUB16ri, {R(P::RSP), I(16)}, 1)},
            {"add ax, [bx]", X(MOpc::ADD16rm, {R(P::RAX), M(B(P::RBX))}, 1)},
            {"sub eax, [bp-4]", X(MOpc::SUB32rm, {R(P::RAX), M(B(P::RBP, -4))}, 1)},
            {"cmp ax, [si]", X(MOpc::CMP16rm, {R(P::RAX), M(B(P::RSI))})},
            {"cmp eax, [ebx]", X(MOpc::CMP32rm, {R(P::RAX), M(A32(B(P::RBX)))})},
            {"cmp eax, [bx]", X(MOpc::CMP32rm, {R(P::RAX), R(P::RAX), M(B(P::RBX))})},
            {"and cx, [di+2]", X(MOpc::AND16rm, {R(P::RCX), M(B(P::RDI, 2))}, 1)},
            {"xor edx, [bx+si]", X(MOpc::XOR32rm, {R(P::RDX), M(BI(P::RBX, P::RSI))}, 1)},
            {"or ax, [0x1234]", X(MOpc::OR16rm, {R(P::RAX), M(D(0x1234))}, 1)},
            {"add ecx, es:[di]", X(MOpc::ADD32rm, {R(P::RCX), M(Seg(B(P::RDI), S::ES))}, 1)},
            {"cmp al, [bx]", X(MOpc::CMP8rm, {R(P::RAX), M(B(P::RBX))})},
            {"mov ax, cx\nadd ax, [bx]", X(MOpc::ADD16rm, {R(P::RAX), R(P::RCX), M(B(P::RBX))}, 1)},
            {"test ax, bx", X(MOpc::TEST16rr, {R(P::RAX), R(P::RBX)})},
            {"test eax, ecx", X(MOpc::TEST32rr, {R(P::RAX), R(P::RCX)})},
            {"test al, bl", X(MOpc::TEST8rr, {R(P::RAX), R(P::RBX)})},
            {"test al, 1", X(MOpc::TEST8ri, {R(P::RAX), I(1)})},
            {"test cl, 4", X(MOpc::TEST8ri, {R(P::RCX), I(4)})},
            {"test bx, 5", X(MOpc::TEST16ri, {R(P::RBX), I(5)})},
            {"test ax, 0x8000", X(MOpc::TEST16ri, {R(P::RAX), I(0x8000)})},
            {"test eax, 0x100", X(MOpc::TEST32ri, {R(P::RAX), I(0x100)})},
            {"not ax", X(MOpc::NOT16r, {R(P::RAX)}, 1)},
            {"not cl", X(MOpc::NOT8r, {R(P::RCX)}, 1)},
            {"neg eax", X(MOpc::NEG32r, {R(P::RAX)}, 1)},
            {"neg bl", X(MOpc::NEG8r, {R(P::RBX)}, 1)},
            {"mov dx, bx\nneg dx", X(MOpc::NEG16r, {R(P::RDX), R(P::RBX)}, 1)},
            {"mul bx", X(MOpc::MUL16r, {R(P::RBX)})},
            {"mul ecx", X(MOpc::MUL32r, {R(P::RCX)})},
            {"div si", X(MOpc::DIV16r, {R(P::RSI)})},
            {"div ebx", X(MOpc::DIV32r, {R(P::RBX)})},
            {"idiv di", X(MOpc::IDIV16r, {R(P::RDI)})},
            {"idiv ecx", X(MOpc::IDIV32r, {R(P::RCX)})},
            {"inc ax", X(MOpc::INC16r, {R(P::RAX)}, 1)},
            {"inc ebx", X(MOpc::INC32r, {R(P::RBX)}, 1)},
            {"dec cx", X(MOpc::DEC16r, {R(P::RCX)}, 1)},
            {"dec edi", X(MOpc::DEC32r, {R(P::RDI)}, 1)},
            {"imul ax, bx", X(MOpc::IMUL16rr, {R(P::RAX), R(P::RBX)}, 1)},
            {"imul eax, ecx", X(MOpc::IMUL32rr, {R(P::RAX), R(P::RAX), R(P::RCX)}, 1)},
            {"imul eax, ebx", X(MOpc::IMUL32rr, {R(P::RAX), R(P::RBX), R(P::RAX)}, 1)},
            {"mov ax, bx\nimul ax, cx", X(MOpc::IMUL16rr, {R(P::RAX), R(P::RBX), R(P::RCX)}, 1)},
            {"imul ax, bx, 5", X(MOpc::IMUL16rri, {R(P::RAX), R(P::RBX), I(5)}, 1)},
            {"imul eax, ecx, 1000", X(MOpc::IMUL32rri, {R(P::RAX), R(P::RCX), I(1000)}, 1)},
            {"imul dx, si, -3", X(MOpc::IMUL16rri, {R(P::RDX), R(P::RSI), I(-3)}, 1)},
            {"shl ax, cl", X(MOpc::SHL16rCL, {R(P::RAX), R(P::RCX)}, 1)},
            {"shr eax, cl", X(MOpc::SHR32rCL, {R(P::RAX), R(P::RCX)}, 1)},
            {"sar dx, cl", X(MOpc::SAR16rCL, {R(P::RDX), R(P::RCX)}, 1)},
            {"mov ax, bx\nshl ax, cl", X(MOpc::SHL16rCL, {R(P::RAX), R(P::RBX), R(P::RCX)}, 1)},
            {"shl ax, 1", X(MOpc::SHL16ri8, {R(P::RAX), I(1)}, 1)},
            {"shr bx, 4", X(MOpc::SHR16ri8, {R(P::RBX), I(4)}, 1)},
            {"sar eax, 31", X(MOpc::SAR32ri8, {R(P::RAX), I(31)}, 1)},
            {"shl ecx, 1", X(MOpc::SHL32ri8, {R(P::RCX), I(1)}, 1)},
            {"shld eax, ebx, cl", X(MOpc::SHLD32rrCL, {R(P::RAX), R(P::RBX), R(P::RCX)}, 1)},
            {"shrd edx, eax, cl", X(MOpc::SHRD32rrCL, {R(P::RDX), R(P::RAX), R(P::RCX)}, 1)},
            {"shld esi, edi, 7", X(MOpc::SHLD32rri8, {R(P::RSI), R(P::RDI), I(7)}, 1)},
            {"shrd edx, eax, 5", X(MOpc::SHRD32rri8, {R(P::RDX), R(P::RAX), I(5)}, 1)},
            {"setz al", X(MOpc::SETEr, {R(P::RAX)}, 1)},
            {"setnz bl", X(MOpc::SETNEr, {R(P::RBX)}, 1)},
            {"setl cl", X(MOpc::SETLr, {R(P::RCX)}, 1)},
            {"setge dl", X(MOpc::SETGEr, {R(P::RDX)}, 1)},
            {"setle al", X(MOpc::SETLEr, {R(P::RAX)}, 1)},
            {"setg al", X(MOpc::SETGr, {R(P::RAX)}, 1)},
            {"setb al", X(MOpc::SETBr, {R(P::RAX)}, 1)},
            {"setae al", X(MOpc::SETAEr, {R(P::RAX)}, 1)},
            {"setbe al", X(MOpc::SETBEr, {R(P::RAX)}, 1)},
            {"seta al", X(MOpc::SETAr, {R(P::RAX)}, 1)},
            {"cbw", X(MOpc::CBW)},
            {"cwd", X(MOpc::CWD)},
            {"cwde", X(MOpc::CWDE)},
            {"cdq", X(MOpc::CDQ)},
            {"lea ax, [bx+si+4]", X(MOpc::LEA16rm, {R(P::RAX), M(BI(P::RBX, P::RSI, 1, 4))}, 1)},
            {"lea si, [bp-2]", X(MOpc::LEA16rm, {R(P::RSI), M(B(P::RBP, -2))}, 1)},
            {"lea eax, [ebx+ecx*2+8]", X(MOpc::LEA32rm, {R(P::RAX), M(A32(BI(P::RBX, P::RCX, 2, 8)))}, 1)},
            {"lea eax, [bx+di]", X(MOpc::LEA32rm, {R(P::RAX), M(BI(P::RBX, P::RDI))}, 1)},
            {"push ax", X(MOpc::PUSH16r, {R(P::RAX)})},
            {"pop bx", X(MOpc::POP16r, {R(P::RBX)}, 1)},
            {"push eax", X(MOpc::PUSH32r, {R(P::RAX)})},
            {"pop edx", X(MOpc::POP32r, {R(P::RDX)}, 1)},
            {"push bp", X(MOpc::PUSH16r, {R(P::RBP)})},
            {"push 5", X(MOpc::PUSH16i, {I(5)})},
            {"push word 1000", X(MOpc::PUSH16i, {I(1000)})},
            {"push word -1", X(MOpc::PUSH16i, {I(-1)})},
            {"push es", X(MOpc::PUSHSEG, {R(P::ES)})},
            {"push cs", X(MOpc::PUSHSEG, {R(P::CS)})},
            {"push ss", X(MOpc::PUSHSEG, {R(P::SS)})},
            {"push ds", X(MOpc::PUSHSEG, {R(P::DS)})},
            {"push fs", X(MOpc::PUSHSEG, {R(P::FS)})},
            {"push gs", X(MOpc::PUSHSEG, {R(P::GS)})},
            {"pop es", X(MOpc::POPSEG, {R(P::ES)})},
            {"pop ss", X(MOpc::POPSEG, {R(P::SS)})},
            {"pop ds", X(MOpc::POPSEG, {R(P::DS)})},
            {"pop fs", X(MOpc::POPSEG, {R(P::FS)})},
            {"pop gs", X(MOpc::POPSEG, {R(P::GS)})},
            {"pushf", X(MOpc::PUSHF16)},
            {"popf", X(MOpc::POPF16)},
            {"cli", X(MOpc::CLI)},
            {"sti", X(MOpc::STI)},
            {"mov es, ax", X(MOpc::MOV16sr, {R(P::ES), R(P::RAX)})},
            {"mov ds, bx", X(MOpc::MOV16sr, {R(P::DS), R(P::RBX)})},
            {"mov ss, cx", X(MOpc::MOV16sr, {R(P::SS), R(P::RCX)})},
            {"mov fs, dx", X(MOpc::MOV16sr, {R(P::FS), R(P::RDX)})},
            {"mov gs, si", X(MOpc::MOV16sr, {R(P::GS), R(P::RSI)})},
            {"mov ax, es", X(MOpc::MOV16rs, {R(P::RAX), R(P::ES)}, 1)},
            {"mov bx, cs", X(MOpc::MOV16rs, {R(P::RBX), R(P::CS)}, 1)},
            {"mov di, gs", X(MOpc::MOV16rs, {R(P::RDI), R(P::GS)}, 1)},
            {"mov es, [bx]", X(MOpc::MOV16sm, {R(P::ES), M(B(P::RBX))})},
            {"mov fs, [0x10]", X(MOpc::MOV16sm, {R(P::FS), M(D(0x10))})},
            {"mov [bp-2], ds", X(MOpc::MOV16ms, {M(B(P::RBP, -2)), R(P::DS)})},
            {"les bx, [si]", X(MOpc::LES16rm, {R(P::RBX), M(B(P::RSI))}, 1)},
            {"les ebx, [bp+4]", X(MOpc::LES32rm, {R(P::RBX), M(B(P::RBP, 4))}, 1)},
            {"lfs si, [bx]", X(MOpc::LFS16rm, {R(P::RSI), M(B(P::RBX))}, 1)},
            {"lfs eax, [di+6]", X(MOpc::LFS32rm, {R(P::RAX), M(B(P::RDI, 6))}, 1)},
            {"lgs ax, [di]", X(MOpc::LGS16rm, {R(P::RAX), M(B(P::RDI))}, 1)},
            {"lgs ecx, [ebx]", X(MOpc::LGS32rm, {R(P::RCX), M(A32(B(P::RBX)))}, 1)},
            {"les di, [ecx]", X(MOpc::LES16rm, {R(P::RDI), M(A32(B(P::RCX)))}, 1)},
            {"rep movsb", X(MOpc::REPMOVS)},
            {"rep movsw", X(MOpc::REPMOVSW)},
            {"rep stosb", X(MOpc::REPSTOS)},
            {"rep stosw", X(MOpc::REPSTOSW)},
            {"a32 rep movsb", X(MOpc::REPMOVS_A32)},
            {"a32 rep movsw", X(MOpc::REPMOVSW_A32)},
            {"a32 rep stosb", X(MOpc::REPSTOS_A32)},
            {"a32 rep stosw", X(MOpc::REPSTOSW_A32)},
            {"call ax", X(MOpc::CALL, {R(P::RAX)})},
            {"call si", X(MOpc::CALL_rel16, {R(P::RSI)})},
            {"jmp bx", X(MOpc::JMP16r, {R(P::RBX)})},
            {"jmp word [bx+si]", X(MOpc::JMP16m, {M(BI(P::RBX, P::RSI))})},
            {"jmp word cs:[bx+0x100]", X(MOpc::JMP16m, {M(Seg(B(P::RBX, 0x100), S::CS))})},
            {"xchg [bx], ax", X(MOpc::XCHG16mr, {M(B(P::RBX)), R(P::RAX)})},
            {"xchg [si], al", X(MOpc::XCHG8mr, {M(B(P::RSI)), R(P::RAX)})},
            {"xchg [bp+2], ecx", X(MOpc::XCHG32mr, {M(B(P::RBP, 2)), R(P::RCX)})},
            {"ret", X(MOpc::RET)},
            {"nop", X(MOpc::NOP)},
        };
    }

    std::vector<std::uint8_t> encode16(MInstr const& instr, std::vector<std::string>* warnings = nullptr)
    {
        MFunction func;
        func.create_block("entry").instrs.push_back(instr);
        auto result = encode_function(func, EncodeMode::Real16);
        if (warnings)
            *warnings = result.warnings;
        return result.bytes;
    }
} // namespace

SECTION("x86: 16-bit encoding");

TEST_CASE("16-bit encodings match nasm byte for byte")
{
    auto cases = encodable_cases();
    REQUIRE(cases.size() > 200u);
    for (auto const& c : cases)
    {
        std::vector<std::string> warnings;
        auto bytes = encode16(c.instr, &warnings);
        auto expected = nasm16(c.text);
        REQUIRE(expected.has_value());
        bool const same = warnings.empty() && bytes == *expected;
        if (!same)
            std::println(std::cerr, "      {}: dcc [{}] nasm [{}] {}", c.text, hex(bytes), hex(*expected), warnings.empty() ? "" : warnings.front());
        CHECK(same);
    }
}

TEST_CASE("16-bit encodings never emit a rex byte before the opcode")
{
    using P = PhysReg;
    for (auto opc : {MOpc::MOV32rm, MOpc::MOV16rm})
    {
        auto bytes = encode16(X(opc, {R(P::RAX), M(B(P::RBX))}, 1));
        REQUIRE(!bytes.empty());
        for (auto b : bytes)
            CHECK(!(b >= 0x40 && b <= 0x4f));
    }
    auto store = encode16(X(MOpc::MOV32mr, {M(B(P::RBX)), R(P::RAX)}));
    CHECK(store == (std::vector<std::uint8_t>{0x66, 0x89, 0x07}));
    auto load32 = encode16(X(MOpc::MOV32rm, {R(P::RAX), M(A32(B(P::RBX)))}, 1));
    CHECK(load32 == (std::vector<std::uint8_t>{0x67, 0x66, 0x8b, 0x03}));
}

TEST_CASE("16-bit instructions without an encoding are rejected")
{
    using P = PhysReg;
    std::vector<MInstr> invalid = {
        X(MOpc::MOV64rr, {R(P::RAX), R(P::RBX)}, 1),
        X(MOpc::ADD64rr, {R(P::RAX), R(P::RAX), R(P::RBX)}, 1),
        X(MOpc::MOV16rr, {R(P::R8), R(P::RAX)}, 1),
        X(MOpc::MOV32rr, {R(P::RAX), R(P::R15)}, 1),
        X(MOpc::MOV8rr, {R(P::RSI), R(P::RAX)}, 1),
        X(MOpc::MOV8rr, {R(P::RAX), R(P::RDI)}, 1),
        X(MOpc::SETEr, {R(P::RSP)}, 1),
        X(MOpc::MOV16rm, {R(P::RAX), M(BI(P::RBX, P::RBP))}, 1),
        X(MOpc::MOV16rm, {R(P::RAX), M(BI(P::RSI, P::RDI))}, 1),
        X(MOpc::MOV16rm, {R(P::RAX), M(B(P::RAX))}, 1),
        X(MOpc::MOV16rm, {R(P::RAX), M(B(P::RSP))}, 1),
        X(MOpc::MOV16rm, {R(P::RAX), M(BI(P::RBX, P::RSI, 2))}, 1),
        X(MOpc::MOV16rm, {R(P::RAX), M(B(P::RBX, 0x10000))}, 1),
        X(MOpc::MOV32rm, {R(P::RAX), M(A32(BI(P::RAX, P::RSP)))}, 1),
        X(MOpc::MOV32rm, {R(P::RAX), M(A32(B(P::R9)))}, 1),
        X(MOpc::SUB16rr, {R(P::RAX), R(P::RBX), R(P::RAX)}, 1),
        X(MOpc::SHL16rCL, {R(P::RAX), R(P::RBX)}, 1),
        X(MOpc::SHL16rCL, {R(P::RCX), R(P::RAX), R(P::RCX)}, 1),
        X(MOpc::MOV16sr, {R(P::CS), R(P::RAX)}),
        X(MOpc::MOV16sm, {R(P::CS), M(B(P::RBX))}),
        X(MOpc::POPSEG, {R(P::CS)}),
        X(MOpc::PUSHSEG, {R(P::RAX)}),
        X(MOpc::LEA16rm, {R(P::RAX), M(Seg(B(P::RBX), SegmentOverride::ES))}, 1),
        X(MOpc::MOVSDrr, {R(P::XMM0), R(P::XMM1)}, 1),
        X(MOpc::LOCK_XADD16mr, {M(B(P::RBX)), R(P::RAX)}),
        X(MOpc::JUMP_TABLE, {R(P::RAX)}),
    };
    for (auto const& instr : invalid)
    {
        std::vector<std::string> warnings;
        auto bytes = encode16(instr, &warnings);
        bool const rejected = !warnings.empty() && bytes == (std::vector<std::uint8_t>{0x0f, 0x0b});
        if (!rejected)
            std::println(std::cerr, "      {} was accepted: [{}]", opc_name(instr.opc), hex(bytes));
        CHECK(rejected);
    }
    CHECK(!encode_single_instruction(X(MOpc::MOV64rr, {R(P::RAX), R(P::RBX)}, 1), EncodeMode::Real16).has_value());
}

TEST_CASE("16-bit symbol references produce 16 and 32-bit absolute and pc16 relocations")
{
    using P = PhysReg;
    auto symbolic = [](std::string_view symbol, std::int32_t disp) { return MMem::make_sym_reloc(symbol, disp); };
    MFunction func;
    auto& block = func.create_block("entry");
    block.instrs.push_back(X(MOpc::MOV16rm, {R(P::RAX), M(symbolic("near_value", 4))}, 1));
    block.instrs.push_back(X(MOpc::MOV16rm, {R(P::RCX), M([&] {
                                                 auto m = symbolic("table", 2);
                                                 m.base = VReg::phys(P::RBX);
                                                 return m;
                                             }())},
                             1));
    block.instrs.push_back(X(MOpc::MOV32rm, {R(P::RAX), M(A32(symbolic("big_value", 0)))}, 1));
    block.instrs.push_back(X(MOpc::MOV16ri, {R(P::RDX), I(0)}, 1));
    block.instrs.push_back(X(MOpc::CALL, {MOp::from_symbol("callee")}));
    block.instrs.push_back(X(MOpc::CALL_rel16, {MOp::from_symbol("other")}));
    block.instrs.push_back(X(MOpc::JMP, {MOp::from_symbol("tail")}));
    auto result = encode_function(func, EncodeMode::Real16);
    CHECK(result.warnings.empty());
    std::vector<std::uint8_t> expected = {0xa1, 0x00, 0x00, 0x8b, 0x8f, 0x00, 0x00, 0x67, 0x66, 0xa1, 0x00, 0x00, 0x00,
                                          0x00, 0xba, 0x00, 0x00, 0xe8, 0x00, 0x00, 0xe8, 0x00, 0x00, 0xe9, 0x00, 0x00};
    CHECK(result.bytes == expected);
    REQUIRE(result.relocs.size() == 6u);
    auto check = [&](std::size_t i, std::uint32_t offset, std::string_view symbol, Reloc::Kind kind, std::int64_t addend) {
        CHECK(result.relocs[i].offset == offset);
        CHECK(result.relocs[i].symbol == symbol);
        CHECK(result.relocs[i].kind == kind);
        CHECK(result.relocs[i].addend == addend);
    };
    check(0, 1, "near_value", Reloc::Kind::Abs16, 4);
    check(1, 5, "table", Reloc::Kind::Abs16, 2);
    check(2, 10, "big_value", Reloc::Kind::Abs32, 0);
    check(3, 18, "callee", Reloc::Kind::Rel16, -2);
    check(4, 21, "other", Reloc::Kind::Rel16, -2);
    check(5, 24, "tail", Reloc::Kind::Rel16, -2);
}

TEST_CASE("16-bit branches patch rel16 displacements like nasm")
{
    using P = PhysReg;
    MFunction func;
    auto& entry = func.create_block("entry");
    auto entry_id = entry.id;
    auto& one = func.create_block("one");
    auto one_id = one.id;
    auto& two = func.create_block("two");
    auto two_id = two.id;
    auto& done = func.create_block("done");
    auto done_id = done.id;
    auto block = [&](std::uint32_t id) -> MBlock& {
        for (auto& b : func.blocks)
            if (b.id == id)
                return b;
        return func.blocks.front();
    };
    block(entry_id).instrs.push_back(X(MOpc::CMP16ri, {R(P::RAX), I(0)}));
    block(entry_id).instrs.push_back(X(MOpc::JE, {MOp::from_label(two_id)}));
    block(one_id).instrs.push_back(X(MOpc::MOV16ri, {R(P::RAX), I(1)}, 1));
    block(one_id).instrs.push_back(X(MOpc::JMP, {MOp::from_label(done_id)}));
    block(two_id).instrs.push_back(X(MOpc::MOV16ri, {R(P::RAX), I(2)}, 1));
    block(two_id).instrs.push_back(X(MOpc::JL, {MOp::from_label(entry_id)}));
    block(two_id).instrs.push_back(X(MOpc::CALL, {MOp::from_label(done_id)}));
    block(done_id).instrs.push_back(X(MOpc::RET));
    auto result = encode_function(func, EncodeMode::Real16);
    CHECK(result.warnings.empty());
    CHECK(result.relocs.empty());
    auto expected = nasm16("top:\ncmp ax, 0\njz near two\nmov ax, 1\njmp near done\ntwo:\nmov ax, 2\njl near top\ncall done\ndone:\nret");
    REQUIRE(expected.has_value());
    if (result.bytes != *expected)
        std::println(std::cerr, "      dcc [{}] nasm [{}]", hex(result.bytes), hex(*expected));
    CHECK(result.bytes == *expected);
}
