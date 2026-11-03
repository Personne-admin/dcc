export module dpm.task;

import std;
import dpm.expand;
import dpm.hash;
import dpm.manifest;
import dpm.scaffold;
import dpm.stamp;

export namespace dpm::task
{
    struct Context
    {
        std::filesystem::path root;
        std::string profile;
        std::filesystem::path dcc;
        std::map<std::string, std::filesystem::path> outs;
        std::map<std::string, std::string> targets;
        std::map<std::string, std::filesystem::path> assets;
        std::map<std::string, std::string> asset_hashes;
        bool offline{};
        bool keep_unresolved{};
    };

    struct Expanded
    {
        std::vector<std::string> argv;
        std::filesystem::path cwd;
        std::map<std::string, std::string> env;
        std::vector<std::filesystem::path> inputs;
        std::vector<std::filesystem::path> outputs;
        std::set<std::string> products;
        std::set<std::string> assets;
    };

    [[nodiscard]] inline std::filesystem::path tasks_dir(std::filesystem::path const& root)
    {
        return root / ".dpm" / "tasks";
    }

    [[nodiscard]] inline std::filesystem::path task_dir(Context const& c, std::string_view name)
    {
        return tasks_dir(c.root) / std::string{name};
    }

    [[nodiscard]] inline std::filesystem::path stamp_path(Context const& c, std::string_view name)
    {
        return tasks_dir(c.root) / (std::string{name} + ".stamp");
    }

    [[nodiscard]] inline std::string env_suffix(std::string_view product)
    {
        std::string out;
        for (char c : product)
            out += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
        return out;
    }

    namespace detail
    {
        struct Text
        {
            std::string text;
            bool anchored{};
        };

        inline std::expected<Text, std::string> expand(std::string_view text, Context const& c, std::string_view task, Expanded* refs)
        {
            auto scanned = expand::scan(text);
            if (!scanned.ok())
                return std::unexpected{scanned.error};
            Text out;
            out.anchored = !scanned.pieces.empty() && expand::is_path_token(scanned.pieces[0].kind);
            for (auto const& piece : scanned.pieces)
            {
                auto unresolved = [&] { return std::format("${{{}{}{}}}", expand::token_name(piece.kind), piece.text.empty() ? "" : ":", piece.text); };
                switch (piece.kind)
                {
                case expand::Kind::Text:
                    out.text += piece.text;
                    break;
                case expand::Kind::Root:
                    out.text += c.root.generic_string();
                    break;
                case expand::Kind::Profile:
                    out.text += c.profile;
                    break;
                case expand::Kind::Dcc:
                    out.text += c.dcc.generic_string();
                    break;
                case expand::Kind::Out:
                case expand::Kind::Target:
                {
                    if (refs)
                        refs->products.insert(piece.text);
                    if (piece.kind == expand::Kind::Out)
                    {
                        auto it = c.outs.find(piece.text);
                        if (it == c.outs.end())
                            return std::unexpected{std::format("product '{}' is not declared", piece.text)};
                        out.text += it->second.generic_string();
                    }
                    else
                    {
                        auto it = c.targets.find(piece.text);
                        if (it == c.targets.end())
                            return std::unexpected{std::format("product '{}' is not declared", piece.text)};
                        out.text += it->second;
                    }
                    break;
                }
                case expand::Kind::Asset:
                {
                    if (refs)
                        refs->assets.insert(piece.text);
                    auto it = c.assets.find(piece.text);
                    if (it != c.assets.end())
                        out.text += it->second.generic_string();
                    else if (c.keep_unresolved)
                        out.text += unresolved();
                    else
                        return std::unexpected{std::format("asset '{}' is not available", piece.text)};
                    break;
                }
                case expand::Kind::TaskDir:
                    out.text += task_dir(c, piece.text.empty() ? task : std::string_view{piece.text}).generic_string();
                    break;
                }
            }
            return out;
        }

