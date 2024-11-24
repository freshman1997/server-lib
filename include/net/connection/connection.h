#ifndef __CONNECTION_H__
#define __CONNECTION_H__
#include "net/handler/select_handler.h"
#include "net/secuity/ssl_handler.h"
#include <functional>
#include <memory>

class Buffer;
class LinkedBuffer;

namespace net
{
    class InetAddress;
    class Channel;
    class ConnectionHandler;
    class Socket;

    enum class ConnectionType
    {
        TCP,
        UDP
    };

    enum class ConnectionState
    {
        connecting,         // 建立连接中
        connected,          // 已连接
        closing,            // 关闭连接中
        closed              // 已关闭
    };

    // 表示一个连接
    class Connection : public SelectHandler
    {
    public:
        virtual ConnectionState get_connection_state() = 0;

        virtual bool is_connected() = 0;

        virtual const InetAddress & get_remote_address() = 0;

        virtual Buffer * get_input_buff(bool take = false) = 0;

        virtual Buffer * get_output_buff(bool take = false) = 0;

        virtual void write(Buffer *buff) = 0;

        virtual void write_and_flush(Buffer *buff) = 0;

        virtual void send() = 0;

        // 丢弃所有未发送的数据
        virtual void abort() = 0;

        // 发送完数据后返回
        virtual void close() = 0;

        virtual ConnectionType get_conn_type() = 0;

        virtual Channel * get_channel() = 0;
        
        virtual void set_connection_handler(ConnectionHandler *handler) = 0;

        virtual ConnectionHandler * get_connection_handler() = 0;

        virtual void process_input_data(std::function<bool (Buffer *buff)>, bool clear = true) = 0;

        virtual LinkedBuffer * get_input_linked_buffer() = 0;

        virtual LinkedBuffer * get_output_linked_buffer() = 0;

        virtual void forward(Connection *conn) = 0;

        virtual void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler) = 0;
    };
}

#endif