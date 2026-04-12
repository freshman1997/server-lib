#ifndef __TCP_CONNECTION_H__
#define __TCP_CONNECTION_H__

#include "connection.h"
#include "stream_transport.h"
#include "net/handler/select_handler.h"
#include <memory>

namespace yuan::net
{
    class TcpConnection : public Connection, public StreamTransport
    {
    public:
        TcpConnection(std::string ip, int port, int fd);

        explicit TcpConnection(Socket *scok);

        virtual ~TcpConnection();

        virtual ConnectionState get_connection_state();

        virtual bool is_connected();

        virtual const InetAddress & get_remote_address();

        virtual void write(const ::yuan::buffer::ByteBuffer &buffer);

        virtual void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer);

        virtual void flush();

        // 丢弃所有未发送的数据
        virtual void abort();

        // 发送完数据后返回
        virtual void close();

        Channel *stream_channel() override;

        virtual void set_connection_handler(ConnectionHandler *connectionHandler);

        virtual ConnectionHandler * get_connection_handler();

        virtual void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler);

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);

    protected:
        void do_close();

        void init();

    protected:
        ConnectionState state_;
        std::unique_ptr<Channel> channel_;
        std::unique_ptr<Socket> socket_;
        ConnectionHandler *connectionHandler_;
        EventHandler *eventHandler_;
        std::shared_ptr<SSLHandler> ssl_handler_;
        bool is_closing_;  // 防止重复调用 do_close 导致 double-free
    };
}

#endif
