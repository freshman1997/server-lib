#ifndef __TCP_CONNECTION_H__
#define __TCP_CONNECTION_H__
#include <memory>

#include "connection.h"
#include "net/channel/channel.h"
#include "net/handler/select_handler.h"
#include "net/socket/inet_address.h"
#include "buff/buffer.h"

namespace net
{
    class TcpConnection : public Connection, public SelectHandler
    {
    public:
        TcpConnection(std::shared_ptr<net::InetAddress> remoteAddr, std::shared_ptr<net::InetAddress> localAddr, std::shared_ptr<net::Channel> channel);

        virtual bool is_connected();

        virtual const InetAddress & get_remote_address() const;

        virtual const InetAddress & get_local_address() const;

        virtual void send(Buffer *buff);

        // 丢弃所有未发送的数据
        virtual void abort();

        // 发送完数据后返回
        virtual void close();

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual int get_fd();

    private:
        std::shared_ptr<net::InetAddress> remote_addr_;
        std::shared_ptr<net::InetAddress> local_addr_;
        std::shared_ptr<net::Channel> channel_;
        std::shared_ptr<Buffer> input_buffer_;
        std::shared_ptr<Buffer> output_buffer_;
    };
}

#endif