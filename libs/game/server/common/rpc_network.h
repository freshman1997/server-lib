#ifndef YUAN_GAME_SERVER_COMMON_RPC_NETWORK_H
#define YUAN_GAME_SERVER_COMMON_RPC_NETWORK_H

#include "yuan/rpc/rpc.h"
#include "yuan/rpc/frame_stream.h"

#include "common/metadata_keys.h"

#include "buffer/byte_buffer.h"
#include "net/runtime/network_runtime.h"
#include "net/async/async_request_client.h"
#include "net/session/stream_server_session.h"
#include "timer/timer_handle.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <condition_variable>
#include <functional>
#include <thread>
#include <unordered_map>

namespace yuan::game::server::rpc_network
{
    struct RpcEndpoint
    {
        std::string host = "127.0.0.1";
        std::uint16_t port = 0;
    };

    struct RpcNetworkServerConfig
    {
        std::string host = "127.0.0.1";
        std::uint16_t port = 0;
        std::size_t stop_after_requests = 0;
        std::size_t max_connections = 0;
        std::size_t max_buffered_bytes = 1024 * 1024;
        std::uint64_t idle_timeout_ms = 0;
    };

    struct RpcNetworkClientConfig
    {
        std::chrono::milliseconds connect_timeout{1000};
        std::chrono::milliseconds read_timeout{1000};
    };

    namespace metadata_key
    {
        inline constexpr const char *connection_id = game_metadata_key::rpc_connection_id;
        inline constexpr const char *close_connection = game_metadata_key::rpc_close_connection;
        inline constexpr const char *defer_response = game_metadata_key::rpc_defer_response;
    }

    class RpcNetworkServer
    {
    public:
        RpcNetworkServer();
        ~RpcNetworkServer();

        RpcNetworkServer(const RpcNetworkServer &) = delete;
        RpcNetworkServer &operator=(const RpcNetworkServer &) = delete;

        bool start(const RpcNetworkServerConfig &config, yuan::rpc::Server &server);
        bool bind_loopback(std::uint16_t port, yuan::rpc::Server &server, std::size_t expected_requests = 1);
        [[nodiscard]] yuan::timer::TimerHandle schedule_periodic(std::uint32_t delay_ms,
                                                                  std::uint32_t interval_ms,
                                                                  std::function<void()> callback,
                                                                  int repeat = 0);
        void cancel_timer(const yuan::timer::TimerHandle &timer);
        void call_async_detached(const RpcEndpoint &endpoint,
                                 yuan::rpc::Message message,
                                 std::function<void(std::optional<yuan::rpc::Response>)> callback,
                                 RpcNetworkClientConfig client_config = {});
        void set_connection_closed_callback(std::function<void(std::uint64_t)> callback);
        [[nodiscard]] bool write_message_to_connection(std::uint64_t connection_id, const yuan::rpc::Message &message);
        [[nodiscard]] bool write_response_to_connection(std::uint64_t connection_id, yuan::rpc::Response response);
        [[nodiscard]] bool close_connection(std::uint64_t connection_id);
        void close_all_connections();
        [[nodiscard]] std::size_t active_connection_count() const;
        bool run();
        void stop();

        [[nodiscard]] bool ok() const;
        [[nodiscard]] std::uint16_t port() const;
        [[nodiscard]] yuan::net::NetworkRuntime &runtime() noexcept { return runtime_; }
        [[nodiscard]] const yuan::net::NetworkRuntime &runtime() const noexcept { return runtime_; }

    private:
        struct ConnectionState
        {
            yuan::net::ConnectionContext context;
            std::uint64_t last_activity_ms = 0;
        };

        void check_idle_connections();

