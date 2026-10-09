export module dcc.backend.i8086.registers;

import std;
import dcc.backend.x86.mir;
import dcc.backend.x86.regalloc;

using namespace dcc::backend::x86;

namespace dcc::backend::i8086
{
    namespace
    {
        constexpr PhysReg kGpr[] = {PhysReg::RAX, PhysReg::RCX, PhysReg::RDX, PhysReg::RBX, PhysReg::RSI};

        constexpr PhysReg kCalleeSavedGpr[] = {PhysReg::RSI, PhysReg::RDI};

        constexpr PhysReg kGprScratchOrder[] = {PhysReg::RDI, PhysReg::RAX, PhysReg::RCX, PhysReg::RDX, PhysReg::RBX, PhysReg::RSI};

        [[nodiscard]] RegisterPolicy register_policy()
        {
            return RegisterPolicy{
                .gpr =
                    {
                        .cls = RegClass::GPR,
                        .bits = 32,
                        .allocatable = std::span<PhysReg const>{kGpr},
                        .callee_saved = std::span<PhysReg const>{kCalleeSavedGpr},
                        .scratch_order = std::span<PhysReg const>{kGprScratchOrder},
                        .spill = {4, 2, MOpc::MOV32mr, MOpc::MOV32rm},
                        .cycle_temp = {4, 2, MOpc::MOV32mr, MOpc::MOV32rm},
                        .move = MOpc::COPY,
                    },
                .xmm =
                    {
                        .cls = RegClass::XMM,
                        .bits = 128,
                        .allocatable = {},
                        .callee_saved = {},
                        .scratch_order = {},
                        .spill = {16, 16, MOpc::MOVAPSmr, MOpc::MOVAPSrm},
                        .cycle_temp = {16, 16, MOpc::MOVAPSmr, MOpc::MOVAPSrm},
                        .move = MOpc::MOVAPSrr,
                    },
                .jump_table_scratch = PhysReg::RDI,
                .gpr_save = MOpc::PUSH32r,
                .gpr_restore = MOpc::POP32r,
                .xmm_save = {16, 16, MOpc::MOVAPSmr, MOpc::MOVAPSrm},
                .is_win64 = false,
            };
        }

    } // namespace

} // namespace dcc::backend::i8086

export namespace dcc::backend::i8086
{
    void regalloc(MFunction& func)
    {
        allocate_registers(func, register_policy());
    }

} // namespace dcc::backend::i8086
