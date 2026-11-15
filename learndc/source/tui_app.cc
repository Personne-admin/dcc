module;

#include "ftxui.hpp"

module learndc.tui;

import std;
import learndc.app;

namespace learndc::tui
{
    int run(app::Context& ctx)
    {
        (void)ctx;
        std::println(std::cerr, "learndc: the interactive interface is not available yet; run 'learndc --help' to see the command line.");
        return 0;
    }
} // namespace learndc::tui
