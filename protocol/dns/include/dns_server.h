#ifndef __NET_DNS_DNS_SERVER_H__
#define __NET_DNS_DNS_SERVER_H__

#include "net/handler/connection_handler.h"
#include "timer/timer_manager.h"
#include "net/poller/poller.h"
#include "event/event_loop.h"
#include "dns_packet.h"
#include <functional>
#include <atomic>
#include <map>
#include <string>

namespace yuan::net
{
    class Connection;
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
    using DnsQueryHandler = std::function<void(const DnsPacket& query, DnsPacket& response)>;

    class DnsServer : public ConnectionHandler
    {
    public:
        DnsServer();
        ~DnsServer();

    public:
        virtual void on_connected(Connection *conn) override;
        virtual void on_error(Connection *conn) override;
        virtual void on_read(Connection *conn) override;
        virtual void on_write(Connection *conn) override;
        virtual void on_close(Connection *conn) override;

    public:
        bool serve(int port);
        bool serve(int port, timer::TimerManager *timer_manager, Poller *poller, EventLoop *ev_loop);
        void stop();

        void set_query_handler(DnsQueryHandler handler);
        void add_record(const std::string &name, const std::string &ip, DnsType type = DnsType::A);

    private:
        void handle_dns_query(Connection *conn, ::yuan::buffer::Buffer *buffer);
        void create_response(const DnsPacket &query, DnsPacket &response);
        DnsResourceRecord find_record(const std::string &name, DnsType type);

    private:
        int port_;
        std::atomic<bool> running_;
        DnsQueryHandler query_handler_;
        std::map<std::string, DnsResourceRecord> dns_records_;
        timer::TimerManager *timer_manager_;
        Poller *poller_;
        EventLoop *ev_loop_;
    };
}

#endif