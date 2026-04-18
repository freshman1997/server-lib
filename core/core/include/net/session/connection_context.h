#ifndef __YUAN_NET_SESSION_CONNECTION_CONTEXT_H__
#define __YUAN_NET_SESSION_CONNECTION_CONTEXT_H__

#include "buffer/byte_buffer.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"

#include <memory>
#include <cstdint>
#include <string_view>

namespace yuan::net
{

    class ConnectionContext
    {
    public:
        ConnectionContext() = default;

        explicit ConnectionContext(Connection *conn)
            : conn_(conn, [](Connection *) {})
        {
        }

        explicit ConnectionContext(std::shared_ptr<Connection> conn)
            : conn_(std::move(conn))
        {
        }

        ::yuan::buffer::ByteBuffer take_input_byte_buffer()
        {
            return conn_->take_input_byte_buffer();
        }

        ::yuan::buffer::ByteBuffer get_input_byte_buffer() const
        {
            return conn_->get_input_byte_buffer();
        }

        void clear_input_buffer()
        {
            conn_->clear_input_buffer();
        }

        void write(const ::yuan::buffer::ByteBuffer &buffer)
        {
            conn_->write(buffer);
        }

        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer)
        {
            conn_->write_and_flush(buffer);
        }

        void append_output(std::string_view text)
        {
            conn_->append_output(text);
        }

        void append_output(const char *data, std::size_t size)
        {
            conn_->append_output(data, size);
        }

        void append_output(const ::yuan::buffer::ByteBuffer &buffer)
        {
            conn_->append_output(buffer);
        }

        void flush()
        {
            conn_->flush();
        }

        void close()
        {
            conn_->close();
        }

        void abort()
        {
            conn_->abort();
        }

        bool is_connected() const
        {
            return conn_ && conn_->is_connected();
        }

        const InetAddress &get_remote_address() const
        {
            return conn_->get_remote_address();
        }

        uintptr_t connection_id() const
        {
            return reinterpret_cast<uintptr_t>(conn_.get());
        }

        void set_max_packet_size(size_t size)
        {
            conn_->set_max_packet_size(size);
        }

        ConnectionState get_connection_state() const
        {
            return conn_ ? conn_->get_connection_state() : ConnectionState::closed;
        }

        Connection *native_handle() const
        {
            return conn_.get();
        }

        std::shared_ptr<Connection> shared_handle() const
        {
            return conn_;
        }

        explicit operator bool() const noexcept
        {
            return conn_ != nullptr;
        }

    private:
        friend class StreamServerSession;
        friend class StreamClientSession;
        friend class DatagramServerSession;
        friend class DatagramClientSession;

        std::shared_ptr<Connection> conn_;
    };

} // namespace yuan::net

#endif
