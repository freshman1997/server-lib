#ifndef __NET_SHADOWSOCKS_SHADOWSOCKS_SERVER_H__
#define __NET_SHADOWSOCKS_SHADOWSOCKS_SERVER_H__

#include "net/async/async_listener_host.h"
#include "net/handler/connection_handler.h"
#include "net/runtime/network_runtime.h"
#include "shadowsocks_config.h"
#include "shadowsocks_handler.h"
#include "shadowsocks_protocol.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::buffer
{
    class ByteBuffer;
}

namespace yuan::coroutine
{
    class RuntimeView;
    template<typename T>
    class Task;
}

namespace yuan::net
{
    class Connection;
    class DatagramAcceptor;
    class InetAddress;
}

namespace yuan::net::shadowsocks
{
    class ShadowsocksSession;

    class ShadowsocksServer
    {
    public:
        ShadowsocksServer();
        explicit ShadowsocksServer(const ShadowsocksServerConfig &config);
        ~ShadowsocksServer();

        ShadowsocksServer(const ShadowsocksServer &) = delete;
        ShadowsocksServer &operator=(const ShadowsocksServer &) = delete;

        bool init(int port);
        bool init(int port, NetworkRuntime &runtime);
        bool init(const std::string &host, int port);
        bool init(const std::string &host, int port, NetworkRuntime &runtime);

        void serve();
        void stop();

        void set_handler(ShadowsocksHandler *handler);

        const ShadowsocksServerConfig &config() const;
        NetworkRuntime *runtime() const noexcept;

    private:
        struct UdpAssociation
        {
            std::shared_ptr<Connection> client_conn;
            std::unique_ptr<InetAddress> bound_client_addr;
            std::string client_ip;
        };

        class UdpRelayHandler final : public ConnectionHandler
        {
        public:
            explicit UdpRelayHandler(ShadowsocksServer &server);
            void on_connected(const std::shared_ptr<Connection> &conn) override;
            void on_read(const std::shared_ptr<Connection> &conn) override;
            void on_write(const std::shared_ptr<Connection> &conn) override;
            void on_error(const std::shared_ptr<Connection> &conn) override;
            void on_close(const std::shared_ptr<Connection> &conn) override;

        private:
            ShadowsocksServer &server_;
        };

        coroutine::Task<void> handle_connection(AsyncConnectionContext ctx);
        coroutine::Task<void> relay_pipe(coroutine::RuntimeView rv,
                                         std::shared_ptr<Connection> src,
                                         std::shared_ptr<Connection> dst,
                                         CipherMethod method,
                                         const std::vector<uint8_t> &subkey,
                                         std::vector<uint8_t> &send_nonce,
                                         std::function<void()> on_close);

        bool validate_config();

        bool parse_first_payload(ShadowsocksSession &session,
                                 const std::vector<uint8_t> &payload,
                                 ::yuan::buffer::ByteBuffer &out_initial_upstream_data);

        void close_udp_association(Connection *client_conn);
        void on_udp_datagram(Connection *conn);
        void forward_udp_to_target(UdpAssociation *assoc, const ::yuan::buffer::ByteBuffer &plain_packet);
        void forward_udp_to_client(UdpAssociation *assoc,
                                   const InetAddress &source_addr,
                                   const ::yuan::buffer::ByteBuffer &payload);

        bool target_allowed(const std::string &host, uint16_t port) const;

    private:
        std::unordered_map<Connection *, std::unique_ptr<UdpAssociation> > udp_associations_;
        std::unordered_map<Connection *, Connection *> udp_conn_to_client_;
        std::atomic_bool stop_requested_{ false };
        UdpRelayHandler udp_relay_handler_;
        AsyncListenerHost listener_;
        std::unique_ptr<DatagramAcceptor> udp_acceptor_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
        ShadowsocksHandler *handler_ = nullptr;
        ShadowsocksServerConfig config_;
        std::optional<CipherMethod> method_;
        std::vector<uint8_t> master_key_;
    };
}

#endif
