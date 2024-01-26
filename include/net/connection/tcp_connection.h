#ifndef __TCP_CONNECTION_H__
#define __TCP_CONNECTION_H__
#include <memory>

#include "connection.h"
#include "net/channel/channel.h"
#include "net/handler/select_handler.h"
#include "net/socket/inet_address.h"
#include "buffer/buffer.h"

namespace net
{
    class AcceptHandler;
    class TcpConnectionHandler;

    class TcpConnection : public Connection
    {
    public:
        TcpConnection(std::shared_ptr<net::InetAddress> remoteAddr, std::shared_ptr<net::Channel> channel);

        virtual ~TcpConnection();

        virtual bool is_connected();

        virtual const InetAddress & get_remote_address() const;

        virtual const InetAddress & get_local_address() const;

        virtual std::shared_ptr<Buffer> get_input_buff();

        virtual std::shared_ptr<Buffer> get_output_buff();

        virtual void send(std::shared_ptr<Buffer> buff);

        virtual void send();

        // 丢弃所有未发送的数据
        virtual void abort();

        // 发送完数据后返回
        virtual void close();

        virtual ConnectionType get_conn_type();

        virtual Channel * get_channel();

        virtual void set_connection_handler(ConnectionHandler *connectionHandler);

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);

    private:
        std::shared_ptr<net::InetAddress> addr_;
        std::shared_ptr<net::Channel> channel_;
        AcceptHandler *acceptHandler_;
        ConnectionHandler *connectionHandler_;
        EventHandler *eventHandler_;
        std::shared_ptr<Buffer> input_buffer_;
        std::shared_ptr<Buffer> output_buffer_;
        bool closed_;
    };
}

#endif