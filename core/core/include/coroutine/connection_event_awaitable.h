#ifndef __YUAN_COROUTINE_CONNECTION_EVENT_AWAITABLE_H__
#define __YUAN_COROUTINE_CONNECTION_EVENT_AWAITABLE_H__

#include <coroutine>
#include <memory>
#include <vector>

#include "coroutine/runtime.h"
#include "event/event_loop.h"
#include "net/connection/connection.h"
#include "net/connection/connection_handle.h"
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
              connection_handle_(net::ConnectionHandle(std::move(connection))),
              event_kind_(event_kind)
        {
        }

        bool await_ready() const noexcept
        {
            return runtime_.event_loop() == nullptr || !connection_handle_;
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept
        {
            if (await_ready()) {
                return false;
            }
            auto *connection = connection_handle_.get();

            handle_ = handle;
            register_waiter(connection, to_connection_event(event_kind_), false);
            if (event_kind_ != ConnectionEventKind::closed) {
                register_waiter(connection, net::ConnectionEvent::closed, true);
            }
            if (event_kind_ != ConnectionEventKind::error) {
                register_waiter(connection, net::ConnectionEvent::error, true);
            }
            return true;
        }

        net::EventLoopExitReason await_resume() noexcept
        {
            restore_handler_if_needed();
            return exit_reason_;
        }

    private:
        void restore_handler_if_needed() noexcept
        {
            auto *connection = connection_handle_.get();
            cancel_waiters(connection);
            if (handler_restored_) {
                return;
            }
            handler_restored_ = true;
        }

        static net::ConnectionEvent to_connection_event(ConnectionEventKind event_kind) noexcept
        {
            switch (event_kind) {
            case ConnectionEventKind::connected:
                return net::ConnectionEvent::connected;
            case ConnectionEventKind::readable:
                return net::ConnectionEvent::readable;
            case ConnectionEventKind::writable:
                return net::ConnectionEvent::writable;
            case ConnectionEventKind::closed:
                return net::ConnectionEvent::closed;
            case ConnectionEventKind::error:
                return net::ConnectionEvent::error;
            }
            return net::ConnectionEvent::closed;
        }

        void register_waiter(net::Connection *connection, net::ConnectionEvent event, bool force_restore)
        {
            if (!connection) {
                return;
            }

            waiter_ids_.push_back(connection->add_event_waiter(event, [this, force_restore](const std::shared_ptr<net::Connection> &) {
                if (force_restore) {
                    restore_handler_if_needed();
                }

                if (!completed_) {
                    completed_ = true;
                    exit_reason_ = force_restore
                                       ? net::EventLoopExitReason::quit_requested
                                       : net::EventLoopExitReason::coroutine_resume_requested;

                    if (runtime_.event_loop() && handle_) {
                        runtime_.event_loop()->post_coroutine(handle_);
                    }
                }
            }));
        }

        void cancel_waiters(net::Connection *connection) noexcept
        {
            if (!connection || waiter_ids_.empty()) {
                waiter_ids_.clear();
                return;
            }
            for (const auto id : waiter_ids_) {
                connection->remove_event_waiter(id);
            }
            waiter_ids_.clear();
        }

        RuntimeView runtime_{};
        net::ConnectionHandle connection_handle_{};
        ConnectionEventKind event_kind_ = ConnectionEventKind::connected;
        bool handler_restored_ = false;
        bool completed_ = false;
        net::EventLoopExitReason exit_reason_ = net::EventLoopExitReason::quit_requested;
        std::coroutine_handle<> handle_{};
        std::vector<uint64_t> waiter_ids_;
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
