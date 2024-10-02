#ifndef __NET_BASE_UDP_ACCEPTOR_H__
#define __NET_BASE_UDP_ACCEPTOR_H__
#include "../../buffer/buffer.h"
#include "acceptor.h"
#include "udp/udp_instance.h"
#include "../channel/channel.h"
#include "../socket/inet_address.h"

namespace timer 
{
    class TimerManager;
}

namespace net
{
    class Socket;

    class UdpAcceptor : public Acceptor
    {
    public:
        UdpAcceptor(Socket *socket, timer::TimerManager *timerManager);

        ~UdpAcceptor();

        virtual bool listen();

        virtual void close();

        virtual Channel * get_channel();

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);

        virtual void set_connection_handler(ConnectionHandler *connHandler);

    public:
        int send_to(Connection *conn, Buffer *buff);

        int send_to(const InetAddress &addr, Buffer *buff);

        timer::TimerManager * get_timer_manager()
        {
            return timer_manager_;
        }

    private:
        Channel *channel_;
        Socket *sock_;
        EventHandler *handler_;
        ConnectionHandler *conn_handler_;
        timer::TimerManager *timer_manager_;
        UdpInstance instance_;
    };
}

#endif
