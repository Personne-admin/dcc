export module learndc.settings;

import std;
import learndc.text;
import learndc.json;
import learndc.platform;

export namespace learndc::settings
{
    enum class ValueKind
    {
        Choice,
        Bool,
        Int,
        String,
        StringList,
    };

    struct KeyInfo
    {
        std::string key;
        ValueKind kind = ValueKind::String;
        std::string description;
        std::vector<std::string> choices;
        int min = 0;
        int max = 0;
    };

    struct Settings
    {
        std::string editor_preset = "auto";
        std::vector<std::string> editor_command;
        std::string editor_kind = "auto";
        bool editor_wait = false;
        int editor_indent_width = 4;
        std::string tools_dcc;
        std::string tools_dpm;
        std::string ui_theme = "dark";
        std::string ui_ascii = "auto";
        bool ui_reduced_motion = false;
        bool ui_open_continue = true;
        int checks_timeout_seconds = 10;
        int checks_build_timeout_seconds = 120;
        int checks_max_output_kib = 256;
        int checks_watch_debounce_ms = 400;
        std::string learner_level = "new";
        bool onboarding_done = false;

        friend bool operator==(Settings const&, Settings const&) = default;
    };

    [[nodiscard]] inline std::vector<KeyInfo> const& keys()
    {
        static std::vector<KeyInfo> const all = {
            {"editor.preset",
             ValueKind::Choice,
             "editor to open files with",
             {"auto", "nvim", "vim", "nano", "code", "codium", "notepad", "custom", "manual"},
             0,
             0},
            {"editor.command", ValueKind::StringList, "command used when editor.preset is custom (placeholders {file} {line} {column} {workspace})", {}, 0, 0},
            {"editor.kind", ValueKind::Choice, "whether the editor takes over the terminal", {"auto", "terminal", "gui"}, 0, 0},
            {"editor.wait", ValueKind::Bool, "wait for a graphical editor to close", {}, 0, 0},
            {"editor.indent_width", ValueKind::Int, "spaces per indentation level in the built-in editor", {}, 1, 8},
            {"tools.dcc", ValueKind::String, "path or name of the dcc compiler (empty: search)", {}, 0, 0},
            {"tools.dpm", ValueKind::String, "path or name of the dpm package manager (empty: search)", {}, 0, 0},
            {"ui.theme", ValueKind::Choice, "color theme", {"dark", "light", "high-contrast", "no-color"}, 0, 0},
            {"ui.ascii", ValueKind::Choice, "use ASCII borders and glyphs", {"auto", "on", "off"}, 0, 0},
            {"ui.reduced_motion", ValueKind::Bool, "disable the spinner", {}, 0, 0},
            {"ui.open_continue", ValueKind::Bool, "open the active exercise at startup", {}, 0, 0},
            {"checks.timeout_seconds", ValueKind::Int, "time limit for each run check, in seconds", {}, 1, 600},
            {"checks.build_timeout_seconds", ValueKind::Int, "time limit for building, in seconds", {}, 10, 600},
            {"checks.max_output_kib", ValueKind::Int, "stored output limit per stream, in KiB", {}, 1, 16384},
            {"checks.watch_debounce_ms", ValueKind::Int, "delay before a watched change starts a check, in milliseconds", {}, 50, 10000},
            {"learner.level", ValueKind::Choice, "learner experience level", {"new", "experienced"}, 0, 0},
            {"onboarding.done", ValueKind::Bool, "the first-run setup was completed", {}, 0, 0},
        };
        return all;
    }

    [[nodiscard]] inline KeyInfo const* find_key(std::string_view key)
    {
        for (auto const& info : keys())
            if (info.key == key)
                return &info;
        return nullptr;
    }

    namespace detail
    {
        [[nodiscard]] inline std::string known_keys_text()
        {
            std::vector<std::string> names;
            for (auto const& info : keys())
                names.push_back(info.key);
            return text::join(names, ", ");
        }

        [[nodiscard]] inline std::unexpected<text::Error> unknown_key(std::string_view key)
        {
            return std::unexpected(text::Error{std::format("unknown setting '{}' (known settings: {})", key, known_keys_text())});
        }

        [[nodiscard]] inline std::optional<bool> parse_bool(std::string_view value)
        {
            auto const v = text::lower(text::trim(value));
            if (v == "true" || v == "yes" || v == "on" || v == "1")
                return true;
            if (v == "false" || v == "no" || v == "off" || v == "0")
                return false;
            return std::nullopt;
        }

