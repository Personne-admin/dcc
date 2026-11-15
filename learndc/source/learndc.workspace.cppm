export module learndc.workspace;

import std;
import learndc.text;
import learndc.json;
import learndc.platform;
import learndc.catalog;
import learndc.diff;

export namespace learndc::workspace
{
    enum class ErrorKind
    {
        InvalidPath,
        Unsafe,
        Refused,
        NotFound,
        Corrupt,
        Io,
    };

    struct WorkspaceError
    {
        ErrorKind kind = ErrorKind::Io;
        std::string message;
    };

    struct Meta
    {
        std::string exercise;
        int revision = 1;
        std::string created_at;
        std::map<std::string, std::string> starter;
    };

    struct Workspace
    {
        std::filesystem::path root;
        Meta meta;
    };

    struct StartResult
    {
        Workspace workspace;
        bool created = false;
        bool outdated = false;
        std::vector<std::string> restored;
    };

    enum class ChangeKind
    {
        Modified,
        Added,
        Missing,
    };

    struct FileChange
    {
        std::string path;
        ChangeKind kind = ChangeKind::Modified;
    };

    struct ResetResult
    {
        std::filesystem::path backup_path;
        std::vector<std::string> restored;
        std::vector<std::string> removed;
        std::vector<std::string> kept;
    };

    inline constexpr std::string_view meta_dir = ".learndc";
    inline constexpr std::string_view meta_file = "workspace.json";

    [[nodiscard]] inline std::uint64_t fnv1a64(std::string_view data)
    {
        std::uint64_t h = 14695981039346656037ull;
        for (char c : data)
        {
            h ^= static_cast<std::uint8_t>(c);
            h *= 1099511628211ull;
        }
        return h;
    }

    [[nodiscard]] inline std::string hash_hex(std::string_view data)
    {
        return std::format("{:016x}", fnv1a64(data));
    }

    namespace detail
    {
        [[nodiscard]] inline std::unexpected<WorkspaceError> fail(ErrorKind kind, std::string message)
        {
            return std::unexpected(WorkspaceError{kind, std::move(message)});
        }

        [[nodiscard]] inline std::filesystem::path join(std::filesystem::path const& root, std::string_view rel)
        {
            auto out = root;
            for (auto const& part : text::split(rel, '/'))
                out /= platform::path_from_utf8(part);
            return out;
        }

        [[nodiscard]] inline std::optional<std::string> relative_problem(std::string_view rel)
        {
            if (auto problem = catalog::path_problem(rel))
                return problem;
            auto const first = rel.substr(0, rel.find('/'));
            if (first == meta_dir || first == ".dpm")
                return std::format("path starts with the reserved directory '{}'", first);
            return std::nullopt;
        }

        [[nodiscard]] inline std::optional<WorkspaceError> check_relative(std::string_view rel)
        {
            if (auto problem = relative_problem(rel))
                return WorkspaceError{ErrorKind::InvalidPath, std::format("invalid file path '{}': {}", text::sanitize(rel), *problem)};
            return std::nullopt;
        }

        [[nodiscard]] inline std::optional<WorkspaceError> check_chain(std::filesystem::path const& root, std::string_view rel)
        {
            if (platform::is_symlink_or_reparse(root))
                return WorkspaceError{ErrorKind::Unsafe,
                                      std::format("'{}' is a symbolic link or reparse point; refusing to use it", platform::path_to_utf8(root))};
            auto current = root;
            auto const parts = rel.empty() ? std::vector<std::string>{} : text::split(rel, '/');
            for (std::size_t i = 0; i < parts.size(); ++i)
            {
                current /= platform::path_from_utf8(parts[i]);
                std::error_code ec;
                auto const status = std::filesystem::symlink_status(current, ec);
                if (ec || status.type() == std::filesystem::file_type::not_found)
                    break;
                if (platform::is_symlink_or_reparse(current))
                    return WorkspaceError{ErrorKind::Unsafe,
                                          std::format("'{}' is a symbolic link or reparse point; refusing to follow it", platform::path_to_utf8(current))};
                if (i + 1 < parts.size() && status.type() != std::filesystem::file_type::directory)
                    return WorkspaceError{ErrorKind::Refused, std::format("'{}' exists but is not a directory", platform::path_to_utf8(current))};
            }
            return std::nullopt;
        }

