#include <ftxui.hpp>

import std;
import learndc.bundle;
import dcc.host.process;

int main()
{
    auto document = ftxui::text("learndc") | ftxui::border;
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(20), ftxui::Dimension::Fit(document));
    ftxui::Render(screen, document);
    std::println("{}", screen.ToString());
    std::println("bundle bytes: {}", learndc::bundle::raw().size());
    return 0;
}