        [[nodiscard]] inline std::optional<std::string> string_problem(std::string_view value)
        {
            for (char c : value)
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
                    return "contains a control character";
            if (!text::utf8_valid(value))
                return "is not valid UTF-8";
            return std::nullopt;
        }

        [[nodiscard]] inline std::expected<std::vector<std::string>, text::Error> split_words(std::string_view value)
        {
            std::vector<std::string> out;
            std::string current;
            bool have = false;
            char quote = 0;
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                char const c = value[i];
                if (quote != 0)
                {
                    if (c == quote)
                        quote = 0;
                    else if (c == '\\' && quote == '"' && i + 1 < value.size() && (value[i + 1] == '"' || value[i + 1] == '\\'))
                        current += value[++i];
                    else
                        current += c;
                }
                else if (c == '"' || c == '\'')
                {
                    quote = c;
                    have = true;
                }
                else if (c == ' ' || c == '\t')
                {
                    if (have || !current.empty())
                        out.push_back(std::move(current));
                    current.clear();
                    have = false;
                }
                else
                    current += c;
            }
            if (quote != 0)
                return std::unexpected(text::Error{"unterminated quote in the command"});
            if (have || !current.empty())
                out.push_back(std::move(current));
            return out;
        }

        [[nodiscard]] inline std::expected<std::vector<std::string>, text::Error> validate_list(std::vector<std::string> list)
        {
            if (list.empty() || list.front().empty())
                return std::unexpected(text::Error{"editor.command must name a program"});
            for (auto const& item : list)
                if (auto problem = string_problem(item))
                    return std::unexpected(text::Error{std::format("editor.command argument '{}' {}", text::sanitize(item), *problem)});
            return list;
        }

        [[nodiscard]] inline std::expected<std::string, text::Error> validate_choice(KeyInfo const& info, std::string_view value)
        {
            auto const v = text::lower(text::trim(value));
            for (auto const& choice : info.choices)
                if (choice == v)
                    return choice;
            return std::unexpected(
                text::Error{std::format("invalid value '{}' for {} (expected one of: {})", text::sanitize(value), info.key, text::join(info.choices, ", "))});
        }

        [[nodiscard]] inline std::expected<int, text::Error> validate_int(KeyInfo const& info, std::int64_t value)
        {
            if (value < info.min || value > info.max)
                return std::unexpected(text::Error{std::format("{} must be between {} and {} (got {})", info.key, info.min, info.max, value)});
            return static_cast<int>(value);
        }

        [[nodiscard]] inline std::expected<std::string, text::Error> validate_string(KeyInfo const& info, std::string_view value)
        {
            if (auto problem = string_problem(value))
                return std::unexpected(text::Error{std::format("{} {}", info.key, *problem)});
            return std::string(text::trim(value));
        }

