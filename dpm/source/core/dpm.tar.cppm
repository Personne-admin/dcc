export module dpm.tar;

import std;
import dpm.extract;

export namespace dpm::tar
{
    class Reader
    {
    public:
        struct Handlers
        {
            std::function<bool(extract::Member const&)> on_member;
            std::function<bool(std::string_view)> on_data;
            std::function<bool()> on_member_end;
        };

        explicit Reader(Handlers handlers, std::uint64_t max_stream = std::numeric_limits<std::uint64_t>::max())
            : handlers_{std::move(handlers)}, max_stream_{max_stream}
        {
        }

        [[nodiscard]] bool feed(std::string_view chunk)
        {
            consumed_total_ += chunk.size();
            if (consumed_total_ > max_stream_)
                return fail("decompressed archive is larger than the limit");
            while (!chunk.empty() && !failed_ && !finished_)
            {
                if (state_ == State::Header)
                {
                    auto take = std::min<std::size_t>(512 - header_.size(), chunk.size());
                    header_.append(chunk.substr(0, take));
                    chunk.remove_prefix(take);
                    if (header_.size() == 512)
                    {
                        if (!handle_header())
                            return false;
                        header_.clear();
                    }
                }
                else if (state_ == State::Data)
                {
                    auto take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, chunk.size()));
                    if (!consume_data(chunk.substr(0, take)))
                        return false;
                    remaining_ -= take;
                    chunk.remove_prefix(take);
                    if (remaining_ == 0)
                    {
                        state_ = padding_ > 0 ? State::Padding : State::Header;
                        if (state_ == State::Header && !end_data())
                            return false;
                    }
                }
                else
                {
                    auto take = static_cast<std::size_t>(std::min<std::uint64_t>(padding_, chunk.size()));
                    chunk.remove_prefix(take);
                    padding_ -= take;
                    if (padding_ == 0)
                    {
                        state_ = State::Header;
                        if (!end_data())
                            return false;
                    }
                }
            }
            return !failed_;
        }

        [[nodiscard]] bool finish()
        {
            if (failed_)
                return false;
            if (!finished_ && !(state_ == State::Header && header_.empty() && seen_any_))
                return fail("archive is truncated");
            return true;
        }

        [[nodiscard]] std::string const& error() const { return error_; }
        [[nodiscard]] bool stopped() const { return stopped_; }

    private:
        enum class State : std::uint8_t
        {
            Header,
            Data,
            Padding,
        };

        enum class Kind : std::uint8_t
        {
            Skip,
            Payload,
            LongName,
            LongLink,
            PaxLocal,
            PaxGlobal,
        };

        bool fail(std::string message)
        {
            if (!failed_)
            {
                failed_ = true;
                error_ = std::move(message);
            }
            return false;
        }

        static std::optional<std::uint64_t> parse_number(std::string_view field)
        {
            if (!field.empty() && (static_cast<unsigned char>(field[0]) & 0x80))
            {
                if ((static_cast<unsigned char>(field[0]) & 0x40) != 0)
                    return std::nullopt;
                std::uint64_t value = static_cast<unsigned char>(field[0]) & 0x3f;
                for (std::size_t i = 1; i < field.size(); ++i)
                {
                    if (value > (std::numeric_limits<std::uint64_t>::max() >> 8))
                        return std::nullopt;
                    value = (value << 8) | static_cast<unsigned char>(field[i]);
                }
                return value;
            }
            std::uint64_t value = 0;
            bool any = false;
            for (char c : field)
            {
                if (c == ' ' || c == '\0')
                {
                    if (any)
                        break;
                    continue;
                }
                if (c < '0' || c > '7')
                    return std::nullopt;
                if (value > (std::numeric_limits<std::uint64_t>::max() >> 3))
                    return std::nullopt;
                value = (value << 3) | static_cast<std::uint64_t>(c - '0');
                any = true;
            }
            return any ? std::optional<std::uint64_t>{value} : std::optional<std::uint64_t>{0};
        }

        static std::string cstr(std::string_view field)
        {
            auto end = field.find('\0');
            return std::string{field.substr(0, end)};
        }

        bool handle_header()
        {
            seen_any_ = true;
            if (std::ranges::all_of(header_, [](char c) { return c == '\0'; }))
            {
                if (++zero_blocks_ >= 2)
                    finished_ = true;
                return true;
            }
            zero_blocks_ = 0;
            std::uint32_t sum = 0;
            for (std::size_t i = 0; i < 512; ++i)
                sum += (i >= 148 && i < 156) ? 32u : static_cast<unsigned char>(header_[i]);
            auto stored = parse_number(std::string_view{header_}.substr(148, 8));
            if (!stored || *stored != sum)
                return fail("bad tar header checksum");
            auto size = parse_number(std::string_view{header_}.substr(124, 12));
            if (!size)
                return fail("bad size field in tar header");
            char type = header_[156];
            auto mode = parse_number(std::string_view{header_}.substr(100, 8)).value_or(0);

            std::string name = cstr(std::string_view{header_}.substr(0, 100));
            bool ustar = std::string_view{header_}.substr(257, 5) == "ustar";
            if (ustar)
                if (auto prefix = cstr(std::string_view{header_}.substr(345, 155)); !prefix.empty())
                    name = prefix + "/" + name;
            std::string target = cstr(std::string_view{header_}.substr(157, 100));

            if (type == 'x' || type == 'g' || type == 'L' || type == 'K')
            {
                if (*size > (1u << 20))
                    return fail("tar metadata record is too large");
                kind_ = type == 'x' ? Kind::PaxLocal : type == 'g' ? Kind::PaxGlobal : type == 'L' ? Kind::LongName : Kind::LongLink;
                meta_.clear();
                begin_data(*size);
                return true;
            }

            if (!pending_path_.empty())
                name = std::exchange(pending_path_, {});
            if (!pending_link_.empty())
                target = std::exchange(pending_link_, {});
            std::uint64_t member_size = *size;
            if (pax_size_)
                member_size = *std::exchange(pax_size_, std::nullopt);

            extract::Member m;
            m.path = normalize(name);
            m.target = target;
            m.exec = (mode & 0100) != 0;
            switch (type)
            {
                case '0':
                case '\0':
                    m.type = extract::MemberType::File;
                    m.size = member_size;
                    break;
                case '5':
                    m.type = extract::MemberType::Directory;
                    break;
                case '1':
                    m.type = extract::MemberType::Hardlink;
                    break;
                case '2':
                    m.type = extract::MemberType::Symlink;
                    break;
                case '3':
                    m.type = extract::MemberType::CharDevice;
                    break;
                case '4':
                    m.type = extract::MemberType::BlockDevice;
                    break;
                case '6':
                    m.type = extract::MemberType::Fifo;
                    break;
                default:
                    m.type = extract::MemberType::Other;
                    break;
            }
            if (handlers_.on_member && !handlers_.on_member(m))
            {
                stopped_ = true;
                failed_ = true;
                error_ = "stopped";
                return false;
            }
            kind_ = m.type == extract::MemberType::File ? Kind::Payload : Kind::Skip;
            auto data_size = (type == '0' || type == '\0' || type == '7') ? member_size : *size;
            if (m.type == extract::MemberType::Directory || m.type == extract::MemberType::Symlink || m.type == extract::MemberType::Hardlink)
                data_size = 0;
            begin_data(data_size);
            return true;
        }

        static std::string normalize(std::string name)
        {
            while (name.starts_with("./"))
                name.erase(0, 2);
            while (name.size() > 1 && name.ends_with('/'))
                name.pop_back();
            return name;
        }

        void begin_data(std::uint64_t size)
        {
            remaining_ = size;
            padding_ = (512 - size % 512) % 512;
            if (size > 0)
                state_ = State::Data;
            else if (padding_ > 0)
                state_ = State::Padding;
            else
                state_ = State::Header;
            if (size == 0)
                (void)end_data();
        }

        bool consume_data(std::string_view data)
        {
            switch (kind_)
            {
                case Kind::Payload:
                    if (handlers_.on_data && !handlers_.on_data(data))
                    {
                        stopped_ = true;
                        return fail("stopped");
                    }
                    return true;
                case Kind::Skip:
                    return true;
                default:
                    meta_.append(data);
                    return true;
            }
        }

        bool end_data()
        {
            switch (kind_)
            {
                case Kind::LongName:
                    pending_path_ = cstr(meta_);
                    break;
                case Kind::LongLink:
                    pending_link_ = cstr(meta_);
                    break;
                case Kind::PaxLocal:
                    if (!parse_pax(true))
                        return false;
                    break;
                case Kind::PaxGlobal:
                    if (!parse_pax(false))
                        return false;
                    break;
                case Kind::Payload:
                    if (handlers_.on_member_end && !handlers_.on_member_end())
                    {
                        stopped_ = true;
                        return fail("stopped");
                    }
                    break;
                case Kind::Skip:
                    break;
            }
            kind_ = Kind::Skip;
            return true;
        }

        bool parse_pax(bool local)
        {
            std::size_t pos = 0;
            while (pos < meta_.size())
            {
                auto space = meta_.find(' ', pos);
                if (space == std::string::npos)
                    return fail("bad pax record");
                std::uint64_t length = 0;
                for (std::size_t i = pos; i < space; ++i)
                {
                    if (meta_[i] < '0' || meta_[i] > '9' || length > 100000)
                        return fail("bad pax record");
                    length = length * 10 + static_cast<std::uint64_t>(meta_[i] - '0');
                }
                if (length == 0 || pos + length > meta_.size() || space + 1 >= pos + length)
                    return fail("bad pax record");
                auto record = std::string_view{meta_}.substr(space + 1, pos + length - space - 2);
                auto eq = record.find('=');
                if (eq == std::string_view::npos)
                    return fail("bad pax record");
                auto key = record.substr(0, eq);
                auto value = std::string{record.substr(eq + 1)};
                if (local)
                {
                    if (key == "path")
                        pending_path_ = value;
                    else if (key == "linkpath")
                        pending_link_ = value;
                    else if (key == "size")
                    {
                        std::uint64_t n = 0;
                        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
                        if (ec != std::errc{} || ptr != value.data() + value.size())
                            return fail("bad pax size");
                        pax_size_ = n;
                    }
                }
                else if (key == "path" || key == "linkpath" || key == "size")
                    return fail("global pax header overrides member fields");
                pos += length;
            }
            return true;
        }

        Handlers handlers_;
        std::uint64_t max_stream_;
        std::uint64_t consumed_total_{};
        State state_{State::Header};
        Kind kind_{Kind::Skip};
        std::string header_;
        std::string meta_;
        std::string pending_path_;
        std::string pending_link_;
        std::optional<std::uint64_t> pax_size_;
        std::uint64_t remaining_{};
        std::uint64_t padding_{};
        int zero_blocks_{};
        bool seen_any_{};
        bool finished_{};
        bool failed_{};
        bool stopped_{};
        std::string error_;
    };
} // namespace dpm::tar
