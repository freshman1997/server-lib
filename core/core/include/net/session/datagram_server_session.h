#ifndef __YUAN_NET_SESSION_DATAGRAM_SERVER_SESSION_H__
#define __YUAN_NET_SESSION_DATAGRAM_SERVER_SESSION_H__

#include <functional>
#include <string>

#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/runtime/network_runtime.h"
#include "net/session/connection_context.h"
#include "net/socket/socket.h"
#include "timer/timer_handle.h"
#include "timer/timer_manager.h"

namespace yuan::net
{

    class DatagramServerSession : public ConnectionHandler
    {
    public:
        using ReadCallback = std::function<void(ConnectionContext &conn)>;

        DatagramServerSession() = default;

        ~DatagramServerSession()
        {
            close();
        }

        DatagramServerSession(const DatagramServerSession &) = delete;
        DatagramServerSession &operator=(const DatagramServerSession &) = delete;

        bool bind(uint16_t port, NetworkRuntime &runtime)
        {
            return setup("", port, runtime, {});
        }

        bool bind(const std::string &host, uint16_t port, NetworkRuntime &runtime)
        {
            return setup(host, port, runtime, {});
        }

        bool bind(uint16_t port, NetworkRuntime &runtime, const ListenOptions &options)
        {
            return setup("", port, runtime, options);
        }

        bool bind(const std::string &host, uint16_t port, NetworkRuntime &runtime, const ListenOptions &options)
        {
            return setup(host, port, runtime, options);
        }

        void close()
        {
            if (acceptor_) {
                acceptor_->close();
                acceptor_.reset();
            }
            runtime_ = nullptr;
        }

        void set_read_callback(ReadCallback callback)
        {
            read_callback_ = std::move(callback);
        }

        NetworkRuntime *runtime() const noexcept
        {
            return runtime_;
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

        void on_connected(const std::shared_ptr<Connection> &conn) override
        {
            if (conn) {
                conn->set_connection_handler(self_handler_holder_);
            }
        }

        void on_read(const std::shared_ptr<Connection> &conn) override
        {
            if (read_callback_ && conn) {
                ConnectionContext ctx(conn);
                read_callback_(ctx);
            }
        }

        void on_error(const std::shared_ptr<Connection> &conn) override
        {
            (void)conn;
        }

        void on_write(const std::shared_ptr<Connection> &conn) override
        {
            (void)conn;
        }

        void on_close(const std::shared_ptr<Connection> &conn) override
        {
            (void)conn;
        }

    private:
        bool setup(const std::string &host, uint16_t port, NetworkRuntime &runtime, const ListenOptions &options)
        {
            runtime_ = &runtime;

            auto *loop = runtime.event_loop();
            auto *tm = runtime.timer_manager();
            if (!loop || !tm) {
                return false;
            }

            auto sock = std::make_unique<Socket>(host.c_str(), port, true);
            if (!sock->valid()) {
                return false;
            }

            if (!sock->apply_listen_options(options)) {
                return false;
            }
            if (!sock->bind()) {
                return false;
            }

            acceptor_.reset(create_datagram_acceptor(sock.release(), tm));
            if (!acceptor_->listen()) {
                acceptor_.reset();
                return false;
            }

            acceptor_->set_connection_handler(self_handler_holder_);
            acceptor_->set_event_handler(loop);
            loop->update_channel(acceptor_->endpoint_channel());
            return true;
        }

        NetworkRuntime *runtime_ = nullptr;
        std::unique_ptr<DatagramAcceptor> acceptor_;
        ReadCallback read_callback_;
        std::shared_ptr<ConnectionHandler> self_handler_holder_{ make_non_owning_handler(*this) };
    };

} // namespace yuan::net

#endif
