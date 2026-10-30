export module dpm.resolve;

import std;
import dpm.hash;
import dpm.lock;
import dpm.manifest;
import dpm.semver;

export namespace dpm::resolve
{
    struct Requirement
    {
        std::string location;
        std::string version;
    };

    struct PackageInfo
    {
        std::string name;
        std::string version;
        std::vector<Requirement> requirements;
        std::string requires_dcc;
        bool yanked{};
        std::string source;
        std::string commit;
        std::string hash;
    };

    class RequirementSource
    {
    public:
        virtual ~RequirementSource() = default;

        virtual std::optional<std::string> canonical(std::string_view alias, std::string& error)
        {
            error = std::format("no registry is available for alias '{}'", alias);
            return std::nullopt;
        }

        virtual std::optional<PackageInfo> info(std::string_view location, std::string_view version, std::string& error) = 0;

        virtual std::vector<std::string> versions(std::string_view) { return {}; }
    };

    inline constexpr std::size_t k_max_nodes = 20000;

    struct Options
    {
        std::string root_name;
        std::string dcc_version;
        manifest::Lock const* previous{};
    };

    struct Reason
    {
        std::string requested;
        std::vector<std::string> chain;
        bool dev{};
    };

    struct Package
    {
        std::string location;
        std::string version;
        std::string name;
        std::vector<std::string> dependencies;
        std::vector<Reason> reasons;
        std::size_t chosen{};
        bool dev{};
    };

    struct Resolution
    {
        bool ok{};
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        manifest::Lock lock;
        std::map<std::string, Package> packages;
        std::vector<std::string> roots;
        std::string root_label;
    };

    namespace detail
    {
        using NodeKey = std::pair<std::string, std::string>;

        struct Req
        {
            std::string location;
            std::string version;
            bool dev{};
            std::vector<std::string> chain;

            [[nodiscard]] auto key() const { return std::tie(location, version, dev, chain); }
        };

        inline bool is_path(std::string_view loc) { return loc.starts_with("path:"); }

        inline bool is_alias(std::string_view loc)
        {
            return loc.find('/') == std::string_view::npos && loc.find('.') == std::string_view::npos && loc.find(':') == std::string_view::npos;
        }

        inline std::string path_of(std::string_view location)
        {
            return std::string{location.substr(5)};
        }

        inline std::string normalize_path(std::filesystem::path const& base, std::string_view relative)
        {
            auto joined = (base / std::string{relative}).lexically_normal();
            auto text = joined.generic_string();
            while (text.size() > 1 && text.ends_with('/'))
                text.pop_back();
            if (text.empty())
                text = ".";
            return "path:" + text;
        }

        inline std::string label(std::string_view location, std::string_view version)
        {
            return version.empty() ? std::string{location} : std::format("{}@{}", location, version);
        }

        inline std::string describe(std::vector<std::string> const& chain)
        {
            std::string out = chain.back();
            if (chain.size() > 2)
            {
                out += " (via ";
                for (std::size_t i = 1; i + 1 < chain.size(); ++i)
                    out += (i > 1 ? " -> " : "") + chain[i];
                out += ")";
            }
            return out;
        }

        inline bool series_less(semver::Series const& a, semver::Series const& b)
        {
            return std::tuple{a.major, a.minor.has_value(), a.minor.value_or(0)} < std::tuple{b.major, b.minor.has_value(), b.minor.value_or(0)};
        }

        struct Resolver
        {
            RequirementSource& source;
            Options const& options;
            Resolution& out;

            Resolver(RequirementSource& s, Options const& o, Resolution& r) : source{s}, options{o}, out{r} {}

            std::map<std::string, std::string> aliases;
            std::map<NodeKey, PackageInfo> infos;
            std::map<NodeKey, std::vector<NodeKey>> children;
            std::map<std::string, std::vector<std::pair<Reason, std::string>>> reasons;
            std::map<std::string, std::vector<std::pair<semver::Series, Reason>>> series_reasons;

            std::optional<std::string> canonicalize(Requirement const& r, std::filesystem::path const& base, std::string& error)
            {
                if (is_path(r.location))
                    return normalize_path(base, path_of(r.location));
                if (!is_alias(r.location))
                    return r.location;
                if (auto it = aliases.find(r.location); it != aliases.end())
                    return it->second;
                auto resolved = source.canonical(r.location, error);
                if (resolved)
                    aliases.emplace(r.location, *resolved);
                return resolved;
            }

