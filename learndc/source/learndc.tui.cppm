export module learndc.tui;

import std;
import learndc.app;
import learndc.tui.model;

export namespace learndc::tui
{
    int run(app::Context& ctx);

    [[nodiscard]] std::string render_snapshot(model::Model const& m, int width, int height, bool ansi = false);

    [[nodiscard]] std::shared_ptr<void> render_element(model::Model const& m);
}
