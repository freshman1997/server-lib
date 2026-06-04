#ifndef __YUAN_NET_SESSION_STREAM_CLIENT_SESSION_H__
#define __YUAN_NET_SESSION_STREAM_CLIENT_SESSION_H__

#include <functional>
#include <memory>
#include <string>

#include "coroutine/completion_event.h"
#include "coroutine/connect_awaitable.h"
#include "coroutine/connection_event_awaitable.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "event/event_loop.h"
#include "net/connection/connection.h"
#include "net/connection/connection_factory.h"
#include "net/connection/stream_transport.h"
#include "net/handler/connection_handler.h"
#include "net/runtime/network_runtime.h"
#include "net/session/connection_context.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "timer/timer_handle.h"
#include "timer/timer_manager.h"

namespace yuan::net
{

    class StreamClientSession : public ConnectionHandler
    {
    public:
        using ReadCallback = std::function<void(ConnectionContext &)>;
        using ConnectedCallback = std::function<void(ConnectionContext &)>;
        using CloseCallback = std::function<void(ConnectionContext &)>;
        using ErrorCallback = std::function<void(ConnectionContext &)>;

        StreamClientSession() = default;

        ~StreamClientSession()
        {
            close();
        }

        StreamClientSession(const StreamClientSession &) = delete;
        StreamClientSession &operator=(const StreamClientSession &) = delete;

        void ensure_self_handler_holder()
        {
            if (!self_handler_holder_) {
                self_handler_holder_ = make_non_owning_handler(*this);
            }
        }

        bool is_connected() const noexcept
        {
            return connection_ != nullptr && connection_->is_connected();
        }

        ConnectionContext context() const noexcept
        {
            return ConnectionContext(connection_);
        }

        NetworkRuntime *runtime() const noexcept
        {
            return runtime_;
        }

        coroutine::RuntimeView runtime_view() const noexcept
        {
            return coroutine::RuntimeView(event_loop_, timer_manager_);
        }

        timer::TimerHandle schedule(uint32_t delay_ms, std::function<void()> callback)
        {
            return runtime_ ? runtime_->schedule_handle(delay_ms, std::move(callback)) : timer::TimerHandle{};
        }

        void cancel_timer(const timer::TimerHandle &timer)
        {
            if (runtime_) {
                runtime_->cancel_timer(timer);
            }
        }

        void dispatch(std::function<void()> callback)
        {
            if (runtime_) {
                runtime_->dispatch(std::move(callback));
            }
        }

        void set_read_callback(ReadCallback cb)
        {
            read_cb_ = std::move(cb);
        }
        void set_connected_callback(ConnectedCallback cb)
        {
            connected_cb_ = std::move(cb);
        }
        void set_close_callback(CloseCallback cb)
        {
            close_cb_ = std::move(cb);
        }
        void set_error_callback(ErrorCallback cb)
        {
            error_cb_ = std::move(cb);
        }

        void bind_connection(Connection *conn, NetworkRuntime &runtime)
        {
            bind_connection(std::shared_ptr<Connection>(conn, [](Connection *) {}), runtime);
        }

        void bind_connection(std::shared_ptr<Connection> conn, NetworkRuntime &runtime)
        {
            ensure_self_handler_holder();
            connection_ = conn;
            runtime_ = &runtime;
            event_loop_ = runtime.event_loop();
            timer_manager_ = runtime.timer_manager();

            conn->set_connection_handler(self_handler_holder_);
            conn->set_event_handler(event_loop_);

            if (auto stream = std::dynamic_pointer_cast<StreamTransport>(conn)) {
                if (auto *channel = stream->stream_channel()) {
                    event_loop_->update_channel(channel);
                }
            }
        }

        void write(const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (connection_) {
                connection_->write(buffer);
            }
        }

        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (connection_) {
                connection_->write_and_flush(buffer);
            }
        }

        void close()
        {
            if (connection_) {
                connection_->close();
                connection_.reset();
            }
            if (timeout_timer_) {
                timeout_timer_.cancel();
                timeout_timer_.reset();
            }
        }

        yuan::coroutine::Task<bool> connect_async(
            coroutine::RuntimeView runtime,
            const std::string &host,
            uint16_t port,
            uint32_t timeout_ms = 0)
        {
            auto result = co_await yuan::coroutine::async_connect(runtime, host, port, timeout_ms);
            if (result.result != yuan::coroutine::ConnectResult::success || !result.connection) {
                co_return false;
            }

            connection_ = result.connection;
            event_loop_ = runtime.event_loop();
            timer_manager_ = runtime.timer_manager();

            ensure_self_handler_holder();
            connection_->set_connection_handler(self_handler_holder_);
            co_return true;
        }

        yuan::coroutine::Task< ::yuan::buffer::ByteBuffer> read_async(uint32_t timeout_ms = 0)
        {
            if (!connection_ || !event_loop_) {
                co_return::yuan::buffer::ByteBuffer{};
            }

            auto rv = runtime_view();

            if (timeout_ms > 0 && timer_manager_) {
                completion_event_.reset(event_loop_);
                auto timed_out = co_await completion_event_.wait_for(timer_manager_, timeout_ms);
                if (timed_out) {
                    co_return::yuan::buffer::ByteBuffer{};
                }
            }

            auto exit_reason = co_await yuan::coroutine::wait_readable(rv, connection_);
            if (exit_reason != EventLoopExitReason::coroutine_resume_requested) {
                co_return::yuan::buffer::ByteBuffer{};
            }

            co_return connection_->take_input_byte_buffer();
        }

        yuan::coroutine::Task<bool> write_async(const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (!connection_) {
                co_return false;
            }

            connection_->write_and_flush(buffer);

            auto rv = runtime_view();
            auto exit_reason = co_await yuan::coroutine::wait_writable(rv, connection_);
            co_return exit_reason == EventLoopExitReason::coroutine_resume_requested;
        }

        void on_connected(Connection &conn) override
        {
            if (!connected_cb_) {
                return;
            }
            ConnectionContext ctx(&conn);
            connected_cb_(ctx);
        }

        void on_error(Connection &conn) override
        {
            if (!completion_event_.completed()) {
                completion_event_.notify();
            }
            if (error_cb_) {
                ConnectionContext ctx(&conn);
                error_cb_(ctx);
            }
        }

        void on_read(Connection &conn) override
        {
            if (read_cb_) {
                ConnectionContext ctx(&conn);
                read_cb_(ctx);
            }
        }

        void on_write(Connection &conn) override
        {
            (void)conn;
        }

        void on_close(Connection &conn) override
        {
            connection_.reset();
            if (!completion_event_.completed()) {
                completion_event_.notify();
            }
            if (close_cb_) {
                ConnectionContext ctx(&conn);
                close_cb_(ctx);
            }
        }

        coroutine::CompletionEvent &completion_event() noexcept
        {
            return completion_event_;
        }

    private:
        std::shared_ptr<Connection> connection_;
        NetworkRuntime *runtime_ = nullptr;
        EventLoop *event_loop_ = nullptr;
        timer::TimerManager *timer_manager_ = nullptr;
        timer::TimerHandle timeout_timer_;
        coroutine::CompletionEvent completion_event_;
        std::shared_ptr<ConnectionHandler> self_handler_holder_;

        ReadCallback read_cb_;
        ConnectedCallback connected_cb_;
        CloseCallback close_cb_;
        ErrorCallback error_cb_;
    };

} // namespace yuan::net

#endif
