import std;
import learndc.platform;
import learndc.app;
import learndc.cli;
import learndc.tui;

namespace
{
    std::atomic<bool> g_cancel{false};

    void on_interrupt(int)
    {
        g_cancel.store(true);
    }
} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    learndc::cli::Io io;
    io.tui = [](learndc::app::Context& ctx) { return learndc::tui::run(ctx); };
    io.cancel = &g_cancel;
    bool const interactive = args.empty() && io.stdin_tty && io.stdout_tty;
    if (!interactive)
    {
        std::signal(2, on_interrupt);
        std::signal(15, on_interrupt);
        std::signal(1, on_interrupt);
    }
    int const code = learndc::cli::run(args, io);
    if (g_cancel.load() && code == 0)
        return 130;
    return code;
}
