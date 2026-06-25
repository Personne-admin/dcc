import std;
import dccd.protocol;
import dccd.server;

namespace
{
    void send_message(dccd::protocol::JsonValue const& msg)
    {
        std::string payload = msg.serialize();
        std::print("Content-Length: {}\r\n\r\n{}", payload.size(), payload);
        std::cout.flush();
    }

    [[nodiscard]] std::int64_t read_content_length()
    {
        std::string line;
        std::int64_t content_length = -1;

        while (true)
        {
            int ch = std::cin.get();
            if (ch == std::char_traits<char>::eof())
            {
                if (line.empty() && content_length == -1)
                    return -1;

                std::println(std::cerr, "[dccd] unexpected EOF while reading headers");
                return -1;
            }

            char c = static_cast<char>(ch);

            if (c == '\r')
            {
                int next = std::cin.peek();
                if (next == '\n')
                {
                    std::cin.get();

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
                            std::println(std::cerr, "[dccd] malformed Content-Length header: {}", val_str);
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

    [[nodiscard]] std::optional<std::string> read_payload(std::int64_t length)
    {
        if (length > 100 * 1024 * 1024)
        {
            std::println(std::cerr, "[dccd] payload too large: {} bytes", length);
            return std::nullopt;
        }

        std::string payload;
        payload.resize(static_cast<std::size_t>(length));

        std::cin.read(payload.data(), length);
        std::streamsize bytes_read = std::cin.gcount();

        if (bytes_read != length)
        {
            std::println(std::cerr, "[dccd] short read: expected {} bytes, got {}", length, bytes_read);
            return std::nullopt;
        }

        return payload;
    }

} // anonymous namespace

auto main() -> int
{
    std::ios::sync_with_stdio(false);

    dccd::LanguageServer server;

    std::println(std::cerr, "[dccd] dcc language server started");

    while (!server.should_exit())
    {
        auto content_length = read_content_length();
        if (content_length < 0)
        {
            if (std::cin.eof())
                break;

            continue;
        }

        if (content_length == 0)
            continue;

        auto payload = read_payload(content_length);
        if (!payload)
        {
            std::println(std::cerr, "[dccd] failed to read payload, aborting connection");
            break;
        }

        auto json_val = dccd::protocol::JsonValue::parse(*payload);
        if (!json_val)
        {
            std::println(std::cerr, "[dccd] failed to parse JSON-RPC message");
            std::println(std::cerr, "[dccd] payload: {}", *payload);
            continue;
        }

        auto rpc = dccd::protocol::parse_rpc(*json_val);
        if (!rpc)
        {
            std::println(std::cerr, "[dccd] invalid JSON-RPC message");
            if (auto const* id_val = json_val->find_member("id"))
            {
                auto err_resp = dccd::protocol::build_error_response(*id_val, -32700, "Parse error");
                send_message(err_resp);
            }
            continue;
        }

        std::optional<dccd::protocol::JsonValue> response;
        try
        {
            response = server.handle_message(*rpc);
        }
        catch (std::exception const& ex)
        {
            std::println(std::cerr, "[dccd] uncaught exception handling message: {}", ex.what());
            if (rpc->id.has_value())
            {
                auto err_resp = dccd::protocol::build_error_response(rpc->id.value(), -32603, std::format("Internal error: {}", ex.what()));
                send_message(err_resp);
            }
            continue;
        }
        catch (...)
        {
            std::println(std::cerr, "[dccd] uncaught non-standard exception handling message");
            if (rpc->id.has_value())
            {
                auto err_resp = dccd::protocol::build_error_response(rpc->id.value(), -32603, "Internal error");
                send_message(err_resp);
            }
            continue;
        }

        if (response)
            send_message(*response);
    }

    std::println(std::cerr, "[dccd] server shutting down");
    return 0;
}
