#include "connection/ssh_direct_tcpip_handler.h"
#include "ssh_session.h"
#include "ssh_handler.h"
#include "coroutine/connect_awaitable.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/runtime_view.h"
#include "net/connection/connection.h"
#include "logger.h"

namespace yuan::net::ssh
{
    SshDirectTcpipHandler::SshDirectTcpipHandler(SshSession * session,
                                                 const std::string & target_host,
                                                 uint16_t target_port)
        : session_(session), target_host_(target_host), target_port_(target_port)
    {
    }

    SshDirectTcpipHandler::~SshDirectTcpipHandler()
    {
        closed_.store(true, std::memory_order_relaxed);
        if (target_conn_) {
            target_conn_->close();
            target_conn_ = nullptr;
        }
    }

    void SshDirectTcpipHandler::on_open(SshChannel * channel)
    {
        auto rv = session_->runtime();
        if (!rv.event_loop()) {
            channel->set_state(SshChannel::State::closing);
            return;
        }

        auto connect_task = [
            this,
            channel,
            rv
        ]()->coroutine::Task<void>
        {
            auto result = co_await coroutine::async_connect(
                rv, target_host_, target_port_, 10000);

            if (result.result != coroutine::ConnectResult::success || !result.connection) {
                channel->set_state(SshChannel::State::closing);
                co_return;
            }

            target_conn_ = result.connection;
            rv.register_connection(target_conn_, nullptr);
            if (auto *stream = dynamic_cast<StreamTransport *>(target_conn_)) {
                if (auto *ch = stream->stream_channel()) {
                    rv.update_channel(ch);
                }
            }

            relay_active_.store(true, std::memory_order_relaxed);
            co_await relay_from_target(channel);
        };

        auto t = connect_task();
        t.resume();
        t.detach();
    }

    void SshDirectTcpipHandler::on_data(SshChannel * channel, const std::vector<uint8_t> & data)
    {
        if (!target_conn_ || closed_.load(std::memory_order_relaxed)) {
            return;
        }

        ::yuan::buffer::ByteBuffer buf(data.size());
        buf.append(data.data(), data.size());
        target_conn_->write_and_flush(buf);
    }

    void SshDirectTcpipHandler::on_eof(SshChannel * channel)
    {
        if (target_conn_) {
            target_conn_->close();
            target_conn_ = nullptr;
        }
    }

    void SshDirectTcpipHandler::on_close(SshChannel * channel)
    {
        closed_.store(true, std::memory_order_relaxed);
        if (target_conn_) {
            target_conn_->close();
            target_conn_ = nullptr;
        }
    }

    void SshDirectTcpipHandler::on_window_adjust(SshChannel * channel, uint32_t bytes_to_add)
    {
    }

    coroutine::Task<void> SshDirectTcpipHandler::relay_from_target(SshChannel * channel)
    {
        auto rv = session_->runtime();

        while (!closed_.load(std::memory_order_relaxed)) {
            if (!target_conn_ || !target_conn_->is_connected()) {
                break;
            }

            auto result = co_await coroutine::async_read(rv, target_conn_);
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
