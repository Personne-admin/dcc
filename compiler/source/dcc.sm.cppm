module;

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

export module dcc.sm;

import std;
import dcc.utf8;

export namespace dcc::sm
{
    enum class Error : std::uint8_t
    {
        FileNotFound,
        PermissionDenied,
        FileTooLarge,
        MmapFailed,
        InvalidUtf8,
        OutOfRange,
        NotOpen,
    };

    [[nodiscard]] constexpr std::string_view to_string(Error e) noexcept
    {
        switch (e)
        {
            case Error::FileNotFound:
                return "file not found";
            case Error::PermissionDenied:
                return "permission denied";
            case Error::FileTooLarge:
                return "file too large";
            case Error::MmapFailed:
                return "mmap failed";
            case Error::InvalidUtf8:
                return "invalid UTF-8";
            case Error::OutOfRange:
                return "offset out of range";
            case Error::NotOpen:
                return "document is not open";
        }
        return "unknown";
    }

    enum class FileId : std::uint32_t
    {
        Invalid = 0xFFFFFFFFu
    };

    using Offset = std::uint32_t;

    struct Location
    {
        FileId fileId{FileId::Invalid};
        Offset offset{};

        [[nodiscard]] constexpr bool valid() const noexcept { return fileId != FileId::Invalid; }
        [[nodiscard]] constexpr bool operator==(Location const&) const noexcept = default;
    };

    struct SourceRange
    {
        Location begin;
        Location end;

        [[nodiscard]] constexpr bool valid() const noexcept { return begin.valid() && end.fileId == begin.fileId && end.offset >= begin.offset; }

        [[nodiscard]] constexpr Offset byte_length() const noexcept { return end.offset - begin.offset; }

        [[nodiscard]] constexpr bool operator==(SourceRange const&) const noexcept = default;
    };

    struct LineCol
    {
        std::uint32_t line;
        std::uint32_t column;
        std::uint32_t byte_col;
    };

    struct Position
    {
        std::uint32_t line{};
        std::uint32_t character{};

        [[nodiscard]] constexpr bool operator==(Position const&) const noexcept = default;
    };

    enum class PositionEncoding : std::uint8_t
    {
        Utf8,
        Utf16,
        Utf32,
    };

    [[nodiscard]] constexpr std::string_view to_string(PositionEncoding enc) noexcept
    {
        switch (enc)
        {
            case PositionEncoding::Utf8:
                return "utf-8";
            case PositionEncoding::Utf16:
                return "utf-16";
            case PositionEncoding::Utf32:
                return "utf-32";
        }
        return "utf-16";
    }

    enum class FileKind : std::uint8_t
    {
        Disk,
        Synthetic,
        InMemory,
    };

    struct DiskSignature
    {
        std::int64_t mtime_sec{0};
        std::int64_t mtime_nsec{0};
        std::uint64_t size{0};

        [[nodiscard]] friend bool operator==(DiskSignature const&, DiskSignature const&) = default;
    };

    [[nodiscard]] constexpr std::uint32_t utf16_code_units(utf8::Codepoint cp) noexcept
    {
        return cp > 0xFFFFu ? 2u : 1u;
    }

    [[nodiscard]] constexpr std::uint32_t position_units(utf8::Codepoint cp, int bytes_consumed, PositionEncoding enc) noexcept
    {
        switch (enc)
        {
            case PositionEncoding::Utf8:
                return static_cast<std::uint32_t>(bytes_consumed);
            case PositionEncoding::Utf16:
                return utf16_code_units(cp);
            case PositionEncoding::Utf32:
                return 1;
        }
        return 1;
    }

    class SourceFile
    {
    public:
        explicit SourceFile(FileId id, std::filesystem::path path, void* mapping, std::size_t size, bool is_mmaped = true) noexcept
            : m_id{id}, m_path{std::move(path)}, m_mapping{mapping}, m_size{size}, m_is_mmaped{is_mmaped}
        {
        }

