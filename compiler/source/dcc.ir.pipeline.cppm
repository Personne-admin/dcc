export module dcc.ir.pipeline;

import std;
export import dcc.ir.pass;
import dcc.ir.transforms;

export namespace dcc::ir::pass
{
    [[nodiscard]] PassManager& global_pass_manager()
    {
        static PassManager pm = [] {
            PassManager manager;
            register_builtin_passes(manager);
            return manager;
        }();
        return pm;
    }

}
