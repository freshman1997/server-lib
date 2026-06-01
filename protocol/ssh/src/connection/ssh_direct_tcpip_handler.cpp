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
    std::shared_ptr<net::Connection> SshDirectTcpipHandler::get_target_connection(
        const std::shared_ptr<SshDirectTcpipHandler::SharedState> &state)
    {
        if (!state) {
            return {};
        }
        std::lock_guard<std::mutex> lock(state->conn_mutex);
        return state->target_conn;
    }

    void SshDirectTcpipHandler::close_target_connection(const std::shared_ptr<net::Connection> &conn)
    {
        if (!conn) {
            return;
        }
        conn->close();
    }

    SshDirectTcpipHandler::SshDirectTcpipHandler(SshSession * session,
                                                 const std::string & target_host,
                                                 uint16_t target_port)
        : session_(session), target_host_(target_host), target_port_(target_port),
          state_(std::make_shared<SshDirectTcpipHandler::SharedState>())
    {
    }

    SshDirectTcpipHandler::~SshDirectTcpipHandler()
    {
        state_->closed.store(true, std::memory_order_relaxed);
        std::shared_ptr<net::Connection> conn;
        {
            std::lock_guard<std::mutex> lock(state_->conn_mutex);
            conn = std::move(state_->target_conn);
        }
        close_target_connection(conn);
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
            rv,
            state = state_,
            channel_remote_id = channel->remote_id()
        ]()->coroutine::Task<void>
        {
            auto result = co_await coroutine::async_connect(
                rv, target_host_, target_port_, 10000);

            if (state->closed.load(std::memory_order_relaxed)) {
                close_target_connection(result.connection);
                co_return;
            }

            if (result.result != coroutine::ConnectResult::success || !result.connection) {
                channel->set_state(SshChannel::State::closing);
                co_return;
            }

            {
                std::lock_guard<std::mutex> lock(state->conn_mutex);
                state->target_conn = result.connection;
            }

            auto target_conn = get_target_connection(state);
            if (!target_conn) {
                co_return;
            }

            rv.register_connection(target_conn, nullptr);
            if (auto stream = std::dynamic_pointer_cast<StreamTransport>(target_conn)) {
                if (auto *ch = stream->stream_channel()) {
                    rv.update_channel(ch);
                }
            }

            std::vector<uint8_t> staged_input;
            bool staged_eof = false;
            {
                std::lock_guard<std::mutex> lock(state->pending_input_mutex);
                staged_input.swap(state->pending_input);
                staged_eof = state->pending_eof;
                state->pending_eof = false;
            }

            if (!staged_input.empty()) {
                ::yuan::buffer::ByteBuffer pending_buf(staged_input.size());
                pending_buf.append(staged_input.data(), staged_input.size());
                target_conn->write_and_flush(pending_buf);
            }
            if (staged_eof) {
                target_conn->shutdown_write();
            }

            co_await relay_from_target(session_, channel_remote_id, state);
        };

        auto t = connect_task();
        t.resume();
        t.detach();
    }

    void SshDirectTcpipHandler::on_data(SshChannel * channel, const std::vector<uint8_t> & data)
    {
        if (state_->closed.load(std::memory_order_relaxed)) {
            return;
        }

        auto target_conn = get_target_connection(state_);
        if (!target_conn) {
            if (!data.empty()) {
                std::lock_guard<std::mutex> lock(state_->pending_input_mutex);
                state_->pending_input.insert(state_->pending_input.end(), data.begin(), data.end());
            }
            return;
        }

        ::yuan::buffer::ByteBuffer buf(data.size());
        buf.append(data.data(), data.size());
        target_conn->write_and_flush(buf);
    }

    void SshDirectTcpipHandler::on_eof(SshChannel * channel)
    {
        (void)channel;
        auto target_conn = get_target_connection(state_);
        if (!target_conn) {
            std::lock_guard<std::mutex> lock(state_->pending_input_mutex);
            state_->pending_eof = true;
            return;
        }
        target_conn->shutdown_write();
    }

    void SshDirectTcpipHandler::on_close(SshChannel * channel)
    {
        state_->closed.store(true, std::memory_order_relaxed);
        std::shared_ptr<net::Connection> conn;
        {
            std::lock_guard<std::mutex> lock(state_->conn_mutex);
            conn = std::move(state_->target_conn);
        }
        close_target_connection(conn);
    }

    void SshDirectTcpipHandler::on_window_adjust(SshChannel * channel, uint32_t bytes_to_add)
    {
    }

    coroutine::Task<void> SshDirectTcpipHandler::relay_from_target(SshSession * session,
                                                                   uint32_t channel_remote_id,
                                                                   std::shared_ptr<SshDirectTcpipHandler::SharedState> state)
    {
        if (!session || !state) {
            co_return;
        }

        auto rv = session->runtime();

        while (!state->closed.load(std::memory_order_relaxed)) {
            auto target_conn = get_target_connection(state);
            if (!target_conn || !target_conn->is_connected()) {
                break;
            }

            auto result = co_await coroutine::async_read(rv, target_conn);
            if (result.status == coroutine::IoStatus::timed_out) {
                continue;
            }
            if (result.status != coroutine::IoStatus::success) {
                break;
            }

            if (result.data.readable_bytes() > 0) {
                auto *channel = session->connection_manager().find_channel_by_remote(channel_remote_id);
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