        SourceFile(FileId id, std::filesystem::path path, FileKind kind, std::string uri, std::optional<std::int64_t> version,
                   std::unique_ptr<std::string> owned_content) noexcept
            : m_id{id}, m_path{std::move(path)}, m_owned_content{std::move(owned_content)}, m_kind{kind}, m_uri{std::move(uri)}, m_version{version},
              m_mapping{m_owned_content ? static_cast<void*>(m_owned_content->data()) : nullptr}, m_size{m_owned_content ? m_owned_content->size() : 0u},
              m_is_mmaped{false}
        {
        }

        SourceFile(SourceFile const&) = delete;
        SourceFile& operator=(SourceFile const&) = delete;

        SourceFile(SourceFile&& o) noexcept
            : m_id{o.m_id}, m_path{std::move(o.m_path)}, m_owned_content{std::move(o.m_owned_content)}, m_kind{o.m_kind}, m_uri{std::move(o.m_uri)},
              m_version{o.m_version}, m_mapping{o.m_mapping}, m_size{o.m_size}, m_is_mmaped{o.m_is_mmaped}, m_line_start{std::move(o.m_line_start)},
              m_line_index_built{o.m_line_index_built}, m_closed{o.m_closed}, m_content_revision{o.m_content_revision}, m_disk_signature{o.m_disk_signature}
        {
            o.m_mapping = nullptr;
            o.m_size = 0;
            o.m_is_mmaped = false;
            o.m_closed = false;
        }

        SourceFile& operator=(SourceFile&& o) noexcept
        {
            if (this != &o)
            {
                close_mapping();

                m_id = o.m_id;
                m_path = std::move(o.m_path);
                m_owned_content = std::move(o.m_owned_content);
                m_kind = o.m_kind;
                m_uri = std::move(o.m_uri);
                m_version = o.m_version;
                m_size = o.m_size;
                m_mapping = o.m_mapping;
                m_is_mmaped = o.m_is_mmaped;
                m_line_start = std::move(o.m_line_start);
                m_line_index_built = o.m_line_index_built;
                m_closed = o.m_closed;
                m_content_revision = o.m_content_revision;
                m_disk_signature = o.m_disk_signature;

                o.m_mapping = nullptr;
                o.m_size = 0;
                o.m_is_mmaped = false;
                o.m_closed = false;
            }

            return *this;
        }

        ~SourceFile() { close_mapping(); }

        [[nodiscard]] FileId id() const noexcept { return m_id; }
        [[nodiscard]] std::filesystem::path const& path() const noexcept { return m_path; }

        [[nodiscard]] std::string const& uri() const noexcept { return m_uri; }
        [[nodiscard]] std::optional<std::int64_t> version() const noexcept { return m_version; }
        [[nodiscard]] FileKind kind() const noexcept { return m_kind; }
        [[nodiscard]] bool is_in_memory() const noexcept { return m_kind == FileKind::InMemory; }
        [[nodiscard]] bool is_closed() const noexcept { return m_closed; }

        [[nodiscard]] std::uint64_t content_revision() const noexcept { return m_content_revision; }

        [[nodiscard]] std::optional<DiskSignature> disk_signature() const noexcept { return m_disk_signature; }

        [[nodiscard]] std::span<std::byte const> bytes() const noexcept { return {static_cast<std::byte const*>(m_mapping), m_size}; }
        [[nodiscard]] std::string_view text() const noexcept { return {static_cast<char const*>(m_mapping), m_size}; }

        [[nodiscard]] std::expected<std::string_view, Error> text(Offset begin, Offset end) const noexcept
        {
            if (begin > end || end > static_cast<Offset>(m_size))
                return std::unexpected{Error::OutOfRange};

            return std::string_view{static_cast<char const*>(m_mapping) + begin, static_cast<std::size_t>(end - begin)};
        }

        [[nodiscard]] std::expected<std::string_view, Error> text(SourceRange range) const noexcept
        {
            if (range.begin.fileId != m_id)
                return std::unexpected{Error::OutOfRange};

            return text(range.begin.offset, range.end.offset);
        }

