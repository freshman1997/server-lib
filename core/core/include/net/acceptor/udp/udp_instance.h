#ifndef __NET_UDP_INSTANCE_H___
#define __NET_UDP_INSTANCE_H___
#include <unordered_map>
#include <unordered_set>

#include "buffer/linked_buffer.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"

namespace yuan::net
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

    enum class UdpMode
    {
        normal      = 0,
        broadcast   = 1,
        multicast   = 2,
    };

    class UdpInstance
    {
    public:
        UdpInstance(UdpAcceptor *acceptor = nullptr);
        ~UdpInstance();

    public:
        void send();
        std::pair<bool, Connection *> on_recv(const InetAddress &address);
        int on_send(Connection *conn, buffer::Buffer *buff);

        buffer::LinkedBuffer * get_input_buff_list()
        {
            return &input_buffer_;
        }

        void on_connection_close(Connection *conn);

        void set_acceptor(UdpAcceptor *acceptor);

        timer::TimerManager * get_timer_manager();

        void enable_rw_events();

        void set_adapter_type(UdpAdapterType type)
        {
            adapter_type_ = type;
        }

    private:
        void try_free_connections();

    private:
        bool is_closing_;
        UdpAdapterType adapter_type_;
        UdpAcceptor *acceptor_;
        buffer::LinkedBuffer input_buffer_;
        std::unordered_map<InetAddress, Connection *> conns_;
        std::unordered_set<InetAddress> free_addrs_;
    };
}

#endif