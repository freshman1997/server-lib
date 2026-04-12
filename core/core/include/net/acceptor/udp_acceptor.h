#ifndef __NET_BASE_UDP_ACCEPTOR_H__
#define __NET_BASE_UDP_ACCEPTOR_H__
#include "../../buffer/byte_buffer.h"
#include "datagram_acceptor.h"
#include "datagram_endpoint.h"
#include "udp/udp_instance.h"
#include "../channel/channel.h"
#include "../socket/inet_address.h"
#include <memory>

namespace yuan::timer 
{
    class TimerManager;
}

namespace yuan::net
{
    class Socket;

    class UdpAcceptor : public DatagramAcceptor
    {
    public:
        explicit UdpAcceptor(Socket *socket, timer::TimerManager *timerManager);

        ~UdpAcceptor();

        virtual bool listen();

        virtual void close();

        virtual Channel *endpoint_channel() override
        {
            return channel_.get();
        }

        virtual void update_channel();

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);

        virtual void set_connection_handler(ConnectionHandler *connHandler);

        virtual void set_ssl_module(std::shared_ptr<SSLModule> module) {}

    public:
        int send_to(Connection *conn, const ::yuan::buffer::ByteBuffer &buff);

        int send_to(const InetAddress &addr, const ::yuan::buffer::ByteBuffer &buff);

        virtual int send_datagram(Connection *conn, const ::yuan::buffer::ByteBuffer &buff) override
        {
            return send_to(conn, buff);
        }

        virtual int send_datagram(const InetAddress &addr, const ::yuan::buffer::ByteBuffer &buff) override;

        virtual void update_endpoint_channel() override
        {
            update_channel();
        }

        timer::TimerManager * get_timer_manager()
        {
            return timer_manager_;
        }

        virtual timer::TimerManager *endpoint_timer_manager() override
        {
            return timer_manager_;
        }

        UdpInstance * get_udp_instance() override
        {
            return instance_.get();
        }

    private:
        std::unique_ptr<Channel> channel_;
        std::unique_ptr<Socket> sock_;
        EventHandler *handler_;
        ConnectionHandler *conn_handler_;
        timer::TimerManager *timer_manager_;
        std::unique_ptr<UdpInstance> instance_;
    };
}

#endif