        [[nodiscard]] std::size_t size() const noexcept { return m_size; }

        [[nodiscard]] std::expected<LineCol, Error> line_col(Offset offset) const
        {
            if (offset > static_cast<Offset>(m_size))
                return std::unexpected{Error::OutOfRange};

            ensure_line_index();

            auto it = std::ranges::upper_bound(m_line_start, offset);
            --it;

            auto const line_idx = static_cast<std::uint32_t>(it - m_line_start.begin());
            auto const line_start = *it;
            auto const line_number = line_idx + 1;

            auto const* data = static_cast<char const*>(m_mapping);
            std::string_view line_sv{data + line_start, offset - line_start};

            int visual_col = 0;
            std::uint32_t byte_col = 0;

            for (auto result : utf8::codepoints(line_sv))
            {
                if (!result)
                    return std::unexpected{Error::InvalidUtf8};

                visual_col += utf8::codepoint_width(result->codepoint);
                byte_col += static_cast<std::uint32_t>(result->bytes_consumed);
            }

            return LineCol{
                .line = line_number,
                .column = static_cast<std::uint32_t>(visual_col) + 1,
                .byte_col = byte_col + 1,
            };
        }

        [[nodiscard]] std::expected<Position, Error> lsp_position(Offset offset, PositionEncoding enc = PositionEncoding::Utf16) const
        {
            if (offset > static_cast<Offset>(m_size))
                return std::unexpected{Error::OutOfRange};

            if (!m_mapping && m_size != 0)
                return std::unexpected{Error::MmapFailed};

            ensure_line_index();

            auto it = std::ranges::upper_bound(m_line_start, offset);
            --it;

            auto const line_idx = static_cast<std::uint32_t>(it - m_line_start.begin());
            auto const line_start = *it;

            auto const scan_end = std::min<std::size_t>(offset, line_content_end(line_idx));

            auto const* data = static_cast<char const*>(m_mapping);
            std::uint32_t character = 0;
            std::size_t pos = line_start;

            while (pos < scan_end)
            {
                auto result = utf8::decode_one(std::string_view{data + pos, scan_end - pos});
                if (!result)
                    return std::unexpected{Error::InvalidUtf8};

                auto consumed = static_cast<std::size_t>(result->bytes_consumed);

                if (pos + consumed > scan_end)
                    return std::unexpected{Error::InvalidUtf8};

                character += position_units(result->codepoint, result->bytes_consumed, enc);
                pos += consumed;
            }

            return Position{line_idx, character};
        }

        [[nodiscard]] std::expected<Offset, Error> offset_at_lsp_position(std::uint32_t line, std::uint32_t unit_col,
                                                                          PositionEncoding enc = PositionEncoding::Utf16) const
        {
            if (!m_mapping && m_size != 0)
                return std::unexpected{Error::MmapFailed};

            ensure_line_index();

            if (line >= m_line_start.size())
                return std::unexpected{Error::OutOfRange};

            auto const line_start = m_line_start[line];
            auto const content_end = line_content_end(line);

            auto const* data = static_cast<char const*>(m_mapping);
            std::size_t pos = line_start;
            std::uint32_t units = 0;

            while (pos < content_end)
            {
                if (units >= unit_col)
                    break;

                auto result = utf8::decode_one(std::string_view{data + pos, content_end - pos});
                if (!result)
                    return std::unexpected{Error::InvalidUtf8};

                auto consumed = static_cast<std::size_t>(result->bytes_consumed);
                auto const next_units = units + position_units(result->codepoint, result->bytes_consumed, enc);

                if (unit_col < next_units)
                    break;

                units = next_units;
                pos += consumed;
            }

            return static_cast<Offset>(pos);
        }