        [[nodiscard]] inline std::expected<void, text::Error> assign_text(Settings& s, KeyInfo const& info, std::string_view value)
        {
            auto const& key = info.key;
            switch (info.kind)
            {
                case ValueKind::Choice: {
                    auto choice = validate_choice(info, value);
                    if (!choice)
                        return std::unexpected(choice.error());
                    if (key == "editor.preset")
                        s.editor_preset = *choice;
                    else if (key == "editor.kind")
                        s.editor_kind = *choice;
                    else if (key == "ui.theme")
                        s.ui_theme = *choice;
                    else if (key == "ui.ascii")
                        s.ui_ascii = *choice;
                    else
                        s.learner_level = *choice;
                    return {};
                }
                case ValueKind::Bool: {
                    auto b = parse_bool(value);
                    if (!b)
                        return std::unexpected(text::Error{std::format("invalid value '{}' for {} (expected true or false)", text::sanitize(value), key)});
                    if (key == "editor.wait")
                        s.editor_wait = *b;
                    else if (key == "ui.reduced_motion")
                        s.ui_reduced_motion = *b;
                    else if (key == "ui.open_continue")
                        s.ui_open_continue = *b;
                    else
                        s.onboarding_done = *b;
                    return {};
                }
                case ValueKind::Int: {
                    auto const trimmed = text::trim(value);
                    std::int64_t parsed = 0;
                    auto const [end, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
                    if (trimmed.empty() || ec != std::errc{} || end != trimmed.data() + trimmed.size())
                        return std::unexpected(text::Error{
                            std::format("invalid value '{}' for {} (expected a whole number from {} to {})", text::sanitize(value), key, info.min, info.max)});
                    auto n = validate_int(info, parsed);
                    if (!n)
                        return std::unexpected(n.error());
                    if (key == "editor.indent_width")
                        s.editor_indent_width = *n;
                    else if (key == "checks.timeout_seconds")
                        s.checks_timeout_seconds = *n;
                    else if (key == "checks.build_timeout_seconds")
                        s.checks_build_timeout_seconds = *n;
                    else if (key == "checks.max_output_kib")
                        s.checks_max_output_kib = *n;
                    else
                        s.checks_watch_debounce_ms = *n;
                    return {};
                }
                case ValueKind::String: {
                    auto str = validate_string(info, value);
                    if (!str)
                        return std::unexpected(str.error());
                    if (key == "tools.dcc")
                        s.tools_dcc = *str;
                    else
                        s.tools_dpm = *str;
                    return {};
                }
                case ValueKind::StringList: {
                    std::vector<std::string> list;
                    auto const trimmed = text::trim(value);
                    if (trimmed.starts_with("["))
                    {
                        auto parsed = json::parse(trimmed);
                        if (!parsed)
                            return std::unexpected(text::Error{std::format("{} is not a valid JSON array: {}", key, parsed.error().format())});
                        auto const* array = parsed->as_array();
                        if (array == nullptr)
                            return std::unexpected(text::Error{std::format("{} must be a JSON array of strings", key)});
                        for (auto const& item : *array)
                        {
                            auto str = item.as_string();
                            if (!str)
                                return std::unexpected(text::Error{std::format("{} must contain only strings", key)});
                            list.emplace_back(*str);
                        }
                    }
                    else
                    {
                        auto words = split_words(trimmed);
                        if (!words)
                            return std::unexpected(words.error());
                        list = std::move(*words);
                    }
                    if (list.empty())
                    {
                        s.editor_command.clear();
                        return {};
                    }
                    auto checked = validate_list(std::move(list));
                    if (!checked)
                        return std::unexpected(checked.error());
                    s.editor_command = std::move(*checked);
                    return {};
                }
            }
            return {};
        }
    } // namespace detail

    [[nodiscard]] inline std::expected<void, text::Error> set(Settings& s, std::string_view key, std::string_view value)
    {
        auto const* info = find_key(key);
        if (info == nullptr)
            return detail::unknown_key(key);
        Settings copy = s;
        if (auto r = detail::assign_text(copy, *info, value); !r)
            return r;
        s = std::move(copy);
        return {};
    }

    [[nodiscard]] inline std::expected<json::Json, text::Error> get_json(Settings const& s, std::string_view key)
    {
        if (find_key(key) == nullptr)
            return detail::unknown_key(key);
        if (key == "editor.preset")
            return json::Json(s.editor_preset);
        if (key == "editor.command")
        {
            auto array = json::Json::make_array();
            for (auto const& item : s.editor_command)
                array.push(json::Json(item));
            return array;
        }
        if (key == "editor.kind")
            return json::Json(s.editor_kind);
        if (key == "editor.wait")
            return json::Json(s.editor_wait);
        if (key == "tools.dcc")
            return json::Json(s.tools_dcc);
        if (key == "tools.dpm")
            return json::Json(s.tools_dpm);
        if (key == "ui.theme")
            return json::Json(s.ui_theme);
        if (key == "ui.ascii")
            return json::Json(s.ui_ascii);
        if (key == "ui.reduced_motion")
            return json::Json(s.ui_reduced_motion);
        if (key == "ui.open_continue")
            return json::Json(s.ui_open_continue);
        if (key == "editor.indent_width")
            return json::Json(s.editor_indent_width);
        if (key == "checks.timeout_seconds")
            return json::Json(s.checks_timeout_seconds);
        if (key == "checks.build_timeout_seconds")
            return json::Json(s.checks_build_timeout_seconds);
        if (key == "checks.max_output_kib")
            return json::Json(s.checks_max_output_kib);
        if (key == "checks.watch_debounce_ms")
            return json::Json(s.checks_watch_debounce_ms);
        if (key == "learner.level")
            return json::Json(s.learner_level);
        return json::Json(s.onboarding_done);
    }

    [[nodiscard]] inline std::expected<std::string, text::Error> get(Settings const& s, std::string_view key)
    {
        auto value = get_json(s, key);
        if (!value)
            return std::unexpected(value.error());
        if (auto str = value->as_string())
            return std::string(*str);
        if (value->is_array())
        {
            std::vector<std::string> parts;
            for (auto const& item : *value->as_array())
                parts.emplace_back(item.as_string().value_or(""));
            return text::join(parts, " ");
        }
        return value->dump(0);
    }