        inline std::expected<std::filesystem::path, std::string> expand_path(std::string_view text, Context const& c, std::string_view task, Expanded* refs)
        {
            auto e = expand(text, c, task, refs);
            if (!e)
                return std::unexpected{e.error()};
            std::filesystem::path p = e->anchored ? std::filesystem::path{e->text} : c.root / e->text;
            p = p.lexically_normal();
            auto s = p.generic_string();
            while (s.size() > 1 && s.ends_with('/'))
                s.pop_back();
            return std::filesystem::path{s};
        }

        inline bool has_glob(std::string_view s)
        {
            return s.find_first_of("*?") != std::string_view::npos;
        }
    } // namespace detail

    [[nodiscard]] inline std::expected<Expanded, std::string> expand_task(manifest::Task const& task, Context const& c, std::span<std::string const> extra_args = {})
    {
        Expanded out;
        for (auto const& b : task.build)
            out.products.insert(b);
        for (std::size_t i = 0; i < task.command.size(); ++i)
        {
            auto e = detail::expand(task.command[i], c, task.name, &out);
            if (!e)
                return std::unexpected{std::format("command: {}", e.error())};
            auto text = std::move(e->text);
            if (i == 0 && !e->anchored && text.find('/') != std::string::npos)
                text = (c.root / text).lexically_normal().generic_string();
            out.argv.push_back(std::move(text));
        }
        for (auto const& a : extra_args)
            out.argv.push_back(a);
        auto cwd = detail::expand_path(task.cwd.empty() ? "." : task.cwd, c, task.name, &out);
        if (!cwd)
            return std::unexpected{std::format("cwd: {}", cwd.error())};
        out.cwd = *cwd;
        out.env["DPM_ROOT"] = c.root.generic_string();
        out.env["DPM_PROFILE"] = c.profile;
        out.env["DPM_TASK"] = task.name;
        for (auto const& [name, path] : c.outs)
            out.env["DPM_OUT_" + env_suffix(name)] = path.generic_string();
        if (c.offline)
            out.env["DPM_OFFLINE"] = "1";
        for (auto const& v : task.env)
        {
            auto e = detail::expand(v.value, c, task.name, &out);
            if (!e)
                return std::unexpected{std::format("env {}: {}", v.name, e.error())};
            out.env[v.name] = std::move(e->text);
        }
        for (auto const& in : task.inputs)
        {
            auto p = detail::expand_path(in, c, task.name, &out);
            if (!p)
                return std::unexpected{std::format("input: {}", p.error())};
            out.inputs.push_back(std::move(*p));
        }
        for (auto const& o : task.outputs)
        {
            auto p = detail::expand_path(o, c, task.name, &out);
            if (!p)
                return std::unexpected{std::format("output: {}", p.error())};
            out.outputs.push_back(std::move(*p));
        }
        return out;
    }

    [[nodiscard]] inline std::vector<manifest::Task const*> order(manifest::Project const& project, std::string_view name)
    {
        std::vector<manifest::Task const*> out;
        std::set<std::string> done;
        std::set<std::string> active;
        std::function<void(std::string_view)> visit = [&](std::string_view n)
        {
            if (done.contains(std::string{n}) || active.contains(std::string{n}))
                return;
            auto it = std::ranges::find_if(project.tasks, [&](manifest::Task const& t) { return t.name == n; });
            if (it == project.tasks.end())
                return;
            active.insert(it->name);
            for (auto const& need : it->needs)
                visit(need);
            active.erase(it->name);
            done.insert(it->name);
            out.push_back(&*it);
        };
        visit(name);
        return out;
    }

    struct Needs
    {
        std::vector<std::string> products;
        std::set<std::string> assets;
    };

