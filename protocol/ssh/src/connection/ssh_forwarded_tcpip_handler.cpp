#include "connection/ssh_forwarded_tcpip_handler.h"

#include "connection/ssh_channel.h"
#include "connection/ssh_connection_manager.h"
#include "ssh_session.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/runtime_view.h"

namespace yuan::net::ssh
{
    SshForwardedTcpipHandler::SshForwardedTcpipHandler(SshSession * session,
                                                        std::shared_ptr<net::Connection> accepted_conn)
        : session_(session), state_(std::make_shared<SshForwardedTcpipHandler::SharedState>())
    {
        state_->accepted_conn = std::move(accepted_conn);
    }

    SshForwardedTcpipHandler::~SshForwardedTcpipHandler()
    {
        if (state_) {
            state_->closed.store(true, std::memory_order_relaxed);
            if (state_->accepted_conn) {
                state_->accepted_conn->close();
                state_->accepted_conn.reset();
            }
        }
    }

    void SshForwardedTcpipHandler::on_open(SshChannel * channel)
    {
        if (!state_ || !state_->accepted_conn || !session_) {
            channel->set_state(SshChannel::State::closing);
            return;
        }

        auto rv = session_->runtime();
        if (!rv.event_loop()) {
            channel->set_state(SshChannel::State::closing);
            return;
        }

        auto task = relay_from_accepted(session_, channel->local_id(), state_);
        task.resume();
        task.detach();
    }

    void SshForwardedTcpipHandler::on_data(SshChannel *, const std::vector<uint8_t> & data)
    {
        if (!state_ || !state_->accepted_conn || state_->closed.load(std::memory_order_relaxed)) {
            return;
        }
        ::yuan::buffer::ByteBuffer buf(data.size());
        buf.append(data.data(), data.size());
        state_->accepted_conn->write_and_flush(buf);
    }

    void SshForwardedTcpipHandler::on_eof(SshChannel *)
    {
        if (state_ && state_->accepted_conn) {
            state_->accepted_conn->shutdown_write();
        }
    }

    void SshForwardedTcpipHandler::on_close(SshChannel *)
    {
        if (state_) {
            state_->closed.store(true, std::memory_order_relaxed);
            if (state_->accepted_conn) {
                state_->accepted_conn->close();
                state_->accepted_conn.reset();
            }
        }
    }

    void SshForwardedTcpipHandler::on_window_adjust(SshChannel *, uint32_t)
    {
    }

    coroutine::Task<void> SshForwardedTcpipHandler::relay_from_accepted(SshSession * session,
                                                                        uint32_t local_channel_id,
                                                                        std::shared_ptr<SshForwardedTcpipHandler::SharedState> state)
    {
        if (!session || !state) {
            co_return;
        }

        auto rv = session->runtime();
        while (!state->closed.load(std::memory_order_relaxed)) {
            auto accepted_conn = state->accepted_conn;
            if (!accepted_conn || !accepted_conn->is_connected()) {
                break;
            }

            auto result = co_await coroutine::async_read(rv, accepted_conn);
            if (state->closed.load(std::memory_order_relaxed)) {
                break;
            }
            if (result.status != coroutine::IoStatus::success) {
                break;
            }

            if (result.data.readable_bytes() > 0) {
                auto *channel = session->connection_manager().find_channel(local_channel_id);
                if (!channel || channel->state() != SshChannel::State::open) {
                    break;
                }
                auto span = result.data.readable_span();
                std::vector<uint8_t> data(span.data(), span.data() + span.size());
                channel->enqueue_data(std::move(data));
                session->flush_channel_pending_data();
            }
        }

        state->closed.store(true, std::memory_order_relaxed);
    }
}
