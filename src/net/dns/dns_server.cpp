#include "net/dns/dns_server.h"
#include "net/base/acceptor/udp_acceptor.h"
#include "net/base/event/event_loop.h"
#include "net/base/poller/select_poller.h"
#include "net/base/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "net/base/connection/connection.h"
#include <iostream>

namespace net::dns 
{
    DnsServer::DnsServer()
    {

    }

    DnsServer::~DnsServer()
    {

    }

    void DnsServer::on_connected(Connection *conn)
    {

    }

    void DnsServer::on_error(Connection *conn)
    {

    }

    void DnsServer::on_read(Connection *conn)
    {
        auto buff = conn->get_input_buff();
        std::string str(buff->peek(), buff->peek() + buff->readable_bytes());
        std::cout << "xxxxxxx: " << str << '\n';
        conn->write_and_flush(conn->get_input_buff(true));
    }

    void DnsServer::on_write(Connection *conn)
    {
        std::cout << "--------------------\n";
    }

    void DnsServer::on_close(Connection *conn)
    {

    }

    bool DnsServer::serve(int port)
    {
        Socket *sock = new Socket("", port, true);
        if (!sock->valid()) {
            std::cout << "cant create socket file descriptor!\n";
            delete sock;
            return false;
        }

        if (!sock->bind()) {
            std::cout << "cant bind port: " << port << "!\n";
            delete sock;
            return false;
        }

        sock->set_no_deylay(true);
        sock->set_reuse(true);
        sock->set_none_block(true);

        timer::WheelTimerManager timerManager;
        UdpAcceptor *acceptor = new UdpAcceptor(sock, &timerManager);
        if (!acceptor->listen()) {
            std::cout << "cant listen on port: " << port << "!\n";
            delete acceptor;
            return false;
        }

        SelectPoller poller;
        EventLoop evLoop(&poller, &timerManager);

        acceptor->set_event_handler(&evLoop);
        acceptor->set_connection_handler(this);

        evLoop.loop();

        return true;
    }
}