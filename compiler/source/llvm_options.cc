#include <cstdio>
#include <string>
#include <utility>

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

extern "C" void dcc_llvm_set_sections(LLVMTargetMachineRef machine, bool functions, bool data)
{
    auto* target = reinterpret_cast<llvm::TargetMachine*>(machine);
    target->Options.FunctionSections = functions;
    target->Options.DataSections = data;
}

extern "C" int dcc_llvm_run_passes(LLVMModuleRef module_ref, LLVMTargetMachineRef machine_ref, char const* pipeline, int restrict_library,
                                   char* error_text, unsigned long error_size)
{
    auto* module = reinterpret_cast<llvm::Module*>(module_ref);
    auto* machine = reinterpret_cast<llvm::TargetMachine*>(machine_ref);

    llvm::LoopAnalysisManager loop_analyses;
    llvm::FunctionAnalysisManager function_analyses;
    llvm::CGSCCAnalysisManager cgscc_analyses;
    llvm::ModuleAnalysisManager module_analyses;

    llvm::PassBuilder builder(machine);

    if (restrict_library)
    {
        llvm::TargetLibraryInfoImpl library_info{llvm::Triple{module->getTargetTriple()}};
        library_info.disableAllFunctions();
        library_info.setAvailable(llvm::LibFunc_memcpy);
        library_info.setAvailable(llvm::LibFunc_memmove);
        library_info.setAvailable(llvm::LibFunc_memset);
        library_info.setAvailable(llvm::LibFunc_memcmp);
        function_analyses.registerPass(
            [library_info = std::move(library_info)]() mutable { return llvm::TargetLibraryAnalysis(std::move(library_info)); });
    }

    builder.registerModuleAnalyses(module_analyses);
    builder.registerCGSCCAnalyses(cgscc_analyses);
    builder.registerFunctionAnalyses(function_analyses);
    builder.registerLoopAnalyses(loop_analyses);
    builder.crossRegisterProxies(loop_analyses, function_analyses, cgscc_analyses, module_analyses);

    llvm::ModulePassManager passes;
    if (auto error = builder.parsePassPipeline(passes, pipeline))
    {
        auto message = llvm::toString(std::move(error));
        std::snprintf(error_text, error_size, "%s", message.c_str());
        return 1;
    }

    passes.run(*module, module_analyses);
    return 0;
}