            std::optional<std::string> effective_version(std::string const& location, std::string const& version, PackageInfo const& info)
            {
                if (!info.yanked)
                    return version;
                if (options.previous)
                    for (auto const& p : options.previous->packages)
                        if (p.location == location && p.version == version)
                        {
                            out.warnings.push_back(std::format("{}@{} is yanked; kept from the lock", location, version));
                            return version;
                        }
                auto requested = semver::parse(version);
                if (!requested)
                    return std::nullopt;
                std::optional<semver::Version> best;
                std::string best_text;
                for (auto const& candidate_text : source.versions(location))
                {
                    auto candidate = semver::parse(candidate_text);
                    if (!candidate || !semver::same_series(*candidate, *requested) || semver::compare(*candidate, *requested) <= 0)
                        continue;
                    if (candidate->is_prerelease() && !requested->is_prerelease())
                        continue;
                    if (best && semver::compare(*candidate, *best) >= 0)
                        continue;
                    std::string ignored;
                    auto candidate_info = source.info(location, candidate_text, ignored);
                    if (!candidate_info || candidate_info->yanked)
                        continue;
                    best = candidate;
                    best_text = candidate_text;
                }
                if (!best)
                    return std::nullopt;
                out.warnings.push_back(std::format("{}@{} is yanked; using {}", location, version, best_text));
                return best_text;
            }

            void run(manifest::Project const& project)
            {
                std::vector<Req> level;
                for (auto const& [list, dev] : {std::pair{&project.dependencies, false}, std::pair{&project.dev_dependencies, true}})
                    for (auto const& d : *list)
                    {
                        std::string error;
                        auto loc = canonicalize({d.location, d.version}, ".", error);
                        if (!loc)
                        {
                            out.errors.push_back(std::format("cannot resolve '{}': {}", d.location, error));
                            continue;
                        }
                        level.push_back({*loc, d.version, dev, {options.root_name}});
                        out.roots.push_back(*loc);
                    }
                std::ranges::sort(out.roots);
                out.roots.erase(std::unique(out.roots.begin(), out.roots.end()), out.roots.end());
                std::set<NodeKey> visited;
                while (!level.empty() && out.errors.empty())
                {
                    std::ranges::sort(level, [](Req const& a, Req const& b) { return a.key() < b.key(); });
                    std::vector<Req> next;
                    for (auto const& r : level)
                    {
                        std::string error;
                        auto requested_info = source.info(r.location, r.version, error);
                        if (!requested_info)
                        {
                            out.errors.push_back(std::format("cannot resolve {}: {}", label(r.location, r.version), error));
                            continue;
                        }
                        std::string version = requested_info->version.empty() ? r.version : requested_info->version;
                        auto effective = effective_version(r.location, version, *requested_info);
                        if (!effective)
                        {
                            out.errors.push_back(std::format("{}@{} is yanked and no later version in its series is available", r.location, version));
                            continue;
                        }
                        if (!is_path(r.location))
                            if (auto parsed = semver::parse(r.version))
                                series_reasons[r.location].push_back({semver::series_of(*parsed), Reason{r.version, r.chain, r.dev}});
                        NodeKey key{r.location, *effective};
                        reasons[r.location].push_back({Reason{r.version, r.chain, r.dev}, *effective});
                        if (!visited.insert(key).second)
                            continue;
                        if (visited.size() > k_max_nodes)
                        {
                            out.errors.push_back(std::format("more than {} package versions in the requirement graph", k_max_nodes));
                            return;
                        }
                        auto info = *effective == version ? *requested_info : source.info(r.location, *effective, error).value_or(PackageInfo{});
                        auto base = is_path(r.location) ? std::filesystem::path{path_of(r.location)} : std::filesystem::path{};
                        auto chain = r.chain;
                        chain.push_back(label(r.location, *effective));
                        for (auto const& dep : info.requirements)
                        {
                            std::string dep_error;
                            if (is_path(dep.location) && !is_path(r.location))
                            {
                                out.errors.push_back(std::format("{} requires '{}'; only path packages may require path: dependencies", chain.back(), dep.location));
                                continue;
                            }
                            auto loc = canonicalize(dep, base, dep_error);
                            if (!loc)
                            {
                                out.errors.push_back(std::format("cannot resolve '{}' required by {}: {}", dep.location, chain.back(), dep_error));
                                continue;
                            }
                            next.push_back({*loc, dep.version, r.dev, chain});
                            std::string child_version = dep.version;
                            children[key].push_back({*loc, child_version});
                        }
                        infos.emplace(key, std::move(info));
                    }
                    level = std::move(next);
                }
            }

