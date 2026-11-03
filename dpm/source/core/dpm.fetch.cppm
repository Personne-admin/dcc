module;

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module dpm.fetch;

import std;
import dpm.cache;
import dpm.extract;
import dpm.git;
import dpm.hash;
import dpm.inflate;
import dpm.manifest;
import dpm.process;
import dpm.resolve;
import dpm.semver;
import dpm.tar;

export namespace dpm::fetch
{
    struct Policy
    {
        bool allow_file{};
        bool offline{};
        std::string git{"git"};
        std::string curl{"curl"};
        extract::Limits limits;
        std::uint64_t max_download = 128ull << 20;
        std::uint64_t max_inflate_ratio = 200;
        std::chrono::milliseconds download_timeout{300000};
        std::chrono::milliseconds connect_timeout{15000};
    };

    struct GitSpec
    {
        std::string url;
        std::string subdir;
        std::filesystem::path repo;
    };

    struct Fetched
    {
        bool ok{};
        std::string error;
        std::string commit;
        std::string hash;
        std::filesystem::path path;
        bool existed{};
    };

    namespace detail
    {
        inline std::string sanitize_segment(std::string_view s)
        {
            std::string out;
            for (char c : s)
            {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '~' || c == '-')
                    out += c;
                else
                    out += std::format("%{:02x}", static_cast<unsigned char>(c));
            }
            return out;
        }

        inline bool ends_with_archive_suffix(std::string_view s)
        {
            return s.ends_with(".tar.gz") || s.ends_with(".tgz");
        }

        inline std::string trim_newlines(std::string s)
        {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                s.pop_back();
            return s;
        }

        inline std::string last_line(std::string text)
        {
            text = trim_newlines(std::move(text));
            if (auto nl = text.rfind('\n'); nl != std::string::npos)
                text = text.substr(nl + 1);
            return text;
        }

        inline std::map<std::string, std::string> base_env()
        {
            std::map<std::string, std::string> env;
            char const* path = std::getenv("PATH");
            env["PATH"] = path ? path : "/usr/local/bin:/usr/bin:/bin";
            env["LC_ALL"] = "C";
            return env;
        }

