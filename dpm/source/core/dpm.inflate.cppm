export module dpm.inflate;

import std;

export namespace dpm::inflate
{
    struct Limits
    {
        std::uint64_t max_output = 1ull << 30;
        std::uint64_t max_ratio = 0;
        std::uint64_t ratio_floor = 1u << 20;
    };

    struct Result
    {
        bool ok{};
        bool aborted{};
        std::string error;
        std::uint64_t output{};
    };

    using Sink = std::function<bool(std::string_view)>;

    namespace detail
    {
        struct Failure
        {
            std::string message;
        };

        struct Aborted
        {
        };

        constexpr int k_max_bits = 15;
        constexpr int k_max_lit_codes = 286;
        constexpr int k_max_dist_codes = 30;
        constexpr int k_fix_lit_codes = 288;
        constexpr std::size_t k_window = 32768;
        constexpr std::size_t k_chunk = 65536;

        struct Huffman
        {
            std::array<std::uint16_t, k_max_bits + 1> count{};
            std::array<std::uint16_t, 320> symbol{};
        };

        inline std::array<std::uint32_t, 256> make_crc_table()
        {
            std::array<std::uint32_t, 256> table{};
            for (std::uint32_t n = 0; n < 256; ++n)
            {
                std::uint32_t c = n;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
                table[n] = c;
            }
            return table;
        }

        inline std::uint32_t crc_update(std::uint32_t crc, std::string_view data)
        {
            static auto const table = make_crc_table();
            crc = ~crc;
            for (char ch : data)
                crc = table[(crc ^ static_cast<unsigned char>(ch)) & 0xff] ^ (crc >> 8);
            return ~crc;
        }