        yuan::net::NetworkRuntime runtime_;
        yuan::net::StreamServerSession session_;
        yuan::rpc::Server *server_ = nullptr;
        std::function<void(std::uint64_t)> connection_closed_callback_;
        mutable std::mutex connections_mutex_;
        std::unordered_map<std::uint64_t, ConnectionState> connections_;
        yuan::timer::TimerHandle idle_monitor_timer_;
        std::size_t max_connections_ = 0;
        std::size_t max_buffered_bytes_ = 1024 * 1024;
        std::uint64_t idle_timeout_ms_ = 0;
        std::unordered_map<std::uintptr_t, yuan::rpc::wire::FrameStreamDecoder> decoders_;
        std::atomic_size_t handled_{0};
        std::size_t expected_requests_ = 1;
        bool stop_after_expected_requests_ = false;
        bool ok_ = false;
        std::uint16_t port_ = 0;
    };

    using CoreRpcServer = RpcNetworkServer;

    class RpcNetworkClient
    {
    public:
        explicit RpcNetworkClient(RpcNetworkClientConfig config = {});

        std::optional<yuan::rpc::Response> call(const RpcEndpoint &endpoint, const yuan::rpc::Message &message) const;

    private:
        RpcNetworkClientConfig config_;
    };

    class RpcNetworkPersistentClient
    {
    public:
        explicit RpcNetworkPersistentClient(RpcNetworkClientConfig config = {});
        ~RpcNetworkPersistentClient();

        RpcNetworkPersistentClient(const RpcNetworkPersistentClient &) = delete;
        RpcNetworkPersistentClient &operator=(const RpcNetworkPersistentClient &) = delete;

        std::optional<yuan::rpc::Response> call(const RpcEndpoint &endpoint, const yuan::rpc::Message &message);
        void close();

    private:
        RpcNetworkClientConfig config_;
        yuan::net::NetworkRuntime runtime_;
        yuan::net::AsyncRequestClient client_;
        std::optional<RpcEndpoint> connected_endpoint_;
        std::mutex mutex_;
    };

    class RpcNetworkPersistentAsyncClient
    {
    public:
        explicit RpcNetworkPersistentAsyncClient(RpcNetworkClientConfig config = {}, std::size_t max_pending = 1024);
        ~RpcNetworkPersistentAsyncClient();

        RpcNetworkPersistentAsyncClient(const RpcNetworkPersistentAsyncClient &) = delete;
        RpcNetworkPersistentAsyncClient &operator=(const RpcNetworkPersistentAsyncClient &) = delete;

        std::future<std::optional<yuan::rpc::Response>> call_async(const RpcEndpoint &endpoint, yuan::rpc::Message message);
        std::optional<yuan::rpc::Response> call(const RpcEndpoint &endpoint, yuan::rpc::Message message);
        void close();
        [[nodiscard]] std::size_t queued_size() const;
        [[nodiscard]] std::size_t pending_size() const;

    private:
        struct QueuedCall
        {
            RpcEndpoint endpoint;
            yuan::rpc::Message message;
            std::shared_ptr<std::promise<std::optional<yuan::rpc::Response>>> promise;
        };

        void worker_loop(std::stop_token stop_token);
        struct ClientState
        {
            std::unique_ptr<yuan::net::AsyncRequestClient> client;
            yuan::rpc::wire::FrameStreamDecoder decoder;
        };

        [[nodiscard]] std::string endpoint_key(const RpcEndpoint &endpoint) const;
        ClientState *ensure_connected(const RpcEndpoint &endpoint);
        void fail_all(std::deque<QueuedCall> &calls);

        RpcNetworkClientConfig config_;
        std::size_t max_pending_ = 1024;
        yuan::net::NetworkRuntime runtime_;
        std::unordered_map<std::string, ClientState> clients_;
        std::atomic<yuan::rpc::RequestId> next_request_id_{1};
        std::atomic_size_t pending_count_{0};
        mutable std::mutex mutex_;
        std::condition_variable_any cv_;
        std::deque<QueuedCall> queue_;
        std::jthread worker_;
    };

    std::optional<yuan::rpc::Response> call(std::uint16_t port,
                                            const yuan::rpc::Message &message,
                                            std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    std::uint16_t reserve_loopback_port();
}

#endif
