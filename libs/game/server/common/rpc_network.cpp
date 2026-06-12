#include "common/rpc_network.h"

#include "coroutine/sync_wait.h"
#include "net/async/async_request_client.h"
#include "net/socket/socket.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace yuan::game::server::rpc_network
{
    namespace
    {
        yuan::buffer::ByteBuffer to_buffer(const yuan::rpc::Bytes &bytes)
        {
            yuan::buffer::ByteBuffer buffer(bytes.size());
            if (!bytes.empty()) {
                buffer.append(bytes.data(), bytes.size());
            }
            return buffer;
        }

        yuan::rpc::Bytes to_bytes(const yuan::buffer::ByteBuffer &buffer)
        {
            const auto span = buffer.readable_span();
            const auto *data = reinterpret_cast<const std::uint8_t *>(span.data());
            return yuan::rpc::Bytes(data, data + span.size());
        }
    }

    RpcNetworkServer::RpcNetworkServer() = default;

    RpcNetworkServer::~RpcNetworkServer()
    {
        stop();
    }

    bool RpcNetworkServer::start(const RpcNetworkServerConfig &config, yuan::rpc::Server &server)
    {
        server_ = &server;
        expected_requests_ = config.stop_after_requests == 0 ? 1 : config.stop_after_requests;
        stop_after_expected_requests_ = config.stop_after_requests != 0;
        port_ = config.port;
        session_.set_read_callback([this](yuan::net::ConnectionContext &context) {
            if (!server_) {
                context.close();
                return;
            }

            auto request_bytes = to_bytes(context.take_input_byte_buffer());
            auto connection_context = context;
            {
                std::lock_guard<std::mutex> lock(workers_mutex_);
                ++active_workers_;
            }
            std::thread([this, connection_context, request_bytes = std::move(request_bytes)]() mutable {
                auto finish_worker = [this] {
                    std::lock_guard<std::mutex> lock(workers_mutex_);
                    if (active_workers_ > 0) {
                        --active_workers_;
                    }
                    workers_idle_.notify_all();
                };

                auto decoded = yuan::rpc::wire::decode_frame(request_bytes);
                if (!decoded.ok) {
                    session_.dispatch([this, connection_context, finish_worker]() mutable {
                        connection_context.close();
                        finish_worker();
                    });
                    return;
                }

                auto response = server_->handle(yuan::rpc::wire::to_message(std::move(decoded.frame)));
                yuan::rpc::Bytes response_frame;
                const bool encoded = yuan::rpc::wire::encode_response(response, response_frame);
                session_.dispatch([this, connection_context, encoded, response_frame = std::move(response_frame), finish_worker]() mutable {
                    if (encoded) {
                        connection_context.write_and_flush(to_buffer(response_frame));
                    }
                    connection_context.close();
                    if (stop_after_expected_requests_ && ++handled_ >= expected_requests_) {
                        runtime_.stop();
                    }
                    finish_worker();
                });
            }).detach();
        });

        ok_ = session_.bind(config.host, port_, runtime_);
        return ok_;
    }

    bool RpcNetworkServer::bind_loopback(std::uint16_t port, yuan::rpc::Server &server, std::size_t expected_requests)
    {
        RpcNetworkServerConfig config;
        config.port = port;
        config.stop_after_requests = std::max<std::size_t>(expected_requests, 1);
        return start(config, server);
    }

    bool RpcNetworkServer::run()
    {
        if (!ok_) {
            return false;
        }
        (void)runtime_.run();
        return true;
    }

    void RpcNetworkServer::stop()
    {
        session_.close();
        runtime_.stop();
        std::unique_lock<std::mutex> lock(workers_mutex_);
        workers_idle_.wait(lock, [this] {
            return active_workers_ == 0;
        });
    }

    bool RpcNetworkServer::ok() const
    {
        return ok_;
    }

    std::uint16_t RpcNetworkServer::port() const
    {
        return port_;
    }

    RpcNetworkClient::RpcNetworkClient(RpcNetworkClientConfig config)
        : config_(config)
    {
    }

    std::optional<yuan::rpc::Response> RpcNetworkClient::call(const RpcEndpoint &endpoint, const yuan::rpc::Message &message) const
    {
        yuan::rpc::Bytes request_frame;
        if (!yuan::rpc::wire::encode_message(message, request_frame)) {
            return std::nullopt;
        }

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();
        auto task = [&](yuan::coroutine::RuntimeView view) -> yuan::coroutine::Task<std::optional<yuan::rpc::Response>> {
            co_await view.schedule();
            yuan::net::AsyncRequestClient client(view);
            const auto read_result = co_await client.request_async(endpoint.host,
                                                                   endpoint.port,
                                                                   to_buffer(request_frame),
                                                                   static_cast<std::uint32_t>(config_.connect_timeout.count()),
                                                                   static_cast<std::uint32_t>(config_.read_timeout.count()));
            if (read_result.status != yuan::coroutine::IoStatus::success) {
                co_return std::nullopt;
            }
            auto decoded = yuan::rpc::wire::decode_frame(to_bytes(read_result.data));
            if (!decoded.ok) {
                co_return std::nullopt;
            }
            co_return yuan::rpc::wire::to_response(std::move(decoded.frame));
        };

        return yuan::coroutine::sync_wait(rv, task(rv));
    }

    std::optional<yuan::rpc::Response> call(std::uint16_t port, const yuan::rpc::Message &message, std::chrono::milliseconds timeout)
    {
        RpcNetworkClientConfig config;
        config.connect_timeout = timeout;
        config.read_timeout = timeout;
        return RpcNetworkClient(config).call(RpcEndpoint{"127.0.0.1", port}, message);
    }

    std::uint16_t reserve_loopback_port()
    {
        yuan::net::Socket socket("127.0.0.1", 0);
        if (!socket.valid() || !socket.set_reuse_addr(true) || !socket.bind()) {
            return 0;
        }
        const auto port = static_cast<std::uint16_t>(socket.get_local_address().get_port());
        socket.close();
        return port;
    }
}
