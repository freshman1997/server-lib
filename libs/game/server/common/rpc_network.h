#ifndef YUAN_GAME_SERVER_COMMON_RPC_NETWORK_H
#define YUAN_GAME_SERVER_COMMON_RPC_NETWORK_H

#include "yuan/rpc/rpc.h"
#include "yuan/rpc/frame_stream.h"

#include "buffer/byte_buffer.h"
#include "net/runtime/network_runtime.h"
#include "net/session/stream_server_session.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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
        inline constexpr const char *connection_id = "rpc.connection_id";
        inline constexpr const char *close_connection = "rpc.close_connection";
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
        void set_connection_closed_callback(std::function<void(std::uint64_t)> callback);
        [[nodiscard]] bool write_message_to_connection(std::uint64_t connection_id, const yuan::rpc::Message &message);
        void close_all_connections();
        [[nodiscard]] std::size_t active_connection_count() const;
        bool run();
        void stop();

        [[nodiscard]] bool ok() const;
        [[nodiscard]] std::uint16_t port() const;

    private:
        struct ConnectionState
        {
            yuan::net::ConnectionContext context;
            std::uint64_t last_activity_ms = 0;
        };

        void idle_monitor_loop(std::stop_token stop_token);

        yuan::net::NetworkRuntime runtime_;
        yuan::net::StreamServerSession session_;
        yuan::rpc::Server *server_ = nullptr;
        std::function<void(std::uint64_t)> connection_closed_callback_;
        mutable std::mutex connections_mutex_;
        std::unordered_map<std::uint64_t, ConnectionState> connections_;
        std::jthread idle_monitor_thread_;
        std::size_t max_connections_ = 0;
        std::size_t max_buffered_bytes_ = 1024 * 1024;
        std::uint64_t idle_timeout_ms_ = 0;
        std::mutex workers_mutex_;
        std::condition_variable workers_idle_;
        std::unordered_map<std::uintptr_t, yuan::rpc::wire::FrameStreamDecoder> decoders_;
        std::size_t active_workers_ = 0;
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

    std::optional<yuan::rpc::Response> call(std::uint16_t port,
                                            const yuan::rpc::Message &message,
                                            std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    std::uint16_t reserve_loopback_port();
}

#endif
