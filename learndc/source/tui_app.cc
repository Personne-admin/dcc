module;

#include "ftxui.hpp"

module learndc.tui;

import std;
import learndc.text;
import learndc.platform;
import learndc.catalog;
import learndc.runner;
import learndc.doctor;
import learndc.editor;
import learndc.watch;
import learndc.app;
import learndc.tui.model;

namespace learndc::tui
{
    namespace
    {
        [[nodiscard]] std::optional<std::string> key_name(ftxui::Event const& e)
        {
            struct Named
            {
                ftxui::Event const* event;
                char const* name;
            };
            static Named const table[] = {
                {&ftxui::Event::ArrowUp, "Up"},       {&ftxui::Event::ArrowDown, "Down"},      {&ftxui::Event::ArrowLeft, "Left"},
                {&ftxui::Event::ArrowRight, "Right"}, {&ftxui::Event::Return, "Enter"},        {&ftxui::Event::Escape, "Esc"},
                {&ftxui::Event::Tab, "Tab"},          {&ftxui::Event::TabReverse, "ShiftTab"}, {&ftxui::Event::Backspace, "Backspace"},
                {&ftxui::Event::PageUp, "PageUp"},    {&ftxui::Event::PageDown, "PageDown"},   {&ftxui::Event::Home, "Home"},
                {&ftxui::Event::End, "End"},          {&ftxui::Event::CtrlC, "CtrlC"},
            };
            for (auto const& n : table)
                if (e == *n.event)
                    return std::string(n.name);
            if (e.is_character())
            {
                auto c = e.character();
                if (!c.empty())
                    return c;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string wall_clock()
        {
            std::time_t const now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            return std::format("{:02}:{:02}:{:02}", local.tm_hour, local.tm_min, local.tm_sec);
        }

        class Shell
        {
        public:
            explicit Shell(app::Context& context) : m_ctx(context), m_model(context, initial_size()), m_start(std::chrono::steady_clock::now())
            {
                for (auto const& w : context.warnings)
                    m_model.push_toast(text::sanitize(w), true);
            }

            int run()
            {
                auto screen = ftxui::App::Fullscreen();
                m_screen = &screen;
                screen.TrackMouse(false);
                screen.ForceHandleCtrlC(false);
                auto root = ftxui::CatchEvent(ftxui::Renderer([this] { return view(); }), [this](ftxui::Event e) { return on_event(e); });
                m_ticker = std::jthread([this](std::stop_token stop) { tick_loop(stop); });
                screen.Loop(root);
                shutdown();
                m_screen = nullptr;
                return app::exit_ok;
            }

        private:
            app::Context& m_ctx;
            model::Model m_model;
            std::chrono::steady_clock::time_point m_start;
            ftxui::App* m_screen = nullptr;
            std::mutex m_mutex;
            std::deque<model::Event> m_inbox;
            std::atomic<bool> m_need_tick{false};
            std::atomic<bool> m_check_cancel{false};
            std::atomic<bool> m_shutdown{false};
            std::jthread m_ticker;
            std::jthread m_check;
            std::jthread m_doctor;
            std::jthread m_editor;
            std::jthread m_watch;
            std::shared_ptr<std::atomic<bool>> m_watch_stop;

            [[nodiscard]] static model::Size initial_size()
            {
                auto t = ftxui::Terminal::Size();
                if (t.dimx <= 0 || t.dimy <= 0)
                    return model::Size{80, 24};
                return model::Size{t.dimx, t.dimy};
            }

            [[nodiscard]] std::int64_t now_ms() const
            {
                return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count();
            }

            void post(model::Event event)
            {
                {
                    std::lock_guard lock(m_mutex);
                    m_inbox.push_back(std::move(event));
                }
                if (m_screen)
                    m_screen->PostEvent(ftxui::Event::Custom);
            }

            void update_tick() { m_need_tick.store(m_model.check_running() || !m_model.toasts.empty() || m_model.doctor_running); }

            void dispatch(model::Event const& event)
            {
                (void)m_model.handle(model::Tick{now_ms(), wall_clock()});
                apply(m_model.handle(event));
                update_tick();
            }

            ftxui::Element view()
            {
                auto const t = ftxui::Terminal::Size();
                if (t.dimx > 0 && t.dimy > 0 && (t.dimx != m_model.size.width || t.dimy != m_model.size.height))
                    dispatch(model::Resize{t.dimx, t.dimy});
                return std::static_pointer_cast<ftxui::Node>(render_element(m_model));
            }

            bool on_event(ftxui::Event const& e)
            {
                if (e == ftxui::Event::Custom)
                {
                    std::deque<model::Event> pending;
                    {
                        std::lock_guard lock(m_mutex);
                        pending.swap(m_inbox);
                    }
                    (void)m_model.handle(model::Tick{now_ms(), wall_clock()});
                    for (auto const& event : pending)
                        dispatch(event);
                    update_tick();
                    return true;
                }
                if (auto name = key_name(e))
                {
                    dispatch(model::Key{*name});
                    return true;
                }
                return false;
            }

            void tick_loop(std::stop_token stop)
            {
                while (!stop.stop_requested())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (m_need_tick.load() && m_screen)
                        m_screen->PostEvent(ftxui::Event::Custom);
                }
            }

            void apply(std::vector<model::Effect> const& effects)
            {
                for (auto const& effect : effects)
                    std::visit([this](auto const& e) { run_effect(e); }, effect);
            }

            void run_effect(model::StartCheck const& e)
            {
                auto const* ex = m_ctx.catalog.find(e.id);
                if (!ex)
                    return;
                if (m_check.joinable())
                    m_check.join();
                m_check_cancel.store(false);
                auto options = app::runner_options(m_ctx, runner::Backend::Dpm, &m_check_cancel,
                                                   [this](std::string_view stage) { post(model::CheckStage{std::string(stage)}); });
                m_check = std::jthread([this, ex, root = e.root, options = std::move(options)] {
                    auto report = runner::run_checks(*ex, root, options);
                    post(model::CheckDone{std::move(report)});
                });
            }

            void run_effect(model::CancelCheck const&) { m_check_cancel.store(true); }

            void run_effect(model::RunProgram const& e)
            {
                auto const* ex = m_ctx.catalog.find(e.id);
                if (!ex || !m_screen)
                    return;
                auto options = app::runner_options(m_ctx, runner::Backend::Dpm, nullptr);
                m_screen->Post(m_screen->WithRestoredIO([this, ex, root = e.root, options = std::move(options)] {
                    std::cout << "\x1b[2J\x1b[3J\x1b[H--- Running " << text::sanitize(ex->title) << " ---\n" << std::flush;
                    auto outcome = runner::build_and_run_interactive(*ex, root, options);
                    if (!outcome.ok && !outcome.diagnostics.empty())
                        std::cout << text::sanitize(outcome.diagnostics) << "\n";
                    std::string message;
                    if (outcome.ok)
                        message = "Your program finished";
                    else if (outcome.cancelled)
                        message = "The program was stopped";
                    else if (!outcome.message.empty())
                        message = text::sanitize(outcome.message);
                    else if (outcome.exit_code)
                        message = std::format("Your program exited with code {}", *outcome.exit_code);
                    else
                        message = "Your program did not finish";
                    std::cout << "\n--- " << message << " ---\nPress Enter to return to learndc.\n" << std::flush;
                    std::string line;
                    std::getline(std::cin, line);
                    post(model::ProgramDone{outcome.ok, message});
                }));
            }

            void run_effect(model::LaunchEditor const& e)
            {
                editor::Placeholders ph;
                ph.file = e.file;
                ph.workspace = e.workspace;
                ph.line = e.line;
                ph.column = e.column;
                editor::LaunchOptions lo;
                lo.folder = e.folder;
                auto const mode = editor::mode_of(e.choice);
                auto const choice = e.choice;
                auto finish = [this](editor::LaunchResult const& r) {
                    std::string message = r.message;
                    if (!r.ok)
                        message += " (change the editor in Settings)";
                    else if (r.mode == editor::Mode::Terminal)
                        message.clear();
                    post(model::EditorDone{r.ok, std::move(message)});
                };
                if (mode == editor::Mode::Terminal && m_screen)
                {
                    m_screen->Post(m_screen->WithRestoredIO([finish, choice, ph, lo] { finish(editor::launch(choice, ph, lo)); }));
                    return;
                }
                if (mode == editor::Mode::GuiWait)
                {
                    if (m_editor.joinable())
                        m_editor.join();
                    m_editor = std::jthread([finish, choice, ph, lo] { finish(editor::launch(choice, ph, lo)); });
                    return;
                }
                finish(editor::launch(choice, ph, lo));
            }

            void stop_watch()
            {
                if (m_watch_stop)
                    m_watch_stop->store(true);
                if (m_watch.joinable())
                    m_watch.join();
                m_watch_stop.reset();
            }

            void run_effect(model::StartWatch const& e)
            {
                stop_watch();
                m_watch_stop = std::make_shared<std::atomic<bool>>(false);
                auto const debounce = std::chrono::milliseconds(m_ctx.settings.checks_watch_debounce_ms);
                m_watch = std::jthread([this, root = e.root, stop = m_watch_stop, debounce] {
                    watch::Options options;
                    options.debounce = debounce;
                    options.cancel = stop.get();
                    watch::Watcher watcher(root, options);
                    while (!stop->load())
                    {
                        auto changes = watcher.wait();
                        if (!changes)
                            break;
                        post(model::FilesChanged{});
                    }
                });
            }

            void run_effect(model::StopWatch const&) { stop_watch(); }

            void run_effect(model::RunDoctor const& e)
            {
                if (m_doctor.joinable())
                    m_doctor.join();
                doctor::DoctorOptions options;
                options.dirs = m_ctx.dirs;
                options.settings = m_ctx.settings;
                options.env = m_ctx.env;
                options.tools = m_ctx.tools;
                options.content = &m_ctx.source;
                options.os = m_ctx.os;
                options.build = e.build;
                options.cancel = &m_shutdown;
                options.stdin_tty = true;
                options.stdout_tty = true;
                options.terminal = platform::TerminalSize{m_model.size.width, m_model.size.height};
                m_doctor = std::jthread([this, options = std::move(options)] { post(model::DoctorDone{doctor::run_doctor(options)}); });
            }

            void run_effect(model::Quit const&)
            {
                if (m_screen)
                    m_screen->Exit();
            }

            void shutdown()
            {
                m_shutdown.store(true);
                m_check_cancel.store(true);
                m_ticker.request_stop();
                if (m_ticker.joinable())
                    m_ticker.join();
                stop_watch();
                for (auto* t : {&m_check, &m_doctor, &m_editor})
                    if (t->joinable())
                        t->join();
            }
        };
    } // namespace

    int run(app::Context& ctx)
    {
        if (auto failure = app::require_content(ctx))
        {
            std::println(std::cerr, "learndc: {}", failure->message);
            return failure->code;
        }
        Shell shell(ctx);
        return shell.run();
    }
} // namespace learndc::tui