    [[nodiscard]] inline std::vector<std::pair<std::string, std::string>> list(Settings const& s)
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (auto const& info : keys())
            out.emplace_back(info.key, get(s, info.key).value_or(""));
        return out;
    }

    [[nodiscard]] inline Settings defaults()
    {
        return Settings{};
    }

    [[nodiscard]] inline std::string default_text(std::string_view key)
    {
        return get(Settings{}, key).value_or("");
    }

    [[nodiscard]] inline json::Json to_json(Settings const& s)
    {
        auto root = json::Json::make_object();
        root.set("schema", json::Json(1));
        for (auto const& info : keys())
        {
            auto const dot = info.key.find('.');
            auto const group = info.key.substr(0, dot);
            auto const name = info.key.substr(dot + 1);
            if (root.find(group) == nullptr)
                root.set(group, json::Json::make_object());
            root.find(group)->set(name, *get_json(s, info.key));
        }
        return root;
    }

    [[nodiscard]] inline std::expected<Settings, text::Error> from_json(json::Json const& root)
    {
        auto const* object = root.as_object();
        if (object == nullptr)
            return std::unexpected(text::Error{"the settings file must contain a JSON object"});
        Settings s;
        for (auto const& [group, value] : *object)
        {
            if (group == "schema")
            {
                auto n = value.as_int();
                if (!n || *n != 1)
                    return std::unexpected(text::Error{std::format("unsupported settings schema (this learndc supports 1)")});
                continue;
            }
            auto const* inner = value.as_object();
            if (inner == nullptr)
                return std::unexpected(text::Error{std::format("unknown setting '{}' (known settings: {})", group, detail::known_keys_text())});
            for (auto const& [name, item] : *inner)
            {
                auto const key = group + "." + name;
                auto const* info = find_key(key);
                if (info == nullptr)
                    return detail::unknown_key(key);
                std::expected<void, text::Error> r;
                switch (info->kind)
                {
                    case ValueKind::Bool:
                        if (!item.is_bool())
                            return std::unexpected(text::Error{std::format("{} must be true or false", key)});
                        r = detail::assign_text(s, *info, *item.as_bool() ? "true" : "false");
                        break;
                    case ValueKind::Int:
                        if (!item.is_int())
                            return std::unexpected(text::Error{std::format("{} must be a whole number", key)});
                        r = detail::assign_text(s, *info, std::to_string(*item.as_int()));
                        break;
                    case ValueKind::StringList:
                        if (!item.is_array())
                            return std::unexpected(text::Error{std::format("{} must be an array of strings", key)});
                        r = detail::assign_text(s, *info, item.dump(0));
                        break;
                    default:
                        if (!item.is_string())
                            return std::unexpected(text::Error{std::format("{} must be a string", key)});
                        r = detail::assign_text(s, *info, *item.as_string());
                        break;
                }
                if (!r)
                    return std::unexpected(r.error());
            }
        }
        return s;
    }

    [[nodiscard]] inline std::expected<Settings, text::Error> parse(std::string_view text_in)
    {
        auto parsed = json::parse(text_in);
        if (!parsed)
            return std::unexpected(text::Error{parsed.error().format()});
        return from_json(*parsed);
    }

    [[nodiscard]] inline std::expected<Settings, text::Error> load(std::filesystem::path const& file)
    {
        auto data = platform::read_file(file);
        if (!data)
        {
            std::error_code ec;
            if (!std::filesystem::exists(file, ec))
                return Settings{};
            return std::unexpected(text::Error{std::format("cannot read '{}'", platform::path_to_utf8(file))});
        }
        auto parsed = parse(*data);
        if (!parsed)
            return std::unexpected(text::Error{std::format("{}: {}", platform::path_to_utf8(file), parsed.error().message)});
        return parsed;
    }

    [[nodiscard]] inline std::expected<void, text::Error> save(std::filesystem::path const& file, Settings const& s)
    {
        return platform::atomic_write(file, to_json(s).dump(2) + "\n");
    }

    [[nodiscard]] inline std::expected<Settings, text::Error> update(std::filesystem::path const& file,
                                                                     std::function<std::expected<void, text::Error>(Settings&)> const& change)
    {
        auto lock_path = file;
        lock_path += ".lock";
        auto lock = platform::FileLock::acquire(lock_path, std::chrono::seconds(10));
        if (!lock)
            return std::unexpected(text::Error{lock.error().message});
        auto current = load(file);
        if (!current)
            return std::unexpected(current.error());
        if (auto r = change(*current); !r)
            return std::unexpected(r.error());
        if (auto r = save(file, *current); !r)
            return std::unexpected(r.error());
        return current;
    }
} // namespace learndc::settings
