export module dccd.transport;

import std;
import dccd.protocol;

export namespace dccd::transport
{
    class CancellationRegistry
    {
    public:
        [[nodiscard]] bool register_pending(protocol::RequestId const& id)
        {
            std::lock_guard lock{m_mutex};
            return m_pending.emplace(id, false).second;
        }

        [[nodiscard]] bool cancel(protocol::RequestId const& id)
        {
            std::lock_guard lock{m_mutex};
            auto it = m_pending.find(id);
            if (it == m_pending.end())
                return false;

            it->second = true;
            return true;
        }

        [[nodiscard]] bool is_pending(protocol::RequestId const& id) const
        {
            std::lock_guard lock{m_mutex};
            return m_pending.contains(id);
        }

        [[nodiscard]] bool is_cancelled(protocol::RequestId const& id) const
        {
            std::lock_guard lock{m_mutex};
            auto it = m_pending.find(id);
            return it != m_pending.end() && it->second;
        }

        void finish(protocol::RequestId const& id) noexcept
        {
            std::lock_guard lock{m_mutex};
            m_pending.erase(id);
        }

    private:
        mutable std::mutex m_mutex;
        std::map<protocol::RequestId, bool> m_pending;
    };

    enum class QueueItemKind : std::uint8_t
    {
        Message,
        ErrorResponse,
    };

    struct QueueItem
    {
        QueueItemKind kind{QueueItemKind::Message};
        std::optional<protocol::RpcInfo> rpc;
        std::optional<protocol::JsonValue> response;
    };

    class MessageQueue
    {
    public:
        MessageQueue() = default;
        MessageQueue(MessageQueue const&) = delete;
        MessageQueue& operator=(MessageQueue const&) = delete;

        void enqueue(QueueItem item)
        {
            {
                std::lock_guard lock{m_mutex};
                m_items.push_back(std::move(item));
            }

            m_cv.notify_one();
        }

        void finish()
        {
            {
                std::lock_guard lock{m_mutex};
                m_done = true;
            }

            m_cv.notify_all();
        }

        [[nodiscard]] bool done() const
        {
            std::lock_guard lock{m_mutex};
            return m_done;
        }

        [[nodiscard]] std::optional<QueueItem> pop()
        {
            std::unique_lock lock{m_mutex};
            m_cv.wait(lock, [this] { return m_done || !m_items.empty(); });
            if (m_items.empty())
                return std::nullopt;

            auto item = std::move(m_items.front());
            m_items.pop_front();
            return item;
        }

        [[nodiscard]] std::optional<QueueItem> try_pop()
        {
            std::lock_guard lock{m_mutex};
            if (m_items.empty())
                return std::nullopt;

            auto item = std::move(m_items.front());
            m_items.pop_front();
            return item;
        }

    private:
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::deque<QueueItem> m_items;
        bool m_done{false};
    };

    class Transport
    {
    public:
        explicit Transport(std::istream& input, CancellationRegistry& registry, std::ostream* log = nullptr)
            : m_input{input}, m_registry{registry}, m_log{log ? *log : std::cerr}
        {
        }

        Transport(Transport const&) = delete;
        Transport& operator=(Transport const&) = delete;

        ~Transport() { join(); }

        void start()
        {
            std::lock_guard lock{m_thread_mutex};
            if (m_thread_running || m_thread_joined)
                return;

            m_thread = std::thread{&Transport::reader_loop, this};
            m_thread_running = true;
        }

        void join()
        {
            std::thread t;
            {
                std::lock_guard lock{m_thread_mutex};
                if (!m_thread_running || m_thread_joined)
                    return;

                t = std::move(m_thread);
                m_thread_running = false;
                m_thread_joined = true;
            }
            t.join();
        }

        [[nodiscard]] bool done() const { return m_queue.done(); }

        [[nodiscard]] std::optional<QueueItem> pop() { return m_queue.pop(); }
        [[nodiscard]] std::optional<QueueItem> try_pop() { return m_queue.try_pop(); }

        [[nodiscard]] CancellationRegistry& registry() noexcept { return m_registry; }
        [[nodiscard]] CancellationRegistry const& registry() const noexcept { return m_registry; }

    private:
        void reader_loop();
        void handle_cancel_request(protocol::RpcInfo const& rpc);

        std::istream& m_input;
        CancellationRegistry& m_registry;
        std::ostream& m_log;
        MessageQueue m_queue;
        std::mutex m_thread_mutex;
        std::thread m_thread;
        bool m_thread_running{false};
        bool m_thread_joined{false};
    };

} // namespace dccd::transport

module :private;