        [[nodiscard]] inline std::expected<void, WorkspaceError> ensure_parent(std::filesystem::path const& root, std::string_view rel)
        {
            auto const parts = text::split(rel, '/');
            auto current = root;
            for (std::size_t i = 0; i + 1 < parts.size(); ++i)
            {
                current /= platform::path_from_utf8(parts[i]);
                std::error_code ec;
                if (!std::filesystem::exists(std::filesystem::symlink_status(current, ec)))
                {
                    std::filesystem::create_directory(current, ec);
                    if (ec)
                        return fail(ErrorKind::Io, std::format("cannot create '{}': {}", platform::path_to_utf8(current), ec.message()));
                }
                if (platform::is_symlink_or_reparse(current))
                    return fail(ErrorKind::Unsafe,
                                std::format("'{}' is a symbolic link or reparse point; refusing to follow it", platform::path_to_utf8(current)));
                if (!std::filesystem::is_directory(current, ec))
                    return fail(ErrorKind::Refused, std::format("'{}' exists but is not a directory", platform::path_to_utf8(current)));
            }
            return {};
        }

        [[nodiscard]] inline bool ignored_name(std::string_view name)
        {
            if (name.empty())
                return true;
            auto const ends = [&](std::string_view suffix) { return name.size() > suffix.size() && name.ends_with(suffix); };
            return ends(".swp") || ends(".swo") || ends(".swx") || ends(".tmp") || ends("~") || name.starts_with(".#") || name == "4913" ||
                   name == ".DS_Store" || name == "Thumbs.db" || (name.size() > 2 && name.front() == '#' && name.back() == '#');
        }

        [[nodiscard]] inline std::string meta_text(Meta const& meta)
        {
            auto root = json::Json::make_object();
            root.set("schema", json::Json(1));
            root.set("exercise", json::Json(meta.exercise));
            root.set("revision", json::Json(meta.revision));
            root.set("created_at", json::Json(meta.created_at));
            auto starter = json::Json::make_object();
            for (auto const& [path, hash] : meta.starter)
                starter.set(path, json::Json(hash));
            root.set("starter", std::move(starter));
            return root.dump(2) + "\n";
        }

        [[nodiscard]] inline std::expected<void, WorkspaceError> write_meta(std::filesystem::path const& root, Meta const& meta)
        {
            auto const dir = root / std::string(meta_dir);
            std::error_code ec;
            if (std::filesystem::exists(std::filesystem::symlink_status(dir, ec)) && platform::is_symlink_or_reparse(dir))
                return fail(ErrorKind::Unsafe, std::format("'{}' is a symbolic link or reparse point; refusing to use it", platform::path_to_utf8(dir)));
            if (auto r = platform::atomic_write(dir / std::string(meta_file), meta_text(meta)); !r)
                return fail(ErrorKind::Io, r.error().message);
            return {};
        }

        [[nodiscard]] inline std::expected<std::optional<Meta>, WorkspaceError> read_meta(std::filesystem::path const& root)
        {
            auto const dir = root / std::string(meta_dir);
            auto const file = dir / std::string(meta_file);
            std::error_code ec;
            auto const status = std::filesystem::symlink_status(file, ec);
            if (ec || status.type() == std::filesystem::file_type::not_found)
                return std::optional<Meta>{};
            if (platform::is_symlink_or_reparse(dir) || platform::is_symlink_or_reparse(file))
                return fail(ErrorKind::Unsafe, std::format("'{}' is a symbolic link or reparse point; refusing to use it", platform::path_to_utf8(file)));
            auto data = platform::read_file(file);
            if (!data)
                return fail(ErrorKind::Corrupt, std::format("cannot read '{}'", platform::path_to_utf8(file)));
            auto parsed = json::parse(*data);
            if (!parsed)
                return fail(ErrorKind::Corrupt, std::format("'{}' is not valid: {}", platform::path_to_utf8(file), parsed.error().format()));
            auto const* exercise = parsed->find("exercise");
            auto const* revision = parsed->find("revision");
            auto const* schema = parsed->find("schema");
            if (exercise == nullptr || !exercise->is_string() || revision == nullptr || !revision->is_int() || schema == nullptr ||
                schema->as_int() != std::optional<std::int64_t>(1))
                return fail(ErrorKind::Corrupt, std::format("'{}' is not a learndc workspace file", platform::path_to_utf8(file)));
            Meta meta;
            meta.exercise = std::string(*exercise->as_string());
            meta.revision = static_cast<int>(*revision->as_int());
            if (auto const* created = parsed->find("created_at"); created != nullptr && created->is_string())
                meta.created_at = std::string(*created->as_string());
            if (auto const* starter = parsed->find("starter"); starter != nullptr && starter->is_object())
                for (auto const& [path, value] : *starter->as_object())
                    if (auto h = value.as_string())
                        meta.starter.emplace(path, std::string(*h));
            return std::optional<Meta>(std::move(meta));
        }

