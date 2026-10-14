export module dccd.compilation_database;

import std;
import dcc.target;
import dcc.cli;
import dccd.protocol;

export namespace dccd
{
    struct CompileCommand
    {
        std::filesystem::path directory;
        std::filesystem::path file;
        std::vector<std::string> arguments;
        std::optional<std::filesystem::path> output;
    };

    struct AnalysisCommand
    {
        std::optional<dcc::target::TargetConfig> target;

        std::vector<std::filesystem::path> import_roots;
        std::vector<std::string> injected_decls;

        bool inject_libdcext_prelude{false};
        dcc::target::LibdcextOs libdcext_os{dcc::target::LibdcextOs::Host};
        bool restricted_check{false};
        bool bounds_check{false};
        bool partial_eval{false};
        bool far_check{false};
    };

    class CompilationDatabase
    {
    public:
        [[nodiscard]] bool load(std::filesystem::path const& path, std::ostream& log);

        void clear();

        [[nodiscard]] CompileCommand const* command_for(std::filesystem::path const& file) const;

        [[nodiscard]] CompileCommand const* nearest_command_for(std::filesystem::path const& file) const;

        [[nodiscard]] std::size_t source_command_count() const;

        [[nodiscard]] std::filesystem::path const& path() const noexcept { return m_path; }

        [[nodiscard]] bool empty() const noexcept { return m_commands.empty(); }

        [[nodiscard]] std::size_t size() const noexcept { return m_commands.size(); }

    private:
        std::filesystem::path m_path;
        std::vector<CompileCommand> m_commands;
        std::map<std::string, std::size_t, std::less<>> m_first_index;
    };

    [[nodiscard]] std::optional<AnalysisCommand> project_analysis_command(CompileCommand const& command, std::ostream& log);

} // namespace dccd

namespace dccd
{
    namespace detail
    {
        [[nodiscard]] inline std::filesystem::path normalize_path(std::filesystem::path p, std::filesystem::path const& base)
        {
            if (!p.is_absolute() && !base.empty())
                p = base / p;

            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(p, ec);
            if (ec)
                canonical = p.lexically_normal();

            return canonical;
        }

        [[nodiscard]] inline bool is_whitespace(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        [[nodiscard]] inline std::optional<std::vector<std::string>> tokenize_command(std::string_view cmd)
        {
            std::vector<std::string> args;
            std::string current;
            bool in_quotes = false;
            bool escaped = false;
            bool have_token = false;

            for (char c : cmd)
            {
                if (escaped)
                {
                    current.push_back(c);
                    escaped = false;
                    have_token = true;
                    continue;
                }

                if (c == '\\')
                {
                    escaped = true;
                    have_token = true;
                    continue;
                }

                if (c == '"')
                {
                    in_quotes = !in_quotes;
                    have_token = true;
                    continue;
                }

                if (!in_quotes && is_whitespace(c))
                {
                    if (have_token)
                    {
                        args.push_back(std::move(current));
                        current.clear();
                        have_token = false;
                    }
                    continue;
                }

                current.push_back(c);
                have_token = true;
            }

            if (escaped || in_quotes)
                return std::nullopt;

            if (have_token)
                args.push_back(std::move(current));

            return args;
        }

        [[nodiscard]] inline std::optional<CompileCommand> parse_command_object(protocol::JsonValue const& obj, std::ostream& log, std::size_t index)
        {
            auto skip = [&](std::string_view reason) {
                std::println(log, "[dccd] compilation database: skipping entry {}: {}", index, reason);
                return std::nullopt;
            };

            if (!obj.is_object())
                return skip("not a JSON object");

            auto directory_raw = obj.get_string("directory");
            if (!directory_raw || directory_raw->empty())
                return skip("missing or invalid directory field");

            auto file_raw = obj.get_string("file");
            if (!file_raw || file_raw->empty())
                return skip("missing or invalid file field");

            std::vector<std::string> arguments;
            if (auto const* args_arr = obj.get_array("arguments"))
            {
                for (auto const& arg : args_arr->as_array())
                {
                    if (!arg.is_string())
                        return skip("non-string element in arguments array");
                    arguments.push_back(arg.as_string());
                }
            }
            else if (auto command_opt = obj.get_string("command"))
            {
                auto tokenized = tokenize_command(*command_opt);
                if (!tokenized)
                    return skip("malformed command string");
                arguments = std::move(*tokenized);
            }
            else
            {
                return skip("neither arguments nor command present");
            }

            auto directory = normalize_path(std::filesystem::path{*directory_raw}, {});
            auto file = normalize_path(std::filesystem::path{*file_raw}, directory);

            std::optional<std::filesystem::path> output;
            if (auto output_opt = obj.get_string("output"))
                output = normalize_path(std::filesystem::path{*output_opt}, directory);

            CompileCommand cmd;
            cmd.directory = std::move(directory);
            cmd.file = std::move(file);
            cmd.arguments = std::move(arguments);
            cmd.output = std::move(output);
            return cmd;
        }

        inline void add_unique_path(std::vector<std::filesystem::path>& paths, std::filesystem::path p)
        {
            for (auto const& existing : paths)
                if (existing == p)
                    return;
            paths.push_back(std::move(p));
        }

    } // namespace detail