        [[nodiscard]] std::expected<std::string_view, Error> line_text(std::uint32_t line_number) const
        {
            ensure_line_index();

            if (line_number == 0 || line_number > static_cast<std::uint32_t>(m_line_start.size()))
                return std::unexpected{Error::OutOfRange};

            auto const start = m_line_start[line_number - 1];
            auto const next_start = (line_number < m_line_start.size()) ? m_line_start[line_number] : static_cast<Offset>(m_size) + 1;

            auto end = next_start - 1;
            if (end > static_cast<Offset>(m_size))
                end = static_cast<Offset>(m_size);

            auto const* data = static_cast<char const*>(m_mapping);
            if (end > start && data[end - 1] == '\r')
                --end;

            return std::string_view{data + start, end - start};
        }

        [[nodiscard]] std::uint32_t line_count() const
        {
            ensure_line_index();
            return static_cast<std::uint32_t>(m_line_start.size());
        }

        [[nodiscard]] utf8::CodepointRange codepoints() const noexcept { return utf8::codepoints(text()); }

        [[nodiscard]] utf8::CodepointRange codepoints(Offset begin, Offset end) const noexcept
        {
            auto const* data = static_cast<char const*>(m_mapping);
            auto const clamped_end = std::min<std::size_t>(end, m_size);
            auto const clamped_begin = std::min<std::size_t>(begin, clamped_end);

            return utf8::codepoints(std::string_view{data + clamped_begin, clamped_end - clamped_begin});
        }

        void replace_buffer(std::unique_ptr<std::string> new_content)
        {
            m_owned_content = std::move(new_content);
            m_mapping = m_owned_content ? static_cast<void*>(m_owned_content->data()) : nullptr;
            m_size = m_owned_content ? m_owned_content->size() : 0u;
            m_line_start.clear();
            m_line_index_built = false;
        }

        void mark_closed() noexcept { m_closed = true; }

        void mark_open() noexcept { m_closed = false; }

        void set_uri(std::string u) { m_uri = std::move(u); }

        void set_version(std::optional<std::int64_t> v) { m_version = v; }

    private:
        friend class SourceManager;

        void close_mapping() noexcept;

        void ensure_line_index() const
        {
            if (m_line_index_built)
                return;

            m_line_start.clear();
            m_line_start.push_back(0);

            auto const* data = static_cast<char const*>(m_mapping);
            std::size_t i = 0;
            while (i < m_size)
            {
                if (data[i] == '\n')
                {
                    m_line_start.push_back(static_cast<Offset>(i + 1));
                    ++i;
                }
                else if (data[i] == '\r')
                {
                    if (i + 1 < m_size && data[i + 1] == '\n')
                        ++i;
                    m_line_start.push_back(static_cast<Offset>(i + 1));
                    ++i;
                }
                else
                {
                    ++i;
                }
            }

            m_line_index_built = true;
        }

        [[nodiscard]] Offset line_content_end(std::uint32_t line_idx) const
        {
            auto const line_start = m_line_start[line_idx];

            Offset end;
            if (line_idx + 1 < m_line_start.size())
                end = m_line_start[line_idx + 1] - 1;
            else
                end = static_cast<Offset>(m_size);

            auto const* data = static_cast<char const*>(m_mapping);
            if (end > line_start && end <= static_cast<Offset>(m_size))
            {
                auto const last = data[end - 1];
                if (last == '\r' || last == '\n')
                    return std::max(end - 1, line_start);
            }

            return std::max(end, line_start);
        }

        FileId m_id;
        std::filesystem::path m_path;
        std::unique_ptr<std::string> m_owned_content;
        FileKind m_kind{FileKind::Disk};
        std::string m_uri;
        std::optional<std::int64_t> m_version;
        void* m_mapping{nullptr};
        std::size_t m_size{};
        bool m_is_mmaped{false};

        mutable std::vector<Offset> m_line_start;
        mutable bool m_line_index_built{false};
        bool m_closed{false};

        std::uint64_t m_content_revision{0};
        std::optional<DiskSignature> m_disk_signature;
    };

    class SourceManager
    {
    public:
        SourceManager() = default;
        ~SourceManager() = default;

        SourceManager(SourceManager const&) = delete;
        SourceManager& operator=(SourceManager const&) = delete;
        SourceManager(SourceManager&&) = default;
        SourceManager& operator=(SourceManager&&) = default;

