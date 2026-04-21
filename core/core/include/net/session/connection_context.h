#ifndef __YUAN_NET_SESSION_CONNECTION_CONTEXT_H__
#define __YUAN_NET_SESSION_CONNECTION_CONTEXT_H__

#include "buffer/byte_buffer.h"
#include "net/connection/connection.h"
#include "net/connection/connection_ref.h"
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
            : conn_ref_(conn)
        {
        }

        explicit ConnectionContext(std::shared_ptr<Connection> conn)
            : conn_ref_(std::move(conn))
        {
        }

        ::yuan::buffer::ByteBuffer take_input_byte_buffer()
        {
            return conn_ref_->take_input_byte_buffer();
        }

        ::yuan::buffer::ByteBuffer get_input_byte_buffer() const
        {
            return conn_ref_->get_input_byte_buffer();
        }

        void clear_input_buffer()
        {
            conn_ref_->clear_input_buffer();
        }

        void write(const ::yuan::buffer::ByteBuffer &buffer)
        {
            conn_ref_->write(buffer);
        }

        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer)
        {
            conn_ref_->write_and_flush(buffer);
        }

        void append_output(std::string_view text)
        {
            conn_ref_->append_output(text);
        }

        void append_output(const char *data, std::size_t size)
        {
            conn_ref_->append_output(data, size);
        }

        void append_output(const ::yuan::buffer::ByteBuffer &buffer)
        {
            conn_ref_->append_output(buffer);
        }

        void flush()
        {
            conn_ref_->flush();
        }

        void close()
        {
            conn_ref_->close();
        }

        void abort()
        {
            conn_ref_->abort();
        }

        bool is_connected() const
        {
            return conn_ref_ && conn_ref_->is_connected();
        }

        const InetAddress &get_remote_address() const
        {
            return conn_ref_->get_remote_address();
        }

        uintptr_t connection_id() const
        {
            return reinterpret_cast<uintptr_t>(conn_ref_.get());
        }

        void set_max_packet_size(size_t size)
        {
            conn_ref_->set_max_packet_size(size);
        }

        ConnectionState get_connection_state() const
        {
            return conn_ref_ ? conn_ref_->get_connection_state() : ConnectionState::closed;
        }

        Connection *native_handle() const
        {
            return conn_ref_.get();
        }

        std::shared_ptr<Connection> shared_handle() const
        {
            return conn_ref_.owner();
        }

        explicit operator bool() const noexcept
        {
            return static_cast<bool>(conn_ref_);
        }

    private:
        friend class StreamServerSession;
        friend class StreamClientSession;
        friend class DatagramServerSession;
        friend class DatagramClientSession;

        ConnectionRef conn_ref_;
    };

} // namespace yuan::net

#endif