    bool CompilationDatabase::load(std::filesystem::path const& path, std::ostream& log)
    {
        clear();
        m_path = path;

        std::ifstream in(path);
        if (!in.is_open())
        {
            std::println(log, "[dccd] compilation database: cannot open file: {}", path.string());
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        auto json = protocol::JsonValue::parse(content);
        if (!json || !json->is_array())
        {
            std::println(log, "[dccd] compilation database: invalid JSON (expected a top-level array): {}", path.string());
            return false;
        }

        std::size_t index = 0;
        for (auto const& entry : json->as_array())
        {
            ++index;
            auto cmd = detail::parse_command_object(entry, log, index);
            if (!cmd)
                continue;

            auto key = cmd->file.string();
            auto it = m_first_index.find(key);
            if (it == m_first_index.end())
                m_first_index.emplace(key, m_commands.size());
            else
                std::println(log, "[dccd] compilation database: duplicate command for \"{}\"; using first", key);

            m_commands.push_back(std::move(*cmd));
        }

        std::println(log, "[dccd] compilation database: {} commands", m_commands.size());
        return true;
    }

    void CompilationDatabase::clear()
    {
        m_path.clear();
        m_commands.clear();
        m_first_index.clear();
    }

    CompileCommand const* CompilationDatabase::command_for(std::filesystem::path const& file) const
    {
        auto key = detail::normalize_path(file, {}).string();
        auto it = m_first_index.find(key);
        if (it == m_first_index.end())
            return nullptr;
        return &m_commands[it->second];
    }

    CompileCommand const* CompilationDatabase::nearest_command_for(std::filesystem::path const& file) const
    {
        auto const directory = detail::normalize_path(file, {}).parent_path();
        CompileCommand const* best = nullptr;
        std::size_t best_shared = 0;
        for (auto const& command : m_commands)
        {
            if (command.file.extension() != ".dc")
                continue;

            std::size_t shared = 0;
            auto const command_directory = command.file.parent_path();
            for (auto a = directory.begin(), b = command_directory.begin(); a != directory.end() && b != command_directory.end() && *a == *b; ++a, ++b)
                ++shared;

            if (!best || shared > best_shared)
            {
                best = &command;
                best_shared = shared;
            }
        }
        return best;
    }

    std::size_t CompilationDatabase::source_command_count() const
    {
        return static_cast<std::size_t>(std::ranges::count_if(m_commands, [](CompileCommand const& command) { return command.file.extension() == ".dc"; }));
    }

    std::optional<AnalysisCommand> project_analysis_command(CompileCommand const& command, std::ostream& log)
    {
        auto parsed = dcc::cli::parse_args_lenient(std::span<std::string const>{command.arguments});
        for (auto const& error : parsed.errors)
            std::println(log, "[dccd] ignoring option in compile command: {}", error);
        auto const& opts = parsed.options;

        AnalysisCommand analysis;
        analysis.injected_decls = opts.injected_decls;
        analysis.inject_libdcext_prelude = opts.libdcext;
        analysis.libdcext_os = opts.libdcext_os;
        analysis.restricted_check = opts.restricted_check;
        analysis.bounds_check = opts.bounds_check;
        analysis.partial_eval = opts.partial_eval;
        analysis.far_check = opts.far_check;

        for (auto const& path : opts.import_paths)
            detail::add_unique_path(analysis.import_roots, detail::normalize_path(path, command.directory));

        std::string error;
        if (auto target = dcc::cli::resolve_target_config(opts, error))
            analysis.target = std::move(*target);
        else
            std::println(log, "[dccd] {}; ignoring for analysis", error);

        return analysis;
    }

} // namespace dccd
