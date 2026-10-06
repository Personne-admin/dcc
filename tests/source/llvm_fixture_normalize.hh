#pragma once

inline std::string normalize_llvm_printer_versions(std::string_view input)
{
    std::vector<std::string> lines;
    std::istringstream stream{std::string(input)};
    for (std::string line; std::getline(stream, line);)
        lines.push_back(std::move(line));
    std::unordered_set<std::string> intrinsic_groups;
    std::unordered_set<std::string> other_groups;
    auto group = [](std::string const& line) {
        auto hash = line.find('#');
        if (hash == std::string::npos)
            return std::string{};
        auto end = hash + 1;
        while (end < line.size() && line[end] >= '0' && line[end] <= '9')
            ++end;
        return line.substr(hash, end - hash);
    };
    auto intrinsic = [](std::string const& line) {
        return line.starts_with("declare ") && line.find("@llvm.") != std::string::npos;
    };
    for (auto const& line : lines)
    {
        if (line.starts_with("attributes #"))
            continue;
        auto id = group(line);
        if (!id.empty())
            (intrinsic(line) ? intrinsic_groups : other_groups).insert(id);
    }
    auto remove_nosync = [](std::string& line) {
        auto pos = line.find(" nosync");
        if (pos != std::string::npos && (pos + 7 == line.size() || line[pos + 7] == ' '))
            line.erase(pos, 7);
    };
    std::string result;
    bool module_asm = false;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        auto line = lines[i];
        if (line.starts_with("; Function Attrs:") && i + 1 < lines.size() && intrinsic(lines[i + 1]))
            remove_nosync(line);
        if (line.starts_with("attributes #") && intrinsic_groups.contains(group(line)) && !other_groups.contains(group(line)))
            remove_nosync(line);
        if (line == "module asm")
        {
            module_asm = true;
            continue;
        }
        auto start = line.find_first_not_of(" \t");
        if (module_asm && start != std::string::npos && line[start] == '"')
            line = "module asm " + line.substr(start);
        else
            module_asm = false;
        result += line + '\n';
    }
    return result;
}
