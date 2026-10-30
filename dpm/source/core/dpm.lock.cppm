export module dpm.lock;

import std;
import dpm.hash;
import dpm.manifest;

export namespace dpm::lock
{
    inline constexpr std::string_view k_lock_file = "project.lock.dcon";

    [[nodiscard]] inline std::string fingerprint(manifest::Project const& project)
    {
        struct Line
        {
            std::string location;
            std::string version;
            bool dev;

            [[nodiscard]] auto key() const { return std::tie(location, version, dev); }
        };
        std::vector<Line> lines;
        for (auto const& d : project.dependencies)
            lines.push_back({d.location, d.version, false});
        for (auto const& d : project.dev_dependencies)
            lines.push_back({d.location, d.version, true});
        std::ranges::sort(lines, [](Line const& a, Line const& b) { return a.key() < b.key(); });
        std::string text = "dpm::v1\n";
        for (auto const& l : lines)
            text += std::format("{}\t{}\t{}\n", l.dev ? "dev" : "dep", l.location, l.version);
        return "sha256:" + hash::sha256_hex(text);
    }

    [[nodiscard]] inline bool is_current(manifest::Project const& project, manifest::Lock const& lock)
    {
        return lock.manifest == fingerprint(project);
    }

    namespace detail
    {
        inline std::string quote(std::string_view s)
        {
            std::string out = "\"";
            for (char c : s)
            {
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out += static_cast<unsigned char>(c) < 0x20 ? '?' : c;
                }
            }
            return out + "\"";
        }
    } // namespace detail

    [[nodiscard]] inline std::string print(manifest::Lock const& lock)
    {
        std::string out = "dpm::v1::Lock {\n";
        out += std::format("    manifest = {},\n", detail::quote(lock.manifest));
        if (!lock.dcc.empty())
            out += std::format("    dcc = {},\n", detail::quote(lock.dcc));
        if (lock.packages.empty())
            return out + "    packages = {},\n}\n";
        out += "    packages = {\n";
        for (auto const& p : lock.packages)
        {
            out += "        {\n";
            out += std::format("            location = {},\n", detail::quote(p.location));
            out += std::format("            version = {},\n", detail::quote(p.version));
            out += std::format("            source = {},\n", detail::quote(p.source));
            if (!p.commit.empty())
                out += std::format("            commit = {},\n", detail::quote(p.commit));
            out += std::format("            hash = {},\n", detail::quote(p.hash));
            if (!p.dependencies.empty())
            {
                out += "            dependencies = {\n";
                for (auto const& d : p.dependencies)
                    out += std::format("                {},\n", detail::quote(d));
                out += "            },\n";
            }
            if (p.dev)
                out += "            dev = Tri::On,\n";
            out += "        },\n";
        }
        return out + "    },\n}\n";
    }

    [[nodiscard]] inline bool same_content(manifest::Lock const& a, manifest::Lock const& b)
    {
        return print(a) == print(b);
    }
} // namespace dpm::lock