        [[nodiscard]] std::expected<FileId, Error> load(std::filesystem::path const& path);
        [[nodiscard]] FileId add_synthetic(std::string name, std::string content);

        [[nodiscard]] SourceFile const* get(FileId id) const noexcept
        {
            auto const idx = static_cast<std::size_t>(id);
            if (idx >= m_files.size())
                return nullptr;

            return m_files[idx].get();
        }

        [[nodiscard]] SourceFile* get_mut(FileId id) noexcept
        {
            auto const idx = static_cast<std::size_t>(id);
            if (idx >= m_files.size())
                return nullptr;

            return m_files[idx].get();
        }

        [[nodiscard]] std::optional<FileId> find_by_path(std::filesystem::path const& p) const noexcept
        {
            auto canonical = std::filesystem::weakly_canonical(p);
            for (auto const& f : m_files)
                if (f->path() == canonical)
                    return f->id();

            return std::nullopt;
        }

        [[nodiscard]] std::optional<FileId> find_by_uri(std::string_view uri) const noexcept
        {
            for (auto const& f : m_files)
                if (f->uri() == uri)
                    return f->id();

            return std::nullopt;
        }

        [[nodiscard]] std::expected<FileId, Error> load_uri(std::string_view uri)
        {
            auto path = parse_file_uri(uri);
            if (!path)
                return std::unexpected{Error::FileNotFound};

            return load(*path);
        }

        [[nodiscard]] FileId open_in_memory(std::string uri, std::string content, std::optional<std::int64_t> version = std::nullopt)
        {
            if (auto existing = find_by_uri(uri))
            {
                auto* file = get_mut(*existing);
                if (file && file->kind() == FileKind::InMemory)
                {
                    file->replace_buffer(std::make_unique<std::string>(std::move(content)));
                    file->set_version(version);
                    file->mark_open();
                    file->m_content_revision = next_content_revision();
                }
                return *existing;
            }

            auto const id = next_id();
            auto owned = std::make_unique<std::string>(std::move(content));
            auto parsed = parse_file_uri(uri);
            auto path = parsed ? *parsed : std::filesystem::path{uri};

            {
                std::error_code wc_ec;
                auto wc = std::filesystem::weakly_canonical(path, wc_ec);
                if (!wc_ec)
                    path = std::move(wc);
            }

            auto file = std::make_unique<SourceFile>(id, std::move(path), FileKind::InMemory, std::move(uri), version, std::move(owned));
            file->m_content_revision = next_content_revision();
            m_files.push_back(std::move(file));
            return id;
        }

        [[nodiscard]] FileId open_virtual_in_memory(std::string uri, std::string content, std::filesystem::path synthetic_path)
        {
            if (auto existing = find_by_uri(uri))
            {
                auto* file = get_mut(*existing);
                if (file && file->kind() == FileKind::InMemory)
                {
                    file->replace_buffer(std::make_unique<std::string>(std::move(content)));
                    file->m_content_revision = next_content_revision();
                }

                return *existing;
            }

            auto const id = next_id();
            auto owned = std::make_unique<std::string>(std::move(content));
            auto file = std::make_unique<SourceFile>(id, std::move(synthetic_path), FileKind::InMemory, std::move(uri), std::nullopt, std::move(owned));
            file->m_content_revision = next_content_revision();
            m_files.push_back(std::move(file));
            return id;
        }

        [[nodiscard]] std::expected<void, Error> update_in_memory(std::string_view uri, std::string new_content,
                                                                  std::optional<std::int64_t> new_version = std::nullopt)
        {
            auto existing = find_by_uri(uri);
            if (!existing)
                return std::unexpected{Error::FileNotFound};

            auto* file = get_mut(*existing);
            if (!file)
                return std::unexpected{Error::FileNotFound};

            if (file->kind() != FileKind::InMemory)
                return std::unexpected{Error::PermissionDenied};

            if (file->is_closed())
                return std::unexpected{Error::NotOpen};

            file->replace_buffer(std::make_unique<std::string>(std::move(new_content)));
            file->set_version(new_version);
            file->m_content_revision = next_content_revision();
            return {};
        }

