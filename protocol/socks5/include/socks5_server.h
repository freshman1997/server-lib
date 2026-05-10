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
#include "net/security/ssl_module.h"
#include "net/auth_rate_limiter.h"
#include "net/handler/connection_handler.h"
#include "timer/timer_handle.h"
#include "timer/timer_util.hpp"

#include <memory>
#include <unordered_map>
#include <atomic>
#include <functional>
#include <mutex>

namespace yuan::net
{
    class Connection;
    class DatagramAcceptor;
}

namespace yuan::net::socks5
{
    struct Socks5ServerMetrics
    {
        std::atomic_uint64_t accepted_sessions{ 0 };
        std::atomic_uint64_t rejected_sessions{ 0 };
        std::atomic_uint64_t completed_sessions{ 0 };
        std::atomic_int active_sessions{ 0 };
        std::atomic_uint64_t bytes_up{ 0 };
        std::atomic_uint64_t bytes_down{ 0 };
        std::atomic_uint32_t active_udp_associations{ 0 };
        std::atomic_uint64_t connect_timeouts{ 0 };
        std::atomic_uint64_t idle_timeouts{ 0 };
        std::atomic_uint64_t closes_by_client{ 0 };
        std::atomic_uint64_t closes_by_upstream{ 0 };
        std::atomic_uint64_t closes_by_ssrf{ 0 };
        std::atomic_uint64_t closes_by_acl{ 0 };
    };

    struct Socks5SessionInfo
    {
        std::string client_addr;
        std::string command;
        std::string target_addr;
        uint64_t duration_ms = 0;
        uint64_t bytes_up = 0;
        uint64_t bytes_down = 0;
        std::string close_reason;
    };

    struct UdpAssociation
    {
        std::shared_ptr<Connection> client_conn;
        std::unique_ptr<DatagramAcceptor> udp_acceptor;
        DatagramAcceptor *udp_endpoint = nullptr;
        InetAddress client_udp_addr;
        InetAddress target_addr;
        timer::TimerHandle idle_timer;
        std::string client_ip;
        uint64_t bytes_up = 0;
        uint64_t bytes_down = 0;
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
        bool init(const std::string &host, int port);
        bool init(const std::string &host, int port, NetworkRuntime &runtime);
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

        const Socks5ServerMetrics &metrics() const
        {
            return metrics_;
        }

        using SessionEventCallback = std::function<void(const std::string &event_name, const Socks5SessionInfo &)>;
        void set_session_event_callback(SessionEventCallback cb)
        {
            session_event_cb_ = std::move(cb);
        }

        using SessionStateCallback = std::function<void(const std::string &event_name, const std::string &client_addr, const std::string &previous_state, const std::string &current_state, const std::string &reason)>;
        void set_session_state_callback(SessionStateCallback cb)
        {
            session_state_cb_ = std::move(cb);
        }

    private:
        coroutine::Task<void> handle_connection(AsyncConnectionContext ctx);
        coroutine::Task<void> relay_pipe(coroutine::RuntimeView rv,
                                         std::shared_ptr<Connection> src,
                                         std::shared_ptr<Connection> dst,
                                         std::function<void(const std::string &)> on_close,
                                         std::atomic_bool *dst_alive,
                                         std::atomic_uint64_t *byte_counter);

        void send_reply(Connection *conn, ReplyCode reply,
                        AddressType atyp = AddressType::ipv4,
                        const std::string &bind_addr = "0.0.0.0",
                        uint16_t bind_port = 0);
        void send_reply(Connection &conn, ReplyCode reply,
                        AddressType atyp = AddressType::ipv4,
                        const std::string &bind_addr = "0.0.0.0",
                        uint16_t bind_port = 0);
        void send_reply(const std::shared_ptr<Connection> &conn, ReplyCode reply,
                        AddressType atyp = AddressType::ipv4,
                        const std::string &bind_addr = "0.0.0.0",
                        uint16_t bind_port = 0);

        void on_udp_datagram(Connection *conn);
        void on_udp_datagram(Connection &conn);
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
        std::unordered_map<std::string, size_t> udp_associations_per_client_;
        std::unordered_map<std::string, size_t> tcp_sessions_per_client_;
        std::mutex tcp_sessions_mutex_;
        Socks5Handler *handler_ = nullptr;
        Socks5ServerConfig config_;
        Socks5ServerMetrics metrics_;
        SessionEventCallback session_event_cb_;
        SessionStateCallback session_state_cb_;
        RelayHandler relay_handler_;
        UdpRelayHandler udp_relay_handler_;
        timer::TimerHandle idle_sweep_timer_;
        std::atomic_bool stop_requested_{ false };
        AuthRateLimiter auth_rate_limiter_;
    };
}

#endif
