export module dcc.library;

import std;
import dcc.target;

export namespace dcc::library
{
    struct Source
    {
        std::string module;
        std::filesystem::path path;
    };

    struct Record
    {
        std::string module;
        std::string symbol;
        std::string section;
    };

    struct Manifest
    {
        std::filesystem::path root;
        std::vector<Source> sources;
        std::vector<Record> records;
        std::vector<std::pair<std::string, std::string>> edges;
        std::string startup;
        std::string main;

        [[nodiscard]] bool owns(std::string_view module, std::filesystem::path const& file) const
        {
            return std::ranges::any_of(sources, [&](auto const& source) {
                return source.module == module && std::filesystem::weakly_canonical(root / source.path) == std::filesystem::weakly_canonical(file);
            });
        }
    };

    [[nodiscard]] Manifest read_manifest(std::filesystem::path const& path, target::Os os)
    {
        Manifest result;
        result.root = path.parent_path();
        std::ifstream input(path);
        if (!input)
            throw std::runtime_error(std::format("cannot read library source manifest '{}'", path.string()));
        std::string header;
        std::getline(input, header);
        if (header != "dcc-libdcext-manifest 1")
            throw std::runtime_error("unsupported library source manifest version");
        auto variant = os == target::Os::Windows ? "windows" : os == target::Os::Linux ? "linux" : "freestanding";
        std::string line;
        std::unordered_set<std::string> modules;
        while (std::getline(input, line))
        {
            if (line.empty())
                continue;
            std::istringstream fields(line);
            std::string kind, profile, first, second, third, extra;
            fields >> kind >> profile >> first >> second;
            if (kind == "record")
                fields >> third;
            if (!fields || (fields >> extra))
                throw std::runtime_error(std::format("invalid library manifest entry '{}'", line));
            if (profile != "all" && profile != "hosted" && profile != "linux" && profile != "windows" && profile != "freestanding")
                throw std::runtime_error(std::format("invalid library manifest profile '{}'", profile));
            if (profile != "all" && profile != variant && !(profile == "hosted" && os != target::Os::Freestanding))
                continue;
            if (kind == "source" || kind == "runtime")
            {
                auto source_path = std::filesystem::path(second);
                if (source_path.is_absolute() || std::ranges::any_of(source_path, [](auto const& component) { return component == ".."; }) ||
                    !modules.insert(first).second)
                    throw std::runtime_error(std::format("invalid or duplicate library source '{}'", first));
                result.sources.push_back({first, source_path});
            }
            else if (kind == "startup")
            {
                if (!result.startup.empty())
                    throw std::runtime_error("duplicate library startup selection");
                result.startup = first;
                result.main = second;
            }
            else if (kind == "edge")
                result.edges.emplace_back(first, second);
            else if (kind == "record")
                result.records.push_back({first, second, third});
            else if (kind != "assembly")
                throw std::runtime_error(std::format("unknown library manifest entry '{}'", kind));
        }
        return result;
    }
} // namespace dcc::library