        inline std::map<std::string, std::string> curl_env()
        {
            auto env = base_env();
            for (char const* name : {"https_proxy", "HTTPS_PROXY", "no_proxy", "NO_PROXY", "SSL_CERT_FILE", "SSL_CERT_DIR", "CURL_CA_BUNDLE"})
                if (char const* v = std::getenv(name))
                    env[name] = v;
            return env;
        }
    } // namespace detail

    [[nodiscard]] inline std::optional<GitSpec> parse_git_location(std::string_view location, Policy const& policy, std::string& error)
    {
        GitSpec spec;
        if (location.starts_with("git+https://") || (policy.allow_file && location.starts_with("git+file://")))
        {
            auto body = location.substr(4);
            auto hash = body.find('#');
            spec.url = std::string{body.substr(0, hash)};
            if (hash != std::string_view::npos)
                spec.subdir = std::string{body.substr(hash + 1)};
            if (spec.url.starts_with("file://"))
                spec.repo = std::filesystem::path{"_file"} / (hash::sha256_hex(spec.url).substr(0, 24) + ".git");
            else
            {
                auto path = std::string_view{spec.url}.substr(8);
                auto slash = path.find('/');
                if (slash == std::string_view::npos)
                {
                    error = "git location needs a host and a path";
                    return std::nullopt;
                }
                auto host = path.substr(0, slash);
                auto remainder = path.substr(slash + 1);
                if (remainder.ends_with(".git"))
                    remainder.remove_suffix(4);
                spec.repo = std::filesystem::path{detail::sanitize_segment(host)};
                std::size_t pos = 0;
                while (pos <= remainder.size())
                {
                    auto next = remainder.find('/', pos);
                    auto part = remainder.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
                    if (part.empty() || part == "." || part == "..")
                    {
                        error = "git location has an empty or relative path segment";
                        return std::nullopt;
                    }
                    spec.repo /= detail::sanitize_segment(part);
                    if (next == std::string_view::npos)
                        break;
                    pos = next + 1;
                }
                spec.repo += ".git";
            }
            return spec;
        }
        if (location.starts_with("git+"))
        {
            error = "only https git URLs are allowed";
            return std::nullopt;
        }
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
        if (parts.size() < 3 || std::ranges::any_of(parts, [](auto p) { return p.empty() || p == "." || p == ".."; }))
        {
            error = "location needs host/owner/repo";
            return std::nullopt;
        }
        spec.url = std::format("https://{}/{}/{}.git", parts[0], parts[1], parts[2]);
        spec.repo = std::filesystem::path{detail::sanitize_segment(parts[0])} / detail::sanitize_segment(parts[1]) / (detail::sanitize_segment(parts[2]) + ".git");
        for (std::size_t i = 3; i < parts.size(); ++i)
            spec.subdir += (i > 3 ? "/" : "") + std::string{parts[i]};
        return spec;
    }

    [[nodiscard]] inline bool is_tarball_location(std::string_view location, Policy const& policy)
    {
        return (location.starts_with("https://") || (policy.allow_file && location.starts_with("file://"))) && detail::ends_with_archive_suffix(location);
    }

    [[nodiscard]] inline std::string tarball_error_text(process::Result const& r)
    {
        if (!r.error.empty())
            return r.error;
        if (r.timed_out)
            return "timed out";
        if (r.signal != 0)
            return "download exceeded the size limit";
        auto line = detail::last_line(r.err);
        return line.empty() ? std::format("exit status {}", r.exit_code) : line;
    }

    [[nodiscard]] inline std::optional<std::filesystem::path> download(cache::Cache const& cache, std::string const& url, Policy const& policy, std::string& error)
    {
        if (!(url.starts_with("https://") || (policy.allow_file && url.starts_with("file://"))))
        {
            error = "only https URLs can be downloaded";
            return std::nullopt;
        }
        std::string ext = ".tar.gz";
        auto dir = cache.download_dir() / hash::sha256_hex(url);
        auto file = dir / ("archive" + ext);
        std::error_code ec;
        if (std::filesystem::is_regular_file(file, ec))
            return file;
        if (policy.offline)
        {
            error = "not in cache (run dpm fetch)";
            return std::nullopt;
        }
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            error = "cannot create " + dir.string();
            return std::nullopt;
        }
        auto part = dir / ("archive" + ext + ".part");
        process::Command cmd;
        cmd.argv = {policy.curl, "-q", "--fail", "--silent", "--show-error", "--location", "--proto", policy.allow_file ? "=https,file" : "=https", "--proto-redir", "=https",
                    "--max-redirs", "5", "--max-filesize", std::to_string(policy.max_download), "--connect-timeout",
                    std::to_string(policy.connect_timeout.count() / 1000), "--max-time", std::to_string(policy.download_timeout.count() / 1000), "-o", part.string(), "--", url};
        cmd.env = detail::curl_env();
        cmd.max_file_size = policy.max_download + 1;
        cmd.timeout = policy.download_timeout + std::chrono::seconds(5);
        auto r = process::run(cmd);
        if (!r.ok())
        {
            std::filesystem::remove(part, ec);
            error = "download failed: " + tarball_error_text(r);
            return std::nullopt;
        }
        std::error_code size_ec;
        auto size = std::filesystem::file_size(part, size_ec);
        if (size_ec || size > policy.max_download)
        {
            std::filesystem::remove(part, ec);
            error = "download exceeds the size limit";
            return std::nullopt;
        }
        std::filesystem::rename(part, file, ec);
        if (ec)
        {
            error = "cannot store the download";
            return std::nullopt;
        }
        return file;
    }

    struct Extracted
    {
        bool ok{};
        std::string error;
        std::vector<extract::Problem> problems;
        std::size_t files{};
        std::uint64_t bytes{};
    };

    [[nodiscard]] inline Extracted extract_tarball(std::filesystem::path const& archive, std::filesystem::path const& staging, Policy const& policy)
    {
        Extracted result;
        std::error_code size_ec;
        auto archive_size = std::filesystem::file_size(archive, size_ec);
        if (size_ec || archive_size > policy.max_download)
        {
            result.error = "cannot read the archive or it exceeds the size limit";
            return result;
        }
        std::string compressed(archive_size, '\0');
        {
            std::ifstream in{archive, std::ios::binary};
            in.read(compressed.data(), static_cast<std::streamsize>(compressed.size()));
            if (!in)
            {
                result.error = "cannot read the archive";
                return result;
            }
        }
        auto decompress = [&](std::function<bool(std::string_view)> sink, std::string& error)
        {
            inflate::Limits inflate_limits;
            inflate_limits.max_output = policy.limits.max_bytes + (static_cast<std::uint64_t>(policy.limits.max_files) * 3 + 16) * 1024;
            inflate_limits.max_ratio = policy.max_inflate_ratio;
            auto r = inflate::gunzip(compressed, inflate_limits, sink);
            if (r.aborted)
                return true;
            if (!r.ok)
            {
                error = "cannot decompress the archive: " + r.error;
                return false;
            }
            return true;
        };
        auto const& limits = policy.limits;
        std::uint64_t max_stream = limits.max_bytes + (static_cast<std::uint64_t>(limits.max_files) * 3 + 16) * 1024;

        std::vector<extract::Member> members;
        std::string rejection;
        std::uint64_t listed_bytes = 0;
        std::size_t listed_files = 0;
        tar::Reader::Handlers listing;
        listing.on_member = [&](extract::Member const& m)
        {
            members.push_back(m);
            if (m.type == extract::MemberType::File)
            {
                ++listed_files;
                listed_bytes += m.size;
            }
            if (auto bad = extract::member_name_problem(m.path, limits))
                rejection = m.path + ": " + *bad;
            else if (m.type != extract::MemberType::File && m.type != extract::MemberType::Directory)
                rejection = m.path + ": " + std::string{extract::member_type_name(m.type)} + " not allowed";
            else if (listed_files > limits.max_files)
                rejection = std::format("more than {} files", limits.max_files);
            else if (listed_bytes > limits.max_bytes)
                rejection = std::format("more than {} bytes", limits.max_bytes);
            return rejection.empty();
        };
        tar::Reader list_reader{listing, max_stream};
        std::string error;
        bool fed_ok = true;
        if (!decompress(
                [&](std::string_view chunk)
                {
                    if (!list_reader.feed(chunk))
                    {
                        fed_ok = false;
                        return false;
                    }
                    return true;
                },
                error))
        {
            result.error = error;
            return result;
        }
        if (!fed_ok)
        {
            result.error = !rejection.empty() ? "unsafe archive: " + rejection : "invalid archive: " + list_reader.error();
            return result;
        }
        if (!list_reader.finish())
        {
            result.error = "invalid archive: " + list_reader.error();
            return result;
        }
        result.problems = extract::check_members(members, limits);
        if (!result.problems.empty())
        {
            result.error = "unsafe archive: " + result.problems.front().format();
            return result;
        }
        std::vector<extract::Member> probe = members;
        std::size_t root_strip = 0;
        if (extract::strip_single_root(probe))
        {
            auto const& first = members.front().path;
            auto slash = first.find('/');
            root_strip = (slash == std::string::npos ? first.size() : slash) + 1;
        }
        std::error_code ec;
        std::filesystem::create_directories(staging, ec);
        std::size_t index = 0;
        int fd = -1;
        std::string write_error;
        auto relative = [&](std::string const& path) -> std::optional<std::string>
        {
            if (root_strip == 0)
                return path;
            if (path.size() < root_strip)
                return std::nullopt;
            return path.substr(root_strip);
        };
        tar::Reader::Handlers extracting;
        extracting.on_member = [&](extract::Member const& m)
        {
            if (index >= members.size() || members[index].path != m.path || members[index].type != m.type || members[index].size != m.size)
            {
                write_error = "archive changed between listing and extraction";
                return false;
            }
            ++index;
            auto rel = relative(m.path);
            if (!rel || rel->empty())
                return true;
            auto target = staging / *rel;
            if (m.type == extract::MemberType::Directory)
            {
                std::error_code dir_ec;
                std::filesystem::create_directories(target, dir_ec);
                return true;
            }
            std::error_code dir_ec;
            std::filesystem::create_directories(target.parent_path(), dir_ec);
            fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, m.exec ? 0755 : 0644);
            if (fd < 0)
            {
                write_error = "cannot create " + target.string();
                return false;
            }
            ++result.files;
            result.bytes += m.size;
            return true;
        };
        extracting.on_data = [&](std::string_view data)
        {
            if (fd < 0)
                return true;
            std::size_t done = 0;
            while (done < data.size())
            {
                auto n = ::write(fd, data.data() + done, data.size() - done);
                if (n <= 0)
                {
                    write_error = "cannot write extracted file";
                    return false;
                }
                done += static_cast<std::size_t>(n);
            }
            return true;
        };
        extracting.on_member_end = [&]
        {
            if (fd >= 0)
            {
                ::close(fd);
                fd = -1;
            }
            return true;
        };
        tar::Reader extract_reader{extracting, max_stream};
        fed_ok = true;
        if (!decompress(
                [&](std::string_view chunk)
                {
                    if (!extract_reader.feed(chunk))
                    {
                        fed_ok = false;
                        return false;
                    }
                    return true;
                },
                error))
        {
            if (fd >= 0)
                ::close(fd);
            result.error = error;
            return result;
        }
        if (fd >= 0)
            ::close(fd);
        if (!fed_ok || !extract_reader.finish() || index != members.size())
        {
            result.error = !write_error.empty() ? write_error : "invalid archive: " + extract_reader.error();
            return result;
        }
        result.ok = true;
        return result;
    }

    [[nodiscard]] inline Fetched fetch_tarball(cache::Cache const& cache, std::string const& url, std::string_view expected_hash, Policy const& policy,
                                                 bool allow_cached = false)
    {
        Fetched out;
        if (!expected_hash.empty() && allow_cached)
        {
            auto path = cache.src_path(expected_hash);
            std::error_code ec;
            if (std::filesystem::is_directory(path, ec))
            {
                out.ok = true;
                out.existed = true;
                out.hash = std::string{expected_hash};
                out.path = path;
                return out;
            }
        }
        std::string error;
        auto archive = download(cache, url, policy, error);
        if (!archive)
        {
            out.error = error;
            return out;
        }
        auto staging = cache.new_staging(error);
        if (!staging)
        {
            out.error = error;
            return out;
        }
        auto extracted = extract_tarball(*archive, *staging, policy);
        if (!extracted.ok)
        {
            cache::detail::remove_tree(*staging);
            out.error = extracted.error;
            return out;
        }
        auto installed = cache.install(*staging, expected_hash, policy.limits);
        if (!installed.ok)
        {
            out.error = installed.error;
            return out;
        }
        out.ok = true;
        out.existed = installed.existed;
        out.hash = installed.hash;
        out.path = installed.path;
        return out;
    }

    struct GitRequest
    {
        GitSpec spec;
        std::vector<std::string> tags;
        std::string commit;
        std::string hash;
        std::string ref;
        bool allow_cached{};
    };

    [[nodiscard]] inline Fetched fetch_git(cache::Cache const& cache, GitRequest const& request, Policy const& policy)
    {
        Fetched out;
        if (!request.hash.empty() && request.allow_cached)
        {
            auto path = cache.src_path(request.hash);
            std::error_code ec;
            if (std::filesystem::is_directory(path, ec))
            {
                out.ok = true;
                out.existed = true;
                out.hash = request.hash;
                out.commit = request.commit;
                out.path = path;
                return out;
            }
        }
        git::Policy git_policy;
        git_policy.allow_file = policy.allow_file;
        git_policy.program = policy.git;
        git::Repository repo{cache.git_dir() / request.spec.repo, git_policy};
        std::string error;
        if (!cache.ensure(error) || !repo.init(error))
        {
            out.error = error;
            return out;
        }
        std::string commit;
        if (!request.commit.empty())
        {
            if (repo.has_commit(request.commit))
                commit = request.commit;
            else if (policy.offline)
            {
                out.error = "not in cache (run dpm fetch)";
                return out;
            }
            else
            {
                auto fetched = repo.fetch_commit(request.spec.url, request.commit);
                if (fetched.ok)
                    commit = fetched.commit;
                else
                {
                    for (auto const& tag : request.tags)
                    {
                        auto by_tag = repo.fetch_tag(request.spec.url, tag);
                        if (!by_tag.ok)
                            continue;
                        if (by_tag.commit != request.commit)
                        {
                            out.error = std::format("tag {} now points to {}, but the lock pins {}", tag, by_tag.commit, request.commit);
                            return out;
                        }
                        commit = by_tag.commit;
                        break;
                    }
                    if (commit.empty() && !request.ref.empty())
                    {
                        auto by_ref = repo.fetch_ref(request.spec.url, request.ref);
                        if (by_ref.ok)
                        {
                            if (by_ref.commit != request.commit)
                            {
                                out.error = std::format("{} now points to {}, but the pin is {}", request.ref, by_ref.commit, request.commit);
                                return out;
                            }
                            commit = by_ref.commit;
                        }
                    }
                    if (commit.empty())
                    {
                        out.error = "cannot fetch the pinned commit: " + fetched.error;
                        return out;
                    }
                }
            }
        }
        else if (!request.ref.empty())
        {
            auto fetched = policy.offline ? repo.cached_ref(request.ref) : repo.fetch_ref(request.spec.url, request.ref);
            if (!fetched.ok)
            {
                out.error = policy.offline ? "not in cache (run dpm fetch)" : fetched.error;
                return out;
            }
            commit = fetched.commit;
        }
        else
        {
            std::string last_error = "no tag candidates";
            for (auto const& tag : request.tags)
            {
                git::Fetched fetched;
                if (policy.offline)
                    fetched = repo.resolve("refs/dpm/tags/" + tag);
                else
                    fetched = repo.fetch_tag(request.spec.url, tag);
                if (fetched.ok)
                {
                    commit = fetched.commit;
                    break;
                }
                last_error = fetched.error;
            }
            if (commit.empty())
            {
                out.error = policy.offline ? "not in cache (run dpm fetch)" : last_error;
                return out;
            }
        }
        auto staging = cache.new_staging(error);
        if (!staging)
        {
            out.error = error;
            return out;
        }
        auto exported = repo.export_tree(commit, request.spec.subdir, *staging, policy.limits);
        if (!exported.ok)
        {
            cache::detail::remove_tree(*staging);
            out.error = exported.error;
            return out;
        }
        auto installed = cache.install(*staging, request.hash, policy.limits);
        if (!installed.ok)
        {
            out.error = installed.error;
            return out;
        }
        out.ok = true;
        out.commit = commit;
        out.hash = installed.hash;
        out.path = installed.path;
        out.existed = installed.existed;
        return out;
    }

    [[nodiscard]] inline std::optional<std::string> latest_version(cache::Cache const& cache, std::string const& location, Policy const& policy,
                                                                    std::optional<semver::Series> within, std::string& error)
    {
        if (policy.offline)
        {
            error = "listing versions needs the network (offline mode)";
            return std::nullopt;
        }
        if (is_tarball_location(location, policy))
        {
            error = "a tarball dependency needs an explicit version";
            return std::nullopt;
        }
        auto spec = parse_git_location(location, policy, error);
        if (!spec)
            return std::nullopt;
        std::string ignored;
        if (!cache.ensure(ignored))
        {
            error = ignored;
            return std::nullopt;
        }
        git::Policy git_policy;
        git_policy.allow_file = policy.allow_file;
        git_policy.program = policy.git;
        git::Repository repo{cache.tmp_dir() / "ls-remote.git", git_policy};
        auto tags = repo.remote_tags(spec->url, error);
        if (!tags)
            return std::nullopt;
        std::optional<semver::Version> best;
        std::string best_text;
        for (auto tag : *tags)
        {
            std::string_view name{tag};
            if (!spec->subdir.empty() && name.starts_with(spec->subdir + "/"))
                name.remove_prefix(spec->subdir.size() + 1);
            if (name.starts_with('v'))
                name.remove_prefix(1);
            auto version = semver::parse(name);
            if (!version || version->is_prerelease())
                continue;
            if (within && !(semver::series_of(*version) == *within))
                continue;
            if (!best || semver::compare(*version, *best) > 0)
            {
                best = version;
                best_text = std::string{name};
            }
        }
        if (!best)
        {
            error = "no release tags found";
            return std::nullopt;
        }
        return best_text;
    }

    class FetchSource : public resolve::RequirementSource
    {
    public:
        FetchSource(cache::Cache cache, Policy policy, std::filesystem::path root, std::filesystem::path schema_root, manifest::Lock const* previous)
            : cache_{std::move(cache)}, policy_{std::move(policy)}, paths_{std::move(root), schema_root}, schema_root_{std::move(schema_root)}, previous_{previous}
        {
        }

        std::vector<std::string> warnings;

        std::optional<resolve::PackageInfo> info(std::string_view location, std::string_view version, std::string& error) override
        {
            if (location.starts_with("path:"))
                return paths_.info(location, version, error);
            auto key = std::pair{std::string{location}, std::string{version}};
            if (auto it = memo_.find(key); it != memo_.end())
            {
                if (!it->second)
                    error = errors_[key];
                return it->second;
            }
            auto result = load(location, version, error);
            if (!result)
                errors_[key] = error;
            memo_[key] = result;
            return result;
        }

    private:
        std::optional<resolve::PackageInfo> load(std::string_view location, std::string_view version, std::string& error)
        {
            manifest::LockedPackage const* pinned = nullptr;
            if (previous_)
                for (auto const& p : previous_->packages)
                    if (p.location == location && p.version == version)
                        pinned = &p;
            Fetched fetched;
            std::string source;
            std::string commit;
            if ((location.starts_with("https://") || location.starts_with("file://")) && (location.ends_with(".tar.xz") || location.ends_with(".txz")))
            {
                error = "only .tar.gz and .tgz tarballs are supported";
                return std::nullopt;
            }
            if (is_tarball_location(location, policy_))
            {
                fetched = fetch_tarball(cache_, std::string{location}, pinned ? pinned->hash : std::string{}, policy_, pinned != nullptr);
                source = std::string{location};
            }
            else
            {
                std::string spec_error;
                auto spec = parse_git_location(location, policy_, spec_error);
                if (!spec)
                {
                    error = spec_error;
                    return std::nullopt;
                }
                GitRequest request;
                request.spec = *spec;
                for (auto const& prefix : tag_names(spec->subdir, version))
                    request.tags.push_back(prefix);
                if (pinned)
                {
                    request.commit = pinned->commit;
                    request.hash = pinned->hash;
                    request.allow_cached = true;
                }
                fetched = fetch_git(cache_, request, policy_);
                source = "git+" + spec->url + (spec->subdir.empty() ? "" : "#" + spec->subdir);
                commit = fetched.commit;
            }
            if (!fetched.ok)
            {
                error = fetched.error;
                return std::nullopt;
            }
            auto text = read_file(fetched.path / "project.dcon");
            if (!text)
            {
                error = "the package has no project.dcon";
                return std::nullopt;
            }
            manifest::LoadOptions options;
            options.file_name = std::format("{}@{}/project.dcon", location, version);
            options.schema_root = schema_root_;
            options.project_root = fetched.path;
            options.allow_file_urls = policy_.allow_file;
            auto loaded = manifest::load(*text, options);
            if (!loaded.ok || !loaded.project)
            {
                error = loaded.diagnostics.empty() ? "invalid package manifest" : loaded.diagnostics.front().format();
                return std::nullopt;
            }
            auto const& project = *loaded.project;
            auto actual = semver::parse(project.version);
            auto wanted = semver::parse(version);
            if (!actual || !wanted)
            {
                error = "invalid version";
                return std::nullopt;
            }
            bool const tarball = is_tarball_location(location, policy_);
            if (tarball ? semver::compare(*actual, *wanted) < 0 : semver::compare(*actual, *wanted) != 0)
            {
                error = std::format("{} contains version {}, expected {}{}", location, project.version, tarball ? ">= " : "", version);
                return std::nullopt;
            }
            resolve::PackageInfo info;
            info.name = project.name;
            info.version = project.version;
            info.requires_dcc = project.requires_dcc;
            info.source = source;
            info.commit = commit;
            info.hash = fetched.hash;
            for (auto const& d : project.dependencies)
                info.requirements.push_back({d.location, d.version});
            if (!pinned)
                warnings.push_back(std::format("new package {}@{}: {}hash {} (first use; nothing earlier to verify against)", location, project.version,
                                               commit.empty() ? std::string{} : "commit " + commit + ", ", fetched.hash));
            else if (pinned->hash.empty() || fetched.hash != pinned->hash)
            {
                error = std::format("integrity mismatch for {}@{}: lock says {}, got {}", location, version, pinned->hash, fetched.hash);
                return std::nullopt;
            }
            return info;
        }

        static std::vector<std::string> tag_names(std::string const& subdir, std::string_view version)
        {
            std::vector<std::string> tags;
            std::string v{version};
            if (!subdir.empty())
            {
                tags.push_back(subdir + "/v" + v);
                tags.push_back(subdir + "/" + v);
            }
            tags.push_back("v" + v);
            tags.push_back(v);
            return tags;
        }

        static std::optional<std::string> read_file(std::filesystem::path const& path)
        {
            std::ifstream in{path, std::ios::binary};
            if (!in)
                return std::nullopt;
            return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        }

        cache::Cache cache_;
        Policy policy_;
        resolve::PathSource paths_;
        std::filesystem::path schema_root_;
        manifest::Lock const* previous_;
        std::map<std::pair<std::string, std::string>, std::optional<resolve::PackageInfo>> memo_;
        std::map<std::pair<std::string, std::string>, std::string> errors_;
    };

    struct FetchAllResult
    {
        bool ok{true};
        std::vector<std::string> errors;
        std::size_t fetched{};
        std::size_t present{};
        std::map<std::string, std::string> names;
        std::map<std::string, std::filesystem::path> assets;
    };

    namespace detail
    {
        inline bool is_alias_location(std::string_view loc)
        {
            return loc.find('/') == std::string_view::npos && loc.find('.') == std::string_view::npos && loc.find(':') == std::string_view::npos;
        }

        [[nodiscard]] inline std::optional<std::string> verify_manifest(Fetched const& f, manifest::LockedPackage const& p, Policy const& policy,
                                                                        std::filesystem::path const& schema_root, std::string& name)
        {
            std::ifstream in{f.path / "project.dcon", std::ios::binary};
            if (!in)
                return "the package has no project.dcon";
            std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            manifest::LoadOptions options;
            options.file_name = std::format("{}@{}/project.dcon", p.location, p.version);
            options.schema_root = schema_root;
            options.project_root = f.path;
            options.allow_file_urls = policy.allow_file;
            auto loaded = manifest::load(text, options);
            if (!loaded.ok || !loaded.project)
                return loaded.diagnostics.empty() ? "invalid package manifest" : loaded.diagnostics.front().format();
            auto const& project = *loaded.project;
            auto actual = semver::parse(project.version);
            auto wanted = semver::parse(p.version);
            if (!actual || !wanted || semver::compare(*actual, *wanted) != 0)
                return std::format("the package contains version {}, the lock says {}", project.version, p.version);
            std::set<std::string> explicit_deps;
            std::set<std::string> aliases;
            for (auto const& d : project.dependencies)
            {
                if (is_alias_location(d.location))
                    aliases.insert(d.location);
                else
                    explicit_deps.insert(d.location);
            }
            std::set<std::string> locked(p.dependencies.begin(), p.dependencies.end());
            std::size_t matched = 0;
            for (auto const& d : explicit_deps)
                if (locked.contains(d))
                    ++matched;
            if (matched != explicit_deps.size() || locked.size() - matched != aliases.size())
                return "the dependencies in its manifest differ from the lock (a registry summary that does not match the package)";
            name = project.name;
            return std::nullopt;
        }

        inline std::vector<std::string> version_tags(std::string const& subdir, std::string const& version)
        {
            std::vector<std::string> tags;
            if (version.empty())
                return tags;
            if (!subdir.empty())
            {
                tags.push_back(subdir + "/v" + version);
                tags.push_back(subdir + "/" + version);
            }
            tags.push_back("v" + version);
            tags.push_back(version);
            return tags;
        }

        [[nodiscard]] inline std::filesystem::path verified_stamp(cache::Cache const& cache, manifest::LockedPackage const& p)
        {
            std::string key = p.hash + "\n" + p.version + "\n" + p.location + "\n";
            for (auto const& d : p.dependencies)
                key += d + "\n";
            return cache.root() / "verified" / hash::sha256_hex(key);
        }
    } // namespace detail

    [[nodiscard]] inline Policy asset_policy(Policy policy, std::int64_t max_size)
    {
        if (max_size > 0)
            policy.limits.max_bytes = static_cast<std::uint64_t>(std::min(max_size, manifest::k_max_asset_size));
        return policy;
    }

    [[nodiscard]] inline FetchAllResult fetch_locked(cache::Cache const& cache, manifest::Lock const& lock, Policy const& policy,
                                                     std::filesystem::path const& schema_root = {}, manifest::Lock const* pinned = nullptr,
                                                     manifest::Project const* project = nullptr)
    {
        FetchAllResult all;
        for (auto const& p : lock.packages)
        {
            if (p.location.starts_with("path:"))
                continue;
            bool already_pinned = false;
            if (pinned)
                for (auto const& q : pinned->packages)
                    if (q.location == p.location && q.version == p.version && q.source == p.source && q.commit == p.commit && q.hash == p.hash)
                        already_pinned = true;
            Fetched f;
            if (p.source.starts_with("registry:"))
            {
                all.errors.push_back(std::format("{}@{}: the source '{}' is reserved; the lock records the real source URL", p.location, p.version, p.source));
                all.ok = false;
                continue;
            }
            if (is_tarball_location(p.source, policy))
                f = fetch_tarball(cache, p.source, p.hash, policy, already_pinned);
            else
            {
                std::string error;
                auto spec = parse_git_location(p.source.starts_with("git+") ? p.source : "git+" + p.source, policy, error);
                if (!spec)
                {
                    all.errors.push_back(std::format("{}@{}: {}", p.location, p.version, error));
                    all.ok = false;
                    continue;
                }
                GitRequest request;
                request.spec = *spec;
                request.commit = p.commit;
                request.hash = p.hash;
                request.allow_cached = already_pinned;
                std::string v = p.version;
                if (!spec->subdir.empty())
                    request.tags.push_back(spec->subdir + "/v" + v);
                request.tags.push_back("v" + v);
                request.tags.push_back(v);
                f = fetch_git(cache, request, policy);
            }
            if (!f.ok)
            {
                all.errors.push_back(std::format("{}@{}: {}", p.location, p.version, f.error));
                all.ok = false;
                continue;
            }
            if (f.existed)
                ++all.present;
            else
                ++all.fetched;
            if (schema_root.empty())
                continue;
            auto stamp = detail::verified_stamp(cache, p);
            std::error_code ec;
            if (std::filesystem::is_regular_file(stamp, ec))
            {
                std::ifstream in{stamp, std::ios::binary};
                all.names[p.location] = std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
                continue;
            }
            std::string name;
            if (auto problem = detail::verify_manifest(f, p, policy, schema_root, name))
            {
                all.errors.push_back(std::format("{}@{}: {}", p.location, p.version, *problem));
                all.ok = false;
                continue;
            }
            all.names[p.location] = name;
            std::filesystem::create_directories(stamp.parent_path(), ec);
            std::ofstream out{stamp, std::ios::binary | std::ios::trunc};
            out << name;
        }
        for (auto const& a : lock.assets)
        {
            if (a.source.starts_with("path:"))
            {
                all.assets[a.name] = std::filesystem::path{a.source.substr(5)};
                continue;
            }
            bool already_pinned = false;
            if (pinned)
                for (auto const& q : pinned->assets)
                    if (q.name == a.name && q.source == a.source && q.version == a.version && q.commit == a.commit && q.hash == a.hash)
                        already_pinned = true;
            std::int64_t max_size = 0;
            if (project)
                for (auto const& declared : project->assets)
                    if (declared.name == a.name)
                        max_size = declared.max_size;
            auto policy_for_asset = asset_policy(policy, max_size);
            Fetched f;
            if (is_tarball_location(a.source, policy_for_asset))
                f = fetch_tarball(cache, a.source, a.hash, policy_for_asset, already_pinned);
            else
            {
                std::string error;
                auto spec = parse_git_location(a.source, policy, error);
                if (!spec)
                {
                    all.errors.push_back(std::format("asset {}: {}", a.name, error));
                    all.ok = false;
                    continue;
                }
                GitRequest request;
                request.spec = *spec;
                request.commit = a.commit;
                request.hash = a.hash;
                request.allow_cached = already_pinned;
                request.tags = detail::version_tags(spec->subdir, a.version);
                f = fetch_git(cache, request, policy_for_asset);
            }
            if (!f.ok)
            {
                all.errors.push_back(std::format("asset {}: {}", a.name, f.error));
                all.ok = false;
                continue;
            }
            if (f.hash != a.hash)
            {
                all.errors.push_back(std::format("integrity mismatch for asset {}: lock says {}, got {}", a.name, a.hash, f.hash));
                all.ok = false;
                continue;
            }
            if (f.existed)
                ++all.present;
            else
                ++all.fetched;
            all.assets[a.name] = f.path;
        }
        return all;
    }

    struct AssetResolution
    {
        bool ok{true};
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        std::vector<manifest::LockedAsset> assets;
    };

    [[nodiscard]] inline std::optional<std::string> asset_source(manifest::Asset const& asset, Policy const& policy, std::string& error)
    {
        if (asset.location.starts_with("path:"))
            return resolve::detail::normalize_path(".", resolve::detail::path_of(asset.location));
        if (is_tarball_location(asset.location, policy))
            return asset.location;
        auto spec = parse_git_location(asset.location, policy, error);
        if (!spec)
            return std::nullopt;
        return "git+" + spec->url + (spec->subdir.empty() ? "" : "#" + spec->subdir);
    }

    [[nodiscard]] inline AssetResolution resolve_assets(cache::Cache const& cache, manifest::Project const& project, Policy const& base_policy,
                                                        std::filesystem::path const& root, manifest::Lock const* previous)
    {
        AssetResolution out;
        std::vector<manifest::Asset const*> order;
        for (auto const& a : project.assets)
            order.push_back(&a);
        std::ranges::sort(order, [](auto const* a, auto const* b) { return a->name < b->name; });
        for (auto const* asset : order)
        {
            auto fail = [&](std::string message)
            {
                out.errors.push_back(std::format("asset {}: {}", asset->name, message));
                out.ok = false;
            };
            auto policy = asset_policy(base_policy, asset->max_size);
            std::string error;
            auto source = asset_source(*asset, policy, error);
            if (!source)
            {
                fail(error);
                continue;
            }
            manifest::LockedAsset locked;
            locked.name = asset->name;
            locked.source = *source;
            locked.version = asset->version;
            if (source->starts_with("path:"))
            {
                std::error_code ec;
                if (!std::filesystem::is_directory(root / source->substr(5), ec))
                {
                    fail(std::format("directory '{}' does not exist", source->substr(5)));
                    continue;
                }
                out.assets.push_back(std::move(locked));
                continue;
            }
            manifest::LockedAsset const* pinned = nullptr;
            if (previous)
                for (auto const& q : previous->assets)
                    if (q.name == asset->name && q.source == *source && q.version == asset->version && !q.hash.empty())
                        pinned = &q;
            Fetched fetched;
            if (is_tarball_location(*source, policy))
                fetched = fetch_tarball(cache, *source, pinned ? pinned->hash : std::string{}, policy, pinned != nullptr);
            else
            {
                auto spec = parse_git_location(*source, policy, error);
                if (!spec)
                {
                    fail(error);
                    continue;
                }
                GitRequest request;
                request.spec = *spec;
                std::string version = asset->version;
                if (pinned)
                {
                    request.commit = pinned->commit;
                    request.hash = pinned->hash;
                    request.allow_cached = true;
                }
                else if (version.empty())
                {
                    auto latest = latest_version(cache, *source, policy, std::nullopt, error);
                    if (!latest)
                    {
                        fail(error);
                        continue;
                    }
                    version = *latest;
                }
                request.tags = detail::version_tags(spec->subdir, version);
                fetched = fetch_git(cache, request, policy);
            }
            if (!fetched.ok)
            {
                fail(fetched.error);
                continue;
            }
            if (pinned && fetched.hash != pinned->hash)
            {
                out.errors.push_back(std::format("integrity mismatch for asset {}: lock says {}, got {}", asset->name, pinned->hash, fetched.hash));
                out.ok = false;
                continue;
            }
            locked.commit = fetched.commit;
            locked.hash = fetched.hash;
            if (!pinned)
                out.warnings.push_back(std::format("new asset {}: {}hash {} (first use; nothing earlier to verify against)", asset->name,
                                                   locked.commit.empty() ? std::string{} : "commit " + locked.commit + ", ", locked.hash));
            out.assets.push_back(std::move(locked));
        }
        return out;
    }
} // namespace dpm::fetch
