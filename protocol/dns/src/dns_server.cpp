#include "dns_server.h"
#include "net/acceptor/udp_acceptor.h"
#include "net/event/event_loop.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "net/connection/connection.h"
#include <iostream>

namespace yuan::net::dns 
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
        conn->process_input_data([this](buffer::Buffer *buff) -> bool {
            std::string str(buff->peek(), buff->peek() + buff->readable_bytes());
            std::cout << "xxxxxxx: " << str << '\n';
            return true;
        }, false);
        conn->forward(conn);
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