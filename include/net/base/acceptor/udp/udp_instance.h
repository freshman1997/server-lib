#ifndef __NET_UDP_INSTANCE_H___
#define __NET_UDP_INSTANCE_H___
#include <deque>
#include <unordered_map>

#include "buffer/buffer.h"
#include "buffer/linked_buffer.h"
#include "net/base/socket/inet_address.h"
#include "timer/timer_manager.h"

namespace net
{
    class Connection;
    class UdpAcceptor;
    class UdpAdapter;

    const int UDP_DATA_LIMIT = 1472;

    enum class UdpAdapterType
    {
        none    = 0,
        kcp     = 1,
    };

    class UdpInstance
    {
    public:
        UdpInstance(UdpAcceptor *acceptor = nullptr);
        ~UdpInstance();

    public:
        void send();
        std::pair<bool, Connection *> on_recv(const InetAddress &address);
        void on_send(Connection *conn, Buffer *buff);

        LinkedBuffer * get_input_buff_list()
        {
            return &input_buffer_;
        }

        void on_connection_close(Connection *conn);

        void set_acceptor(UdpAcceptor *acceptor);

        timer::TimerManager * get_timer_manager();

    private:
        bool is_closing_;
        UdpAdapterType adapter_type_;
        UdpAcceptor *acceptor_;
        LinkedBuffer input_buffer_;
        std::unordered_map<InetAddress, Connection *> conns_;
        std::deque<std::pair<Connection *, Buffer *>> queue_;
    };
}

#endif