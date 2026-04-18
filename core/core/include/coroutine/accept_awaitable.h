#ifndef __YUAN_COROUTINE_ACCEPT_AWAITABLE_H__
#define __YUAN_COROUTINE_ACCEPT_AWAITABLE_H__

#include <coroutine>
#include <memory>

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
            : runtime_(runtime),
              acceptor_(acceptor)
        {
        }

        bool await_ready() const noexcept
        {
            return !acceptor_ || !runtime_.event_loop();
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept
        {
            if (await_ready()) {
                return false;
            }

            handle_ = handle;
            original_handler_ = acceptor_->connection_handler();
            proxy_ = std::make_unique<AcceptProxyHandler>(*this);
            acceptor_->set_connection_handler(proxy_.get());
            return true;
        }

        net::Connection *await_resume() noexcept
        {
            if (acceptor_) {
                acceptor_->set_connection_handler(original_handler_);
            }
            proxy_.reset();
            return accepted_conn_;
        }

    private:
        void on_connection_accepted(net::Connection *conn) noexcept
        {
            if (conn) {
                conn->set_connection_handler(original_handler_);
            }
            accepted_conn_ = conn;
            resume();
        }

        void resume() noexcept
        {
            if (completed_ || !handle_) {
                return;
            }
            completed_ = true;
            if (runtime_.event_loop()) {
                runtime_.event_loop()->post_coroutine(handle_);
            }
        }

        class AcceptProxyHandler final : public net::ConnectionHandler
        {
        public:
            AcceptProxyHandler(AcceptAwaitable &owner) noexcept
                : owner_(owner)
            {
            }

            void on_connected(net::Connection *conn) override
            {
                owner_.on_connection_accepted(conn);
            }

            void on_error(net::Connection *conn) override
            {
                (void)conn;
            }

            void on_read(net::Connection *conn) override
            {
                (void)conn;
            }

            void on_write(net::Connection *conn) override
            {
                (void)conn;
            }

            void on_close(net::Connection *conn) override
            {
                (void)conn;
            }

        private:
            AcceptAwaitable &owner_;
        };

        RuntimeView runtime_{}; 
        net::StreamAcceptor *acceptor_ = nullptr;
        net::ConnectionHandler *original_handler_ = nullptr;
        net::Connection *accepted_conn_ = nullptr;

        std::coroutine_handle<> handle_{};
        std::unique_ptr<AcceptProxyHandler> proxy_;
        bool completed_ = false;
    };

    inline AcceptAwaitable async_accept(
        RuntimeView runtime,
        net::StreamAcceptor * acceptor) noexcept
    {
        return AcceptAwaitable(runtime, acceptor);
    }

} // namespace yuan::coroutine

#endif
