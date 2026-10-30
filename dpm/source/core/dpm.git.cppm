module;

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module dpm.git;

import std;
import dpm.extract;
import dpm.process;

export namespace dpm::git
{
    struct Policy
    {
        bool allow_file{};
        std::chrono::milliseconds timeout{300000};
        std::string program{"git"};
    };

    [[nodiscard]] inline bool is_object_id(std::string_view s)
    {
        return (s.size() == 40 || s.size() == 64) && std::ranges::all_of(s, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    struct Fetched
    {
        bool ok{};
        std::string commit;
        std::string error;
    };

    struct Exported
    {
        bool ok{};
        std::string error;
        std::vector<extract::Problem> problems;
        std::size_t files{};
        std::uint64_t bytes{};
    };

    class Repository
    {
    public:
        Repository(std::filesystem::path git_dir, Policy policy) : dir_{std::move(git_dir)}, policy_{std::move(policy)} {}

        [[nodiscard]] std::filesystem::path const& dir() const { return dir_; }

        [[nodiscard]] bool init(std::string& error) const
        {
            if (std::filesystem::exists(dir_ / "HEAD"))
                return true;
            std::error_code ec;
            std::filesystem::create_directories(dir_.parent_path(), ec);
            auto r = run({"init", "--bare", "--quiet", "--template=", dir_.string()}, false);
            if (!r.ok())
            {
                error = "git init failed: " + message(r);
                return false;
            }
            return true;
        }

        [[nodiscard]] Fetched fetch_tag(std::string_view url, std::string_view tag) const
        {
            if (!safe_ref_component(tag))
                return {false, {}, "unsafe tag name"};
            auto local = "refs/dpm/tags/" + std::string{tag};
            auto r = run({"fetch", "--quiet", "--depth=1", "--no-tags", "--no-write-fetch-head", "--no-recurse-submodules", std::string{url},
                          "+refs/tags/" + std::string{tag} + ":" + local});
            if (!r.ok())
                return {false, {}, "cannot fetch tag " + std::string{tag} + ": " + message(r)};
            return resolve(local);
        }

        [[nodiscard]] Fetched fetch_commit(std::string_view url, std::string_view commit) const
        {
            if (!is_object_id(commit))
                return {false, {}, "not a commit id"};
            auto r = run({"fetch", "--quiet", "--depth=1", "--no-tags", "--no-write-fetch-head", "--no-recurse-submodules", std::string{url},
                          "+" + std::string{commit} + ":refs/dpm/commits/" + std::string{commit}});
            if (!r.ok())
                return {false, {}, "cannot fetch commit " + std::string{commit} + ": " + message(r)};
            return resolve("refs/dpm/commits/" + std::string{commit});
        }

        [[nodiscard]] bool has_commit(std::string_view commit) const
        {
            if (!is_object_id(commit))
                return false;
            return run({"cat-file", "-e", std::string{commit} + "^{commit}"}, true).ok();
        }

        [[nodiscard]] Fetched resolve(std::string const& ref) const
        {
            auto r = run({"rev-parse", "--verify", "--quiet", "--end-of-options", ref + "^{commit}"});
            auto id = r.out;
            while (!id.empty() && (id.back() == '\n' || id.back() == '\r'))
                id.pop_back();
            if (!r.ok() || !is_object_id(id))
                return {false, {}, "cannot resolve " + ref};
            return {true, id, {}};
        }

        [[nodiscard]] Exported export_tree(std::string_view commit, std::string_view subdir, std::filesystem::path const& staging, extract::Limits const& limits = {}) const
        {
            Exported result;
            if (!is_object_id(commit))
            {
                result.error = "not a commit id";
                return result;
            }
            if (!subdir.empty())
                if (auto bad = extract::member_name_problem(subdir))
                {
                    result.error = "unsafe subdirectory: " + *bad;
                    return result;
                }
            std::string spec = std::string{commit} + (subdir.empty() ? "^{tree}" : ":" + std::string{subdir});
            auto listing = run({"ls-tree", "-r", "-z", "-l", "--full-tree", spec}, true, limits.max_files * 400 + (1u << 20));
            if (!listing.ok())
            {
                result.error = "cannot list tree: " + message(listing);
                return result;
            }
            std::vector<extract::Member> members;
            std::vector<std::string> ids;
            std::size_t pos = 0;
            while (pos < listing.out.size())
            {
                auto nul = listing.out.find('\0', pos);
                if (nul == std::string::npos)
                {
                    result.error = "malformed tree listing";
                    return result;
                }
                std::string_view entry{listing.out.data() + pos, nul - pos};
                pos = nul + 1;
                auto tab = entry.find('\t');
                if (tab == std::string_view::npos)
                {
                    result.error = "malformed tree listing";
                    return result;
                }
                std::istringstream head{std::string{entry.substr(0, tab)}};
                std::string mode, type, id, size;
                head >> mode >> type >> id >> size;
                extract::Member m;
                m.path = std::string{entry.substr(tab + 1)};
                if (mode == "100644" || mode == "100755")
                {
                    m.type = extract::MemberType::File;
                    m.exec = mode == "100755";
                    std::uint64_t n = 0;
                    auto [ptr, ec] = std::from_chars(size.data(), size.data() + size.size(), n);
                    if (type != "blob" || ec != std::errc{} || ptr != size.data() + size.size() || !is_object_id(id))
                    {
                        result.error = "malformed tree listing";
                        return result;
                    }
                    m.size = n;
                }
                else if (mode == "120000")
                    m.type = extract::MemberType::Symlink;
                else if (mode == "160000")
                    m.type = extract::MemberType::Gitlink;
                else
                    m.type = extract::MemberType::Other;
                members.push_back(std::move(m));
                ids.push_back(std::move(id));
            }
            result.problems = extract::check_members(members, limits);
            if (!result.problems.empty())
            {
                result.error = "unsafe repository contents: " + result.problems.front().format();
                return result;
            }
            std::error_code ec;
            std::filesystem::create_directories(staging, ec);
            auto oid_file = staging.parent_path() / (staging.filename().string() + ".oids");
            auto blob_file = staging.parent_path() / (staging.filename().string() + ".blobs");
            struct Cleanup
            {
                std::filesystem::path a, b;
                ~Cleanup()
                {
                    std::error_code ignored;
                    std::filesystem::remove(a, ignored);
                    std::filesystem::remove(b, ignored);
                }
            } cleanup{oid_file, blob_file};
            {
                std::ofstream out{oid_file, std::ios::binary};
                for (std::size_t i = 0; i < members.size(); ++i)
                    out << ids[i] << "\n";
                if (!out)
                {
                    result.error = "cannot write object list";
                    return result;
                }
            }
            std::uint64_t total = 0;
            for (auto const& m : members)
                total += m.size;
            process::Command cmd = command({"cat-file", "--batch"});
            cmd.stdin_file = oid_file;
            cmd.stdout_file = blob_file;
            cmd.max_file_size = total + members.size() * 128 + (1u << 16);
            auto cat = process::run(cmd);
            if (!cat.ok())
            {
                result.error = "cannot read objects: " + message(cat);
                return result;
            }
            std::ifstream blobs{blob_file, std::ios::binary};
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                auto const& m = members[i];
                std::string line;
                if (!std::getline(blobs, line))
                {
                    result.error = "object stream ended early";
                    return result;
                }
                std::istringstream h{line};
                std::string id, type;
                std::uint64_t size = 0;
                h >> id >> type >> size;
                if (id != ids[i] || type != "blob" || size != m.size)
                {
                    result.error = "object stream does not match the tree listing for " + m.path;
                    return result;
                }
                if (m.type == extract::MemberType::Directory)
                    continue;
                if (!write_file(staging / m.path, blobs, m.size, m.exec, result.error))
                    return result;
                char newline = 0;
                blobs.get(newline);
                if (newline != '\n')
                {
                    result.error = "malformed object stream";
                    return result;
                }
                ++result.files;
                result.bytes += m.size;
            }
            result.ok = true;
            return result;
        }

    private:
        static bool safe_ref_component(std::string_view tag)
        {
            if (tag.empty() || tag.size() > 200 || tag.front() == '-' || tag.front() == '/' || tag.back() == '/' || tag.back() == '.' || tag.find("..") != std::string_view::npos ||
                tag.find("//") != std::string_view::npos || tag.find("@{") != std::string_view::npos || tag.ends_with(".lock"))
                return false;
            return std::ranges::all_of(tag, [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == '/' || c == '+';
            });
        }

        static bool write_file(std::filesystem::path const& path, std::istream& in, std::uint64_t size, bool exec, std::string& error)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, exec ? 0755 : 0644);
            if (fd < 0)
            {
                error = "cannot create " + path.string();
                return false;
            }
            std::array<char, 65536> buffer{};
            std::uint64_t left = size;
            while (left > 0)
            {
                auto want = static_cast<std::streamsize>(std::min<std::uint64_t>(left, buffer.size()));
                in.read(buffer.data(), want);
                auto got = static_cast<std::size_t>(in.gcount());
                if (got == 0)
                {
                    ::close(fd);
                    error = "object stream ended early";
                    return false;
                }
                std::size_t done = 0;
                while (done < got)
                {
                    auto n = ::write(fd, buffer.data() + done, got - done);
                    if (n <= 0)
                    {
                        ::close(fd);
                        error = "cannot write " + path.string();
                        return false;
                    }
                    done += static_cast<std::size_t>(n);
                }
                left -= got;
            }
            ::fchmod(fd, exec ? 0755 : 0644);
            ::close(fd);
            return true;
        }

