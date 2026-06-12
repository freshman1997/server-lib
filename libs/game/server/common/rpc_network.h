#ifndef YUAN_GAME_SERVER_COMMON_RPC_NETWORK_H
#define YUAN_GAME_SERVER_COMMON_RPC_NETWORK_H

#include "yuan/rpc/rpc.h"

#include "buffer/byte_buffer.h"
#include "net/runtime/network_runtime.h"
#include "net/session/stream_server_session.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <condition_variable>

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
    };

    struct RpcNetworkClientConfig
    {
        std::chrono::milliseconds connect_timeout{1000};
        std::chrono::milliseconds read_timeout{1000};
    };

    class RpcNetworkServer
    {
    public:
        RpcNetworkServer();
        ~RpcNetworkServer();

        RpcNetworkServer(const RpcNetworkServer &) = delete;
        RpcNetworkServer &operator=(const RpcNetworkServer &) = delete;

        bool start(const RpcNetworkServerConfig &config, yuan::rpc::Server &server);
        bool bind_loopback(std::uint16_t port, yuan::rpc::Server &server, std::size_t expected_requests = 1);
        bool run();
        void stop();

        [[nodiscard]] bool ok() const;
        [[nodiscard]] std::uint16_t port() const;

    private:
        yuan::net::NetworkRuntime runtime_;
        yuan::net::StreamServerSession session_;
        yuan::rpc::Server *server_ = nullptr;
        std::mutex workers_mutex_;
        std::condition_variable workers_idle_;
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
