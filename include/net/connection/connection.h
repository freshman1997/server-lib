#ifndef __CONNECTION_H__
#define __CONNECTION_H__

class Buffer;

namespace net
{
    class InetAddress;

    // 表示一个连接
    class Connection
    {
    public:
        virtual bool is_connected() = 0;

        virtual const InetAddress & get_remote_address() const = 0;

        virtual const InetAddress & get_local_address() const = 0;

        virtual void send(Buffer *buff) = 0;

        // 丢弃所有未发送的数据
        virtual void abort() = 0;

        // 发送完数据后返回
        virtual void close() = 0;
    };
}

#endif