module;

#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#endif

export module dcc.help;

import std;

export namespace dcc::help
{
    enum class ArgKind : std::uint8_t
    {
        None,
        Required,
        Optional,
        Attached,
    };

    struct Flag
    {
        std::string_view name;
        std::string_view negated;
        ArgKind arg{ArgKind::None};
        std::string_view metavar;
    };

    struct Row
    {
        std::string flag;
        std::string description;
    };

    inline constexpr int k_flag_column = 26;

    [[nodiscard]] inline int terminal_width()
    {
        if (char const* columns = std::getenv("COLUMNS"))
        {
            int parsed = 0;
            for (char const* p = columns; *p >= '0' && *p <= '9'; ++p)
                parsed = parsed * 10 + (*p - '0');

            if (parsed > 0)
                return parsed;
        }

#ifndef _WIN32
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1)
            return w.ws_col;
#endif
        return 80;
    }

    [[nodiscard]] inline std::string join(std::span<std::string_view const> items, std::string_view separator)
    {
        std::string out;
        for (std::size_t k = 0; k < items.size(); ++k)
        {
            if (k)
                out += separator;
            out += items[k];
        }
        return out;
    }

    [[nodiscard]] inline std::string flag_text(Flag const& flag)
    {
        std::string out;

        if (!flag.negated.empty() && flag.name.starts_with("-f") && flag.negated == std::format("-fno-{}", flag.name.substr(2)))
            out = std::format("-f[no-]{}", flag.name.substr(2));
        else if (!flag.negated.empty())
            out = std::format("{} | {}", flag.name, flag.negated);
        else
            out = std::string{flag.name};

        if (flag.arg == ArgKind::None)
            return out;

        if (flag.arg == ArgKind::Attached)
            return out + std::string{flag.metavar};

        if (flag.arg == ArgKind::Optional)
            return std::format("{} [{}]", out, flag.metavar);

        return std::format("{} {}", out, flag.metavar);
    }

    [[nodiscard]] inline std::string description_text(std::string_view help, std::span<std::string_view const> choices, std::span<std::string_view const> aliases,
                                                      std::string_view default_text)
    {
        std::string out{help};

        if (!choices.empty())
            out += std::format(" (one of: {})", join(choices, ", "));

        if (!aliases.empty())
        {
            out += aliases.size() == 1 ? " (alias: " : " (aliases: ";
            out += join(aliases, ", ");
            out += ")";
        }

        if (!default_text.empty())
            out += std::format(" [default: {}]", default_text);

        return out;
    }

    inline void append_wrapped(std::string& out, std::string_view text, int indent, int width)
    {
        bool first_line = true;
        while (!text.empty())
        {
            if (!first_line)
                out.append(static_cast<std::size_t>(indent), ' ');

            if (text.length() <= static_cast<std::size_t>(width))
            {
                out += text;
                out += '\n';
                break;
            }

            auto wrap_pos = text.find_last_of(" \t", static_cast<std::size_t>(width));
            if (wrap_pos == std::string_view::npos)
                wrap_pos = static_cast<std::size_t>(width);

            out += text.substr(0, wrap_pos);
            out += '\n';

            text = text.substr(wrap_pos);
            auto first_non_space = text.find_first_not_of(" \t");
            if (first_non_space != std::string_view::npos)
                text = text.substr(first_non_space);
            else
                text = "";

            first_line = false;
        }
    }

    [[nodiscard]] inline std::string format_wrapped(std::string_view text, int indent, int term_width)
    {
        std::string out;
        append_wrapped(out, text, indent, std::max(20, term_width - indent - 2));
        return out;
    }

    [[nodiscard]] inline std::string format_rows(std::span<Row const> rows, int term_width, int flag_col_width = k_flag_column)
    {
        std::string out;
        int const desc_col_width = std::max(20, term_width - flag_col_width - 4);

        for (auto const& row : rows)
        {
            if (row.flag.length() >= static_cast<std::size_t>(flag_col_width))
            {
                out += "  " + row.flag + "\n";
                out.append(static_cast<std::size_t>(flag_col_width + 2), ' ');
            }
            else
            {
                out += "  " + row.flag;
                out.append(static_cast<std::size_t>(flag_col_width) - row.flag.length(), ' ');
            }

            append_wrapped(out, row.description, flag_col_width + 2, desc_col_width);
        }
        return out;
    }
} // namespace dcc::help