        [[nodiscard]] inline std::filesystem::path normalized_root(std::filesystem::path const& root)
        {
            std::error_code ec;
            auto absolute = std::filesystem::absolute(root, ec);
            if (ec)
                absolute = root;
            absolute = absolute.lexically_normal();
            if (!absolute.empty() && !absolute.has_filename() && absolute != absolute.root_path())
                absolute = absolute.parent_path();
            return absolute;
        }

        [[nodiscard]] inline std::expected<void, WorkspaceError> validate_files(std::map<std::string, std::string> const& files)
        {
            for (auto const& [path, bytes] : files)
                if (auto e = check_relative(path))
                    return std::unexpected(*e);
            return {};
        }

        [[nodiscard]] inline std::map<std::string, std::string> hashes_of(std::map<std::string, std::string> const& files)
        {
            std::map<std::string, std::string> out;
            for (auto const& [path, bytes] : files)
                out.emplace(path, hash_hex(bytes));
            return out;
        }

        [[nodiscard]] inline std::expected<void, WorkspaceError> write_new(std::filesystem::path const& root, std::string const& rel, std::string_view bytes)
        {
            if (auto e = check_chain(root, rel))
                return std::unexpected(*e);
            if (auto r = ensure_parent(root, rel); !r)
                return r;
            if (auto r = platform::create_new_file(join(root, rel), bytes); !r)
                return fail(r.error().exists ? ErrorKind::Refused : ErrorKind::Io, r.error().message);
            return {};
        }

        [[nodiscard]] inline bool is_binary(std::string_view data)
        {
            return data.find('\0') != std::string_view::npos || !text::utf8_valid(data);
        }

        [[nodiscard]] inline std::string diff_one(std::string const& path, std::optional<std::string> const& a, std::optional<std::string> const& b,
                                                  std::string_view prefix_a, std::string_view prefix_b)
        {
            auto const label_a = a ? std::format("{}/{}", prefix_a, path) : std::string("/dev/null");
            auto const label_b = b ? std::format("{}/{}", prefix_b, path) : std::string("/dev/null");
            if ((a && is_binary(*a)) || (b && is_binary(*b)))
                return std::format("Binary files {} and {} differ\n", label_a, label_b);
            return diff::unified(a.value_or(""), b.value_or(""), label_a, label_b);
        }
    } // namespace detail

    [[nodiscard]] inline std::filesystem::path default_root(platform::Dirs const& dirs, catalog::Exercise const& ex)
    {
        return dirs.workspaces_dir / platform::path_from_utf8(ex.track) / platform::path_from_utf8(ex.name);
    }

