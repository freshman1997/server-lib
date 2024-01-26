#ifndef __CONNECTION_H__
#define __CONNECTION_H__
#include "net/handler/select_handler.h"
#include <memory>

class Buffer;

namespace net
{
    class InetAddress;
    class Channel;
    class ConnectionHandler;

    enum class ConnectionType
    {
        TCP,
        UDP
    };

    // 表示一个连接
    class Connection : public SelectHandler
    {
    public:
        virtual bool is_connected() = 0;

        virtual const InetAddress & get_remote_address() const = 0;

        virtual const InetAddress & get_local_address() const = 0;

        virtual std::shared_ptr<Buffer> get_input_buff() = 0;

        virtual std::shared_ptr<Buffer> get_output_buff() = 0;

        virtual void send(std::shared_ptr<Buffer> buff) = 0;

        virtual void send() = 0;

        // 丢弃所有未发送的数据
        virtual void abort() = 0;

        // 发送完数据后返回
        virtual void close() = 0;

        virtual ConnectionType get_conn_type() = 0;

        virtual Channel * get_channel() = 0;
        
        virtual void set_connection_handler(ConnectionHandler *handler) = 0;
    };
}

#endif