#ifndef __NET_SOCKS5_SOCKS5_SERVER_H__
#define __NET_SOCKS5_SOCKS5_SERVER_H__

#include "socks5_config.h"
#include "socks5_handler.h"
#include "socks5_session.h"
#include "socks5_protocol.h"
#include "buffer/byte_buffer.h"
#include "coroutine/runtime_view.h"
#include "net/socket/inet_address.h"
#include "coroutine/task.h"
#include "net/async/async_listener_host.h"
#include "net/async/async_connection_context.h"
#include "net/runtime/network_runtime.h"
#include "net/secuity/ssl_module.h"
#include "net/handler/connection_handler.h"

#include <memory>
#include <unordered_map>
#include <atomic>
#include <functional>

namespace yuan::net
{
    class Connection;
    class DatagramAcceptor;
}

namespace yuan::net::socks5
{
    struct UdpAssociation
    {
        std::shared_ptr<Connection> client_conn;
        std::unique_ptr<DatagramAcceptor> udp_acceptor;
        InetAddress client_udp_addr;
        InetAddress target_addr;
        timer::Timer *idle_timer;
    };

    class Socks5Server
    {
    public:
        Socks5Server();
        explicit Socks5Server(const Socks5ServerConfig &config);
        ~Socks5Server();

        Socks5Server(const Socks5Server &) = delete;
        Socks5Server &operator=(const Socks5Server &) = delete;

    public:
        bool init(int port);
        bool init(int port, NetworkRuntime &runtime);
        void serve();
        void stop();

        NetworkRuntime *runtime() const noexcept
        {
            return listener_.runtime();
        }

        void set_handler(Socks5Handler *handler)
        {
            handler_ = handler;
        }
        const Socks5ServerConfig &config() const
        {
            return config_;
        }

    private:
        coroutine::Task<void> handle_connection(AsyncConnectionContext ctx);
        coroutine::Task<void> relay_pipe(coroutine::RuntimeView rv,
                                         std::shared_ptr<Connection> src,
                                         std::shared_ptr<Connection> dst,
                                         std::function<void()> close_both,
                                         std::atomic_bool *dst_alive);

        void send_reply(Connection *conn, ReplyCode reply,
                        AddressType atyp = AddressType::ipv4,
                        const std::string &bind_addr = "0.0.0.0",
                        uint16_t bind_port = 0);
        void send_reply(const std::shared_ptr<Connection> &conn, ReplyCode reply,
                        AddressType atyp = AddressType::ipv4,
                        const std::string &bind_addr = "0.0.0.0",
                        uint16_t bind_port = 0);

        void on_udp_datagram(Connection *conn);
        void forward_udp_to_target(UdpAssociation *assoc, const Socks5UdpHeader &header, const ::yuan::buffer::ByteBuffer &payload);
        void forward_udp_to_client(UdpAssociation *assoc, const InetAddress &target_addr, const ::yuan::buffer::ByteBuffer &payload);
        void close_udp_association(Connection *client_conn);
        void close_udp_association(const std::shared_ptr<Connection> &client_conn);

    private:
        class RelayHandler final : public ConnectionHandler
        {
        public:
            void on_connected(const std::shared_ptr<Connection> &conn) override
            {
                (void)conn;
            }
            void on_error(const std::shared_ptr<Connection> &conn) override
            {
                (void)conn;
            }
            void on_read(const std::shared_ptr<Connection> &conn) override
            {
                (void)conn;
            }
            void on_write(const std::shared_ptr<Connection> &conn) override
            {
                (void)conn;
            }
            void on_close(const std::shared_ptr<Connection> &conn) override
            {
                (void)conn;
            }
        };

        class UdpRelayHandler final : public ConnectionHandler
        {
        public:
            explicit UdpRelayHandler(Socks5Server &server);
            void on_connected(const std::shared_ptr<Connection> &conn) override
            {
                (void)conn;
            }
            void on_read(const std::shared_ptr<Connection> &conn) override;
            void on_error(const std::shared_ptr<Connection> &conn) override;
            void on_write(const std::shared_ptr<Connection> &conn) override
            {
                (void)conn;
            }
            void on_close(const std::shared_ptr<Connection> &conn) override
            {
                (void)conn;
            }

        private:
            Socks5Server &server_;
        };

        AsyncListenerHost listener_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
        std::shared_ptr<SSLModule> ssl_module_;
        std::unordered_map<Connection *, std::unique_ptr<UdpAssociation> > udp_associations_;
        std::unordered_map<Connection *, Connection *> udp_conn_to_client_;
        Socks5Handler *handler_ = nullptr;
        Socks5ServerConfig config_;
        RelayHandler relay_handler_;
        UdpRelayHandler udp_relay_handler_;
    };
}

#endif