        static std::string message(process::Result const& r)
        {
            if (!r.error.empty())
                return r.error;
            if (r.timed_out)
                return "timed out";
            if (r.truncated)
                return "output too large";
            auto text = r.err;
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            if (auto nl = text.rfind('\n'); nl != std::string::npos)
                text = text.substr(nl + 1);
            return text.empty() ? std::format("exit status {}", r.exit_code) : text;
        }

        [[nodiscard]] process::Command command(std::vector<std::string> args, bool with_git_dir = true, std::size_t capture = 0) const
        {
            process::Command cmd;
            cmd.argv.push_back(policy_.program);
            cmd.argv.push_back("--no-pager");
            if (with_git_dir)
                cmd.argv.push_back("--git-dir=" + dir_.string());
            for (auto const& c : {"core.hooksPath=/dev/null", "protocol.allow=never", "protocol.https.allow=always", "fetch.recurseSubmodules=false",
                                  "submodule.recurse=false", "transfer.fsckObjects=true", "core.fsmonitor=false", "credential.helper=", "gc.auto=0",
                                  "maintenance.auto=false", "core.askPass=", "http.followRedirects=initial", "core.symlinks=false"})
            {
                cmd.argv.push_back("-c");
                cmd.argv.push_back(c);
            }
            if (policy_.allow_file)
            {
                cmd.argv.push_back("-c");
                cmd.argv.push_back("protocol.file.allow=always");
            }
            for (auto& a : args)
                cmd.argv.push_back(std::move(a));
            char const* path = std::getenv("PATH");
            cmd.env["PATH"] = path ? path : "/usr/local/bin:/usr/bin:/bin";
            cmd.env["HOME"] = dir_.parent_path().string();
            cmd.env["LC_ALL"] = "C";
            cmd.env["GIT_TERMINAL_PROMPT"] = "0";
            cmd.env["GIT_ASKPASS"] = "/bin/false";
            cmd.env["SSH_ASKPASS"] = "/bin/false";
            cmd.env["GIT_CONFIG_NOSYSTEM"] = "1";
            cmd.env["GIT_CONFIG_GLOBAL"] = "/dev/null";
            cmd.env["GIT_CONFIG_SYSTEM"] = "/dev/null";
            cmd.env["GIT_ALLOW_PROTOCOL"] = policy_.allow_file ? "https:file" : "https";
            cmd.env["GIT_ATTR_NOSYSTEM"] = "1";
            cmd.env["GIT_NO_REPLACE_OBJECTS"] = "1";
            cmd.env["GIT_OPTIONAL_LOCKS"] = "0";
            for (char const* name : {"https_proxy", "HTTPS_PROXY", "no_proxy", "NO_PROXY", "SSL_CERT_FILE", "SSL_CERT_DIR", "GIT_SSL_CAINFO"})
                if (char const* v = std::getenv(name))
                    cmd.env[name] = v;
            cmd.timeout = policy_.timeout;
            if (capture)
                cmd.max_capture = capture;
            return cmd;
        }

        [[nodiscard]] process::Result run(std::vector<std::string> args, bool with_git_dir = true, std::size_t capture = 0) const
        {
            return process::run(command(std::move(args), with_git_dir, capture));
        }

        std::filesystem::path dir_;
        Policy policy_;
    };
} // namespace dpm::git
