#ifndef __CONNECTION_H__
#define __CONNECTION_H__

#include "buffer/byte_buffer.h"
#include "buffer/buffer_chain.h"
#include "net/handler/select_handler.h"
#include "net/secuity/ssl_handler.h"

#include <cassert>
#include <functional>
#include <memory>
#include <string_view>

namespace yuan::buffer
{
    class BufferChain;
}

namespace yuan::net
{
    namespace buffer
    {
        using ::yuan::buffer::BufferChain;
    }
}

namespace yuan::net
{
    class InetAddress;
    class Channel;
    class ConnectionHandler;
    class Socket;

    enum class ConnectionState {
        connecting,
        connected,
        closing,
        closed
    };

    static constexpr size_t DEFAULT_MAX_PACKET_SIZE = 1024 * 1024 * 5;

    using SslHandshakeCallback = std::function<void(bool success)>;

    class Connection : public SelectHandler, public std::enable_shared_from_this<Connection>
    {
    public:
        Connection()
            : max_packet_size_(0)
        {
            set_max_packet_size(DEFAULT_MAX_PACKET_SIZE);
        }

        virtual ~Connection() = default;

        Connection(const Connection &) = delete;
        Connection &operator=(const Connection &) = delete;
        Connection(Connection &&) = delete;
        Connection &operator=(Connection &&) = delete;

        virtual ConnectionState get_connection_state() const = 0;
        virtual bool is_connected() const = 0;
        virtual const InetAddress &get_remote_address() const = 0;
        virtual const InetAddress &get_local_address() const = 0;

        virtual void write(const ::yuan::buffer::ByteBuffer &buffer) = 0;

        virtual void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer) = 0;

        virtual void flush() = 0;
        virtual void abort() = 0;
        virtual void close() = 0;
        virtual bool shutdown_write()
        {
            return false;
        }
        virtual bool input_shutdown() const
        {
            return false;
        }

        virtual void set_connection_handler(std::shared_ptr<ConnectionHandler> handler) = 0;
        virtual ConnectionHandler *get_connection_handler() const = 0;
        virtual std::shared_ptr<ConnectionHandler> get_connection_handler_owner() const
        {
            return nullptr;
        }
        virtual void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler) = 0;

        virtual std::shared_ptr<SSLHandler> get_ssl_handler() const
        {
            return nullptr;
        }

        virtual bool is_ssl_handshaking() const
        {
            return false;
        }

        virtual void set_ssl_handshaking(bool handshaking)
        {
            (void)handshaking;
        }

        virtual void set_ssl_handshake_callback(SslHandshakeCallback callback)
        {
            (void)callback;
        }

        ::yuan::buffer::ByteBuffer get_input_byte_buffer() const
        {
            return input_buffer_.copy_readable();
        }

        size_t input_readable_bytes() const noexcept
        {
            return input_buffer_.readable_bytes();
        }

        ::yuan::buffer::ByteBuffer take_input_byte_buffer()
        {
            auto byte_buffer = input_buffer_.copy_readable();
            input_buffer_.clear();
            return byte_buffer;
        }

        void clear_input_buffer()
        {
            input_buffer_.clear();
            input_buffer_.reserve(max_packet_size_);
        }

        std::size_t output_readable_bytes() const noexcept
        {
            return output_buffer_.readable_bytes();
        }

        void append_output(std::string_view text)
        {
            if (!text.empty()) {
                ensure_output_chunk()->append(text);
            }
        }

        void append_output(const char *data, std::size_t size)
        {
            if (data && size > 0) {
                ensure_output_chunk()->append(data, size);
            }
        }

        void append_output(const ::yuan::buffer::ByteBuffer &buffer)
        {
            const auto span = buffer.readable_span();
            if (!span.empty()) {
                ensure_output_chunk()->append(span);
            }
        }

        void set_max_packet_size(size_t size)
        {
            assert(size > 0 && size <= 1024 * 1024 * 100);
            if (size == max_packet_size_) {
                return;
            }

            max_packet_size_ = size;
            input_buffer_.reserve(size);
        }

    protected:
        ::yuan::buffer::ByteBuffer *ensure_output_chunk(std::size_t capacity = ::yuan::buffer::ByteBuffer::kDefaultCapacity)
        {
            auto *chunk = output_buffer_.back();
            if (!chunk) {
                chunk = output_buffer_.emplace_back(capacity);
            }
            return chunk;
        }

        void replace_input_buffer(::yuan::buffer::ByteBuffer buffer)
        {
            input_buffer_ = std::move(buffer);
            max_packet_size_ = std::max<size_t>(max_packet_size_, input_buffer_.capacity());
        }
        size_t max_packet_size_;
        ::yuan::buffer::ByteBuffer input_buffer_;
        ::yuan::buffer::BufferChain output_buffer_;
    };

    using ConnectionPtr = std::shared_ptr<Connection>;
}

#endif
