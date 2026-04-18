#ifndef __YUAN_COROUTINE_CONNECTION_EVENT_AWAITABLE_H__
#define __YUAN_COROUTINE_CONNECTION_EVENT_AWAITABLE_H__

#include <coroutine>
#include <memory>

#include "coroutine/runtime.h"
#include "event/event_loop.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"

namespace yuan::coroutine
{

    enum class ConnectionEventKind {
        connected,
        readable,
        writable,
        closed,
        error,
    };

    class ConnectionEventAwaiter
    {
    public:
        ConnectionEventAwaiter(RuntimeView runtime, std::shared_ptr<net::Connection> connection, ConnectionEventKind event_kind) noexcept
            : runtime_(runtime),
              connection_(std::move(connection)),
              event_kind_(event_kind)
        {
        }

        bool await_ready() const noexcept
        {
            return runtime_.event_loop() == nullptr || connection_ == nullptr;
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept
        {
            if (await_ready()) {
                return false;
            }

            handle_ = handle;
            proxy_ = std::make_shared<ProxyHandler>(*this, connection_,
                                                    connection_->get_connection_handler(),
                                                    connection_->get_connection_handler_owner());
            connection_->set_connection_handler(proxy_);
            return true;
        }

        net::EventLoopExitReason await_resume() noexcept
        {
            restore_handler_if_needed();
            proxy_.reset();
            return exit_reason_;
        }

    private:
        class ProxyHandler final : public net::ConnectionHandler
        {
        public:
            friend class ConnectionEventAwaiter;

            ProxyHandler(ConnectionEventAwaiter &owner, std::shared_ptr<net::Connection> connection,
                         net::ConnectionHandler *next,
                         std::shared_ptr<net::ConnectionHandler> next_owner) noexcept
                : owner_(owner),
                  connection_(std::move(connection)),
                  next_(next),
                  next_owner_(std::move(next_owner))
            {
            }

            void on_connected(const std::shared_ptr<net::Connection> &conn) override
            {
                notify(ConnectionEventKind::connected);
                if (next_) {
                    next_->on_connected(conn);
                }
            }

            void on_error(const std::shared_ptr<net::Connection> &conn) override
            {
                notify(ConnectionEventKind::error, true);
                if (next_) {
                    next_->on_error(conn);
                }
            }

            void on_read(const std::shared_ptr<net::Connection> &conn) override
            {
                notify(ConnectionEventKind::readable);
                if (next_) {
                    next_->on_read(conn);
                }
            }

            void on_write(const std::shared_ptr<net::Connection> &conn) override
            {
                notify(ConnectionEventKind::writable);
                if (next_) {
                    next_->on_write(conn);
                }
            }

            void on_close(const std::shared_ptr<net::Connection> &conn) override
            {
                notify(ConnectionEventKind::closed, true);
                if (next_) {
                    next_->on_close(conn);
                }
            }

        private:
            void notify(ConnectionEventKind event_kind, bool force_restore = false) noexcept
            {
                if (force_restore || event_kind == owner_.event_kind_) {
                    owner_.restore_handler_if_needed();
                }

                if (!owner_.completed_ && (event_kind == owner_.event_kind_ || force_restore)) {
                    owner_.completed_ = true;
                    owner_.exit_reason_ = event_kind == owner_.event_kind_
                                              ? net::EventLoopExitReason::coroutine_resume_requested
                                              : net::EventLoopExitReason::quit_requested;

                    if (owner_.runtime_.event_loop() && owner_.handle_) {
                        owner_.runtime_.event_loop()->post_coroutine(owner_.handle_);
                    }
                }
            }

            ConnectionEventAwaiter &owner_;
            std::shared_ptr<net::Connection> connection_;
            net::ConnectionHandler *next_ = nullptr;
            std::shared_ptr<net::ConnectionHandler> next_owner_;
        };

        void restore_handler_if_needed() noexcept
        {
            if (handler_restored_ || !proxy_ || !connection_) {
                return;
            }

            if (connection_->get_connection_handler() == proxy_.get()) {
                if (proxy_->next_owner_) {
                    connection_->set_connection_handler(proxy_->next_owner_);
                } else {
                    connection_->set_connection_handler(make_non_owning_handler(proxy_->next_));
                }
            }
            handler_restored_ = true;
        }

        RuntimeView runtime_{};
        std::shared_ptr<net::Connection> connection_;
        ConnectionEventKind event_kind_ = ConnectionEventKind::connected;
        bool handler_restored_ = false;
        bool completed_ = false;
        net::EventLoopExitReason exit_reason_ = net::EventLoopExitReason::quit_requested;
        std::coroutine_handle<> handle_{};
        std::shared_ptr<ProxyHandler> proxy_;
    };

    inline ConnectionEventAwaiter wait_for_connection_event(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection,
        ConnectionEventKind event_kind) noexcept
    {
        return ConnectionEventAwaiter(runtime, connection, event_kind);
    }

    inline ConnectionEventAwaiter wait_connected(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection) noexcept
    {
        return wait_for_connection_event(runtime, connection, ConnectionEventKind::connected);
    }

    inline ConnectionEventAwaiter wait_readable(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection) noexcept
    {
        return wait_for_connection_event(runtime, connection, ConnectionEventKind::readable);
    }

    inline ConnectionEventAwaiter wait_writable(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection) noexcept
    {
        return wait_for_connection_event(runtime, connection, ConnectionEventKind::writable);
    }

    inline ConnectionEventAwaiter wait_closed(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection) noexcept
    {
        return wait_for_connection_event(runtime, connection, ConnectionEventKind::closed);
    }

    inline ConnectionEventAwaiter wait_error(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection) noexcept
    {
        return wait_for_connection_event(runtime, connection, ConnectionEventKind::error);
    }

} // namespace yuan::coroutine

#endif
