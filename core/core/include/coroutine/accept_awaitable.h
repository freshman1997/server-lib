#ifndef __YUAN_COROUTINE_ACCEPT_AWAITABLE_H__
#define __YUAN_COROUTINE_ACCEPT_AWAITABLE_H__

#include <coroutine>
#include <cstdint>
#include <memory>
#include <utility>

#include "coroutine/runtime.h"
#include "net/acceptor/stream_acceptor.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"

namespace yuan::coroutine
{

    class AcceptAwaitable
    {
    public:
        AcceptAwaitable(RuntimeView runtime, net::StreamAcceptor *acceptor) noexcept
            : state_(std::make_shared<State>())
        {
            state_->runtime = runtime;
            state_->acceptor = acceptor;
        }

        ~AcceptAwaitable()
        {
            cancel_waiter();
        }

        bool await_ready() const noexcept
        {
            if (!state_ || !state_->acceptor || !state_->runtime.event_loop()) {
                return true;
            }
            return state_->acceptor->has_pending_connections();
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept
        {
            if (await_ready()) {
                return false;
            }

            state_->handle = handle;
            auto state = state_;
            state_->waiter_id = state_->acceptor->add_accept_waiter([state](const std::shared_ptr<net::Connection> &conn) {
                on_connection_accepted(state, conn);
            });
            return true;
        }

        std::shared_ptr<net::Connection> await_resume() noexcept
        {
            cancel_waiter();

            if (state_ && state_->acceptor && state_->acceptor->has_pending_connections()) {
                return state_->acceptor->dequeue_pending_connection();
            }
            return state_ ? std::move(state_->accepted_conn) : std::shared_ptr<net::Connection>{};
        }

    private:
        struct State
        {
            RuntimeView runtime{};
            net::StreamAcceptor *acceptor = nullptr;
            std::shared_ptr<net::Connection> accepted_conn;
            std::coroutine_handle<> handle{};
            uint64_t waiter_id = 0;
            bool completed = false;
        };

        static void on_connection_accepted(const std::shared_ptr<State> &state, std::shared_ptr<net::Connection> conn) noexcept
        {
            if (!state) {
                return;
            }
            if (!conn) {
                state->acceptor = nullptr;
            }
            if (state->completed) {
                if (state->acceptor && conn) {
                    state->acceptor->enqueue_pending_connection(std::move(conn));
                }
                return;
            }
            state->accepted_conn = std::move(conn);
            resume(state);
        }

        static void resume(const std::shared_ptr<State> &state) noexcept
        {
            if (!state || state->completed || !state->handle) {
                return;
            }
            state->completed = true;
            state->waiter_id = 0;
            if (state->runtime.event_loop()) {
                state->runtime.event_loop()->post_coroutine(state->handle);
            }
        }

        void cancel_waiter() noexcept
        {
            if (state_ && state_->acceptor && state_->waiter_id != 0) {
                state_->acceptor->remove_accept_waiter(state_->waiter_id);
                state_->waiter_id = 0;
            }
        }

        std::shared_ptr<State> state_;
    };

    inline AcceptAwaitable async_accept(
        RuntimeView runtime,
        net::StreamAcceptor * acceptor) noexcept
    {
        return AcceptAwaitable(runtime, acceptor);
    }

} // namespace yuan::coroutine

#endif
