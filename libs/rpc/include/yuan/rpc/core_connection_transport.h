#ifndef YUAN_RPC_CORE_CONNECTION_TRANSPORT_H
#define YUAN_RPC_CORE_CONNECTION_TRANSPORT_H

#include "transport.h"

#include "buffer/byte_buffer.h"
#include "net/connection/connection.h"
#include "net/session/connection_context.h"

#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace yuan::rpc
{
    class CoreConnectionTransport final : public IFrameTransport
    {
    public:
        CoreConnectionTransport() = default;

        explicit CoreConnectionTransport(yuan::net::ConnectionContext context)
            : context_(std::move(context))
        {
        }

        explicit CoreConnectionTransport(std::shared_ptr<yuan::net::Connection> connection)
            : context_(std::move(connection))
        {
        }

        void bind(yuan::net::ConnectionContext context)
        {
            context_ = std::move(context);
        }

        bool send_frame(const Bytes &frame) override
        {
            if (!is_open()) {
                return false;
            }
            yuan::buffer::ByteBuffer buffer(frame.size());
            buffer.append(frame.data(), frame.size());
            context_.write_and_flush(buffer);
            return true;
        }

        bool is_open() const override
        {
            return context_ && context_.is_connected();
        }

        void close() override
        {
            if (context_) {
                context_.close();
            }
            if (close_callback_) {
                close_callback_();
            }
        }

        void set_receive_callback(ReceiveCallback callback) override
        {
            receive_callback_ = std::move(callback);
        }

        void set_close_callback(CloseCallback callback) override
        {
            close_callback_ = std::move(callback);
        }

        void on_readable(yuan::net::ConnectionContext &context)
        {
            auto input = context.take_input_byte_buffer();
            const auto span = input.readable_span();
            if (!span.empty() && receive_callback_) {
                receive_callback_(reinterpret_cast<const std::uint8_t *>(span.data()), span.size());
            }
        }

        void on_closed()
        {
            if (close_callback_) {
                close_callback_();
            }
        }

    private:
        yuan::net::ConnectionContext context_;
        ReceiveCallback receive_callback_;
        CloseCallback close_callback_;
    };
}

#endif