            void check_series()
            {
                for (auto& [location, list] : series_reasons)
                {
                    std::ranges::stable_sort(list, [](auto const& a, auto const& b) { return series_less(a.first, b.first); });
                    std::vector<std::pair<semver::Series, Reason>> firsts;
                    for (auto const& item : list)
                        if (firsts.empty() || !(firsts.back().first == item.first))
                            firsts.push_back(item);
                    if (firsts.size() < 2)
                        continue;
                    out.errors.push_back(std::format("{} needed as {} by {} and as {} by {}", location, firsts[0].first.to_string(), describe(firsts[0].second.chain),
                                                     firsts[1].first.to_string(), describe(firsts[1].second.chain)));
                }
            }

            void select()
            {
                std::map<std::string, std::string> selected;
                for (auto const& [location, list] : reasons)
                {
                    std::string best;
                    for (auto const& [reason, version] : list)
                    {
                        if (best.empty())
                        {
                            best = version;
                            continue;
                        }
                        auto a = semver::parse(version);
                        auto b = semver::parse(best);
                        if (a && b)
                        {
                            auto order = semver::compare(*a, *b);
                            if (order > 0 || (order == 0 && version < best))
                                best = version;
                        }
                    }
                    selected[location] = best;
                }
                std::set<NodeKey> normal;
                std::vector<NodeKey> stack;
                for (auto const& [location, list] : reasons)
                    for (auto const& [reason, version] : list)
                        if (reason.chain.size() == 1 && !reason.dev)
                            stack.push_back({location, version});
                while (!stack.empty())
                {
                    auto key = stack.back();
                    stack.pop_back();
                    if (!normal.insert(key).second)
                        continue;
                    auto it = children.find(key);
                    if (it == children.end())
                        continue;
                    for (auto const& [child_location, requested] : it->second)
                        for (auto const& [reason, version] : reasons[child_location])
                            if (reason.requested == requested || is_path(child_location))
                                stack.push_back({child_location, version});
                }
                for (auto const& [location, version] : selected)
                {
                    Package p;
                    p.location = location;
                    p.version = version;
                    auto const& info = infos[{location, version}];
                    p.name = info.name;
                    for (auto const& [reason, effective] : reasons[location])
                        p.reasons.push_back(reason);
                    std::ranges::stable_sort(p.reasons, [](Reason const& a, Reason const& b) { return std::tie(a.chain, a.requested) < std::tie(b.chain, b.requested); });
                    for (std::size_t i = 0; i < p.reasons.size(); ++i)
                    {
                        auto a = semver::parse(p.reasons[i].requested);
                        auto b = semver::parse(p.reasons[p.chosen].requested);
                        if (a && b && semver::compare(*a, *b) > 0)
                            p.chosen = i;
                    }
                    bool is_normal = false;
                    for (auto const& key : normal)
                        if (key.first == location)
                            is_normal = true;
                    p.dev = !is_normal;
                    std::set<std::string> deps;
                    if (auto it = children.find({location, version}); it != children.end())
                        for (auto const& child : it->second)
                            deps.insert(child.first);
                    p.dependencies.assign(deps.begin(), deps.end());
                    out.packages.emplace(location, std::move(p));
                }
            }

            void check_selected(manifest::Project const& project)
            {
                std::map<std::string, std::string> names;
                if (!options.root_name.empty())
                    names[options.root_name] = options.root_name;
                for (auto const& [location, p] : out.packages)
                {
                    auto const& info = infos[{location, p.version}];
                    if (!info.requires_dcc.empty() && !options.dcc_version.empty())
                    {
                        auto need = semver::parse(info.requires_dcc);
                        auto have = semver::parse(options.dcc_version);
                        if (need && have && semver::compare(*have, *need) < 0)
                            out.errors.push_back(std::format("{}@{} requires dcc {}; this is dcc {}", location, p.version, info.requires_dcc, options.dcc_version));
                    }
                    if (!p.name.empty())
                    {
                        auto [it, inserted] = names.emplace(p.name, location);
                        if (!inserted)
                            out.errors.push_back(std::format("{} and {} both provide the namespace '{}'", it->second, location, p.name));
                    }
                }
                (void)project;
            }

