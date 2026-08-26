import std;
import dccd.protocol;
import dccd.transport;

#include "harness.hh"

namespace
{
    using dccd::protocol::JsonValue;

    [[nodiscard]] std::string framed(std::string_view body)
    {
        return std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
    }

    [[nodiscard]] std::string request_body(std::int64_t id, std::string_view method)
    {
        auto req = dccd::protocol::build_request(JsonValue::integer(id), std::string{method}, JsonValue::empty_object());
        return req.serialize();
    }

    [[nodiscard]] std::string notification_body(std::string_view method, JsonValue params)
    {
        auto notif = dccd::protocol::build_notification(std::string{method}, std::move(params));
        return notif.serialize();
    }

    [[nodiscard]] std::string cancel_body(dccd::protocol::RequestId const& id)
    {
        auto params = JsonValue::empty_object();
        params.set("id", id.to_json());
        return notification_body(dccd::protocol::kCancelRequestMethod, std::move(params));
    }

} // namespace

SECTION("transport: request registration and cancellation");

TEST_CASE("reader registers a request id before enqueue and applies a later cancel before dispatch")
{
    dccd::transport::CancellationRegistry registry;
    auto id = dccd::protocol::RequestId::from_json(JsonValue::integer(1));

    std::istringstream stream{framed(request_body(1, "textDocument/hover")) + framed(cancel_body(id))};

    dccd::transport::Transport transport{stream, registry};
    transport.start();
    transport.join();

    CHECK(registry.is_pending(id));
    CHECK(registry.is_cancelled(id));
    CHECK(transport.done());

    auto item = transport.pop();
    REQUIRE(item.has_value());
    CHECK(item->kind == dccd::transport::QueueItemKind::Message);
    REQUIRE(item->rpc.has_value());
    CHECK(item->rpc->is_request());
    REQUIRE(item->rpc->id.has_value());
    CHECK(item->rpc->id->is_number());
    CHECK_EQ(item->rpc->id->as_integer(), 1);

    CHECK(!transport.pop().has_value());
}

TEST_CASE("cancel before the target request is read is a no-op")
{
    dccd::transport::CancellationRegistry registry;
    auto id = dccd::protocol::RequestId::from_json(JsonValue::integer(42));

    std::istringstream stream{framed(cancel_body(id)) + framed(request_body(42, "textDocument/hover"))};

    dccd::transport::Transport transport{stream, registry};
    transport.start();
    transport.join();

    CHECK(registry.is_pending(id));
    CHECK(!registry.is_cancelled(id));
    CHECK(transport.done());

    auto item = transport.pop();
    REQUIRE(item.has_value());
    CHECK(item->kind == dccd::transport::QueueItemKind::Message);
    REQUIRE(item->rpc.has_value());
    CHECK(item->rpc->is_request());

    CHECK(!transport.pop().has_value());
}

TEST_CASE("numeric and string request ids stay type-distinct in the transport")
{
    dccd::transport::CancellationRegistry registry;
    auto numeric = dccd::protocol::RequestId::from_json(JsonValue::integer(7));
    auto string_id = dccd::protocol::RequestId::from_json(JsonValue::string_val("7"));

    std::istringstream stream{framed(request_body(7, "textDocument/hover")) + framed(cancel_body(string_id)) + framed(cancel_body(numeric))};

    dccd::transport::Transport transport{stream, registry};
    transport.start();
    transport.join();

    CHECK(registry.is_pending(numeric));
    CHECK(registry.is_cancelled(numeric));
    CHECK(!registry.is_pending(string_id));

    auto item = transport.pop();
    REQUIRE(item.has_value());
    REQUIRE(item->rpc.has_value());
    CHECK(item->rpc->is_request());
    CHECK(!transport.pop().has_value());
}

SECTION("transport: ordering and stream termination");

TEST_CASE("ordinary messages are queued in stream order")
{
    dccd::transport::CancellationRegistry registry;
    std::istringstream stream{framed(notification_body("textDocument/didOpen", JsonValue::empty_object())) + framed(request_body(1, "textDocument/hover")) +
                              framed(notification_body("initialized", JsonValue::empty_object())) + framed(request_body(2, "textDocument/definition"))};

    dccd::transport::Transport transport{stream, registry};
    transport.start();
    transport.join();

    std::vector<std::string> methods;
    while (auto item = transport.pop())
    {
        REQUIRE(item->rpc.has_value());
        REQUIRE(item->rpc->method.has_value());
        methods.push_back(*item->rpc->method);
    }

    REQUIRE(methods.size() == 4);
    CHECK_EQ(methods[0], "textDocument/didOpen");
    CHECK_EQ(methods[1], "textDocument/hover");
    CHECK_EQ(methods[2], "initialized");
    CHECK_EQ(methods[3], "textDocument/definition");
    CHECK(transport.done());
}

TEST_CASE("EOF after messages ends the stream cleanly")
{
    dccd::transport::CancellationRegistry registry;
    std::istringstream stream{framed(request_body(1, "textDocument/hover")) + framed(request_body(2, "textDocument/definition"))};

    dccd::transport::Transport transport{stream, registry};
    transport.start();
    transport.join();

    auto first = transport.pop();
    REQUIRE(first.has_value());
    CHECK_EQ(first->rpc->id->as_integer(), 1);
    auto second = transport.pop();
    REQUIRE(second.has_value());
    CHECK_EQ(second->rpc->id->as_integer(), 2);
    CHECK(!transport.pop().has_value());
    CHECK(transport.done());
}

