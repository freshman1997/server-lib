#include "yuan/rpc/rpc.h"

#include <cstdlib>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    int test_plain_message_roundtrip()
    {
        yuan::rpc::Message message;
        message.kind = yuan::rpc::MessageKind::request;
        message.request_id = 99;
        message.set_continuation_id(777);
        message.route.service = 12;
        message.route.method = 34;
        message.route.name = "game.player.move";
        message.serialization = yuan::rpc::Serialization::protobuf;
        message.compression = yuan::rpc::Compression::zstd;
        message.metadata.emplace("trace", "abc");
        message.metadata.emplace("lang", "cpp");
        message.payload = {1, 2, 3, 4};

        yuan::rpc::Bytes encoded;
        if (!require(yuan::rpc::wire::encode_message(message, encoded), "encode_message should succeed")) {
            return 10;
        }

        const auto decoded = yuan::rpc::wire::decode_frame(encoded);
        if (!require(decoded.ok, "decode_frame should succeed")) {
            return 11;
        }
        const auto restored = yuan::rpc::wire::to_message(decoded.frame);
        if (!require(restored.request_id == 99 && restored.continuation_id() == 777 && restored.route.name == "game.player.move", "message route mismatch")) {
            return 12;
        }
        if (!require(restored.serialization == yuan::rpc::Serialization::protobuf && restored.compression == yuan::rpc::Compression::zstd,
                     "message codec flags mismatch")) {
            return 13;
        }
        if (!require(restored.metadata.at("trace") == "abc" && restored.payload == message.payload, "metadata or payload mismatch")) {
            return 14;
        }
        return 0;
    }

    int test_encrypted_response_roundtrip()
    {
        yuan::rpc::Response response;
        response.request_id = 1234;
        response.coroutine_id = 5678;
        response.status = yuan::rpc::RpcStatus::ok;
        response.serialization = yuan::rpc::Serialization::json;
        response.encryption = yuan::rpc::Encryption::xor_stream;
        response.key_id = 77;
        response.nonce = 0xABCDEFULL;
        response.metadata.emplace("content-type", "application/json");
        response.payload = {'{', '}', '\n'};

        yuan::rpc::security::XorStreamCipher cipher("dev-key");
        yuan::rpc::wire::EncodeOptions encode_options;
        encode_options.encrypt = cipher;
        yuan::rpc::wire::DecodeOptions decode_options;
        decode_options.decrypt = cipher;

        yuan::rpc::Bytes encoded;
        if (!require(yuan::rpc::wire::encode_response(response, encoded, encode_options), "encrypted encode_response should succeed")) {
            return 30;
        }

        const auto decoded_without_key = yuan::rpc::wire::decode_frame(encoded);
        if (!require(!decoded_without_key.ok, "encrypted decode without key should fail")) {
            return 31;
        }

        const auto decoded = yuan::rpc::wire::decode_frame(encoded, decode_options);
        if (!require(decoded.ok, "encrypted decode with key should succeed")) {
            return 32;
        }
        const auto restored = yuan::rpc::wire::to_response(decoded.frame);
        if (!require(restored.request_id == 1234 && restored.coroutine_id == 5678 && restored.status == yuan::rpc::RpcStatus::ok, "response header mismatch")) {
            return 33;
        }
        if (!require(restored.metadata.at("content-type") == "application/json" && restored.payload == response.payload,
                     "encrypted response payload mismatch")) {
            return 34;
        }
        return 0;
    }

    int test_stream_decoder()
    {
        yuan::rpc::Message first;
        first.request_id = 1;
        first.route.name = "a";
        first.payload = {1};

        yuan::rpc::Message second;
        second.request_id = 2;
        second.route.name = "b";
        second.payload = {2};

        yuan::rpc::Bytes a;
        yuan::rpc::Bytes b;
        if (!require(yuan::rpc::wire::encode_message(first, a) && yuan::rpc::wire::encode_message(second, b),
                     "stream frame encoding should succeed")) {
            return 50;
        }

        yuan::rpc::wire::FrameStreamDecoder decoder;
        decoder.append(a.data(), a.size() / 2);
        if (!require(!decoder.next().ok, "partial frame should not decode")) {
            return 51;
        }
        decoder.append(a.data() + a.size() / 2, a.size() - a.size() / 2);
        decoder.append(b);

        const auto decoded_first = decoder.next();
        if (!require(decoded_first.ok && yuan::rpc::wire::to_message(decoded_first.frame).request_id == 1,
                     "first streamed frame mismatch")) {
            return 52;
        }
        const auto decoded_second = decoder.next();
        if (!require(decoded_second.ok && yuan::rpc::wire::to_message(decoded_second.frame).request_id == 2,
                     "second streamed frame mismatch")) {
            return 53;
        }
        return 0;
    }

    int test_decode_errors()
    {
        yuan::rpc::Message message;
        message.request_id = 9;
        message.route.name = "error.check";
        message.payload = {1, 2, 3};

        yuan::rpc::Bytes encoded;
        if (!require(yuan::rpc::wire::encode_message(message, encoded), "error test encode should succeed")) {
            return 70;
        }

        auto partial = encoded;
        partial.resize(yuan::rpc::wire::header_size - 1);
        const auto partial_result = yuan::rpc::wire::decode_frame(partial);
        if (!require(!partial_result.ok && partial_result.error == yuan::rpc::wire::DecodeError::need_more,
                     "short header should need more")) {
            return 71;
        }

        auto bad_magic = encoded;
        bad_magic[0] = 0;
        const auto bad_magic_result = yuan::rpc::wire::decode_frame(bad_magic);
        if (!require(!bad_magic_result.ok && bad_magic_result.error == yuan::rpc::wire::DecodeError::bad_magic,
                     "bad magic should fail")) {
            return 72;
        }

        auto bad_version = encoded;
        bad_version[4] = 99;
        const auto bad_version_result = yuan::rpc::wire::decode_frame(bad_version);
        if (!require(!bad_version_result.ok && bad_version_result.error == yuan::rpc::wire::DecodeError::unsupported_version,
                     "bad version should fail")) {
            return 73;
        }

        yuan::rpc::wire::DecodeOptions limit;
        limit.max_frame_size = 1;
        const auto too_large = yuan::rpc::wire::decode_frame(encoded, limit);
        if (!require(!too_large.ok && too_large.error == yuan::rpc::wire::DecodeError::frame_too_large,
                     "small max frame should reject frame")) {
            return 74;
        }

        return 0;
    }

    int test_typed_rpc_helpers()
    {
        yuan::rpc::Server server;
        yuan::rpc::Route route;
        route.name = "json.echo";
        if (!require(server.register_typed_handler<yuan::rpc::JsonText, yuan::rpc::JsonText>(
                route,
                [](const yuan::rpc::JsonText &request) {
                    return yuan::rpc::JsonText{request.value + "!"};
                }), "typed handler registration should succeed")) {
            return 90;
        }

        yuan::rpc::InProcessChannel channel(server);
        yuan::rpc::Client client(channel);
        bool done = false;
        bool ok = false;
        yuan::rpc::CallOptions call_options;
        call_options.set_continuation_id(9001);
        if (!require(client.call_typed<yuan::rpc::JsonText, yuan::rpc::JsonText>(
                route,
                yuan::rpc::JsonText{"{\"cmd\":1}"},
                [&](yuan::rpc::RpcStatus status, yuan::rpc::JsonText response, std::string error) {
                    done = true;
                    ok = status == yuan::rpc::RpcStatus::ok && error.empty() && response.value == "{\"cmd\":1}!";
                },
                call_options), "typed client call should be sent")) {
            return 91;
        }
        if (!require(done && ok, "typed client call should complete")) {
            return 92;
        }
        return 0;
    }

    int test_rpc_session_loopback()
    {
        auto client_transport = std::make_shared<yuan::rpc::LoopbackTransport>();
        auto server_transport = std::make_shared<yuan::rpc::LoopbackTransport>();
        client_transport->connect(*server_transport);
        server_transport->connect(*client_transport);

        yuan::rpc::Server server;
        yuan::rpc::Route route;
        route.name = "session.echo";
        if (!require(server.register_typed_handler<yuan::rpc::JsonText, yuan::rpc::JsonText>(
                route,
                [](const yuan::rpc::JsonText &request) {
                    return yuan::rpc::JsonText{"echo:" + request.value};
                }), "session typed handler registration should succeed")) {
            return 110;
        }

        yuan::rpc::RpcClientSession client_session(client_transport);
        yuan::rpc::RpcServerSession server_session(server, server_transport);
        bool done = false;
        bool ok = false;
        if (!require(client_session.call_typed<yuan::rpc::JsonText, yuan::rpc::JsonText>(
                route,
                yuan::rpc::JsonText{"hello"},
                [&](yuan::rpc::RpcStatus status, yuan::rpc::JsonText response, std::string error) {
                    done = true;
                    ok = status == yuan::rpc::RpcStatus::ok && error.empty() && response.value == "echo:hello";
                }), "session typed call should be sent")) {
            return 111;
        }
        if (!require(done && ok, "session typed call should complete")) {
            return 112;
        }
        const auto client_rpc_session = client_session.session();
        const auto server_rpc_session = server_session.session();
        if (!require(client_rpc_session && client_rpc_session->pending_size() == 0, "client pending calls should drain")) {
            return 113;
        }
        if (!require(client_rpc_session->stats().frames_sent == 1 && client_rpc_session->stats().frames_received == 1,
                     "client session stats mismatch")) {
            return 114;
        }
        if (!require(server_rpc_session && server_rpc_session->stats().frames_sent == 1 && server_rpc_session->stats().frames_received == 1,
                     "server session stats mismatch")) {
            return 115;
        }
        return 0;
    }

    yuan::rpc::RpcTask<yuan::rpc::JsonText> coroutine_echo(yuan::rpc::CoroutineRpcClient &client, yuan::rpc::Route route)
    {
        yuan::rpc::CallOptions options;
        options.coroutine_id = 4242;
        co_return co_await client.call_async<yuan::rpc::JsonText, yuan::rpc::JsonText>(route, yuan::rpc::JsonText{"co"}, options);
    }

    yuan::rpc::RpcTask<yuan::rpc::JsonText> coroutine_timeout(yuan::rpc::CoroutineRpcClient &client, yuan::rpc::Route route)
    {
        yuan::rpc::CallOptions options;
        options.timeout = std::chrono::milliseconds(1);
        options.set_continuation_id(5151);
        co_return co_await client.call_async<yuan::rpc::JsonText, yuan::rpc::JsonText>(route, yuan::rpc::JsonText{"timeout"}, options);
    }

    int test_coroutine_rpc_client()
    {
        yuan::rpc::Server server;
        yuan::rpc::Route route;
        route.name = "coroutine.echo";
        if (!require(server.register_typed_handler<yuan::rpc::JsonText, yuan::rpc::JsonText>(
                route,
                [](const yuan::rpc::JsonText &request) {
                    return yuan::rpc::JsonText{"resume:" + request.value};
                }), "coroutine handler registration should succeed")) {
            return 130;
        }

        yuan::rpc::InProcessChannel channel(server);
        yuan::rpc::Client raw_client(channel);
        yuan::rpc::RpcCoroutineRegistry registry;
        yuan::rpc::CoroutineRpcClient client(raw_client, registry);
        auto task = coroutine_echo(client, route);
        const auto response = task.result();
        if (!require(response.value == "resume:co", "coroutine rpc should resume with response")) {
            return 131;
        }
        if (!require(registry.size() == 0, "coroutine registry should drain after resume")) {
            return 132;
        }
        return 0;
    }

    int test_coroutine_rpc_timeout()
    {
        auto transport = std::make_shared<yuan::rpc::BlackholeTransport>();
        yuan::rpc::RpcClientSession session(transport);
        yuan::rpc::RpcCoroutineRegistry registry;
        yuan::rpc::CoroutineRpcClient client(session.client(), registry);
        yuan::rpc::Route route;
        route.name = "never.reply";

        auto task = coroutine_timeout(client, route);
        task.resume();
        if (!require(registry.size() == 1, "timeout coroutine should be pending after first resume")) {
            return 150;
        }
        if (!require(session.session() && session.session()->pending_size() == 1, "timeout rpc session should have pending call")) {
            return 151;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        session.poll_timeouts();
        try {
            (void)task.result();
            return 152;
        } catch (const yuan::rpc::RpcError &error) {
            if (!require(error.status() == yuan::rpc::RpcStatus::timeout, "coroutine rpc timeout should throw timeout status")) {
                return 153;
            }
        }
        if (!require(registry.size() == 0, "timeout coroutine registry should drain")) {
            return 154;
        }
        return 0;
    }
}

int main()
{
    if (const int rc = test_plain_message_roundtrip(); rc != 0) {
        return rc;
    }
    if (const int rc = test_encrypted_response_roundtrip(); rc != 0) {
        return rc;
    }
    if (const int rc = test_stream_decoder(); rc != 0) {
        return rc;
    }
    if (const int rc = test_decode_errors(); rc != 0) {
        return rc;
    }
    if (const int rc = test_typed_rpc_helpers(); rc != 0) {
        return rc;
    }
    if (const int rc = test_rpc_session_loopback(); rc != 0) {
        return rc;
    }
    if (const int rc = test_coroutine_rpc_client(); rc != 0) {
        return rc;
    }
    if (const int rc = test_coroutine_rpc_timeout(); rc != 0) {
        return rc;
    }
    return EXIT_SUCCESS;
}
