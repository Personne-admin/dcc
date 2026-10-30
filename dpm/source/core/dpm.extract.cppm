module;

#include <sys/stat.h>

export module dpm.extract;

import std;

export namespace dpm::extract
{
    struct Limits
    {
        std::uint64_t max_bytes = 256ull * 1024 * 1024;
        std::size_t max_files = 20000;
        std::size_t max_path = 4096;
    };

    struct Problem
    {
        std::string path;
        std::string message;

        [[nodiscard]] std::string format() const { return path.empty() ? message : path + ": " + message; }
    };

    [[nodiscard]] inline std::optional<std::string> member_name_problem(std::string_view name, Limits const& limits = {})
    {
        if (name.empty())
            return "empty path";
        if (name.size() > limits.max_path)
            return "path longer than " + std::to_string(limits.max_path) + " bytes";
        if (name.find('\0') != std::string_view::npos)
            return "path contains a NUL byte";
        if (name.find('\\') != std::string_view::npos)
            return "path contains a backslash";
        if (name.front() == '/')
            return "absolute path";
        if (name.size() >= 2 && name[1] == ':' && std::isalpha(static_cast<unsigned char>(name[0])))
            return "drive-qualified path";
        std::size_t pos = 0;
        while (pos <= name.size())
        {
            auto slash = name.find('/', pos);
            auto part = name.substr(pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
            if (part == "..")
                return "path escapes the package root";
            if (part.empty() && slash != std::string_view::npos)
                return "empty path component";
            if (slash == std::string_view::npos)
                break;
            pos = slash + 1;
        }
        return std::nullopt;
    }

    namespace detail
    {
        [[nodiscard]] inline std::string fold_case(std::string_view s)
        {
            std::string out{s};
            for (auto& c : out)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return out;
        }
    } // namespace detail

    [[nodiscard]] inline std::vector<Problem> check_tree(std::filesystem::path const& root, Limits const& limits = {})
    {
        std::vector<Problem> problems;
        std::error_code ec;
        auto root_status = std::filesystem::symlink_status(root, ec);
        if (ec || !std::filesystem::is_directory(root_status))
        {
            problems.push_back({"", "not a directory: " + root.string()});
            return problems;
        }
        std::size_t files = 0;
        std::uint64_t bytes = 0;
        std::vector<std::filesystem::path> stack{root};
        while (!stack.empty())
        {
            auto dir = std::move(stack.back());
            stack.pop_back();
            std::filesystem::directory_iterator it{dir, ec};
            if (ec)
            {
                problems.push_back({dir.lexically_relative(root).generic_string(), "cannot list directory"});
                continue;
            }
            std::map<std::string, std::string> folded;
            for (auto const& item : it)
            {
                auto rel = item.path().lexically_relative(root).generic_string();
                auto leaf = item.path().filename().string();
                if (auto bad = member_name_problem(rel, limits))
                {
                    problems.push_back({rel, *bad});
                    continue;
                }
                auto [pos, inserted] = folded.emplace(detail::fold_case(leaf), leaf);
                if (!inserted)
                    problems.push_back({rel, "name differs only in case from '" + pos->second + "'"});
                auto status = item.symlink_status(ec);
                if (ec)
                {
                    problems.push_back({rel, "cannot stat"});
                    continue;
                }
                if (std::filesystem::is_symlink(status))
                    problems.push_back({rel, "symbolic link"});
                else if (std::filesystem::is_directory(status))
                    stack.push_back(item.path());
                else if (std::filesystem::is_regular_file(status))
                {
                    ++files;
                    auto size = item.file_size(ec);
                    if (!ec)
                        bytes += size;
#if !defined(_WIN32)
                    struct stat st{};
                    if (::lstat(item.path().c_str(), &st) == 0 && st.st_nlink > 1)
                        problems.push_back({rel, "hard link"});
#endif
                }
                else if (std::filesystem::is_block_file(status) || std::filesystem::is_character_file(status))
                    problems.push_back({rel, "device node"});
                else
                    problems.push_back({rel, "special file"});
            }
        }
        if (files > limits.max_files)
            problems.push_back({"", "more than " + std::to_string(limits.max_files) + " files"});
        if (bytes > limits.max_bytes)
            problems.push_back({"", "more than " + std::to_string(limits.max_bytes) + " bytes"});
        std::ranges::sort(problems, [](Problem const& a, Problem const& b) { return std::tie(a.path, a.message) < std::tie(b.path, b.message); });
        return problems;
    }
} // namespace dpm::extract
