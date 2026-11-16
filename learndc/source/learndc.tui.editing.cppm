export module learndc.tui.editing;

import std;
import learndc.text;
import learndc.platform;
import learndc.workspace;
import learndc.textedit;
import learndc.highlight;

export namespace learndc::tui::editing
{
    enum class SidePanel
    {
        Task,
        Results,
        Hidden,
    };

    struct OpenFile
    {
        std::string rel;
        textedit::Buffer buf;
        textedit::View view;
        highlight::Language language = highlight::Language::Plain;
        workspace::FileStamp disk;
        bool conflict = false;
        std::string read_only_reason;
        std::uint64_t draft_revision = 0;
    };

    struct FindBar
    {
        bool active = false;
        std::string query;
        bool match_case = false;
        std::string status;
    };

    struct Picker
    {
        bool active = false;
        std::string filter;
        std::size_t sel = 0;
    };

    struct EditorState
    {
        bool active = false;
        std::vector<OpenFile> files;
        std::size_t current = 0;
        std::string clipboard;
        FindBar find;
        Picker picker;
        SidePanel side = SidePanel::Task;
        int indent_width = 4;
        bool insert_spaces = true;
        int page_height = 20;
        int text_width = 60;
        std::int64_t last_draft_ms = 0;
        bool help = false;

        [[nodiscard]] OpenFile* open_file() { return current < files.size() ? &files[current] : nullptr; }

        [[nodiscard]] OpenFile const* open_file() const { return current < files.size() ? &files[current] : nullptr; }

        [[nodiscard]] bool any_modified() const
        {
            return std::ranges::any_of(files, [](OpenFile const& f) { return f.buf.modified(); });
        }

        [[nodiscard]] std::vector<std::string> modified_names() const
        {
            std::vector<std::string> out;
            for (auto const& f : files)
                if (f.buf.modified())
                    out.push_back(f.rel);
            return out;
        }

        [[nodiscard]] std::optional<std::size_t> index_of(std::string_view rel) const
        {
            for (std::size_t i = 0; i < files.size(); ++i)
                if (files[i].rel == rel)
                    return i;
            return std::nullopt;
        }
    };

    [[nodiscard]] inline bool protected_file(std::string_view rel)
    {
        return rel == "project.lock.dcon" || rel.ends_with("/project.lock.dcon");
    }

    [[nodiscard]] inline std::filesystem::path draft_path(workspace::Workspace const& ws, std::string_view rel)
    {
        return ws.root / std::string(workspace::meta_dir) / "drafts" / platform::path_from_utf8(std::string(rel) + ".draft");
    }

    [[nodiscard]] inline std::optional<std::string> read_draft(workspace::Workspace const& ws, std::string_view rel)
    {
        auto const path = draft_path(ws, rel);
        if (platform::is_symlink_or_reparse(path))
            return std::nullopt;
        return platform::read_file(path);
    }

    inline void remove_draft(workspace::Workspace const& ws, std::string_view rel)
    {
        std::error_code ec;
        std::filesystem::remove(draft_path(ws, rel), ec);
    }

    inline void configure(textedit::Buffer& buf, EditorState const& state)
    {
        buf.indent_width = state.indent_width;
        buf.insert_spaces = state.insert_spaces;
    }

    [[nodiscard]] inline std::expected<std::size_t, std::string> open(EditorState& state, workspace::Workspace const& ws, std::string const& rel)
    {
        if (auto at = state.index_of(rel))
        {
            state.current = *at;
            return *at;
        }
        auto bytes = workspace::read_learner_file(ws, rel);
        if (!bytes)
            return std::unexpected(bytes.error().message);
        OpenFile f;
        f.rel = rel;
        f.language = highlight::language_for(rel);
        f.disk = workspace::stamp_of_bytes(*bytes);
        auto const loaded = f.buf.load(*bytes, textedit::Limits{});
        if (!loaded.editable)
        {
            f.read_only_reason = loaded.reason;
            f.buf.read_only = true;
        }
        else if (protected_file(rel))
        {
            f.read_only_reason = "dpm writes this lock file; change project.dcon and let dpm update it";
            f.buf.read_only = true;
        }
        configure(f.buf, state);
        f.draft_revision = f.buf.revision();
        state.files.push_back(std::move(f));
        state.current = state.files.size() - 1;
        return state.current;
    }

