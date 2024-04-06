#ifndef __TCP_CONNECTION_H__
#define __TCP_CONNECTION_H__

#include "buffer/linked_buffer.h"
#include "connection.h"
#include "../channel/channel.h"
#include "../handler/select_handler.h"
#include "../socket/inet_address.h"
#include "buffer/buffer.h"

namespace net
{
    class AcceptHandler;
    class TcpConnectionHandler;

    class TcpConnection : public Connection
    {
    public:
        TcpConnection(const std::string ip, int port, int fd);

        TcpConnection(Socket *scok);

        virtual ~TcpConnection();

        virtual bool is_connected();

        virtual const InetAddress & get_remote_address();

        virtual Buffer * get_input_buff(bool take = false);

        virtual Buffer * get_output_buff(bool take = false);

        virtual void send(Buffer * buff);

        virtual void send();

        // 丢弃所有未发送的数据
        virtual void abort();

        // 发送完数据后返回
        virtual void close();

        virtual ConnectionType get_conn_type();

        virtual Channel * get_channel();

        virtual void set_connection_handler(ConnectionHandler *connectionHandler);

        virtual const Socket * get_scoket();

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);

    private:
        void do_close();

        void init();

    private:
        Channel channel_;
        Socket *socket_;
        AcceptHandler *acceptHandler_;
        ConnectionHandler *connectionHandler_;
        EventHandler *eventHandler_;
        LinkedBuffer input_buffer_;
        LinkedBuffer output_buffer_;
        bool closed_;
    };
}

#endif