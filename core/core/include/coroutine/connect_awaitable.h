#ifndef __YUAN_COROUTINE_CONNECT_AWAITABLE_H__
#define __YUAN_COROUTINE_CONNECT_AWAITABLE_H__

#include <coroutine>
#include <memory>
#include <string>

#include "coroutine/connection_event_awaitable.h"
#include "coroutine/runtime.h"
#include "net/connection/connection.h"
#include "net/connection/connection_factory.h"
#include "net/connection/stream_transport.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "timer/timer_manager.h"
#include "timer/timer_util.hpp"

namespace yuan::coroutine
{

    enum class ConnectResult {
        success,
        invalid_address,
        socket_error,
        connect_failed,
        timed_out,
        connection_error,
    };

    struct ConnectAwaitableResult
    {
        std::shared_ptr<net::Connection> connection;
        ConnectResult result = ConnectResult::connect_failed;
    };

    class ConnectAwaitable
    {
    public:
        ConnectAwaitable(RuntimeView runtime,
                         const std::string &host,
                         uint16_t port,
                         uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              host_(host),
              port_(port),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            handle_ = handle;

            net::InetAddress addr{ host_.c_str(), port_ };
            if (addr.get_ip().empty()) {
                result_.result = ConnectResult::invalid_address;
                return false;
            }

            net::Socket *sock = new net::Socket(addr.get_ip().c_str(), port_);
            if (!sock->valid()) {
                delete sock;
                result_.result = ConnectResult::socket_error;
                return false;
            }

            sock->set_none_block(true);
            if (!sock->connect()) {
                delete sock;
                result_.result = ConnectResult::connect_failed;
                return false;
            }

            conn_ = net::create_stream_connection(sock);
            if (!conn_) {
                result_.result = ConnectResult::socket_error;
                return false;
            }

            original_handler_ = conn_->get_connection_handler();
            original_handler_owner_ = conn_->get_connection_handler_owner();
            proxy_ = std::make_shared<ConnectProxyHandler>(*this, conn_.get(),
                                                           original_handler_,
                                                           original_handler_owner_);
            conn_->set_connection_handler(proxy_);
            conn_->set_event_handler(runtime_.event_loop());

            if (auto *stream = dynamic_cast<net::StreamTransport *>(conn_.get())) {
                if (auto *channel = stream->stream_channel()) {
                    runtime_.event_loop()->update_channel(channel);
                }
            }

            if (timeout_ms_ > 0 && runtime_.timer_manager()) {
                timeout_timer_ = timer::TimerUtil::build_timeout_timer(
                    runtime_.timer_manager(),
                    timeout_ms_,
                    [this](timer::Timer *timer) {
                        if (completed_)
                        {
                            return;
                        }
                        timed_out_ = true;
                        timeout_timer_ = nullptr;
                        if (timer)
                        {
                            timer->cancel();
                        }
                        resume();
                    });
            }

            return true;
        }

        ConnectAwaitableResult await_resume() noexcept
        {
            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
            }

            restore_handler_if_needed();
            proxy_.reset();

            if (timed_out_) {
                if (conn_) {
                    conn_->abort();
                    conn_ = nullptr;
                }
                result_.result = ConnectResult::timed_out;
            }

            if (result_.result == ConnectResult::success) {
                result_.connection = conn_;
            } else if (conn_) {
                conn_->abort();
                conn_ = nullptr;
            }

            return result_;
        }

    private:
        void restore_handler_if_needed() noexcept
        {
            if (handler_restored_ || !conn_) {
                return;
            }

            if (proxy_ && conn_->get_connection_handler() == proxy_.get()) {
                if (original_handler_owner_) {
                    conn_->set_connection_handler(original_handler_owner_);
                } else {
                    conn_->set_connection_handler(make_non_owning_handler(original_handler_));
                }
            }
            handler_restored_ = true;
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

        class ConnectProxyHandler final : public net::ConnectionHandler
        {
        public:
            ConnectProxyHandler(ConnectAwaitable &owner,
                                net::Connection *conn,
                                net::ConnectionHandler *next,
                                std::shared_ptr<net::ConnectionHandler> next_owner) noexcept
                : owner_(owner),
                  conn_(conn),
                  next_(next),
                  next_owner_(std::move(next_owner))
            {
            }

            void on_connected(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    owner_.result_.result = ConnectResult::success;
                    owner_.resume();
                }
                if (next_) {
                    next_->on_connected(conn);
                }
            }

            void on_error(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    owner_.result_.result = ConnectResult::connection_error;
                    owner_.resume();
                }
                if (next_) {
                    next_->on_error(conn);
                }
            }

            void on_read(const std::shared_ptr<net::Connection> &conn) override
            {
            }

            void on_write(const std::shared_ptr<net::Connection> &conn) override
            {
            }

            void on_close(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    owner_.result_.result = ConnectResult::connection_error;
                    owner_.resume();
                }
                if (next_) {
                    next_->on_close(conn);
                }
            }

        private:
            ConnectAwaitable &owner_;
            net::Connection *conn_;
            net::ConnectionHandler *next_ = nullptr;
            std::shared_ptr<net::ConnectionHandler> next_owner_;
        };

        RuntimeView runtime_{};
        std::string host_;
        uint16_t port_ = 0;
        uint32_t timeout_ms_ = 0;
        std::shared_ptr<net::Connection> conn_;
        timer::Timer *timeout_timer_ = nullptr;
        net::ConnectionHandler *original_handler_ = nullptr;
        std::shared_ptr<net::ConnectionHandler> original_handler_owner_;

        std::coroutine_handle<> handle_{};
        std::shared_ptr<ConnectProxyHandler> proxy_;
        ConnectAwaitableResult result_{};
        bool completed_ = false;
        bool timed_out_ = false;
        bool handler_restored_ = false;
    };

    inline ConnectAwaitable async_connect(
        RuntimeView runtime,
        const std::string & host,
        uint16_t port,
        uint32_t timeout_ms = 0) noexcept
    {
        return ConnectAwaitable(runtime, host, port, timeout_ms);
    }

} // namespace yuan::coroutine

#endif