    [[nodiscard]] inline bool draft_differs(workspace::Workspace const& ws, OpenFile const& f)
    {
        if (f.buf.read_only)
            return false;
        auto draft = read_draft(ws, f.rel);
        return draft && *draft != f.buf.text();
    }

    inline bool restore_draft(workspace::Workspace const& ws, OpenFile& f)
    {
        auto draft = read_draft(ws, f.rel);
        if (!draft || f.buf.read_only)
            return false;
        f.buf.select_all();
        f.buf.paste(*draft);
        f.buf.buffer_start(false);
        return true;
    }

    enum class SaveStatus
    {
        Saved,
        Unchanged,
        Conflict,
        ReadOnly,
        Failed,
    };

    struct SaveResult
    {
        SaveStatus status = SaveStatus::Unchanged;
        std::string message;
    };

    [[nodiscard]] inline SaveResult save(EditorState& state, workspace::Workspace const& ws, std::size_t index, bool force = false)
    {
        SaveResult r;
        if (index >= state.files.size())
            return r;
        auto& f = state.files[index];
        if (f.buf.read_only)
        {
            r.status = SaveStatus::ReadOnly;
            r.message = std::format("{} is read-only: {}", f.rel, f.read_only_reason);
            return r;
        }
        if (!f.buf.modified() && !force)
            return r;
        auto const now = workspace::stamp(ws, f.rel);
        if (!force && now != f.disk)
        {
            f.conflict = true;
            r.status = SaveStatus::Conflict;
            r.message = std::format("{} changed on disk since you opened it", f.rel);
            return r;
        }
        auto written = workspace::write_learner_file(ws, f.rel, f.buf.text());
        if (!written)
        {
            r.status = SaveStatus::Failed;
            r.message = std::format("{} was not saved: {}. Your edits are still here.", f.rel, written.error().message);
            return r;
        }
        f.disk = *written;
        f.conflict = false;
        f.buf.mark_saved();
        f.draft_revision = f.buf.revision();
        remove_draft(ws, f.rel);
        r.status = SaveStatus::Saved;
        r.message = std::format("Saved {}", f.rel);
        return r;
    }

    struct SaveAllResult
    {
        int saved = 0;
        std::vector<SaveResult> problems;
    };

    [[nodiscard]] inline SaveAllResult save_all(EditorState& state, workspace::Workspace const& ws)
    {
        SaveAllResult out;
        for (std::size_t i = 0; i < state.files.size(); ++i)
        {
            if (!state.files[i].buf.modified())
                continue;
            auto r = save(state, ws, i);
            if (r.status == SaveStatus::Saved)
                ++out.saved;
            else if (r.status != SaveStatus::Unchanged)
                out.problems.push_back(std::move(r));
        }
        return out;
    }

    inline bool reload(EditorState& state, workspace::Workspace const& ws, std::size_t index)
    {
        if (index >= state.files.size())
            return false;
        auto& f = state.files[index];
        auto bytes = workspace::read_learner_file(ws, f.rel);
        if (!bytes)
            return false;
        auto const cursor = f.buf.cursor;
        auto const view = f.view;
        textedit::Buffer fresh;
        auto const loaded = fresh.load(*bytes, textedit::Limits{});
        fresh.read_only = !loaded.editable || protected_file(f.rel);
        f.read_only_reason = loaded.editable ? (protected_file(f.rel) ? f.read_only_reason : std::string()) : loaded.reason;
        configure(fresh, state);
        fresh.set_cursor(cursor);
        f.buf = std::move(fresh);
        f.view = view;
        f.disk = workspace::stamp_of_bytes(*bytes);
        f.conflict = false;
        f.draft_revision = f.buf.revision();
        remove_draft(ws, f.rel);
        return true;
    }

