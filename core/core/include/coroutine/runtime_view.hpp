#ifndef __YUAN_COROUTINE_RUNTIME_VIEW_HPP__
#define __YUAN_COROUTINE_RUNTIME_VIEW_HPP__

#include "coroutine/runtime_view.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/datagram_io_awaitable.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"

namespace yuan::coroutine
{

    inline AsyncReadAwaiter RuntimeView::read(net::Connection * conn, uint32_t timeout_ms) const noexcept
    {
        return async_read(*this, conn, timeout_ms);
    }

    inline AsyncWriteAwaiter RuntimeView::write(net::Connection * conn, const ::yuan::buffer::ByteBuffer & buf,
                                                uint32_t timeout_ms) const noexcept
    {
        return async_write(*this, conn, buf, timeout_ms);
    }

    inline AsyncFlushAwaiter RuntimeView::flush(net::Connection * conn, uint32_t timeout_ms) const noexcept
    {
        return async_flush(*this, conn, timeout_ms);
    }

    inline AsyncCloseAwaiter RuntimeView::close(net::Connection * conn) const noexcept
    {
        return async_close(*this, conn);
    }

    inline AsyncSslHandshakeAwaiter RuntimeView::ssl_handshake(net::Connection * conn, uint32_t timeout_ms) const noexcept
    {
        return async_ssl_handshake(*this, conn, timeout_ms);
    }

    inline AsyncReceiveFromAwaiter RuntimeView::receive_from(net::Connection * conn, uint32_t timeout_ms) const noexcept
    {
        return async_receive_from(*this, conn, timeout_ms);
    }

    inline void RuntimeView::register_connection(net::Connection * conn, net::ConnectionHandler * handler) const
    {
        if (!event_loop_ || !conn) {
            return;
        }
        conn->set_connection_handler(handler);
        conn->set_event_handler(event_loop_);
        event_loop_->on_new_connection(conn);
    }

    inline void RuntimeView::update_channel(net::Channel * channel) const
    {
        if (event_loop_ && channel) {
            event_loop_->update_channel(channel);
        }
    }

} // namespace yuan::coroutine

#endif
