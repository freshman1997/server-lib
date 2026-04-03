#ifndef __NET_DNS_DNS_CLIENT_H__
#define __NET_DNS_DNS_CLIENT_H__

#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"
#include "timer/timer_task.h"
#include "net/poller/poller.h"
#include "event/event_loop.h"
#include "dns_packet.h"
#include <string>
#include <functional>
#include <atomic>

namespace yuan::net
{
    class Connection;
    class UdpAcceptor;
}

namespace yuan::buffer
{
    class Buffer;
    class LinkedBuffer;
}

namespace yuan::net
{
    namespace buffer { using ::yuan::buffer::Buffer; }
}

namespace yuan::net::dns
{
    using DnsResponseHandler = std::function<void(const DnsPacket& response)>;

    class DnsClient : public ConnectionHandler, public timer::TimerTask
    {
    public:
        static constexpr uint32_t DEFAULT_TIMEOUT_MS = 5000;

    public:
        DnsClient();
        ~DnsClient();

    public:
        virtual void on_connected(Connection *conn) override;
        virtual void on_error(Connection *conn) override;
        virtual void on_read(Connection *conn) override;
        virtual void on_write(Connection *conn) override;
        virtual void on_close(Connection *conn) override;

    public:
        virtual void on_timer(timer::Timer *timer) override;

    public:
        bool connect(const std::string &ip, short port, timer::TimerManager *timer_manager = nullptr,
                     Poller *poller = nullptr, EventLoop *ev_loop = nullptr);
        void disconnect();

        bool query(const std::string &domain, DnsType type = DnsType::A, uint32_t timeout_ms = DEFAULT_TIMEOUT_MS);
        bool query(const std::string &domain, DnsType type, DnsResponseHandler handler, uint32_t timeout_ms = DEFAULT_TIMEOUT_MS);

        const DnsPacket& get_last_response() const;

    private:
        void handle_dns_response(Connection *conn, ::yuan::buffer::Buffer *buffer);

    private:
        std::string server_ip_;
        uint16_t server_port_;
        InetAddress addr_;
        timer::TimerManager *timer_manager_;
        Poller *poller_;
        EventLoop *ev_loop_;
        Connection *connection_;
        UdpAcceptor *acceptor_;
        std::atomic<bool> own_loop_;
        uint16_t next_session_id_;
        DnsResponseHandler response_handler_;
        bool got_response_;
        DnsPacket last_response_;
        timer::Timer *timeout_timer_;
    };
}

#endif
