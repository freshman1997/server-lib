#ifndef __YUAN_NET_SESSION_CONNECTION_CONTEXT_H__
#define __YUAN_NET_SESSION_CONNECTION_CONTEXT_H__

#include "buffer/byte_buffer.h"
#include "net/connection/connection.h"
#include "net/connection/connection_handle.h"
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
            : conn_handle_(make_handle(conn))
        {
        }

        explicit ConnectionContext(std::shared_ptr<Connection> conn)
            : conn_handle_(ConnectionHandle(std::move(conn)))
        {
        }

        ::yuan::buffer::ByteBuffer take_input_byte_buffer()
        {
            return conn_handle_->take_input_byte_buffer();
        }

        ::yuan::buffer::ByteBuffer get_input_byte_buffer() const
        {
            return conn_handle_->get_input_byte_buffer();
        }

        void clear_input_buffer()
        {
            conn_handle_->clear_input_buffer();
        }

        void write(const ::yuan::buffer::ByteBuffer &buffer)
        {
            conn_handle_->write(buffer);
        }

        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer)
        {
            conn_handle_->write_and_flush(buffer);
        }

        void append_output(std::string_view text)
        {
            conn_handle_->append_output(text);
        }

        void append_output(const char *data, std::size_t size)
        {
            conn_handle_->append_output(data, size);
        }

        void append_output(const ::yuan::buffer::ByteBuffer &buffer)
        {
            conn_handle_->append_output(buffer);
        }

        void flush()
        {
            conn_handle_->flush();
        }

        void close()
        {
            conn_handle_->close();
        }

        void abort()
        {
            conn_handle_->abort();
        }

        bool is_connected() const
        {
            return conn_handle_ && conn_handle_->is_connected();
        }

        const InetAddress &get_remote_address() const
        {
            return conn_handle_->get_remote_address();
        }

        uintptr_t connection_id() const
        {
            return reinterpret_cast<uintptr_t>(conn_handle_.get());
        }

        void set_max_packet_size(size_t size)
        {
            conn_handle_->set_max_packet_size(size);
        }

        ConnectionState get_connection_state() const
        {
            return conn_handle_ ? conn_handle_->get_connection_state() : ConnectionState::closed;
        }

        Connection *native_handle() const
        {
            return conn_handle_.get();
        }

        std::shared_ptr<Connection> shared_handle() const
        {
            return conn_handle_.shared();
        }

        explicit operator bool() const noexcept
        {
            return static_cast<bool>(conn_handle_);
        }

    private:
        friend class StreamServerSession;
        friend class StreamClientSession;
        friend class DatagramServerSession;
        friend class DatagramClientSession;

        static ConnectionHandle make_handle(Connection *conn) noexcept
        {
            if (!conn) {
                return {};
            }

            try {
                return ConnectionHandle(conn->shared_from_this());
            } catch (const std::bad_weak_ptr &) {
                return {};
            }
        }

        ConnectionHandle conn_handle_;
    };

} // namespace yuan::net

#endif