    [[nodiscard]] inline Needs needs_of(manifest::Project const& project, std::span<manifest::Task const* const> tasks)
    {
        Needs out;
        std::set<std::string> products;
        auto scan_assets = [&](std::string const& text)
        {
            auto scanned = expand::scan(text);
            for (auto const& piece : scanned.pieces)
                if (piece.kind == expand::Kind::Asset)
                    out.assets.insert(piece.text);
        };
        for (auto const* t : tasks)
        {
            for (auto const& b : t->build)
                products.insert(b);
            for (auto const& s : t->command)
                scan_assets(s);
            scan_assets(t->cwd);
            for (auto const& v : t->env)
                scan_assets(v.value);
            for (auto const& s : t->inputs)
                scan_assets(s);
            for (auto const& s : t->outputs)
                scan_assets(s);
        }
        for (auto const& p : project.products)
            if (products.contains(p.name))
                out.products.push_back(p.name);
        return out;
    }

    [[nodiscard]] inline std::expected<std::vector<std::filesystem::path>, std::string> collect_inputs(std::span<std::filesystem::path const> patterns)
    {
        std::set<std::filesystem::path> found;
        auto add_tree = [&](std::filesystem::path const& dir, std::string_view pattern) -> std::optional<std::string>
        {
            std::error_code ec;
            std::filesystem::recursive_directory_iterator it{dir, std::filesystem::directory_options::none, ec};
            if (ec)
                return std::nullopt;
            for (; it != std::filesystem::recursive_directory_iterator{}; it.increment(ec))
            {
                if (ec)
                    return std::format("cannot list {}", dir.generic_string());
                if (!it->is_regular_file(ec))
                    continue;
                auto rel = it->path().lexically_relative(dir).generic_string();
                if (pattern.empty() || scaffold::glob_match(pattern, rel))
                    found.insert(it->path().lexically_normal());
                if (found.size() > stamp::k_max_inputs)
                    return std::format("more than {} input files", stamp::k_max_inputs);
            }
            return std::nullopt;
        };
        for (auto const& pattern : patterns)
        {
            auto text = pattern.generic_string();
            if (!detail::has_glob(text))
            {
                std::error_code ec;
                auto status = std::filesystem::status(pattern, ec);
                if (std::filesystem::is_regular_file(status))
                    found.insert(pattern);
                else if (std::filesystem::is_directory(status))
                {
                    if (auto problem = add_tree(pattern, {}))
                        return std::unexpected{*problem};
                }
                else
                    return std::unexpected{std::format("input {} does not exist", text)};
                continue;
            }
            auto star = text.find_first_of("*?");
            auto slash = text.rfind('/', star);
            auto base = std::filesystem::path{text.substr(0, slash == std::string::npos ? 0 : slash)};
            auto rest = slash == std::string::npos ? text : text.substr(slash + 1);
            if (auto problem = add_tree(base, rest))
                return std::unexpected{*problem};
        }
        return std::vector<std::filesystem::path>{found.begin(), found.end()};
    }

    [[nodiscard]] inline std::string make_key(Expanded const& e, Context const& c)
    {
        std::vector<std::string> parts;
        parts.push_back("argv");
        for (auto const& a : e.argv)
            parts.push_back(a);
        parts.push_back("cwd");
        parts.push_back(e.cwd.generic_string());
        parts.push_back("env");
        for (auto const& [k, v] : e.env)
            parts.push_back(k + "=" + v);
        for (auto const& p : e.products)
        {
            parts.push_back("product " + p);
            auto it = c.outs.find(p);
            auto h = it == c.outs.end() ? hash::FileHash{} : hash::hash_file(it->second);
            parts.push_back(h.ok ? h.hex : std::string{"missing"});
        }
        for (auto const& a : e.assets)
        {
            parts.push_back("asset " + a);
            auto it = c.asset_hashes.find(a);
            parts.push_back(it == c.asset_hashes.end() ? std::string{"missing"} : it->second);
        }
        parts.push_back("outputs");
        for (auto const& o : e.outputs)
            parts.push_back(o.generic_string());
        return stamp::make_task_key(parts);
    }
} // namespace dpm::task
