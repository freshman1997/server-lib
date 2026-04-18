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
            original_handler_owner_ = acceptor_->connection_handler_owner();
            proxy_ = std::make_shared<AcceptProxyHandler>(*this);
            acceptor_->set_connection_handler(proxy_);
            return true;
        }

        std::shared_ptr<net::Connection> await_resume() noexcept
        {
            if (acceptor_) {
                if (original_handler_owner_) {
                    acceptor_->set_connection_handler(original_handler_owner_);
                } else {
                    acceptor_->set_connection_handler(make_non_owning_handler(original_handler_));
                }
            }
            proxy_.reset();
            return accepted_conn_;
        }

    private:
        void on_connection_accepted(std::shared_ptr<net::Connection> conn) noexcept
        {
            if (conn) {
                if (original_handler_owner_) {
                    conn->set_connection_handler(original_handler_owner_);
                } else {
                    conn->set_connection_handler(make_non_owning_handler(original_handler_));
                }
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

            void on_connected(const std::shared_ptr<net::Connection> &conn) override
            {
                owner_.on_connection_accepted(conn ? conn->shared_from_this() : std::shared_ptr<net::Connection>{});
            }

            void on_error(const std::shared_ptr<net::Connection> &conn) override
            {
                (void)conn;
            }

            void on_read(const std::shared_ptr<net::Connection> &conn) override
            {
                (void)conn;
            }

            void on_write(const std::shared_ptr<net::Connection> &conn) override
            {
                (void)conn;
            }

            void on_close(const std::shared_ptr<net::Connection> &conn) override
            {
                (void)conn;
            }

        private:
            AcceptAwaitable &owner_;
        };

        RuntimeView runtime_{}; 
        net::StreamAcceptor *acceptor_ = nullptr;
        net::ConnectionHandler *original_handler_ = nullptr;
        std::shared_ptr<net::ConnectionHandler> original_handler_owner_;
        std::shared_ptr<net::Connection> accepted_conn_;

        std::coroutine_handle<> handle_{};
        std::shared_ptr<AcceptProxyHandler> proxy_;
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

