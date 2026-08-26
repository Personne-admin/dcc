import std;
import dccd.protocol;
import dccd.server;
import dccd.transport;

namespace
{
    void send_message(dccd::protocol::JsonValue const& msg)
    {
        std::string payload = msg.serialize();
        std::print("Content-Length: {}\r\n\r\n{}", payload.size(), payload);
        std::cout.flush();
    }

} // anonymous namespace

auto main() -> int
{
    std::ios::sync_with_stdio(false);

    auto registry = std::make_shared<dccd::transport::CancellationRegistry>();

    dccd::LanguageServer server{nullptr, registry};
    dccd::transport::Transport transport{std::cin, *registry};

    std::println(std::cerr, "[dccd] dcc language server started");

    transport.start();

    while (auto item = transport.pop())
    {
        if (item->kind == dccd::transport::QueueItemKind::ErrorResponse)
        {
            send_message(*item->response);
            continue;
        }

        std::optional<dccd::protocol::JsonValue> response;
        try
        {
            response = server.handle_message(*item->rpc);
        }
        catch (std::exception const& ex)
        {
            std::println(std::cerr, "[dccd] uncaught exception handling message: {}", ex.what());
            if (item->rpc->id.has_value())
            {
                auto err_resp = dccd::protocol::build_error_response(item->rpc->id.value(), -32603, std::format("Internal error: {}", ex.what()));
                send_message(err_resp);
            }
            continue;
        }
        catch (...)
        {
            std::println(std::cerr, "[dccd] uncaught non-standard exception handling message");
            if (item->rpc->id.has_value())
            {
                auto err_resp = dccd::protocol::build_error_response(item->rpc->id.value(), -32603, "Internal error");
                send_message(err_resp);
            }
            continue;
        }

        if (response)
            send_message(*response);
    }

    transport.join();

    std::println(std::cerr, "[dccd] server shutting down");
    return 0;
}