        inline int construct(Huffman& h, std::uint16_t const* lengths, int n)
        {
            h.count.fill(0);
            for (int i = 0; i < n; ++i)
                ++h.count[lengths[i]];
            if (h.count[0] == n)
                return 0;
            int left = 1;
            for (int len = 1; len <= k_max_bits; ++len)
            {
                left <<= 1;
                left -= h.count[static_cast<std::size_t>(len)];
                if (left < 0)
                    return left;
            }
            std::array<std::uint16_t, k_max_bits + 1> offs{};
            for (int len = 1; len < k_max_bits; ++len)
                offs[static_cast<std::size_t>(len) + 1] = static_cast<std::uint16_t>(offs[static_cast<std::size_t>(len)] + h.count[static_cast<std::size_t>(len)]);
            for (int symbol = 0; symbol < n; ++symbol)
                if (lengths[symbol] != 0)
                    h.symbol[offs[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
            return left;
        }

        class Inflater
        {
        public:
            Inflater(std::string_view input, Limits const& limits, Sink const& sink) : in_{input}, limits_{limits}, sink_{sink} { chunk_.reserve(k_chunk); }

            Result run()
            {
                Result result;
                try
                {
                    header();
                    stream();
                    trailer();
                    flush();
                    check_ratio();
                    result.ok = true;
                }
                catch (Failure const& f)
                {
                    result.error = f.message;
                }
                catch (Aborted const&)
                {
                    result.aborted = true;
                }
                result.output = total_;
                return result;
            }

        private:
            [[noreturn]] static void fail(std::string message) { throw Failure{std::move(message)}; }

            std::uint8_t byte()
            {
                if (pos_ >= in_.size())
                    fail("truncated gzip stream");
                return static_cast<std::uint8_t>(in_[pos_++]);
            }

            std::uint32_t le(int bytes)
            {
                std::uint32_t v = 0;
                for (int i = 0; i < bytes; ++i)
                    v |= static_cast<std::uint32_t>(byte()) << (8 * i);
                return v;
            }

            void header()
            {
                std::size_t start = pos_;
                if (byte() != 0x1f || byte() != 0x8b)
                    fail("not a gzip stream");
                if (byte() != 8)
                    fail("unsupported gzip compression method");
                auto flags = byte();
                if (flags & 0xe0)
                    fail("reserved gzip flags are set");
                for (int i = 0; i < 6; ++i)
                    byte();
                if (flags & 4)
                {
                    auto xlen = le(2);
                    if (pos_ + xlen > in_.size())
                        fail("truncated gzip stream");
                    pos_ += xlen;
                }
                for (auto flag : {8, 16})
                    if (flags & flag)
                        while (byte() != 0)
                        {
                        }
                if (flags & 2)
                {
                    auto expected = static_cast<std::uint16_t>(crc_update(0, in_.substr(start, pos_ - start)) & 0xffff);
                    if (le(2) != expected)
                        fail("gzip header checksum mismatch");
                }
            }

            std::uint32_t bits(int need)
            {
                std::uint32_t val = bitbuf_;
                while (bitcnt_ < need)
                {
                    val |= static_cast<std::uint32_t>(byte()) << bitcnt_;
                    bitcnt_ += 8;
                }
                bitbuf_ = val >> need;
                bitcnt_ -= need;
                return val & ((1u << need) - 1);
            }

            void put(std::uint8_t b)
            {
                if (total_ >= limits_.max_output)
                    fail("expanded size exceeds the limit");
                window_[total_ & (k_window - 1)] = b;
                chunk_.push_back(static_cast<char>(b));
                ++total_;
                if (chunk_.size() >= k_chunk)
                {
                    flush();
                    check_ratio();
                }
            }

            void flush()
            {
                if (chunk_.empty())
                    return;
                crc_ = crc_update(crc_, chunk_);
                bool keep = sink_ ? sink_(chunk_) : true;
                chunk_.clear();
                if (!keep)
                    throw Aborted{};
            }

            void check_ratio()
            {
                if (limits_.max_ratio == 0 || total_ <= limits_.ratio_floor)
                    return;
                if (total_ > limits_.max_ratio * std::max<std::uint64_t>(pos_, 1))
                    fail("compression ratio exceeds the limit");
            }

            int decode(Huffman const& h)
            {
                int code = 0;
                int first = 0;
                int index = 0;
                for (int len = 1; len <= k_max_bits; ++len)
                {
                    code |= static_cast<int>(bits(1));
                    int count = h.count[static_cast<std::size_t>(len)];
                    if (code - count < first)
                        return h.symbol[static_cast<std::size_t>(index + (code - first))];
                    index += count;
                    first += count;
                    first <<= 1;
                    code <<= 1;
                }
                fail("invalid Huffman code");
            }

            void stored()
            {
                bitbuf_ = 0;
                bitcnt_ = 0;
                auto len = le(2);
                auto nlen = le(2);
                if (len != (~nlen & 0xffff))
                    fail("stored block length check failed");
                if (pos_ + len > in_.size())
                    fail("truncated gzip stream");
                for (std::uint32_t i = 0; i < len; ++i)
                    put(static_cast<std::uint8_t>(in_[pos_++]));
            }

            void codes(Huffman const& lencode, Huffman const& distcode)
            {
                static constexpr std::array<std::uint16_t, 29> lens = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
                static constexpr std::array<std::uint16_t, 29> lext = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
                static constexpr std::array<std::uint16_t, 30> dists = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
                static constexpr std::array<std::uint16_t, 30> dext = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
                for (;;)
                {
                    int symbol = decode(lencode);
                    if (symbol < 256)
                    {
                        put(static_cast<std::uint8_t>(symbol));
                        continue;
                    }
                    if (symbol == 256)
                        return;
                    symbol -= 257;
                    if (symbol >= 29)
                        fail("invalid length code");
                    auto len = static_cast<std::uint32_t>(lens[static_cast<std::size_t>(symbol)] + bits(lext[static_cast<std::size_t>(symbol)]));
                    int dsym = decode(distcode);
                    if (dsym >= k_max_dist_codes)
                        fail("invalid distance code");
                    auto dist = static_cast<std::uint64_t>(dists[static_cast<std::size_t>(dsym)] + bits(dext[static_cast<std::size_t>(dsym)]));
                    if (dist > total_)
                        fail("distance reaches before the start of the output");
                    for (std::uint32_t i = 0; i < len; ++i)
                        put(window_[(total_ - dist) & (k_window - 1)]);
                }
            }

            void fixed()
            {
                static Huffman const lencode = []
                {
                    Huffman h;
                    std::array<std::uint16_t, k_fix_lit_codes> lengths{};
                    int symbol = 0;
                    for (; symbol < 144; ++symbol)
                        lengths[static_cast<std::size_t>(symbol)] = 8;
                    for (; symbol < 256; ++symbol)
                        lengths[static_cast<std::size_t>(symbol)] = 9;
                    for (; symbol < 280; ++symbol)
                        lengths[static_cast<std::size_t>(symbol)] = 7;
                    for (; symbol < k_fix_lit_codes; ++symbol)
                        lengths[static_cast<std::size_t>(symbol)] = 8;
                    construct(h, lengths.data(), k_fix_lit_codes);
                    return h;
                }();
                static Huffman const distcode = []
                {
                    Huffman h;
                    std::array<std::uint16_t, k_max_dist_codes> lengths{};
                    lengths.fill(5);
                    construct(h, lengths.data(), k_max_dist_codes);
                    return h;
                }();
                codes(lencode, distcode);
            }

            void dynamic()
            {
                static constexpr std::array<std::uint8_t, 19> order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                int nlen = static_cast<int>(bits(5)) + 257;
                int ndist = static_cast<int>(bits(5)) + 1;
                int ncode = static_cast<int>(bits(4)) + 4;
                if (nlen > k_max_lit_codes || ndist > k_max_dist_codes)
                    fail("too many length or distance codes");
                std::array<std::uint16_t, 320> lengths{};
                for (int i = 0; i < ncode; ++i)
                    lengths[order[static_cast<std::size_t>(i)]] = static_cast<std::uint16_t>(bits(3));
                Huffman lencode;
                if (construct(lencode, lengths.data(), 19) != 0)
                    fail("invalid code length code");
                lengths.fill(0);
                int index = 0;
                while (index < nlen + ndist)
                {
                    int symbol = decode(lencode);
                    if (symbol < 16)
                    {
                        lengths[static_cast<std::size_t>(index++)] = static_cast<std::uint16_t>(symbol);
                        continue;
                    }
                    std::uint16_t len = 0;
                    int repeat;
                    if (symbol == 16)
                    {
                        if (index == 0)
                            fail("repeat with no previous length");
                        len = lengths[static_cast<std::size_t>(index) - 1];
                        repeat = 3 + static_cast<int>(bits(2));
                    }
                    else if (symbol == 17)
                        repeat = 3 + static_cast<int>(bits(3));
                    else
                        repeat = 11 + static_cast<int>(bits(7));
                    if (index + repeat > nlen + ndist)
                        fail("too many code lengths");
                    while (repeat--)
                        lengths[static_cast<std::size_t>(index++)] = len;
                }
                if (lengths[256] == 0)
                    fail("missing end-of-block code");
                Huffman litcode;
                int err = construct(litcode, lengths.data(), nlen);
                if (err < 0 || (err > 0 && nlen - litcode.count[0] != 1))
                    fail("invalid literal/length code");
                Huffman distcode;
                err = construct(distcode, lengths.data() + nlen, ndist);
                if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
                    fail("invalid distance code");
                codes(litcode, distcode);
            }

            void stream()
            {
                int last;
                do
                {
                    last = static_cast<int>(bits(1));
                    switch (bits(2))
                    {
                        case 0:
                            stored();
                            break;
                        case 1:
                            fixed();
                            break;
                        case 2:
                            dynamic();
                            break;
                        default:
                            fail("invalid block type");
                    }
                } while (!last);
                bitbuf_ = 0;
                bitcnt_ = 0;
            }

            void trailer()
            {
                flush();
                auto crc = le(4);
                auto isize = le(4);
                if (crc != crc_)
                    fail("gzip checksum mismatch");
                if (isize != static_cast<std::uint32_t>(total_ & 0xffffffffu))
                    fail("gzip size mismatch");
                if (pos_ != in_.size())
                {
                    if (in_.size() - pos_ >= 2 && static_cast<std::uint8_t>(in_[pos_]) == 0x1f && static_cast<std::uint8_t>(in_[pos_ + 1]) == 0x8b)
                        fail("concatenated gzip members are not supported");
                    fail("trailing data after the gzip stream");
                }
            }

            std::string_view in_;
            Limits limits_;
            Sink const& sink_;
            std::size_t pos_{};
            std::uint32_t bitbuf_{};
            int bitcnt_{};
            std::array<std::uint8_t, k_window> window_{};
            std::uint64_t total_{};
            std::string chunk_;
            std::uint32_t crc_{};
        };
    } // namespace detail

    [[nodiscard]] inline Result gunzip(std::string_view input, Limits const& limits, Sink const& sink)
    {
        return detail::Inflater{input, limits, sink}.run();
    }
} // namespace dpm::inflate
