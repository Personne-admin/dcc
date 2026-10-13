#include <llvm-c/TargetMachine.h>
#include <llvm/Target/TargetMachine.h>

extern "C" void dcc_llvm_set_sections(LLVMTargetMachineRef machine, bool functions, bool data)
{
    auto* target = reinterpret_cast<llvm::TargetMachine*>(machine);
    target->Options.FunctionSections = functions;
    target->Options.DataSections = data;
}
