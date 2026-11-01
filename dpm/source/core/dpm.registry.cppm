module;

#include <unistd.h>

export module dpm.registry;

import std;
import dpm.cache;
import dpm.fetch;
import dpm.hash;
import dpm.manifest;
import dpm.process;
import dpm.resolve;
import dpm.semver;

export namespace dpm::registry
{
    inline constexpr std::string_view k_default_url = "https://raw.githubusercontent.com/Personne-admin/dc-registry/main";
    inline constexpr std::uint64_t k_max_file = 256ull * 1024;

    struct Config
    {
        std::vector<std::string> urls;
        std::filesystem::path schema_root;
        std::string curl{"curl"};
        bool allow_file{};
        bool allow_loopback{};
        bool offline{};
        bool refresh{};
        std::uint64_t max_file = k_max_file;
        std::chrono::seconds fresh{24 * 3600};
        std::chrono::seconds connect_timeout{15};
        std::chrono::seconds total_timeout{60};
    };

    [[nodiscard]] inline std::optional<std::string> url_error(std::string_view url, bool allow_file, bool allow_loopback)
    {
        if (std::ranges::any_of(url, [](char c) { return static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) == 0x7f || c == '\\'; }))
            return "URL contains whitespace, control characters or a backslash";
        if (url.find('?') != std::string_view::npos || url.find('#') != std::string_view::npos)
            return "URL must not have a query or a fragment";
        auto authority_of = [](std::string_view rest) { return rest.substr(0, rest.find('/')); };
        if (url.starts_with("https://"))
        {
            auto authority = authority_of(url.substr(8));
            if (authority.empty() || authority.find('@') != std::string_view::npos)
                return "https URL needs a host and no credentials";
            return std::nullopt;
        }
        if (url.starts_with("file://"))
        {
            if (!allow_file)
                return "file:// registries are only available for tests (DPM_ALLOW_FILE_URLS=1)";
            if (url.size() < 9 || url[7] != '/')
                return "file URL needs an absolute path";
            return std::nullopt;
        }
        if (url.starts_with("http://"))
        {
            if (!allow_loopback)
                return "registries must use https";
            auto authority = authority_of(url.substr(7));
            auto host = authority;
            if (authority.starts_with('['))
                host = authority.substr(0, authority.find(']') == std::string_view::npos ? authority.size() : authority.find(']') + 1);
            else
                host = authority.substr(0, authority.find(':'));
            auto port = authority.substr(host.size());
            bool const port_ok = port.empty() || (port.size() > 1 && port[0] == ':' && std::ranges::all_of(port.substr(1), [](char c) { return c >= '0' && c <= '9'; }));
            if (!port_ok || !(host == "127.0.0.1" || host == "localhost" || host == "[::1]"))
                return "plain http is only accepted for loopback hosts";
            return std::nullopt;
        }
        return "registries must use https";
    }

    [[nodiscard]] inline std::string normalize_url(std::string url)
    {
        while (url.size() > 1 && url.back() == '/')
            url.pop_back();
        return url;
    }

    struct Choice
    {
        std::vector<std::string> urls;
        std::string error;
    };

    [[nodiscard]] inline Choice choose_registries(std::optional<std::string> env, std::string const& project, std::vector<std::string> const& user, bool allow_file,
                                                  bool allow_loopback)
    {
        Choice choice;
        if (env)
        {
            if (!env->empty())
                choice.urls.push_back(*env);
        }
        else if (!project.empty())
            choice.urls.push_back(project);
        else if (!user.empty())
            choice.urls = user;
        else
            choice.urls.emplace_back(k_default_url);
        for (auto& url : choice.urls)
        {
            if (auto bad = url_error(url, allow_file, allow_loopback))
            {
                choice.error = std::format("registry '{}': {}", url, *bad);
                choice.urls.clear();
                return choice;
            }
            url = normalize_url(url);
        }
        return choice;
    }

    [[nodiscard]] inline std::string alias_path(std::string_view alias)
    {
        auto shard = alias.size() >= 2 ? std::string{alias.substr(0, 2)} : "_" + std::string{alias};
        return std::format("aliases/{}/{}.dcon", shard, alias);
    }

    [[nodiscard]] inline std::string template_path(std::string_view name)
    {
        auto shard = name.size() >= 2 ? std::string{name.substr(0, 2)} : "_" + std::string{name};
        return std::format("templates/{}/{}.dcon", shard, name);
    }

    [[nodiscard]] inline std::optional<std::string> location_path(std::string_view location)
    {
        static constexpr std::array<std::string_view, 5> hosts{"github.com", "gitlab.com", "codeberg.org", "bitbucket.org", "sr.ht"};
        std::vector<std::string_view> parts;
        std::size_t pos = 0;
        while (pos <= location.size())
        {
            auto next = location.find('/', pos);
            parts.push_back(location.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
            if (next == std::string_view::npos)
                break;
            pos = next + 1;
        }
        if (parts.size() < 3 || std::ranges::find(hosts, parts[0]) == hosts.end())
            return std::nullopt;
        for (auto part : parts)
        {
            if (part.empty() || part == "." || part == "..")
                return std::nullopt;
            bool const ok = std::ranges::all_of(part, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-' || c == '~'; });
            if (!ok)
                return std::nullopt;
        }
        return "locations/" + std::string{location} + ".dcon";
    }

    enum class Outcome : std::uint8_t
    {
        Ok,
        Missing,
        Failed,
    };

    struct Got
    {
        Outcome outcome{Outcome::Failed};
        std::string text;
        std::string error;
        bool stale{};
    };

    using Validate = std::function<std::optional<std::string>(std::string const&)>;

    namespace detail
    {
        inline std::optional<std::string> read_limited(std::filesystem::path const& path, std::uint64_t limit, bool& too_big)
        {
            too_big = false;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
                return std::nullopt;
            std::ifstream in{path, std::ios::binary};
            if (!in)
                return std::nullopt;
            std::string text;
            std::array<char, 8192> buffer{};
            while (in)
            {
                in.read(buffer.data(), buffer.size());
                text.append(buffer.data(), static_cast<std::size_t>(in.gcount()));
                if (text.size() > limit)
                {
                    too_big = true;
                    return std::nullopt;
                }
            }
            return text;
        }

        inline bool write_atomic(std::filesystem::path const& path, std::string_view data)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec)
                return false;
            auto tmp = path;
            tmp += std::format(".tmp{}", ::getpid());
            {
                std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
                if (!out)
                    return false;
                out.write(data.data(), static_cast<std::streamsize>(data.size()));
                if (!out)
                    return false;
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec)
            {
                std::filesystem::remove(tmp, ec);
                return false;
            }
            return true;
        }

        inline bool etag_ok(std::string_view etag)
        {
            return !etag.empty() && etag.size() <= 200 &&
                   std::ranges::all_of(etag, [](char c) { return static_cast<unsigned char>(c) > 0x20 && static_cast<unsigned char>(c) < 0x7f; });
        }

        inline std::string header_etag(std::string_view headers)
        {
            std::string found;
            std::size_t pos = 0;
            while (pos < headers.size())
            {
                auto end = headers.find('\n', pos);
                auto line = headers.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
                pos = end == std::string_view::npos ? headers.size() : end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.remove_suffix(1);
                auto colon = line.find(':');
                if (colon == std::string_view::npos)
                    continue;
                std::string name{line.substr(0, colon)};
                for (auto& c : name)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (name != "etag")
                    continue;
                auto value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ')
                    value.remove_prefix(1);
                found = etag_ok(value) ? std::string{value} : std::string{};
            }
            return found;
        }
    } // namespace detail

    class Transport
    {
    public:
        Transport(cache::Cache cache, Config config) : cache_{std::move(cache)}, config_{std::move(config)} {}

        std::vector<std::string> warnings;

        [[nodiscard]] Config const& config() const { return config_; }

        Got get(std::string const& base, std::string const& rel, Validate const& validate)
        {
            if (base.starts_with("file://"))
                return get_file(base, rel, validate);
            return get_remote(base, rel, validate);
        }

    private:
        static Got failure(std::string message)
        {
            Got g;
            g.error = std::move(message);
            return g;
        }

        Got checked(std::string text, Validate const& validate) const
        {
            if (auto bad = validate(text))
                return failure(*bad);
            Got g;
            g.outcome = Outcome::Ok;
            g.text = std::move(text);
            return g;
        }

        Got get_file(std::string const& base, std::string const& rel, Validate const& validate) const
        {
            auto path = std::filesystem::path{base.substr(7)} / rel;
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
            {
                Got g;
                g.outcome = Outcome::Missing;
                return g;
            }
            bool too_big = false;
            auto text = detail::read_limited(path, config_.max_file, too_big);
            if (too_big)
                return failure(std::format("{} is larger than {} bytes", rel, config_.max_file));
            if (!text)
                return failure("cannot read " + path.string());
            return checked(std::move(*text), validate);
        }

        Got get_remote(std::string const& base, std::string const& rel, Validate const& validate)
        {
            namespace fs = std::filesystem;
            auto id = hash::sha256_hex(base).substr(0, 16);
            auto path = cache_.registry_dir() / id / rel;
            auto etag_path = path;
            etag_path += ".etag";
            auto missing_path = path;
            missing_path += ".missing";
            std::error_code ec;
            auto age_of = [&](fs::path const& p) -> std::optional<std::chrono::seconds>
            {
                auto time = fs::last_write_time(p, ec);
                if (ec)
                    return std::nullopt;
                auto age = std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - time);
                return age < std::chrono::seconds{0} ? std::chrono::seconds{0} : age;
            };
            std::optional<std::string> cached;
            bool too_big = false;
            if (auto text = detail::read_limited(path, config_.max_file, too_big); text && !validate(*text))
                cached = std::move(text);
            auto cached_age = cached ? age_of(path) : std::nullopt;
            std::string etag;
            if (cached)
                if (auto text = detail::read_limited(etag_path, 256, too_big); text && detail::etag_ok(*text))
                    etag = *text;
            auto from_cache = [&](bool stale)
            {
                Got g;
                g.outcome = Outcome::Ok;
                g.text = *cached;
                g.stale = stale;
                return g;
            };
            if (cached && (config_.offline || (!config_.refresh && cached_age && *cached_age < config_.fresh)))
                return from_cache(false);
            if (!cached)
            {
                auto missing_age = age_of(missing_path);
                if (missing_age && (config_.offline || (!config_.refresh && *missing_age < config_.fresh)))
                {
                    Got g;
                    g.outcome = Outcome::Missing;
                    return g;
                }
            }
            if (config_.offline)
                return failure(std::format("not in cache: registry file {} (offline mode)", rel));

            auto url = base + "/" + rel;
            auto work = cache_.tmp_dir() / std::format("reg-{}-{}", ::getpid(), hash::sha256_hex(url).substr(0, 12));
            fs::create_directories(work, ec);
            auto body = work / "body";
            auto headers = work / "headers";
            auto cleanup = [&] { fs::remove_all(work, ec); };
            bool const plain = base.starts_with("http://");
            process::Command cmd;
            cmd.argv = {config_.curl, "-q", "--silent", "--show-error", "--location", "--proto", plain ? "=https,http" : "=https", "--proto-redir", "=https", "--max-redirs", "3",
                        "--max-filesize", std::to_string(config_.max_file), "--connect-timeout", std::to_string(config_.connect_timeout.count()), "--max-time",
                        std::to_string(config_.total_timeout.count()), "-D", headers.string(), "-o", body.string(), "-w", "%{http_code}"};
            if (plain)
            {
                cmd.argv.emplace_back("--noproxy");
                cmd.argv.emplace_back("*");
            }
            if (cached && !etag.empty())
            {
                cmd.argv.emplace_back("-H");
                cmd.argv.emplace_back("If-None-Match: " + etag);
            }
            cmd.argv.emplace_back("--");
            cmd.argv.push_back(url);
            cmd.env = fetch::detail::curl_env();
            cmd.max_file_size = config_.max_file + 1;
            cmd.timeout = config_.total_timeout + std::chrono::seconds(5);
            auto r = process::run(cmd);
            auto fail = [&](std::string message)
            {
                cleanup();
                if (cached)
                {
                    warnings.push_back(std::format("could not refresh registry file {} ({}); using the cached copy", rel, message));
                    return from_cache(true);
                }
                return failure(std::format("cannot fetch {}: {}", url, message));
            };
            if (!r.ok())
                return fail(fetch::tarball_error_text(r));
            int status = 0;
            {
                auto text = r.out;
                std::from_chars(text.data(), text.data() + text.size(), status);
            }
            if (status == 304 && cached)
            {
                fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
                cleanup();
                return from_cache(false);
            }
            if (status == 404 || status == 410)
            {
                cleanup();
                if (cached)
                {
                    fs::remove(path, ec);
                    fs::remove(etag_path, ec);
                }
                detail::write_atomic(missing_path, "");
                Got g;
                g.outcome = Outcome::Missing;
                return g;
            }
            if (status != 200)
                return fail(std::format("HTTP status {}", status));
            auto text = detail::read_limited(body, config_.max_file, too_big);
            if (too_big)
                return fail(std::format("response is larger than {} bytes", config_.max_file));
            if (!text)
                return fail("empty response");
            if (auto bad = validate(*text))
            {
                cleanup();
                return failure(std::format("{}: {}", url, *bad));
            }
            auto new_etag = detail::header_etag(detail::read_limited(headers, 65536, too_big).value_or(""));
            detail::write_atomic(path, *text);
            if (new_etag.empty())
                fs::remove(etag_path, ec);
            else
                detail::write_atomic(etag_path, new_etag);
            fs::remove(missing_path, ec);
            cleanup();
            Got g;
            g.outcome = Outcome::Ok;
            g.text = std::move(*text);
            return g;
        }

        cache::Cache cache_;
        Config config_;
    };

    struct AliasHit
    {
        manifest::RegistryAlias record;
        std::string registry;
    };

    struct TemplateHit
    {
        manifest::RegistryTemplate record;
        std::string registry;
    };

    [[nodiscard]] inline std::optional<std::string> expected_git_source(std::string_view location, fetch::Policy const& policy)
    {
        std::string ignored;
        auto spec = fetch::parse_git_location(location, policy, ignored);
        if (!spec)
            return std::nullopt;
        return "git+" + spec->url + (spec->subdir.empty() ? std::string{} : "#" + spec->subdir);
    }

    [[nodiscard]] inline std::optional<std::string> versions_problem(std::string const& location, std::vector<manifest::RegistryVersion> const& versions, bool allow_file)
    {
        fetch::Policy policy;
        policy.allow_file = allow_file;
        bool const tarball_location = fetch::is_tarball_location(location, policy);
        auto expected = expected_git_source(location, policy);
        for (auto const& v : versions)
        {
            if (!v.source.starts_with("git+"))
                continue;
            if (tarball_location || !expected || v.source != *expected)
                return std::format("version {} has source {}, which does not belong to {}", v.version, v.source, location);
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::string> record_problem(manifest::RegistryAlias const& record, bool allow_file)
    {
        return versions_problem(record.location, record.versions, allow_file);
    }

    class Client
    {
    public:
        Client(cache::Cache cache, Config config) : transport_{std::move(cache), config}, config_{std::move(config)} {}

        [[nodiscard]] bool enabled() const { return !config_.urls.empty(); }
        [[nodiscard]] std::vector<std::string> take_warnings()
        {
            auto out = std::move(transport_.warnings);
            transport_.warnings.clear();
            for (auto& w : warnings_)
                out.push_back(std::move(w));
            warnings_.clear();
            return out;
        }

        std::optional<AliasHit> alias(std::string const& name, std::string& error)
        {
            if (auto it = aliases_.find(name); it != aliases_.end())
            {
                if (!it->second)
                    error = alias_errors_[name];
                return it->second;
            }
            auto result = lookup_alias(name, error);
            if (!result)
                alias_errors_[name] = error;
            aliases_[name] = result;
            return result;
        }

        std::optional<TemplateHit> tmpl(std::string const& name, std::string& error)
        {
            if (auto it = templates_.find(name); it != templates_.end())
            {
                if (!it->second)
                    error = template_errors_[name];
                return it->second;
            }
            auto rel = template_path(name);
            auto found = lookup_record<manifest::RegistryTemplate>(
                "template", name, rel, error, [&](std::string const& text, std::optional<manifest::RegistryTemplate>& out) { return load_template(text, name, rel, out); });
            std::optional<TemplateHit> result;
            if (found)
                result = TemplateHit{std::move(found->first), std::move(found->second)};
            else
                template_errors_[name] = error;
            templates_[name] = result;
            return result;
        }

        std::optional<AliasHit> by_location(std::string const& location)
        {
            if (auto it = pointers_.find(location); it != pointers_.end())
                return it->second;
            std::optional<AliasHit> result;
            if (auto rel = location_path(location))
            {
                for (auto const& base : config_.urls)
                {
                    if (!usable(base))
                        continue;
                    std::optional<manifest::RegistryLocation> pointer;
                    auto got = transport_.get(base, *rel, [&](std::string const& text) { return load_pointer(text, *rel, pointer); });
                    if (got.outcome == Outcome::Failed)
                    {
                        if (!got.error.starts_with("not in cache"))
                            warn_once(std::format("registry {}: {}", base, got.error));
                        continue;
                    }
                    if (got.outcome == Outcome::Missing || !pointer)
                        continue;
                    std::string error;
                    auto hit = alias(pointer->alias, error);
                    if (!hit)
                    {
                        warn_once(std::format("registry pointer for {} names alias '{}': {}", location, pointer->alias, error));
                        continue;
                    }
                    if (hit->record.location != location)
                    {
                        warn_once(std::format("registry pointer for {} names alias '{}', which belongs to {}; ignored", location, pointer->alias, hit->record.location));
                        continue;
                    }
                    result = std::move(hit);
                    break;
                }
            }
            pointers_[location] = result;
            return result;
        }

    private:
        void warn_once(std::string message)
        {
            if (std::ranges::find(seen_, message) != seen_.end())
                return;
            seen_.push_back(message);
            warnings_.push_back(std::move(message));
        }

        std::optional<std::string> load_file(std::string_view text, manifest::FileKind kind, std::string const& label, manifest::LoadResult& out) const
        {
            manifest::LoadOptions options;
            options.file_name = label;
            options.schema_root = config_.schema_root;
            options.kind = kind;
            options.allow_file_urls = config_.allow_file;
            out = manifest::load(text, options);
            if (!out.ok)
                return out.diagnostics.empty() ? "invalid file" : out.diagnostics.front().format();
            return std::nullopt;
        }

        std::optional<std::string> load_pointer(std::string const& text, std::string const& rel, std::optional<manifest::RegistryLocation>& out) const
        {
            manifest::LoadResult loaded;
            if (auto bad = load_file(text, manifest::FileKind::RegistryLocation, rel, loaded))
                return bad;
            out = loaded.registry_location;
            return std::nullopt;
        }

        std::optional<std::string> load_template(std::string const& text, std::string const& name, std::string const& rel, std::optional<manifest::RegistryTemplate>& out) const
        {
            manifest::LoadResult loaded;
            if (auto bad = load_file(text, manifest::FileKind::RegistryTemplate, rel, loaded))
                return bad;
            auto const& record = *loaded.registry_template;
            if (record.name != name)
                return std::format("{} describes template '{}', not '{}'", rel, record.name, name);
            if (auto problem = versions_problem(record.location, record.versions, config_.allow_file))
                return std::format("{}: {}", rel, *problem);
            out = record;
            return std::nullopt;
        }

        std::optional<std::string> load_alias(std::string const& text, std::string const& name, std::string const& rel, std::optional<manifest::RegistryAlias>& out) const
        {
            manifest::LoadResult loaded;
            if (auto bad = load_file(text, manifest::FileKind::RegistryAlias, rel, loaded))
                return bad;
            auto const& record = *loaded.registry_alias;
            if (record.alias != name)
                return std::format("{} describes alias '{}', not '{}'", rel, record.alias, name);
            if (auto problem = record_problem(record, config_.allow_file))
                return std::format("{}: {}", rel, *problem);
            out = record;
            return std::nullopt;
        }

        bool usable(std::string const& base)
        {
            if (auto it = usable_.find(base); it != usable_.end())
                return it->second;
            std::optional<manifest::RegistryConfig> descriptor;
            auto got = transport_.get(base, "registry.dcon",
                                      [&](std::string const& text)
                                      {
                                          manifest::LoadResult loaded;
                                          if (auto bad = load_file(text, manifest::FileKind::RegistryConfig, "registry.dcon", loaded))
                                              return bad;
                                          descriptor = loaded.registry_config;
                                          return std::optional<std::string>{};
                                      });
            bool ok = got.outcome == Outcome::Ok;
            if (got.outcome == Outcome::Missing)
                unusable_[base] = "it has no registry.dcon, so it is not a dpm registry";
            else if (got.outcome == Outcome::Failed)
                unusable_[base] = got.error;
            usable_[base] = ok;
            return ok;
        }

        template <class Record, class Load>
        std::optional<std::pair<Record, std::string>> lookup_record(std::string const& what, std::string const& name, std::string const& rel, std::string& error, Load load)
        {
            if (config_.urls.empty())
            {
                error = "no registry is configured";
                return std::nullopt;
            }
            std::vector<std::string> problems;
            for (auto const& base : config_.urls)
            {
                if (!usable(base))
                {
                    problems.push_back(std::format("{}: {}", base, unusable_[base]));
                    continue;
                }
                std::optional<Record> record;
                auto got = transport_.get(base, rel, [&](std::string const& text) { return load(text, record); });
                if (got.outcome == Outcome::Missing)
                    continue;
                if (got.outcome == Outcome::Failed)
                {
                    problems.push_back(std::format("{}: {}", base, got.error));
                    continue;
                }
                if (!record)
                {
                    error = std::format("{}: cannot read {}", base, rel);
                    return std::nullopt;
                }
                return std::pair<Record, std::string>{std::move(*record), base};
            }
            if (problems.empty())
                error = std::format("{} '{}' is not in any registry", what, name);
            else
            {
                error = std::format("{} '{}' could not be looked up", what, name);
                for (auto const& p : problems)
                    error += "; " + p;
            }
            return std::nullopt;
        }

        std::optional<AliasHit> lookup_alias(std::string const& name, std::string& error)
        {
            auto rel = alias_path(name);
            auto found = lookup_record<manifest::RegistryAlias>("alias", name, rel, error,
                                                                [&](std::string const& text, std::optional<manifest::RegistryAlias>& out) { return load_alias(text, name, rel, out); });
            if (!found)
                return std::nullopt;
            return AliasHit{std::move(found->first), std::move(found->second)};
        }

        Transport transport_;
        Config config_;
        std::map<std::string, std::optional<AliasHit>> aliases_;
        std::map<std::string, std::string> alias_errors_;
        std::map<std::string, std::optional<AliasHit>> pointers_;
        std::map<std::string, std::optional<TemplateHit>> templates_;
        std::map<std::string, std::string> template_errors_;
        std::map<std::string, bool> usable_;
        std::map<std::string, std::string> unusable_;
        std::vector<std::string> warnings_;
        std::vector<std::string> seen_;
    };

    [[nodiscard]] inline std::optional<std::string> latest_version(std::span<manifest::RegistryVersion const> versions, std::optional<semver::Series> within)
    {
        std::optional<semver::Version> best;
        std::string best_text;
        for (auto const& v : versions)
        {
            if (v.yanked)
                continue;
            auto parsed = semver::parse(v.version);
            if (!parsed || parsed->is_prerelease())
                continue;
            if (within && !(semver::series_of(*parsed) == *within))
                continue;
            if (!best || semver::compare(*parsed, *best) > 0)
            {
                best = parsed;
                best_text = v.version;
            }
        }
        if (!best)
            return std::nullopt;
        return best_text;
    }

    [[nodiscard]] inline std::optional<std::string> latest_version(manifest::RegistryAlias const& record, std::optional<semver::Series> within)
    {
        return latest_version(std::span<manifest::RegistryVersion const>{record.versions}, within);
    }

    class RegistrySource : public resolve::RequirementSource
    {
    public:
        RegistrySource(Client& client, resolve::RequirementSource& inner, manifest::Lock const* previous)
            : client_{client}, inner_{inner}, previous_{previous}
        {
        }

        std::vector<std::string> warnings;

        std::optional<std::string> canonical(std::string_view alias, std::string& error) override
        {
            auto hit = client_.alias(std::string{alias}, error);
            if (!hit)
                return std::nullopt;
            records_[hit->record.location] = hit->record;
            return hit->record.location;
        }

        std::optional<resolve::PackageInfo> info(std::string_view location, std::string_view version, std::string& error) override
        {
            if (!location.starts_with("path:"))
                if (auto const* record = record_for(std::string{location}))
                    if (auto const* entry = entry_for(*record, version))
                        return summarize(std::string{location}, *entry);
            return inner_.info(location, version, error);
        }

        std::vector<std::string> versions(std::string_view location) override
        {
            if (!location.starts_with("path:"))
                if (auto const* record = record_for(std::string{location}))
                {
                    std::vector<std::string> out;
                    for (auto const& v : record->versions)
                        out.push_back(v.version);
                    return out;
                }
            return inner_.versions(location);
        }

    private:
        manifest::RegistryAlias const* record_for(std::string const& location)
        {
            if (auto it = records_.find(location); it != records_.end())
                return &it->second;
            if (auto hit = client_.by_location(location))
                return &records_.emplace(location, std::move(hit->record)).first->second;
            return nullptr;
        }

        static manifest::RegistryVersion const* entry_for(manifest::RegistryAlias const& record, std::string_view version)
        {
            auto wanted = semver::parse(version);
            if (!wanted)
                return nullptr;
            for (auto const& v : record.versions)
                if (auto parsed = semver::parse(v.version); parsed && semver::compare(*parsed, *wanted) == 0)
                    return &v;
            return nullptr;
        }

        resolve::PackageInfo summarize(std::string const& location, manifest::RegistryVersion const& entry)
        {
            resolve::PackageInfo info;
            info.version = entry.version;
            info.requires_dcc = entry.requires_dcc;
            info.yanked = entry.yanked;
            info.source = entry.source;
            info.commit = entry.commit;
            info.hash = entry.hash;
            for (auto const& d : entry.dependencies)
                info.requirements.push_back({d.location, d.version});
            if (previous_)
                for (auto const& p : previous_->packages)
                    if (p.location == location && p.version == entry.version)
                    {
                        if (p.source != entry.source || p.commit != entry.commit || p.hash != entry.hash)
                            warnings.push_back(std::format("the registry entry for {}@{} differs from the lock; keeping the lock's source, commit and hash", location, entry.version));
                        info.source = p.source;
                        info.commit = p.commit;
                        info.hash = p.hash;
                        break;
                    }
            return info;
        }

        Client& client_;
        resolve::RequirementSource& inner_;
        manifest::Lock const* previous_;
        std::map<std::string, manifest::RegistryAlias> records_;
    };
} // namespace dpm::registry