    [[nodiscard]] inline std::vector<std::string> files(Workspace const& ws)
    {
        std::vector<std::string> out;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(ws.root, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator const end;
        for (; !ec && it != end; it.increment(ec))
        {
            auto const& entry = *it;
            auto const rel = std::filesystem::relative(entry.path(), ws.root, ec);
            if (ec)
                break;
            auto const generic_u8 = rel.generic_u8string();
            auto const generic = std::string(reinterpret_cast<char const*>(generic_u8.data()), generic_u8.size());
            auto const status = entry.symlink_status(ec);
            if (ec)
                break;
            if (it.depth() == 0 && (generic == meta_dir || generic == ".dpm"))
            {
                it.disable_recursion_pending();
                continue;
            }
            if (status.type() == std::filesystem::file_type::directory)
            {
                std::error_code nested;
                if (std::filesystem::is_regular_file(entry.path() / std::string(meta_dir) / std::string(meta_file), nested))
                    it.disable_recursion_pending();
                continue;
            }
            if (status.type() != std::filesystem::file_type::regular)
                continue;
            if (detail::ignored_name(platform::path_to_utf8(entry.path().filename())))
                continue;
            out.push_back(generic);
        }
        std::ranges::sort(out);
        return out;
    }

    [[nodiscard]] inline std::expected<Workspace, WorkspaceError> load(std::filesystem::path const& root_in)
    {
        auto const root = detail::normalized_root(root_in);
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
            return detail::fail(ErrorKind::NotFound, std::format("workspace '{}' does not exist", platform::path_to_utf8(root)));
        auto meta = detail::read_meta(root);
        if (!meta)
            return std::unexpected(meta.error());
        if (!*meta)
            return detail::fail(ErrorKind::NotFound, std::format("'{}' is not a learndc workspace", platform::path_to_utf8(root)));
        return Workspace{root, std::move(**meta)};
    }

    [[nodiscard]] inline std::optional<Workspace> find_enclosing(std::filesystem::path const& cwd)
    {
        auto current = detail::normalized_root(cwd);
        while (true)
        {
            std::error_code ec;
            auto const file = current / std::string(meta_dir) / std::string(meta_file);
            if (std::filesystem::is_regular_file(file, ec) && !ec)
                if (auto ws = load(current))
                    return *ws;
            auto parent = current.parent_path();
            if (parent.empty() || parent == current)
                return std::nullopt;
            current = std::move(parent);
        }
    }

    [[nodiscard]] inline std::expected<StartResult, WorkspaceError> start(catalog::Exercise const& ex, std::filesystem::path const& root_in)
    {
        if (root_in.empty())
            return detail::fail(ErrorKind::InvalidPath, "the workspace directory is empty");
        if (auto r = detail::validate_files(ex.starter); !r)
            return std::unexpected(r.error());
        auto const root = detail::normalized_root(root_in);
        std::error_code ec;
        auto const status = std::filesystem::symlink_status(root, ec);
        bool const exists = !ec && status.type() != std::filesystem::file_type::not_found;
        if (exists && platform::is_symlink_or_reparse(root))
            return detail::fail(ErrorKind::Unsafe, std::format("'{}' is a symbolic link or reparse point; refusing to use it", platform::path_to_utf8(root)));
        StartResult result;
        if (exists)
        {
            if (status.type() != std::filesystem::file_type::directory)
                return detail::fail(ErrorKind::Refused, std::format("'{}' exists and is not a directory", platform::path_to_utf8(root)));
            auto meta = detail::read_meta(root);
            if (!meta)
                return std::unexpected(meta.error());
            if (*meta)
            {
                if ((*meta)->exercise != ex.id)
                    return detail::fail(ErrorKind::Refused, std::format("'{}' is the workspace of '{}', not '{}'; choose another directory",
                                                                        platform::path_to_utf8(root), text::sanitize((*meta)->exercise), ex.id));
                result.workspace = Workspace{root, std::move(**meta)};
                result.outdated = result.workspace.meta.revision < ex.revision;
                for (auto const& [path, bytes] : ex.starter)
                    if (auto e = detail::check_chain(root, path))
                        return std::unexpected(*e);
                if (!result.outdated)
                    for (auto const& [path, bytes] : ex.starter)
                    {
                        auto const target = detail::join(root, path);
                        std::error_code probe;
                        if (std::filesystem::exists(std::filesystem::symlink_status(target, probe)))
                            continue;
                        if (auto r = detail::write_new(root, path, bytes); !r)
                            return std::unexpected(r.error());
                        result.restored.push_back(path);
                    }
                return result;
            }
            std::filesystem::directory_iterator it(root, ec);
            if (ec)
                return detail::fail(ErrorKind::Io, std::format("cannot read '{}': {}", platform::path_to_utf8(root), ec.message()));
            if (it != std::filesystem::directory_iterator{})
                return detail::fail(ErrorKind::Refused, std::format("'{}' already exists and is not a learndc workspace for '{}'; choose another directory",
                                                                    platform::path_to_utf8(root), ex.id));
        }
        else
        {
            std::filesystem::create_directories(root, ec);
            if (ec)
                return detail::fail(ErrorKind::Io, std::format("cannot create '{}': {}", platform::path_to_utf8(root), ec.message()));
        }
        if (auto outer = find_enclosing(root.parent_path()))
            return detail::fail(ErrorKind::Refused, std::format("'{}' is inside the workspace '{}' of '{}'; choose a directory outside it",
                                                                platform::path_to_utf8(root), platform::path_to_utf8(outer->root),
                                                                text::sanitize(outer->meta.exercise)));
        Meta meta;
        meta.exercise = ex.id;
        meta.revision = ex.revision;
        meta.created_at = platform::now_utc_iso8601();
        meta.starter = detail::hashes_of(ex.starter);
        if (auto r = detail::write_meta(root, meta); !r)
            return std::unexpected(r.error());
        for (auto const& [path, bytes] : ex.starter)
            if (auto r = detail::write_new(root, path, bytes); !r)
                return std::unexpected(r.error());
        result.workspace = Workspace{root, std::move(meta)};
        result.created = true;
        return result;
    }

    [[nodiscard]] inline std::vector<FileChange> changes_against(Workspace const& ws, std::map<std::string, std::string> const& baseline)
    {
        std::vector<FileChange> out;
        auto const present = files(ws);
        for (auto const& path : present)
        {
            auto it = baseline.find(path);
            if (it == baseline.end())
            {
                out.push_back({path, ChangeKind::Added});
                continue;
            }
            auto data = platform::read_file(detail::join(ws.root, path));
            if (!data || hash_hex(*data) != it->second)
                out.push_back({path, ChangeKind::Modified});
        }
        for (auto const& [path, hash] : baseline)
            if (!std::ranges::binary_search(present, path))
                out.push_back({path, ChangeKind::Missing});
        std::ranges::sort(out, {}, &FileChange::path);
        return out;
    }

    [[nodiscard]] inline std::vector<FileChange> modified(Workspace const& ws)
    {
        return changes_against(ws, ws.meta.starter);
    }

    [[nodiscard]] inline std::vector<FileChange> modified(Workspace const& ws, catalog::Exercise const& ex)
    {
        return changes_against(ws, detail::hashes_of(ex.starter));
    }

    [[nodiscard]] inline std::expected<ResetResult, WorkspaceError> reset(Workspace& ws, catalog::Exercise const& ex, std::filesystem::path const& backups_dir,
                                                                          bool backup = true)
    {
        if (ws.meta.exercise != ex.id)
            return detail::fail(ErrorKind::Refused, std::format("'{}' is the workspace of '{}', not '{}'", platform::path_to_utf8(ws.root),
                                                                text::sanitize(ws.meta.exercise), ex.id));
        if (auto r = detail::validate_files(ex.starter); !r)
            return std::unexpected(r.error());
        if (auto e = detail::check_chain(ws.root, ""))
            return std::unexpected(*e);
        for (auto const& [path, bytes] : ex.starter)
            if (auto e = detail::check_chain(ws.root, path))
                return std::unexpected(*e);
        ResetResult result;
        auto const present = files(ws);
        if (backup)
        {
            auto const root_text = platform::path_to_utf8(ws.root);
            auto const backups_abs = detail::normalized_root(backups_dir);
            auto const rel_to_root = backups_abs.lexically_relative(ws.root);
            if (!rel_to_root.empty() && *rel_to_root.begin() != "..")
                return detail::fail(ErrorKind::Refused,
                                    std::format("the backup directory '{}' is inside the workspace '{}'", platform::path_to_utf8(backups_abs), root_text));
            if (!present.empty())
            {
                auto stamp = platform::now_utc_iso8601();
                std::erase_if(stamp, [](char c) { return c == '-' || c == ':'; });
                auto const base = backups_abs / platform::path_from_utf8(ex.id);
                auto target = base / platform::path_from_utf8(stamp);
                std::error_code ec;
                for (int n = 1; std::filesystem::exists(std::filesystem::symlink_status(target, ec)); ++n)
                    target = base / platform::path_from_utf8(std::format("{}-{}", stamp, n));
                std::filesystem::create_directories(target, ec);
                if (ec)
                    return detail::fail(ErrorKind::Io,
                                        std::format("cannot create the backup directory '{}': {}", platform::path_to_utf8(target), ec.message()));
                for (auto const& rel : present)
                {
                    auto data = platform::read_file(detail::join(ws.root, rel));
                    if (!data)
                        return detail::fail(ErrorKind::Io, std::format("cannot read '{}' for the backup; nothing was changed", rel));
                    if (auto r = detail::write_new(target, rel, *data); !r)
                        return detail::fail(r.error().kind, std::format("backup failed, nothing was changed: {}", r.error().message));
                    auto copy = platform::read_file(detail::join(target, rel));
                    if (!copy || *copy != *data)
                        return detail::fail(ErrorKind::Io, std::format("backup of '{}' could not be verified; nothing was changed", rel));
                }
                result.backup_path = target;
            }
        }
        for (auto const& [path, bytes] : ex.starter)
        {
            if (auto r = detail::ensure_parent(ws.root, path); !r)
                return std::unexpected(r.error());
            if (auto w = platform::atomic_write(detail::join(ws.root, path), bytes); !w)
                return detail::fail(ErrorKind::Io, w.error().message);
            result.restored.push_back(path);
        }
        for (auto const& rel : present)
        {
            if (ex.starter.contains(rel))
                continue;
            if (!backup)
            {
                result.kept.push_back(rel);
                continue;
            }
            std::error_code ec;
            std::filesystem::remove(detail::join(ws.root, rel), ec);
            if (ec)
            {
                result.kept.push_back(rel);
                continue;
            }
            result.removed.push_back(rel);
            auto dir = detail::join(ws.root, rel).parent_path();
            while (dir != ws.root && !dir.empty())
            {
                std::error_code rm;
                if (!std::filesystem::is_empty(dir, rm) || rm)
                    break;
                std::filesystem::remove(dir, rm);
                dir = dir.parent_path();
            }
        }
        ws.meta.revision = ex.revision;
        ws.meta.starter = detail::hashes_of(ex.starter);
        if (auto r = detail::write_meta(ws.root, ws.meta); !r)
            return std::unexpected(r.error());
        return result;
    }

    [[nodiscard]] inline std::expected<StartResult, WorkspaceError> fresh_copy(Workspace const& ws, catalog::Exercise const& ex)
    {
        if (ws.meta.exercise != ex.id)
            return detail::fail(ErrorKind::Refused, std::format("'{}' is the workspace of '{}', not '{}'", platform::path_to_utf8(ws.root),
                                                                text::sanitize(ws.meta.exercise), ex.id));
        auto name = platform::path_to_utf8(ws.root.filename());
        if (auto const pos = name.rfind(".r");
            pos != std::string::npos && pos + 2 < name.size() && std::ranges::all_of(name.substr(pos + 2), [](char c) { return c >= '0' && c <= '9'; }))
            name.resize(pos);
        auto const target = ws.root.parent_path() / platform::path_from_utf8(std::format("{}.r{}", name, ex.revision));
        return start(ex, target);
    }

    [[nodiscard]] inline std::string starter_diff(Workspace const& ws, catalog::Exercise const& ex)
    {
        std::set<std::string> paths;
        for (auto const& [path, bytes] : ex.starter)
            paths.insert(path);
        for (auto const& path : files(ws))
            paths.insert(path);
        std::string out;
        for (auto const& path : paths)
        {
            std::optional<std::string> a;
            if (auto it = ex.starter.find(path); it != ex.starter.end())
                a = it->second;
            std::optional<std::string> b;
            if (!detail::relative_problem(path) && !detail::check_chain(ws.root, path))
                b = platform::read_file(detail::join(ws.root, path));
            if (a == b)
                continue;
            out += detail::diff_one(path, a, b, "starter", "yours");
        }
        return out;
    }

    [[nodiscard]] inline std::string solution_diff(Workspace const& ws, catalog::Exercise const& ex)
    {
        std::map<std::string, std::string> target = ex.starter;
        for (auto const& [path, bytes] : ex.solution)
            target[path] = bytes;
        std::string out;
        for (auto const& [path, bytes] : target)
        {
            std::optional<std::string> mine;
            if (!detail::relative_problem(path) && !detail::check_chain(ws.root, path))
                mine = platform::read_file(detail::join(ws.root, path));
            if (mine && *mine == bytes)
                continue;
            out += detail::diff_one(path, mine, std::optional<std::string>(bytes), "yours", "solution");
        }
        return out;
    }
} // namespace learndc::workspace
