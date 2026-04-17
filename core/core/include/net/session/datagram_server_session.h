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
            return setup("", port, runtime);
        }

        bool bind(const std::string &host, uint16_t port, NetworkRuntime &runtime)
        {
            return setup(host, port, runtime);
        }

        void close()
        {
            if (acceptor_) {
                delete acceptor_;
                acceptor_ = nullptr;
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

        timer::Timer *schedule(uint32_t delay_ms, std::function<void()> callback)
        {
            return runtime_ ? runtime_->schedule(delay_ms, std::move(callback)) : nullptr;
        }

        void cancel_timer(timer::Timer *timer)
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

        void on_connected(Connection *conn) override
        {
            conn->set_connection_handler(this);
        }

        void on_read(Connection *conn) override
        {
            if (read_callback_) {
                ConnectionContext ctx(conn);
                read_callback_(ctx);
            }
        }

        void on_error(Connection *conn) override
        {
        }

        void on_write(Connection *conn) override
        {
        }

        void on_close(Connection *conn) override
        {
        }

    private:
        bool setup(const std::string &host, uint16_t port, NetworkRuntime &runtime)
        {
            runtime_ = &runtime;

            auto *loop = runtime.event_loop();
            auto *tm = runtime.timer_manager();
            if (!loop || !tm) {
                return false;
            }

            Socket *sock = new Socket(host.c_str(), port, true);
            if (!sock->valid()) {
                delete sock;
                return false;
            }

            if (!sock->bind()) {
                delete sock;
                return false;
            }

            sock->set_no_delay(true);
            sock->set_reuse(true);
            sock->set_none_block(true);

            acceptor_ = create_datagram_acceptor(sock, tm);
            if (!acceptor_->listen()) {
                delete acceptor_;
                acceptor_ = nullptr;
                return false;
            }

            acceptor_->set_connection_handler(this);
            acceptor_->set_event_handler(loop);
            loop->update_channel(acceptor_->endpoint_channel());
            return true;
        }

        NetworkRuntime *runtime_ = nullptr;
        DatagramAcceptor *acceptor_ = nullptr;
        ReadCallback read_callback_;
    };

} // namespace yuan::net

#endif
