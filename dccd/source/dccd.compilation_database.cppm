export module dccd.compilation_database;

import std;
import dcc.target;
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
    };

    class CompilationDatabase
    {
    public:
        [[nodiscard]] bool load(std::filesystem::path const& path, std::ostream& log);

        void clear();

        [[nodiscard]] CompileCommand const* command_for(std::filesystem::path const& file) const;

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

        [[nodiscard]] inline bool is_known_irrelevant_flag(std::string_view arg)
        {
            using namespace std::string_view_literals;
            static constexpr std::string_view flags[] = {
                "-c"sv,
                "-S"sv,
                "-shared"sv,
                "-fbounds-check"sv,
                "-frestricted-check"sv,
                "-fno-red-zone"sv,
                "-fno-simd"sv,
                "-fno-x87"sv,
                "-fno-stack-protector"sv,
                "-fno-stack-probe"sv,
                "-fomit-frame-pointer"sv,
                "-fno-omit-frame-pointer"sv,
                "-fPIC"sv,
                "-fpic"sv,
                "-fPIE"sv,
                "-O0"sv,
                "-O1"sv,
                "-O2"sv,
                "-Os"sv,
                "-g0"sv,
                "-gnone"sv,
                "-g3"sv,
                "-g"sv,
                "-gdwarf"sv,
                "-gpdb"sv,
                "-fdump-ast"sv,
                "-fdump-ir"sv,
                "-fdump-llvm"sv,
                "-fdump-mir"sv,
            };

            for (auto f : flags)
                if (arg == f)
                    return true;
            return false;
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

    std::optional<AnalysisCommand> project_analysis_command(CompileCommand const& command, std::ostream& log)
    {
        AnalysisCommand analysis;

        auto const& argv = command.arguments;
        for (std::size_t i = 0; i < argv.size(); ++i)
        {
            std::string_view arg{argv[i]};

            if (arg == "-target" && i + 1 < argv.size())
            {
                if (auto parsed = dcc::target::TargetConfig::parse_triple(argv[i + 1]))
                    analysis.target = std::move(*parsed);
                else
                    std::println(log, "[dccd] unsupported target triple \"{}\"; ignoring for analysis", argv[i + 1]);
                ++i;
                continue;
            }
            if (arg.starts_with("--target="))
            {
                std::string_view triple = arg.substr(9);
                if (auto parsed = dcc::target::TargetConfig::parse_triple(triple))
                    analysis.target = std::move(*parsed);
                else
                    std::println(log, "[dccd] unsupported target triple \"{}\"; ignoring for analysis", triple);
                continue;
            }

            if (arg == "-I" && i + 1 < argv.size())
            {
                detail::add_unique_path(analysis.import_roots, detail::normalize_path(std::filesystem::path{argv[i + 1]}, command.directory));
                ++i;
                continue;
            }
            if (arg.starts_with("-I") && arg.size() > 2)
            {
                detail::add_unique_path(analysis.import_roots, detail::normalize_path(std::filesystem::path{std::string{arg.substr(2)}}, command.directory));
                continue;
            }

            if ((arg == "-J" || arg == "--inject") && i + 1 < argv.size())
            {
                analysis.injected_decls.emplace_back(argv[i + 1]);
                ++i;
                continue;
            }
            if (arg.starts_with("-J") && arg.size() > 2)
            {
                analysis.injected_decls.emplace_back(arg.substr(2));
                continue;
            }
            if (arg.starts_with("--inject="))
            {
                analysis.injected_decls.emplace_back(arg.substr(9));
                continue;
            }

            if (arg == "-flibdcext")
            {
                analysis.inject_libdcext_prelude = true;
                continue;
            }

            if ((arg == "-o" || arg == "--depfile" || arg == "-mcmodel" || arg == "-fbackend" || arg == "-farch") && i + 1 < argv.size())
            {
                ++i;
                continue;
            }
            if (arg.starts_with("-mcmodel=") || arg.starts_with("-fbackend=") || arg.starts_with("-farch="))
                continue;

            if (detail::is_known_irrelevant_flag(arg))
                continue;

            if (!arg.empty() && arg[0] == '-')
            {
                std::println(log, "[dccd] ignoring unknown option in compile command: {}", arg);
                continue;
            }
        }

        return analysis;
    }

} // namespace dccd
