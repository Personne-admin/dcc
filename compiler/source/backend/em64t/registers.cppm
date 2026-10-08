export module dcc.backend.em64t.registers;

import std;
import dcc.ir;
import dcc.target;
import dcc.backend.x86.mir;
import dcc.backend.x86.regalloc;

using namespace dcc::backend::x86;

namespace dcc::backend::em64t
{
    namespace
    {
        [[nodiscard]] bool is_win64(MFunction const& func, target::TargetConfig const& target)
        {
            if (target.os == dcc::target::Os::Windows)
                return true;

            if (target.object_format == dcc::target::ObjectFormat::Coff)
                return true;

            if (func.conv == ir::CallingConv::Win64)
                return true;

            return false;
        }

        constexpr x86::PhysReg kSysVGPR[] = {
            x86::PhysReg::RAX, x86::PhysReg::RCX, x86::PhysReg::RDX, x86::PhysReg::RSI, x86::PhysReg::RDI, x86::PhysReg::R8,  x86::PhysReg::R9,
            x86::PhysReg::R10, x86::PhysReg::RBX, x86::PhysReg::R12, x86::PhysReg::R13, x86::PhysReg::R14, x86::PhysReg::R15,
        };

        constexpr x86::PhysReg kWin64GPR[] = {
            x86::PhysReg::RAX, x86::PhysReg::RCX, x86::PhysReg::RDX, x86::PhysReg::R8,  x86::PhysReg::R9,  x86::PhysReg::R10, x86::PhysReg::RBX,
            x86::PhysReg::RDI, x86::PhysReg::RSI, x86::PhysReg::R12, x86::PhysReg::R13, x86::PhysReg::R14, x86::PhysReg::R15,
        };

        constexpr x86::PhysReg kXMM[] = {
            x86::PhysReg::XMM0, x86::PhysReg::XMM1, x86::PhysReg::XMM2,  x86::PhysReg::XMM3,  x86::PhysReg::XMM4,  x86::PhysReg::XMM5,  x86::PhysReg::XMM6,  x86::PhysReg::XMM7,
            x86::PhysReg::XMM8, x86::PhysReg::XMM9, x86::PhysReg::XMM10, x86::PhysReg::XMM11, x86::PhysReg::XMM12, x86::PhysReg::XMM13, x86::PhysReg::XMM14,
        };

        constexpr x86::PhysReg kSysVCalleeSavedGPR[] = {
            x86::PhysReg::RBX, x86::PhysReg::R12, x86::PhysReg::R13, x86::PhysReg::R14, x86::PhysReg::R15,
        };

        constexpr x86::PhysReg kWin64CalleeSavedGPR[] = {
            x86::PhysReg::RBX, x86::PhysReg::RDI, x86::PhysReg::RSI, x86::PhysReg::R12, x86::PhysReg::R13, x86::PhysReg::R14, x86::PhysReg::R15,
        };
        constexpr x86::PhysReg kWin64CalleeSavedXMM[] = {
            x86::PhysReg::XMM6, x86::PhysReg::XMM7, x86::PhysReg::XMM8, x86::PhysReg::XMM9, x86::PhysReg::XMM10,
            x86::PhysReg::XMM11, x86::PhysReg::XMM12, x86::PhysReg::XMM13, x86::PhysReg::XMM14, x86::PhysReg::XMM15,
        };

        constexpr x86::PhysReg kGprScratchOrder[] = {
            x86::PhysReg::R11, x86::PhysReg::R10, x86::PhysReg::RAX, x86::PhysReg::RCX, x86::PhysReg::RDX, x86::PhysReg::R8,  x86::PhysReg::R9,
            x86::PhysReg::RSI, x86::PhysReg::RDI, x86::PhysReg::RBX, x86::PhysReg::R12, x86::PhysReg::R13, x86::PhysReg::R14, x86::PhysReg::R15,
        };

        constexpr x86::PhysReg kXmmScratchOrder[] = {
            x86::PhysReg::XMM15, x86::PhysReg::XMM14, x86::PhysReg::XMM13, x86::PhysReg::XMM12,
            x86::PhysReg::XMM11, x86::PhysReg::XMM10, x86::PhysReg::XMM9,  x86::PhysReg::XMM8,
        };

        [[nodiscard]] x86::RegisterPolicy get_reg_set(MFunction const& func, target::TargetConfig const& target)
        {
            bool w64 = is_win64(func, target);
            return x86::RegisterPolicy{
                .gpr =
                    {
                        .cls = x86::RegClass::GPR,
                        .bits = 64,
                        .allocatable = w64 ? std::span<x86::PhysReg const>{kWin64GPR} : std::span<x86::PhysReg const>{kSysVGPR},
                        .callee_saved = w64 ? std::span<x86::PhysReg const>{kWin64CalleeSavedGPR} : std::span<x86::PhysReg const>{kSysVCalleeSavedGPR},
                        .scratch_order = std::span<x86::PhysReg const>{kGprScratchOrder},
                        .spill = {8, 8, x86::MOpc::MOV64mr, x86::MOpc::MOV64rm},
                        .cycle_temp = {8, 8, x86::MOpc::MOV64mr, x86::MOpc::MOV64rm},
                        .move = x86::MOpc::COPY,
                    },
                .xmm =
                    {
                        .cls = x86::RegClass::XMM,
                        .bits = 128,
                        .allocatable = std::span<x86::PhysReg const>{kXMM},
                        .callee_saved = w64 ? std::span<x86::PhysReg const>{kWin64CalleeSavedXMM} : std::span<x86::PhysReg const>{},
                        .scratch_order = std::span<x86::PhysReg const>{kXmmScratchOrder},
                        .spill = {8, 8, x86::MOpc::MOVSD_mr, x86::MOpc::MOVSD_rm},
                        .cycle_temp = {8, 8, x86::MOpc::MOVSDmr, x86::MOpc::MOVSD_rm},
                        .move = x86::MOpc::MOVSDrr,
                    },
                .jump_table_scratch = x86::PhysReg::R10,
                .gpr_save = x86::MOpc::PUSH64r,
                .gpr_restore = x86::MOpc::POP64r,
                .xmm_save = {16, 16, x86::MOpc::MOVAPSmr, x86::MOpc::MOVAPSrm},
                .is_win64 = w64,
            };
        }

    }
}

export namespace dcc::backend::em64t
{
    void regalloc(x86::MFunction& func, target::TargetConfig const& target)
    {
        x86::allocate_registers(func, get_reg_set(func, target));
    }
}