namespace dccd::transport
{
    namespace
    {
        constexpr std::int64_t kMaxPayloadBytes = 100 * 1024 * 1024;

        [[nodiscard]] std::int64_t read_content_length(std::istream& in, std::ostream& log)
        {
            std::string line;
            std::int64_t content_length = -1;

            while (true)
            {
                int ch = in.get();
                if (ch == std::char_traits<char>::eof())
                {
                    if (line.empty() && content_length == -1)
                        return -1;

                    std::println(log, "[dccd] unexpected EOF while reading headers");
                    return -1;
                }

                char c = static_cast<char>(ch);

                if (c == '\r')
                {
                    int next = in.peek();
                    if (next == '\n')
                    {
                        in.get();

                        if (line.empty())
                            return content_length;

                        if (line.starts_with("Content-Length:"))
                        {
                            auto val_str = line.substr(15);
                            while (!val_str.empty() && val_str.front() == ' ')
                                val_str = val_str.substr(1);

                            auto [ptr, ec] = std::from_chars(val_str.data(), val_str.data() + val_str.size(), content_length);
                            if (ec != std::errc{} || content_length < 0)
                            {
                                std::println(log, "[dccd] malformed Content-Length header: {}", val_str);
                                return -1;
                            }
                        }

                        line.clear();
                        continue;
                    }
                }

                line += c;
            }
        }

        [[nodiscard]] std::optional<std::string> read_payload(std::istream& in, std::ostream& log, std::int64_t length)
        {
            if (length > kMaxPayloadBytes)
            {
                std::println(log, "[dccd] payload too large: {} bytes", length);
                return std::nullopt;
            }

            std::string payload;
            payload.resize(static_cast<std::size_t>(length));

            in.read(payload.data(), length);
            std::streamsize bytes_read = in.gcount();

            if (bytes_read != length)
            {
                std::println(log, "[dccd] short read: expected {} bytes, got {}", length, bytes_read);
                return std::nullopt;
            }

            return payload;
        }

    } // namespace

    void Transport::handle_cancel_request(protocol::RpcInfo const& rpc)
    {
        if (!rpc.params.has_value())
        {
            std::println(m_log, "[dccd] $/cancelRequest: missing params");
            return;
        }

        auto params = protocol::CancelParams::from_json(rpc.params.value());
        if (!params.id.valid())
        {
            std::println(m_log, "[dccd] $/cancelRequest: id must be a number or a string");
            return;
        }

        bool cancelled = m_registry.cancel(params.id);
        std::println(m_log, "[dccd] $/cancelRequest: id={} {}", params.id.to_json().serialize(), cancelled ? "cancelled" : "no-op (not pending)");
    }

    void Transport::reader_loop()
    {
        while (true)
        {
            auto content_length = read_content_length(m_input, m_log);
            if (content_length < 0)
            {
                if (m_input.eof())
                    break;

                continue;
            }

            if (content_length == 0)
                continue;

            auto payload = read_payload(m_input, m_log, content_length);
            if (!payload)
            {
                std::println(m_log, "[dccd] failed to read payload, aborting connection");
                break;
            }

            auto json_val = protocol::JsonValue::parse(*payload);
            if (!json_val)
            {
                std::println(m_log, "[dccd] failed to parse JSON-RPC message");
                std::println(m_log, "[dccd] payload: {}", *payload);
                continue;
            }

            auto rpc = protocol::parse_rpc(*json_val);
            if (!rpc)
            {
                std::println(m_log, "[dccd] invalid JSON-RPC message");
                if (auto const* id_val = json_val->find_member("id"))
                {
                    QueueItem item;
                    item.kind = QueueItemKind::ErrorResponse;
                    item.response = protocol::build_error_response(*id_val, -32700, "Parse error");
                    m_queue.enqueue(std::move(item));
                }
                continue;
            }

            if (rpc->is_request())
            {
                auto request_id = protocol::RequestId::from_json(rpc->id.value());
                if (request_id.valid())
                    std::ignore = m_registry.register_pending(request_id);
            }
            else if (rpc->is_notification() && rpc->method.has_value() && *rpc->method == protocol::kCancelRequestMethod)
            {
                handle_cancel_request(*rpc);
                continue;
            }

            bool is_exit = rpc->is_notification() && rpc->method.has_value() && *rpc->method == "exit";

            QueueItem item;
            item.kind = QueueItemKind::Message;
            item.rpc = std::move(rpc);
            m_queue.enqueue(std::move(item));

            if (is_exit)
                break;
        }

        m_queue.finish();
    }

} // namespace dccd::transport