            void build_lock(manifest::Project const& project)
            {
                out.lock.manifest = lock::fingerprint(project);
                out.lock.dcc = options.dcc_version;
                for (auto const& [location, p] : out.packages)
                {
                    auto const& info = infos[{location, p.version}];
                    manifest::LockedPackage lp;
                    lp.location = location;
                    lp.version = p.version;
                    lp.source = is_path(location) ? location : info.source;
                    lp.commit = info.commit;
                    lp.hash = is_path(location) ? std::string{} : info.hash;
                    lp.dependencies = p.dependencies;
                    lp.dev = p.dev;
                    out.lock.packages.push_back(std::move(lp));
                }
            }
        };
    } // namespace detail

    [[nodiscard]] inline Resolution resolve(manifest::Project const& project, RequirementSource& source, Options const& options)
    {
        Resolution out;
        out.root_label = options.root_name.empty() ? project.name : options.root_name;
        Options effective = options;
        if (effective.root_name.empty())
            effective.root_name = project.name;
        detail::Resolver resolver{source, effective, out};
        resolver.run(project);
        if (out.errors.empty())
            resolver.check_series();
        if (!out.errors.empty())
            return out;
        resolver.select();
        resolver.check_selected(project);
        if (!out.errors.empty())
            return out;
        resolver.build_lock(project);
        out.ok = true;
        return out;
    }

    class PathSource : public RequirementSource
    {
    public:
        PathSource(std::filesystem::path root, std::filesystem::path schema_root) : root_{std::move(root)}, schema_root_{std::move(schema_root)} {}

        std::optional<PackageInfo> info(std::string_view location, std::string_view, std::string& error) override
        {
            if (!detail::is_path(location))
            {
                error = "this source can only read path: packages";
                return std::nullopt;
            }
            auto dir = root_ / detail::path_of(location);
            auto file = dir / "project.dcon";
            std::ifstream in{file, std::ios::binary};
            if (!in)
            {
                error = "cannot read " + file.string();
                return std::nullopt;
            }
            std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            manifest::LoadOptions options;
            options.file_name = file.string();
            options.schema_root = schema_root_;
            options.project_root = dir;
            auto loaded = manifest::load(text, options);
            if (!loaded.ok || !loaded.project)
            {
                error = loaded.diagnostics.empty() ? "invalid manifest" : loaded.diagnostics.front().format();
                return std::nullopt;
            }
            PackageInfo info;
            info.name = loaded.project->name;
            info.version = loaded.project->version;
            info.requires_dcc = loaded.project->requires_dcc;
            info.source = std::string{location};
            for (auto const& d : loaded.project->dependencies)
                info.requirements.push_back({d.location, d.version});
            return info;
        }

    private:
        std::filesystem::path root_;
        std::filesystem::path schema_root_;
    };

    [[nodiscard]] inline std::string render_tree(Resolution const& r, std::string const& root_version)
    {
        std::string out = detail::label(r.root_label, root_version) + "\n";
        std::set<std::string> expanded;
        std::function<void(std::vector<std::string> const&, std::string const&)> walk = [&](std::vector<std::string> const& children, std::string const& prefix)
        {
            for (std::size_t i = 0; i < children.size(); ++i)
            {
                auto const& p = r.packages.at(children[i]);
                bool const last = i + 1 == children.size();
                bool const again = !expanded.insert(p.location).second;
                out += prefix + (last ? "`-- " : "|-- ") + detail::label(p.location, p.version) + (p.dev ? " [dev]" : "") + (again && !p.dependencies.empty() ? " (*)" : "") + "\n";
                if (!again)
                    walk(p.dependencies, prefix + (last ? "    " : "|   "));
            }
        };
        walk(r.roots, "");
        return out;
    }

    [[nodiscard]] inline std::optional<std::string> why(Resolution const& r, std::string const& location)
    {
        auto it = r.packages.find(location);
        if (it == r.packages.end())
            return std::nullopt;
        auto const& p = it->second;
        std::string out = std::format("{} {}\n", p.location, p.version);
        for (std::size_t i = 0; i < p.reasons.size(); ++i)
        {
            auto const& reason = p.reasons[i];
            out += std::format("  {} needs {}{}{}\n", detail::describe(reason.chain), reason.requested.empty() ? "it" : ">= " + reason.requested, reason.dev ? " [dev]" : "",
                               i == p.chosen ? "  <- selected" : "");
        }
        return out;
    }
} // namespace dpm::resolve