    struct DiskSync
    {
        std::vector<std::string> reloaded;
        std::vector<std::string> conflicts;
        std::vector<std::string> removed;
    };

    [[nodiscard]] inline DiskSync sync_with_disk(EditorState& state, workspace::Workspace const& ws)
    {
        DiskSync out;
        for (std::size_t i = 0; i < state.files.size(); ++i)
        {
            auto& f = state.files[i];
            auto const now = workspace::stamp(ws, f.rel);
            if (now == f.disk)
                continue;
            if (!now.exists)
            {
                if (!f.conflict)
                    out.removed.push_back(f.rel);
                f.conflict = true;
                continue;
            }
            if (!f.buf.modified())
            {
                if (reload(state, ws, i))
                    out.reloaded.push_back(f.rel);
            }
            else if (!f.conflict)
            {
                f.conflict = true;
                out.conflicts.push_back(f.rel);
            }
        }
        return out;
    }

    inline int write_drafts(EditorState& state, workspace::Workspace const& ws)
    {
        int written = 0;
        for (auto& f : state.files)
        {
            if (f.buf.read_only || !f.buf.modified() || f.draft_revision == f.buf.revision())
                continue;
            auto const path = draft_path(ws, f.rel);
            auto const dir = path.parent_path();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (platform::is_symlink_or_reparse(ws.root / std::string(workspace::meta_dir)) || platform::is_symlink_or_reparse(dir))
                continue;
            auto const body = f.buf.text();
            if (body.size() > (4u << 20))
                continue;
            if (platform::atomic_write(path, body))
            {
                f.draft_revision = f.buf.revision();
                ++written;
            }
        }
        return written;
    }

    inline void close_file(EditorState& state, workspace::Workspace const& ws, std::size_t index)
    {
        if (index >= state.files.size())
            return;
        if (!state.files[index].buf.modified())
            remove_draft(ws, state.files[index].rel);
        state.files.erase(state.files.begin() + static_cast<std::ptrdiff_t>(index));
        if (state.current >= state.files.size())
            state.current = state.files.empty() ? 0 : state.files.size() - 1;
        if (state.files.empty())
            state.active = false;
    }

    enum class Action
    {
        None,
        Handled,
        Save,
        SaveAll,
        RunChecks,
        RunProgram,
        Leave,
        NextDiagnostic,
        PrevDiagnostic,
        ToggleSide,
        Picker,
        Close,
        Help,
    };

    [[nodiscard]] inline bool printable(std::string_view k)
    {
        if (k.empty())
            return false;
        if (k.size() == 1)
            return static_cast<unsigned char>(k[0]) >= 0x20 && k[0] != 0x7f;
        return static_cast<unsigned char>(k[0]) >= 0x80 && text::utf8_valid(k);
    }