TEST_CASE("the exit notification is enqueued and the reader terminates after it")
{
    dccd::transport::CancellationRegistry registry;
    std::istringstream stream{framed(request_body(1, "textDocument/hover")) + framed(notification_body("exit", JsonValue::empty_object()))};

    dccd::transport::Transport transport{stream, registry};
    transport.start();
    transport.join();

    auto first = transport.pop();
    REQUIRE(first.has_value());
    CHECK(first->rpc->is_request());

    auto exit_item = transport.pop();
    REQUIRE(exit_item.has_value());
    REQUIRE(exit_item->rpc.has_value());
    CHECK(exit_item->rpc->is_notification());
    REQUIRE(exit_item->rpc->method.has_value());
    CHECK_EQ(*exit_item->rpc->method, "exit");

    CHECK(!transport.pop().has_value());
    CHECK(transport.done());
}

SECTION("transport: protocol error handling");

TEST_CASE("a client JSON-RPC response is delivered as a Message for the server to ignore")
{
    dccd::transport::CancellationRegistry registry;

    auto res = dccd::protocol::build_response(dccd::protocol::JsonValue::string_val("dccd-register-capability"), dccd::protocol::JsonValue::null_val());
    std::istringstream stream{framed(res.serialize())};

    dccd::transport::Transport transport{stream, registry};
    transport.start();
    transport.join();

    auto item = transport.pop();
    REQUIRE(item.has_value());
    CHECK(item->kind == dccd::transport::QueueItemKind::Message);
    REQUIRE(item->rpc.has_value());
    REQUIRE(item->rpc->is_response());
    REQUIRE(!item->rpc->is_request());
    REQUIRE(!item->rpc->is_notification());
    REQUIRE(item->rpc->id.has_value());
    REQUIRE(item->rpc->id->is_string());
    CHECK_EQ(item->rpc->id->as_string(), "dccd-register-capability");

    CHECK(!registry.is_pending(dccd::protocol::RequestId::from_json(dccd::protocol::JsonValue::string_val("dccd-register-capability"))));

    CHECK(!transport.pop().has_value());
    CHECK(transport.done());
}

TEST_CASE("an invalid JSON-RPC message with an id enqueues a Parse error response item")
{
    dccd::transport::CancellationRegistry registry;
    std::istringstream stream{framed(R"({"jsonrpc":"1.0","id":5,"method":"x"})")};

    dccd::transport::Transport transport{stream, registry};
    transport.start();
    transport.join();

    auto item = transport.pop();
    REQUIRE(item.has_value());
    CHECK(item->kind == dccd::transport::QueueItemKind::ErrorResponse);
    REQUIRE(item->response.has_value());

    auto const* err = item->response->get_object("error");
    REQUIRE(err != nullptr);
    auto code = err->get_integer("code");
    REQUIRE(code.has_value());
    CHECK_EQ(*code, -32700);
    auto message = err->get_string("message");
    REQUIRE(message.has_value());
    CHECK_EQ(*message, "Parse error");

    auto const* resp_id = item->response->find_member("id");
    REQUIRE(resp_id != nullptr);
    REQUIRE(resp_id->is_number());
    CHECK_EQ(resp_id->as_integer(), 5);

    CHECK(!transport.pop().has_value());
    CHECK(transport.done());
}

TEST_CASE("unparseable JSON is logged and skipped without a queue item")
{
    dccd::transport::CancellationRegistry registry;
    std::ostringstream log;
    std::istringstream stream{framed("{ this is not json") + framed(request_body(1, "textDocument/hover"))};

    dccd::transport::Transport transport{stream, registry, &log};
    transport.start();
    transport.join();

    auto item = transport.pop();
    REQUIRE(item.has_value());
    CHECK(item->kind == dccd::transport::QueueItemKind::Message);
    REQUIRE(item->rpc.has_value());
    CHECK(item->rpc->is_request());
    CHECK(!transport.pop().has_value());
    CHECK(transport.done());
    CHECK(log.str().find("failed to parse JSON-RPC message") != std::string::npos);
}

TEST_CASE("a malformed Content-Length header is logged and the frame is skipped")
{
    dccd::transport::CancellationRegistry registry;
    std::ostringstream log;
    std::istringstream stream{"Content-Length: abc\r\n\r\n" + framed(request_body(1, "textDocument/hover"))};

    dccd::transport::Transport transport{stream, registry, &log};
    transport.start();
    transport.join();

    auto item = transport.pop();
    REQUIRE(item.has_value());
    REQUIRE(item->rpc.has_value());
    CHECK(item->rpc->is_request());
    CHECK(!transport.pop().has_value());
    CHECK(transport.done());
    CHECK(log.str().find("malformed Content-Length header") != std::string::npos);
}

TEST_CASE("an over-cap payload aborts the connection")
{
    dccd::transport::CancellationRegistry registry;
    std::ostringstream log;
    std::string header = std::format("Content-Length: {}\r\n\r\n", (100 * 1024 * 1024) + 1);
    std::istringstream stream{header};

    dccd::transport::Transport transport{stream, registry, &log};
    transport.start();
    transport.join();

    CHECK(transport.done());
    CHECK(!transport.pop().has_value());
    CHECK(log.str().find("payload too large") != std::string::npos);
}

TEST_CASE("a short read aborts the connection")
{
    dccd::transport::CancellationRegistry registry;
    std::ostringstream log;
    std::istringstream stream{"Content-Length: 10\r\n\r\nabc"};

    dccd::transport::Transport transport{stream, registry, &log};
    transport.start();
    transport.join();

    CHECK(transport.done());
    CHECK(!transport.pop().has_value());
    CHECK(log.str().find("short read") != std::string::npos);
}