        [[nodiscard]] std::expected<void, Error> close_in_memory(std::string_view uri)
        {
            auto existing = find_by_uri(uri);
            if (!existing)
                return std::unexpected{Error::FileNotFound};

            auto* file = get_mut(*existing);
            if (!file)
                return std::unexpected{Error::FileNotFound};

            if (file->kind() != FileKind::InMemory)
                return std::unexpected{Error::PermissionDenied};

            file->mark_closed();
            return {};
        }

        [[nodiscard]] std::expected<Location, Error> lsp_position_to_location(FileId fid, Position pos) const
        {
            auto const* f = get(fid);
            if (!f)
                return std::unexpected{Error::OutOfRange};

            auto offset = f->offset_at_lsp_position(pos.line, pos.character, m_position_encoding);
            if (!offset)
                return std::unexpected{offset.error()};

            return Location{fid, *offset};
        }

        [[nodiscard]] std::expected<Location, Error> lsp_position_to_location(FileId fid, std::uint32_t line, std::uint32_t unit_col) const
        {
            auto const* f = get(fid);
            if (!f)
                return std::unexpected{Error::OutOfRange};

            auto offset = f->offset_at_lsp_position(line, unit_col, m_position_encoding);
            if (!offset)
                return std::unexpected{offset.error()};

            return Location{fid, *offset};
        }

        [[nodiscard]] std::expected<Position, Error> location_to_lsp_position(Location loc) const
        {
            auto const* f = get(loc.fileId);
            if (!f)
                return std::unexpected{Error::OutOfRange};

            return f->lsp_position(loc.offset, m_position_encoding);
        }

        void set_position_encoding(PositionEncoding enc) noexcept { m_position_encoding = enc; }

        [[nodiscard]] PositionEncoding position_encoding() const noexcept { return m_position_encoding; }

        [[nodiscard]] SourceFile const* get(Location loc) const noexcept { return get(loc.fileId); }
        [[nodiscard]] constexpr Location location(FileId id, Offset offset) const noexcept { return Location{id, offset}; }

        [[nodiscard]] std::expected<LineCol, Error> line_col(Location loc) const
        {
            auto const* f = get(loc.fileId);
            if (!f)
                return std::unexpected{Error::OutOfRange};

            return f->line_col(loc.offset);
        }

        [[nodiscard]] std::expected<std::string_view, Error> text(SourceRange range) const
        {
            auto const* f = get(range.begin.fileId);
            if (!f)
                return std::unexpected{Error::OutOfRange};

            return f->text(range);
        }

        [[nodiscard]] std::size_t file_count() const noexcept { return m_files.size(); }

        [[nodiscard]] std::uint64_t content_revision(FileId id) const noexcept
        {
            auto const* f = get(id);
            return f ? f->content_revision() : 0u;
        }

        [[nodiscard]] std::optional<DiskSignature> disk_signature(FileId id) const noexcept
        {
            auto const* f = get(id);
            return f ? f->disk_signature() : std::nullopt;
        }

        [[nodiscard]] static std::optional<DiskSignature> stat_disk_signature(std::filesystem::path const& path) noexcept;

        [[nodiscard]] bool refresh_disk_file(FileId id);
        [[nodiscard]] bool refresh_disk_file(std::filesystem::path const& path);

        template <typename Fn> void for_each_file(Fn&& fn) const
        {
            for (auto const& f : m_files)
                fn(*f);
        }

        [[nodiscard]] static std::optional<std::filesystem::path> parse_file_uri(std::string_view uri) noexcept;
        [[nodiscard]] static std::string to_file_uri(std::filesystem::path const& path);

    private:
        std::vector<std::unique_ptr<SourceFile>> m_files;
        std::uint64_t m_next_content_revision{1};
        PositionEncoding m_position_encoding{PositionEncoding::Utf16};

