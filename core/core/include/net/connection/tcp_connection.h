#ifndef __TCP_CONNECTION_H__
#define __TCP_CONNECTION_H__

#include "connection.h"
#include "stream_transport.h"
#include "net/handler/select_handler.h"
#include <memory>
#include <string>

namespace yuan::net
{
    class TcpConnection : public Connection, public StreamTransport
    {
    public:
        TcpConnection(std::string ip, int port, int fd);

        explicit TcpConnection(Socket *scok);

        virtual ~TcpConnection();

        virtual ConnectionState get_connection_state() const override;

        virtual bool is_connected() const override;

        virtual const InetAddress &get_remote_address() const override;
        virtual const InetAddress &get_local_address() const override;

        virtual void write(const ::yuan::buffer::ByteBuffer &buffer);

        virtual void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer);

        virtual void flush();

        // 丢弃所有未发送的数据
        virtual void abort();

        // 发送完数据后返回
        virtual void close();

        Channel *stream_channel() const override;

        virtual void set_connection_handler(ConnectionHandler *connectionHandler);

        virtual ConnectionHandler *get_connection_handler() const override;

        virtual void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler);

        virtual std::shared_ptr<SSLHandler> get_ssl_handler() const override
        {
            return ssl_handler_;
        }

        virtual bool is_ssl_handshaking() const override
        {
            return ssl_handshaking_;
        }

        virtual void set_ssl_handshaking(bool handshaking) override
        {
            ssl_handshaking_ = handshaking;
        }

        virtual void set_ssl_handshake_callback(SslHandshakeCallback callback) override
        {
            ssl_handshake_callback_ = std::move(callback);
        }

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
        bool is_closing_;
        bool ssl_handshaking_ = false;
        SslHandshakeCallback ssl_handshake_callback_;
    };
}

#endif
