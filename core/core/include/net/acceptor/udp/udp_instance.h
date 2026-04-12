#ifndef __NET_UDP_INSTANCE_H___
#define __NET_UDP_INSTANCE_H___
#include <unordered_map>
#include "buffer/byte_buffer.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"

namespace yuan::buffer
{
}

namespace yuan::net
{
    class Connection;
    class DatagramEndpoint;
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
        UdpInstance(DatagramEndpoint *acceptor = nullptr);
        ~UdpInstance();

        UdpInstance(const UdpInstance &) = delete;
        UdpInstance & operator=(const UdpInstance &) = delete;

    public:
        void send();
        std::pair<bool, Connection *> on_recv(const InetAddress &address);
        int on_send(Connection *conn, const ::yuan::buffer::ByteBuffer &buff);

        void set_input_packet(::yuan::buffer::ByteBuffer packet)
        {
            input_packet_ = std::move(packet);
        }

        ::yuan::buffer::ByteBuffer take_input_packet()
        {
            auto packet = input_packet_.copy_readable();
            input_packet_.clear();
            return packet;
        }

        void on_connection_close(Connection *conn);

        void set_acceptor(DatagramEndpoint *acceptor);

        timer::TimerManager * get_timer_manager();

        void enable_rw_events();

        bool is_closing() const { return is_closing_; }

        void set_adapter_type(UdpAdapterType type)
        {
            adapter_type_ = type;
        }

    private:
        bool is_closing_;
        UdpAdapterType adapter_type_;
        DatagramEndpoint *acceptor_;
        ::yuan::buffer::ByteBuffer input_packet_;
        std::unordered_map<InetAddress, Connection *> conns_;
    };
}

#endif