        [[nodiscard]] FileId next_id() const noexcept { return static_cast<FileId>(m_files.size()); }

        [[nodiscard]] std::uint64_t next_content_revision() noexcept { return m_next_content_revision++; }
    };

} // namespace dcc::sm

module :private;

namespace dcc::sm
{
    void SourceFile::close_mapping() noexcept
    {
        if (m_is_mmaped && m_mapping && m_mapping != MAP_FAILED && m_size > 0)
            ::munmap(m_mapping, m_size);

        m_mapping = nullptr;
        m_size = 0;
    }

    namespace
    {
        [[nodiscard]] std::optional<DiskSignature> stat_signature(std::filesystem::path const& path) noexcept
        {
            struct ::stat st{};
            if (::stat(path.c_str(), &st) == -1)
                return std::nullopt;

            return DiskSignature{
                .mtime_sec = static_cast<std::int64_t>(st.st_mtim.tv_sec),
                .mtime_nsec = static_cast<std::int64_t>(st.st_mtim.tv_nsec),
                .size = static_cast<std::uint64_t>(st.st_size),
            };
        }

        [[nodiscard]] std::expected<std::pair<void*, std::size_t>, Error> mmap_file(std::filesystem::path const& path) noexcept
        {
            auto const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);

            if (fd == -1)
            {
                switch (errno)
                {
                    case ENOENT:
                        return std::unexpected{Error::FileNotFound};
                    case EACCES:
                        return std::unexpected{Error::PermissionDenied};
                    default:
                        return std::unexpected{Error::MmapFailed};
                }
            }

            struct ::stat st{};
            if (::fstat(fd, &st) == -1)
            {
                ::close(fd);
                return std::unexpected{Error::MmapFailed};
            }

            auto const file_size = static_cast<std::size_t>(st.st_size);

            constexpr std::size_t kMaxSize = std::size_t{1} << 32;
            if (file_size >= kMaxSize)
            {
                ::close(fd);
                return std::unexpected{Error::FileTooLarge};
            }

            if (file_size == 0)
            {
                ::close(fd);
                static char const empty = '\0';
                return std::pair{const_cast<char*>(&empty), std::size_t{}};
            }

            void* mapping = ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE | MAP_FILE, fd, 0);
            ::close(fd);

            if (mapping == MAP_FAILED)
                return std::unexpected{Error::MmapFailed};

            ::madvise(mapping, file_size, MADV_SEQUENTIAL);

