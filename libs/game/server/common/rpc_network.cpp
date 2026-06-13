#include "common/rpc_network.h"

#include "base/time.h"
#include "coroutine/sync_wait.h"
#include "net/async/async_request_client.h"
#include "net/socket/socket.h"
#include "yuan/rpc/frame_stream.h"

#include <algorithm>
#include <thread>
#include <utility>
#include <vector>

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
        max_connections_ = config.max_connections;
        max_buffered_bytes_ = config.max_buffered_bytes == 0 ? 1024 * 1024 : config.max_buffered_bytes;
        idle_timeout_ms_ = config.idle_timeout_ms;
        port_ = config.port;
        session_.set_connected_callback([this](yuan::net::ConnectionContext &context) {
            const auto connection_id = static_cast<std::uint64_t>(context.connection_id());
            std::lock_guard<std::mutex> lock(connections_mutex_);
            if (max_connections_ != 0 && connections_.size() >= max_connections_) {
                context.close();
                return;
            }
            connections_[connection_id] = ConnectionState{context, yuan::base::time::steady_now_ms()};
        });
        session_.set_read_callback([this](yuan::net::ConnectionContext &context) {
            if (!server_) {
                context.close();
                return;
            }

            auto request_bytes = to_bytes(context.take_input_byte_buffer());
            auto connection_context = context;
            const auto connection_id = static_cast<std::uint64_t>(context.connection_id());
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                connections_[connection_id] = ConnectionState{context, yuan::base::time::steady_now_ms()};
            }
            auto &decoder = decoders_[connection_id];
            decoder.append(request_bytes);
            if (decoder.buffered_size() > max_buffered_bytes_) {
                decoders_.erase(connection_id);
                context.close();
                return;
            }

            for (;;) {
                auto decoded = decoder.next();
                if (!decoded.ok) {
                    if (decoded.error != yuan::rpc::wire::DecodeError::need_more) {
                        decoders_.erase(connection_id);
                        context.close();
                    }
                    break;
                }

                {
                    std::lock_guard<std::mutex> lock(workers_mutex_);
                    ++active_workers_;
                }

                std::thread([this, connection_context, connection_id, frame = std::move(decoded.frame)]() mutable {
                auto finish_worker = [this] {
                    std::lock_guard<std::mutex> lock(workers_mutex_);
                    if (active_workers_ > 0) {
                        --active_workers_;
                    }
                    workers_idle_.notify_all();
                };

                auto message = yuan::rpc::wire::to_message(std::move(frame));
                message.metadata[metadata_key::connection_id] = std::to_string(connection_id);
                auto response = server_->handle(std::move(message));
                const bool close_after_response = response.metadata.erase(metadata_key::close_connection) != 0;
                yuan::rpc::Bytes response_frame;
                const bool encoded = yuan::rpc::wire::encode_response(response, response_frame);
                session_.dispatch([this, connection_context, encoded, close_after_response, response_frame = std::move(response_frame), finish_worker]() mutable {
                    if (encoded) {
                        connection_context.write_and_flush(to_buffer(response_frame));
                    }
                    if (!encoded || close_after_response) {
                        connection_context.close();
                    }
                    if (stop_after_expected_requests_ && ++handled_ >= expected_requests_) {
                        runtime_.stop();
                    }
                    finish_worker();
                });
                }).detach();
            }
        });
        session_.set_close_callback([this](yuan::net::ConnectionContext &context) {
            const auto connection_id = static_cast<std::uint64_t>(context.connection_id());
            decoders_.erase(connection_id);
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                connections_.erase(connection_id);
            }
            if (connection_closed_callback_) {
                connection_closed_callback_(connection_id);
            }
        });

        ok_ = session_.bind(config.host, port_, runtime_);
        if (ok_ && idle_timeout_ms_ != 0) {
            idle_monitor_thread_ = std::jthread([this](std::stop_token stop_token) {
                idle_monitor_loop(stop_token);
            });
        }
        return ok_;
    }

    void RpcNetworkServer::idle_monitor_loop(std::stop_token stop_token)
    {
        const auto sleep_for = std::chrono::milliseconds(std::max<std::uint64_t>(100, std::min<std::uint64_t>(idle_timeout_ms_, 1000)));
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(sleep_for);
            if (stop_token.stop_requested()) {
                break;
            }
            const auto now = yuan::base::time::steady_now_ms();
            std::vector<yuan::net::ConnectionContext> expired;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                for (const auto &[connection_id, state] : connections_) {
                    if (state.last_activity_ms + idle_timeout_ms_ <= now) {
                        expired.push_back(state.context);
                    }
                }
            }
            for (auto &connection : expired) {
                connection.close();
            }
        }
    }

    void RpcNetworkServer::set_connection_closed_callback(std::function<void(std::uint64_t)> callback)
    {
        connection_closed_callback_ = std::move(callback);
    }

    bool RpcNetworkServer::write_message_to_connection(std::uint64_t connection_id, const yuan::rpc::Message &message)
    {
        yuan::rpc::Bytes frame;
        if (!yuan::rpc::wire::encode_message(message, frame)) {
            return false;
        }

        yuan::net::ConnectionContext connection;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            const auto it = connections_.find(connection_id);
            if (it == connections_.end()) {
                return false;
            }
            connection = it->second.context;
        }

        session_.dispatch([connection, frame = std::move(frame)]() mutable {
            if (connection.is_connected()) {
                connection.write_and_flush(to_buffer(frame));
            }
        });
        return true;
    }

    void RpcNetworkServer::close_all_connections()
    {
        std::vector<yuan::net::ConnectionContext> connections;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            for (const auto &[connection_id, state] : connections_) {
                connections.push_back(state.context);
            }
        }
        for (auto &connection : connections) {
            connection.close();
        }
    }

    std::size_t RpcNetworkServer::active_connection_count() const
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        return connections_.size();
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
        idle_monitor_thread_.request_stop();
        if (idle_monitor_thread_.joinable()) {
            idle_monitor_thread_.join();
        }
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
