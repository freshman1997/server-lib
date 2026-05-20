#include "connection/ssh_forwarded_tcpip_handler.h"

#include "connection/ssh_channel.h"
#include "ssh_session.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/runtime_view.h"

namespace yuan::net::ssh
{
    SshForwardedTcpipHandler::SshForwardedTcpipHandler(SshSession * session,
                                                       std::shared_ptr<net::Connection> accepted_conn)
        : session_(session), accepted_conn_(std::move(accepted_conn))
    {
    }

    SshForwardedTcpipHandler::~SshForwardedTcpipHandler()
    {
        closed_.store(true, std::memory_order_relaxed);
        if (accepted_conn_) {
            accepted_conn_->close();
            accepted_conn_.reset();
        }
    }

    void SshForwardedTcpipHandler::on_open(SshChannel * channel)
    {
        if (!accepted_conn_ || !session_) {
            channel->set_state(SshChannel::State::closing);
            return;
        }

        auto rv = session_->runtime();
        if (!rv.event_loop()) {
            channel->set_state(SshChannel::State::closing);
            return;
        }

        relay_active_.store(true, std::memory_order_relaxed);
        auto task = [this, channel]() -> coroutine::Task<void> {
            co_await relay_from_accepted(channel);
        }();
        task.resume();
        task.detach();
    }

    void SshForwardedTcpipHandler::on_data(SshChannel *, const std::vector<uint8_t> & data)
    {
        if (!accepted_conn_ || closed_.load(std::memory_order_relaxed)) {
            return;
        }
        ::yuan::buffer::ByteBuffer buf(data.size());
        buf.append(data.data(), data.size());
        accepted_conn_->write_and_flush(buf);
    }

    void SshForwardedTcpipHandler::on_eof(SshChannel *)
    {
        if (accepted_conn_) {
            accepted_conn_->shutdown_write();
        }
    }

    void SshForwardedTcpipHandler::on_close(SshChannel *)
    {
        closed_.store(true, std::memory_order_relaxed);
        if (accepted_conn_) {
            accepted_conn_->close();
            accepted_conn_.reset();
        }
    }

    void SshForwardedTcpipHandler::on_window_adjust(SshChannel *, uint32_t)
    {
    }

    coroutine::Task<void> SshForwardedTcpipHandler::relay_from_accepted(SshChannel * channel)
    {
        if (!session_) {
            co_return;
        }

        auto rv = session_->runtime();
        while (!closed_.load(std::memory_order_relaxed)) {
            if (!accepted_conn_ || !accepted_conn_->is_connected()) {
                break;
            }

            auto result = co_await coroutine::async_read(rv, accepted_conn_);
            if (result.status != coroutine::IoStatus::success) {
                break;
            }

            if (result.data.readable_bytes() > 0) {
                auto span = result.data.readable_span();
                std::vector<uint8_t> data(span.data(), span.data() + span.size());
                channel->enqueue_data(std::move(data));
                session_->flush_channel_pending_data();
            }
        }

        closed_.store(true, std::memory_order_relaxed);
        if (channel->state() == SshChannel::State::open) {
            channel->set_state(SshChannel::State::eof);
        }
    }
}