            return std::pair{mapping, file_size};
        }

        [[nodiscard]] std::string uri_encode_path(std::filesystem::path const& path)
        {
            std::string s = path.string();
            std::string result;
            result.reserve(s.size() + 16);
            for (char c : s)
            {
                if (c == '%')
                    result += "%25";
                else if (c == ' ')
                    result += "%20";
                else if (c == '#')
                    result += "%23";
                else
                    result += c;
            }
            return result;
        }

        [[nodiscard]] std::string uri_decode(std::string_view s)
        {
            std::string result;
            result.reserve(s.size());
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '%' && i + 2 < s.size())
                {
                    auto hi = s[i + 1];
                    auto lo = s[i + 2];
                    int value = 0;
                    auto decode_hex = [](char c) -> int {
                        if (c >= '0' && c <= '9')
                            return static_cast<int>(c - '0');
                        if (c >= 'A' && c <= 'F')
                            return static_cast<int>(c - 'A') + 10;
                        if (c >= 'a' && c <= 'f')
                            return static_cast<int>(c - 'a') + 10;
                        return -1;
                    };
                    int hi_val = decode_hex(hi);
                    int lo_val = decode_hex(lo);
                    if (hi_val >= 0 && lo_val >= 0)
                    {
                        value = (hi_val << 4) | lo_val;
                        result += static_cast<char>(value);
                        i += 2;
                        continue;
                    }
                }
                result += s[i];
            }
            return result;
        }

    } // anonymous namespace

    std::expected<FileId, Error> SourceManager::load(std::filesystem::path const& path)
    {
        if (auto existing = find_by_path(path))
            return *existing;

        {
            auto wc = std::filesystem::weakly_canonical(path);
            for (auto const& f : m_files)
            {
                if (f && f->kind() != FileKind::Disk)
                {
                    auto fwc = std::filesystem::weakly_canonical(f->path());
                    if (fwc == wc)
                        return f->id();
                }
            }
        }

        std::error_code ec;
        auto canonical = std::filesystem::canonical(path, ec);
        if (ec)
        {
            auto wc = std::filesystem::weakly_canonical(path);
            for (auto const& f : m_files)
            {
                if (f && f->kind() != FileKind::Disk)
                {
                    auto fwc = std::filesystem::weakly_canonical(f->path());
                    if (fwc == wc)
                        return f->id();
                }
            }

            switch (ec.value())
            {
                case ENOENT:
                    return std::unexpected{Error::FileNotFound};
                case EACCES:
                    return std::unexpected{Error::PermissionDenied};
                default:
                    return std::unexpected{Error::MmapFailed};
            }
        }

        for (auto const& f : m_files)
            if (f && f->path() == canonical)
                return f->id();

        auto result = mmap_file(canonical);
        if (!result)
            return std::unexpected{result.error()};

        auto [mapping, size] = *result;

        auto const id = next_id();
        auto sf = std::make_unique<SourceFile>(id, canonical, mapping, size);
        sf->set_uri(to_file_uri(canonical));
        sf->m_content_revision = next_content_revision();
        sf->m_disk_signature = stat_signature(canonical);

        if (size > 0)
            ::madvise(mapping, size, MADV_RANDOM);

        m_files.push_back(std::move(sf));
        return id;
    }

    FileId SourceManager::add_synthetic(std::string name, std::string content)
    {
        auto const id = next_id();
        auto owned = std::make_unique<std::string>(std::move(content));
        auto sf = std::make_unique<SourceFile>(id, std::filesystem::path{std::move(name)}, FileKind::Synthetic, std::string{}, std::nullopt, std::move(owned));
        sf->m_content_revision = next_content_revision();
        m_files.push_back(std::move(sf));
        return id;
    }

    std::optional<DiskSignature> SourceManager::stat_disk_signature(std::filesystem::path const& path) noexcept
    {
        return stat_signature(path);
    }

    bool SourceManager::refresh_disk_file(std::filesystem::path const& path)
    {
        auto existing = find_by_path(path);
        if (!existing)
            return false;

        return refresh_disk_file(*existing);
    }

    bool SourceManager::refresh_disk_file(FileId id)
    {
        auto* f = get_mut(id);
        if (!f || f->kind() != FileKind::Disk)
            return false;

        auto sig = stat_signature(f->path());
        if (!sig)
            return false;

        if (f->m_disk_signature && *f->m_disk_signature == *sig)
            return false;

        auto result = mmap_file(f->path());
        if (!result)
            return false;

        f->close_mapping();
        auto [mapping, size] = *result;
        f->m_mapping = mapping;
        f->m_size = size;
        f->m_is_mmaped = true;
        f->m_disk_signature = *sig;
        f->m_content_revision = next_content_revision();
        f->m_line_start.clear();
        f->m_line_index_built = false;

        if (size > 0)
            ::madvise(mapping, size, MADV_RANDOM);

        return true;
    }

    std::optional<std::filesystem::path> SourceManager::parse_file_uri(std::string_view uri) noexcept
    {
        constexpr std::string_view kFilePrefix = "file://";

        if (!uri.starts_with(kFilePrefix))
            return std::nullopt;

        auto rest = uri.substr(kFilePrefix.size());

        if (rest.empty())
            return std::nullopt;

        if (rest[0] == '/')
        {
            auto decoded = uri_decode(rest);
            return std::filesystem::path{std::move(decoded)};
        }

        return std::nullopt;
    }

    std::string SourceManager::to_file_uri(std::filesystem::path const& path)
    {
        auto encoded = uri_encode_path(path);
        return std::string{"file://"} + encoded;
    }

} // namespace dcc::sm
