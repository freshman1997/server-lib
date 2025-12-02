#ifndef __CONNECTION_H__
#define __CONNECTION_H__
#include "net/handler/select_handler.h"
#include "net/secuity/ssl_handler.h"
#include "buffer/pool.h"
#include "buffer/linked_buffer.h"
#include <cassert>
#include <memory>
#include <wincon.h>

namespace yuan::buffer
{
    class Buffer;
    class LinkedBuffer;
}

namespace yuan::net
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

    static constexpr size_t DEFAULT_MAX_PACKET_SIZE = 1024 * 1024 * 5; // 5mb

    // 表示一个连接
    class Connection : public SelectHandler
    {
    public:
        Connection() : max_packet_size_(0), input_buffer_(nullptr)
        {
            set_max_packet_size(DEFAULT_MAX_PACKET_SIZE);
        }

        virtual ~Connection()
        {
            buffer::BufferedPool::get_instance()->free(input_buffer_);
            input_buffer_ = nullptr;
        }

        Connection(const Connection &) = delete;
        Connection & operator=(const Connection &) = delete;
        Connection(Connection &&) = delete;
        Connection & operator=(Connection &&) = delete;

    public:
        virtual ConnectionState get_connection_state() = 0;

        virtual bool is_connected() = 0;

        virtual const InetAddress & get_remote_address() = 0;

        virtual void write(buffer::Buffer *buff) = 0;

        virtual void write_and_flush(buffer::Buffer *buff) = 0;

        virtual void flush() = 0;

        // 丢弃所有未发送的数据
        virtual void abort() = 0;

        // 发送完数据后返回
        virtual void close() = 0;

        virtual ConnectionType get_conn_type() = 0;

        virtual Channel * get_channel() = 0;
        
        virtual void set_connection_handler(ConnectionHandler *handler) = 0;

        virtual ConnectionHandler * get_connection_handler() = 0;

        virtual void forward(Connection *conn) = 0;

        virtual void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler) = 0;

    public:
        buffer::Buffer * get_input_buff(bool take = false)
        {
            if (!take) {
                return input_buffer_;
            }

            auto buf = input_buffer_;
            set_max_packet_size(max_packet_size_);
            return buf;
        }
        
        buffer::LinkedBuffer * get_output_linked_buffer()
        {
            return &output_buffer_;
        }

        void set_max_packet_size(size_t size)
        {
            assert(size > 0 && size <= 1024 * 1024 * 100); // 100mb
            if (size == max_packet_size_) {
                return;
            }

            max_packet_size_ = size;
            if (!input_buffer_) {
                input_buffer_ = buffer::BufferedPool::get_instance()->allocate(size);
            } else {
                input_buffer_->resize(size);
            }
        }

    protected:
        size_t max_packet_size_;
        buffer::Buffer *input_buffer_;
        buffer::LinkedBuffer output_buffer_;
    };
}

#endif