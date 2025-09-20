#ifndef __NET_DNS_DNS_CLIENT_H__
#define __NET_DNS_DNS_CLIENT_H__
#include "net/connection/connection.h"
#include "net/poller/poller.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"
#include "event/event_loop.h"

namespace yuan::net::dns 
{
    class DnsClient : public ConnectionHandler
    {
    public:
        DnsClient();
        ~DnsClient();
        
    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        bool connect(const std::string &ip, short port);

    private:
        InetAddress addr_;
        timer::TimerManager *timer_manager_;
        Poller *poller_;
        EventLoop *ev_loop_;
        int retry_cnt_;
    };
}

#endif