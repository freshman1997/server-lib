#include "common/rpc_network.h"

#include "base/time.h"
#include "coroutine/sync_wait.h"
#include "net/async/async_request_client.h"
#include "net/socket/socket.h"
#include "yuan/rpc/frame_stream.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yuan::game::server::rpc_network
{
    namespace
    {
        bool wait_for_stop(std::stop_token stop_token, std::chrono::milliseconds delay)
        {
            std::mutex mutex;
            std::condition_variable_any cv;
            std::unique_lock<std::mutex> lock(mutex);
            (void)cv.wait_for(lock, stop_token, delay, [] { return false; });
            return stop_token.stop_requested();
        }

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

                // Game RPC handlers are intentionally executed on the network runtime
                // thread. Do not add per-request threads or an implicit worker pool here:
                // upper game state is designed around a single-threaded handler model.
                // Blocking/heavy work must be explicitly offloaded by the owning service.
                auto message = yuan::rpc::wire::to_message(std::move(decoded.frame));
                message.metadata[metadata_key::connection_id] = std::to_string(connection_id);
                auto response = server_->handle(std::move(message));
                if (response.metadata.erase(metadata_key::defer_response) != 0) {
                    continue;
                }
                const bool close_after_response = response.metadata.erase(metadata_key::close_connection) != 0;
                yuan::rpc::Bytes response_frame;
                const bool encoded = yuan::rpc::wire::encode_response(response, response_frame);
                if (encoded) {
                    connection_context.write_and_flush(to_buffer(response_frame));
                }
                if (!encoded || close_after_response) {
                    connection_context.close();
                }
                if (stop_after_expected_requests_ && ++handled_ >= expected_requests_) {
                    runtime_.stop();
                }
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
            if (wait_for_stop(stop_token, sleep_for)) {
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

    bool RpcNetworkServer::write_response_to_connection(std::uint64_t connection_id, yuan::rpc::Response response)
    {
        const bool close_after_response = response.metadata.erase(metadata_key::close_connection) != 0;
        yuan::rpc::Bytes frame;
        if (!yuan::rpc::wire::encode_response(response, frame)) {
            if (close_after_response) {
                (void)close_connection(connection_id);
            }
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

        session_.dispatch([this, connection, frame = std::move(frame), close_after_response]() mutable {
            if (connection.is_connected()) {
                connection.write_and_flush(to_buffer(frame));
                if (close_after_response) {
                    connection.close();
                }
            }
        });
        return true;
    }

    bool RpcNetworkServer::close_connection(std::uint64_t connection_id)
    {
        yuan::net::ConnectionContext connection;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            const auto it = connections_.find(connection_id);
            if (it == connections_.end()) {
                return false;
            }
            connection = it->second.context;
        }

        session_.dispatch([connection]() mutable {
            connection.close();
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

    RpcNetworkPersistentClient::RpcNetworkPersistentClient(RpcNetworkClientConfig config)
        : config_(config),
          client_(runtime_.runtime_view())
    {
    }

    RpcNetworkPersistentClient::~RpcNetworkPersistentClient()
    {
        close();
    }

    void RpcNetworkPersistentClient::close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client_.disconnect();
        connected_endpoint_.reset();
    }

    std::optional<yuan::rpc::Response> RpcNetworkPersistentClient::call(const RpcEndpoint &endpoint, const yuan::rpc::Message &message)
    {
        yuan::rpc::Bytes request_frame;
        if (!yuan::rpc::wire::encode_message(message, request_frame)) {
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto rv = runtime_.runtime_view();
        if (!client_.is_connected() || !connected_endpoint_ || connected_endpoint_->host != endpoint.host || connected_endpoint_->port != endpoint.port) {
            client_.disconnect();
            const bool connected = yuan::coroutine::sync_wait(rv, client_.connect_async(endpoint.host, endpoint.port, static_cast<std::uint32_t>(config_.connect_timeout.count())));
            if (!connected) {
                connected_endpoint_.reset();
                return std::nullopt;
            }
            connected_endpoint_ = endpoint;
        }
        auto read_result = yuan::coroutine::sync_wait(rv, client_.request_async(to_buffer(request_frame), static_cast<std::uint32_t>(config_.read_timeout.count())));
        if (read_result.status != yuan::coroutine::IoStatus::success) {
            client_.disconnect();
            connected_endpoint_.reset();
            return std::nullopt;
        }
        auto decoded = yuan::rpc::wire::decode_frame(to_bytes(read_result.data));
        if (!decoded.ok) {
            client_.disconnect();
            connected_endpoint_.reset();
            return std::nullopt;
        }
        return yuan::rpc::wire::to_response(std::move(decoded.frame));
    }

    RpcNetworkPersistentAsyncClient::RpcNetworkPersistentAsyncClient(RpcNetworkClientConfig config, std::size_t max_pending)
        : config_(config),
          max_pending_(max_pending == 0 ? 1024 : max_pending),
          client_(runtime_.runtime_view()),
          worker_([this](std::stop_token stop_token) { worker_loop(stop_token); })
    {
    }

    RpcNetworkPersistentAsyncClient::~RpcNetworkPersistentAsyncClient()
    {
        close();
    }

    std::future<std::optional<yuan::rpc::Response>> RpcNetworkPersistentAsyncClient::call_async(const RpcEndpoint &endpoint, yuan::rpc::Message message)
    {
        auto promise = std::make_shared<std::promise<std::optional<yuan::rpc::Response>>>();
        auto future = promise->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= max_pending_) {
                promise->set_value(std::nullopt);
                return future;
            }
            queue_.push_back(QueuedCall{endpoint, std::move(message), promise});
        }
        cv_.notify_one();
        return future;
    }

    std::optional<yuan::rpc::Response> RpcNetworkPersistentAsyncClient::call(const RpcEndpoint &endpoint, yuan::rpc::Message message)
    {
        return call_async(endpoint, std::move(message)).get();
    }

    void RpcNetworkPersistentAsyncClient::close()
    {
        worker_.request_stop();
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        client_.disconnect();
        connected_endpoint_.reset();
    }

    std::size_t RpcNetworkPersistentAsyncClient::queued_size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t RpcNetworkPersistentAsyncClient::pending_size() const
    {
        return pending_count_.load(std::memory_order_relaxed);
    }

    void RpcNetworkPersistentAsyncClient::fail_all(std::deque<QueuedCall> &calls)
    {
        for (auto &call : calls) {
            call.promise->set_value(std::nullopt);
        }
        calls.clear();
    }

    bool RpcNetworkPersistentAsyncClient::ensure_connected(const RpcEndpoint &endpoint)
    {
        auto rv = runtime_.runtime_view();
        if (client_.is_connected() && connected_endpoint_ && connected_endpoint_->host == endpoint.host && connected_endpoint_->port == endpoint.port) {
            return true;
        }
        client_.disconnect();
        connected_endpoint_.reset();
        decoder_.clear();
        const bool connected = yuan::coroutine::sync_wait(rv, client_.connect_async(endpoint.host, endpoint.port, static_cast<std::uint32_t>(config_.connect_timeout.count())));
        if (!connected) {
            return false;
        }
        connected_endpoint_ = endpoint;
        return true;
    }

    void RpcNetworkPersistentAsyncClient::worker_loop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            std::deque<QueuedCall> calls;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, stop_token, [this] { return !queue_.empty(); });
                if (stop_token.stop_requested()) {
                    break;
                }
                calls.swap(queue_);
            }

            if (calls.empty()) {
                continue;
            }

            const auto endpoint = calls.front().endpoint;
            bool same_endpoint = true;
            for (const auto &call : calls) {
                if (call.endpoint.host != endpoint.host || call.endpoint.port != endpoint.port) {
                    same_endpoint = false;
                    break;
                }
            }
            if (!same_endpoint || !ensure_connected(endpoint)) {
                fail_all(calls);
                continue;
            }

            std::unordered_map<yuan::rpc::RequestId, std::shared_ptr<std::promise<std::optional<yuan::rpc::Response>>>> pending;
            bool write_failed = false;
            auto rv = runtime_.runtime_view();
            for (auto &call : calls) {
                if (write_failed) {
                    call.promise->set_value(std::nullopt);
                    continue;
                }
                call.message.kind = yuan::rpc::MessageKind::request;
                if (call.message.request_id == 0) {
                    call.message.request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
                    if (call.message.request_id == 0) {
                        call.message.request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                yuan::rpc::Bytes frame;
                if (!yuan::rpc::wire::encode_message(call.message, frame)) {
                    call.promise->set_value(std::nullopt);
                    continue;
                }
                const auto write_result = yuan::coroutine::sync_wait(rv, client_.session().write_async(to_buffer(frame), static_cast<std::uint32_t>(config_.connect_timeout.count())));
                if (write_result.status != yuan::coroutine::IoStatus::success) {
                    call.promise->set_value(std::nullopt);
                    write_failed = true;
                    continue;
                }
                pending[call.message.request_id] = call.promise;
                pending_count_.fetch_add(1, std::memory_order_relaxed);
            }

            if (write_failed) {
                for (const auto &[request_id, promise] : pending) {
                    promise->set_value(std::nullopt);
                }
                pending_count_.fetch_sub(pending.size(), std::memory_order_relaxed);
                client_.disconnect();
                connected_endpoint_.reset();
                decoder_.clear();
                continue;
            }

            while (!pending.empty() && !stop_token.stop_requested()) {
                const auto read_result = yuan::coroutine::sync_wait(rv, client_.session().read_async(static_cast<std::uint32_t>(config_.read_timeout.count())));
                if (read_result.status != yuan::coroutine::IoStatus::success) {
                    for (const auto &[request_id, promise] : pending) {
                        promise->set_value(std::nullopt);
                    }
                    pending_count_.fetch_sub(pending.size(), std::memory_order_relaxed);
                    pending.clear();
                    client_.disconnect();
                    connected_endpoint_.reset();
                    decoder_.clear();
                    break;
                }
                const auto bytes = to_bytes(read_result.data);
                decoder_.append(bytes);
                for (;;) {
                    auto decoded = decoder_.next();
                    if (!decoded.ok) {
                        if (decoded.error != yuan::rpc::wire::DecodeError::need_more) {
                            for (const auto &[request_id, promise] : pending) {
                                promise->set_value(std::nullopt);
                            }
                            pending_count_.fetch_sub(pending.size(), std::memory_order_relaxed);
                            pending.clear();
                            client_.disconnect();
                            connected_endpoint_.reset();
                            decoder_.clear();
                        }
                        break;
                    }
                    auto response = yuan::rpc::wire::to_response(std::move(decoded.frame));
                    const auto it = pending.find(response.request_id);
                    if (it != pending.end()) {
                        it->second->set_value(std::move(response));
                        pending.erase(it);
                        pending_count_.fetch_sub(1, std::memory_order_relaxed);
                    }
                }
            }
            if (!pending.empty()) {
                for (const auto &[request_id, promise] : pending) {
                    promise->set_value(std::nullopt);
                }
                pending_count_.fetch_sub(pending.size(), std::memory_order_relaxed);
            }
        }

        std::deque<QueuedCall> remaining;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remaining.swap(queue_);
        }
        fail_all(remaining);
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