    [[nodiscard]] inline Action find_key(EditorState& state, std::string const& k)
    {
        auto* f = state.open_file();
        auto& find = state.find;
        auto const search = [&](bool forward) {
            if (!f || find.query.empty())
                return;
            bool const hit = forward ? f->buf.find_next(find.query, find.match_case) : f->buf.find_prev(find.query, find.match_case);
            auto const count = f->buf.count_matches(find.query, find.match_case);
            find.status = hit ? std::format("{} match{}", count, count == 1 ? "" : "es") : "no match";
            f->buf.ensure_visible(f->view, state.page_height, state.text_width);
        };
        if (k == "Esc")
        {
            find.active = false;
            return Action::Handled;
        }
        if (k == "Enter" || k == "CtrlG" || k == "F3")
        {
            search(true);
            return Action::Handled;
        }
        if (k == "ShiftF3" || k == "Up")
        {
            search(false);
            return Action::Handled;
        }
        if (k == "Down")
        {
            search(true);
            return Action::Handled;
        }
        if (k == "AltC")
        {
            find.match_case = !find.match_case;
            find.status = find.match_case ? "match case on" : "match case off";
            return Action::Handled;
        }
        if (k == "Backspace")
        {
            if (!find.query.empty())
            {
                auto cut = find.query.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(find.query[cut]) & 0xC0) == 0x80)
                    --cut;
                find.query.resize(cut);
            }
            find.status.clear();
            return Action::Handled;
        }
        if (printable(k))
        {
            find.query += k;
            if (f)
            {
                if (auto hit = f->buf.find(find.query, f->buf.selection_start(), true, find.match_case))
                {
                    f->buf.select_range(hit->first, hit->second);
                    f->buf.ensure_visible(f->view, state.page_height, state.text_width);
                }
                auto const count = f->buf.count_matches(find.query, find.match_case);
                find.status = count == 0 ? "no match" : std::format("{} match{}", count, count == 1 ? "" : "es");
            }
            return Action::Handled;
        }
        return Action::Handled;
    }

    [[nodiscard]] inline Action picker_key(EditorState& state, std::vector<std::string> const& files, std::string const& k, std::string& chosen)
    {
        auto& p = state.picker;
        auto const matches = [&] {
            std::vector<std::string> out;
            for (auto const& f : files)
                if (p.filter.empty() || text::lower(f).find(text::lower(p.filter)) != std::string::npos)
                    out.push_back(f);
            return out;
        }();
        if (k == "Esc")
        {
            p.active = false;
            return Action::Handled;
        }
        if (k == "Down")
        {
            if (!matches.empty())
                p.sel = std::min(p.sel + 1, matches.size() - 1);
            return Action::Handled;
        }
        if (k == "Up")
        {
            if (p.sel > 0)
                --p.sel;
            return Action::Handled;
        }
        if (k == "Enter")
        {
            if (!matches.empty())
            {
                chosen = matches[std::min(p.sel, matches.size() - 1)];
                p.active = false;
            }
            return Action::Handled;
        }
        if (k == "Backspace")
        {
            if (!p.filter.empty())
                p.filter.pop_back();
            p.sel = 0;
            return Action::Handled;
        }
        if (printable(k))
        {
            p.filter += k;
            p.sel = 0;
        }
        return Action::Handled;
    }

    [[nodiscard]] inline Action handle_key(EditorState& state, std::string const& k)
    {
        if (k == "CtrlS")
            return Action::Save;
        if (k == "AltS")
            return Action::SaveAll;
        if (k == "F5" || k == "CtrlT")
            return Action::RunChecks;
        if (k == "F6")
            return Action::RunProgram;
        if (k == "F8" || k == "CtrlE")
            return Action::NextDiagnostic;
        if (k == "ShiftF8")
            return Action::PrevDiagnostic;
        if (k == "CtrlB")
            return Action::ToggleSide;
        if (k == "CtrlO" || k == "CtrlP")
            return Action::Picker;
        if (k == "CtrlW")
            return Action::Close;
        if (k == "F1")
            return Action::Help;
        if (k == "Esc")
            return Action::Leave;
        auto* f = state.open_file();
        if (!f)
            return Action::Leave;
        auto& b = f->buf;
        bool const page = true;
        (void)page;
        auto const after = [&] { b.ensure_visible(f->view, state.page_height, state.text_width); };
        auto const edit = [&](bool changed) {
            if (!changed && b.read_only)
                return;
            after();
        };
        if (k == "CtrlF")
        {
            state.find.active = true;
            state.find.status.clear();
            auto const sel = b.selected_text();
            if (!sel.empty() && sel.find('\n') == std::string::npos)
                state.find.query = sel;
            return Action::Handled;
        }
        if (k == "CtrlG" || k == "F3")
        {
            if (!state.find.query.empty())
                b.find_next(state.find.query, state.find.match_case);
            after();
            return Action::Handled;
        }
        if (k == "ShiftF3")
        {
            if (!state.find.query.empty())
                b.find_prev(state.find.query, state.find.match_case);
            after();
            return Action::Handled;
        }
        if (k == "PageUp" || k == "PageDown" || k == "ShiftPageUp" || k == "ShiftPageDown")
        {
            bool const sel = k.starts_with("Shift");
            int const n = std::max(1, state.page_height - 1);
            if (k.ends_with("Up"))
                b.page_up(static_cast<std::size_t>(n), sel);
            else
                b.page_down(static_cast<std::size_t>(n), sel);
            after();
            return Action::Handled;
        }
        struct Move
        {
            std::string_view key;
            std::string_view shift;
            void (*fn)(textedit::Buffer&, bool);
        };
        static constexpr std::array<Move, 12> moves = {{
            {"Left", "ShiftLeft", [](textedit::Buffer& x, bool s) { x.left(s); }},
            {"Right", "ShiftRight", [](textedit::Buffer& x, bool s) { x.right(s); }},
            {"Up", "ShiftUp", [](textedit::Buffer& x, bool s) { x.up(s); }},
            {"Down", "ShiftDown", [](textedit::Buffer& x, bool s) { x.down(s); }},
            {"Home", "ShiftHome", [](textedit::Buffer& x, bool s) { x.home(s); }},
            {"End", "ShiftEnd", [](textedit::Buffer& x, bool s) { x.end(s); }},
            {"CtrlLeft", "CtrlShiftLeft", [](textedit::Buffer& x, bool s) { x.word_left(s); }},
            {"CtrlRight", "CtrlShiftRight", [](textedit::Buffer& x, bool s) { x.word_right(s); }},
            {"CtrlHome", "CtrlShiftHome", [](textedit::Buffer& x, bool s) { x.buffer_start(s); }},
            {"CtrlEnd", "CtrlShiftEnd", [](textedit::Buffer& x, bool s) { x.buffer_end(s); }},
            {"CtrlUp", "CtrlShiftUp", [](textedit::Buffer& x, bool s) { x.up(s); }},
            {"CtrlDown", "CtrlShiftDown", [](textedit::Buffer& x, bool s) { x.down(s); }},
        }};
        for (auto const& m : moves)
            if (k == m.key || k == m.shift)
            {
                m.fn(b, k == m.shift);
                after();
                return Action::Handled;
            }
        if (k == "CtrlA")
        {
            b.select_all();
            after();
            return Action::Handled;
        }
        if (k == "CtrlC")
        {
            auto const sel = b.selected_text();
            if (!sel.empty())
                state.clipboard = sel;
            return Action::Handled;
        }
        if (k == "CtrlX")
        {
            if (!b.read_only)
            {
                auto const sel = b.cut();
                if (!sel.empty())
                    state.clipboard = sel;
            }
            edit(true);
            return Action::Handled;
        }
        if (k == "CtrlV")
        {
            if (!state.clipboard.empty())
                b.paste(state.clipboard);
            edit(true);
            return Action::Handled;
        }
        if (k == "CtrlZ")
        {
            b.undo();
            edit(true);
            return Action::Handled;
        }
        if (k == "CtrlY")
        {
            b.redo();
            edit(true);
            return Action::Handled;
        }
        if (k == "CtrlK")
        {
            b.delete_line();
            edit(true);
            return Action::Handled;
        }
        if (k == "CtrlD")
        {
            b.duplicate_line();
            edit(true);
            return Action::Handled;
        }
        if (k == "Enter")
        {
            b.newline();
            edit(true);
            return Action::Handled;
        }
        if (k == "Backspace")
        {
            b.backspace();
            edit(true);
            return Action::Handled;
        }
        if (k == "Delete")
        {
            b.del();
            edit(true);
            return Action::Handled;
        }
        if (k == "Tab")
        {
            b.tab();
            edit(true);
            return Action::Handled;
        }
        if (k == "ShiftTab")
        {
            b.untab();
            edit(true);
            return Action::Handled;
        }
        if (printable(k))
        {
            b.insert(k);
            edit(true);
            return Action::Handled;
        }
        return Action::None;
    }

    inline void paste_text(EditorState& state, std::string_view text)
    {
        if (state.find.active)
        {
            for (char c : text)
                if (c != '\n' && c != '\r')
                    state.find.query += c;
            return;
        }
        if (auto* f = state.open_file())
        {
            f->buf.paste(text);
            f->buf.ensure_visible(f->view, state.page_height, state.text_width);
        }
    }
} // namespace learndc::tui::editing
