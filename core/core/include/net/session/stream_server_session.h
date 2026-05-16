#ifndef __YUAN_NET_SESSION_STREAM_SERVER_SESSION_H__
#define __YUAN_NET_SESSION_STREAM_SERVER_SESSION_H__

#include <functional>
#include <memory>
#include <string>

#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/stream_acceptor.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/runtime/network_runtime.h"
#include "net/session/connection_context.h"
#include "net/socket/socket.h"
#include "net/security/ssl_module.h"
#include "timer/timer_handle.h"
#include "timer/timer_manager.h"

namespace yuan::net
{

    class StreamServerSession : public ConnectionHandler
    {
    public:
        using ConnectedCallback = std::function<void(ConnectionContext &)>;
        using ReadCallback = std::function<void(ConnectionContext &)>;
        using WriteCallback = std::function<void(ConnectionContext &)>;
        using CloseCallback = std::function<void(ConnectionContext &)>;
        using ErrorCallback = std::function<void(ConnectionContext &)>;

        StreamServerSession() = default;

        ~StreamServerSession()
        {
            close();
        }

        StreamServerSession(const StreamServerSession &) = delete;
        StreamServerSession &operator=(const StreamServerSession &) = delete;

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

        void set_connected_callback(ConnectedCallback cb)
        {
            connected_cb_ = std::move(cb);
        }

        void set_read_callback(ReadCallback cb)
        {
            read_cb_ = std::move(cb);
        }

        void set_write_callback(WriteCallback cb)
        {
            write_cb_ = std::move(cb);
        }

        void set_close_callback(CloseCallback cb)
        {
            close_cb_ = std::move(cb);
        }

        void set_error_callback(ErrorCallback cb)
        {
            error_cb_ = std::move(cb);
        }

        void set_ssl_module(std::shared_ptr<SSLModule> ssl_module)
        {
            ssl_module_ = std::move(ssl_module);
            if (acceptor_ && ssl_module_) {
                acceptor_->set_ssl_module(ssl_module_);
            }
        }

        NetworkRuntime *runtime() const noexcept
        {
            return runtime_;
        }

        timer::TimerHandle schedule(uint32_t delay_ms, std::function<void()> callback)
        {
            return runtime_ ? runtime_->schedule_handle(delay_ms, std::move(callback)) : timer::TimerHandle{};
        }

        timer::TimerHandle schedule_periodic(uint32_t delay_ms, uint32_t interval_ms, std::function<void()> callback, int repeat = 0)
        {
            return runtime_ ? runtime_->schedule_periodic_handle(delay_ms, interval_ms, std::move(callback), repeat) : timer::TimerHandle{};
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
            if (connected_cb_ && conn) {
                ConnectionContext ctx(conn);
                connected_cb_(ctx);
            }
        }

        void on_read(const std::shared_ptr<Connection> &conn) override
        {
            if (read_cb_ && conn) {
                ConnectionContext ctx(conn);
                read_cb_(ctx);
            }
        }

        void on_write(const std::shared_ptr<Connection> &conn) override
        {
            if (write_cb_ && conn) {
                ConnectionContext ctx(conn);
                write_cb_(ctx);
            }
        }

        void on_close(const std::shared_ptr<Connection> &conn) override
        {
            if (close_cb_ && conn) {
                ConnectionContext ctx(conn);
                close_cb_(ctx);
            }
        }

        void on_error(const std::shared_ptr<Connection> &conn) override
        {
            if (error_cb_ && conn) {
                ConnectionContext ctx(conn);
                error_cb_(ctx);
            }
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

            auto sock = std::make_unique<Socket>(host.c_str(), port);
            if (!sock->valid()) {
                return false;
            }

            if (!sock->apply_listen_options(options)) {
                return false;
            }
            if (!sock->bind()) {
                return false;
            }

            acceptor_.reset(create_stream_acceptor(sock.release()));
            if (!acceptor_->listen(options.backlog)) {
                acceptor_.reset();
                return false;
            }

            if (ssl_module_) {
                acceptor_->set_ssl_module(ssl_module_);
            }

            acceptor_->set_connection_handler(self_handler_holder_);
            acceptor_->set_event_handler(loop);
            if (auto *channel = acceptor_->listener_channel()) {
                loop->update_channel(channel);
            }
            return true;
        }

        NetworkRuntime *runtime_ = nullptr;
        std::unique_ptr<StreamAcceptor> acceptor_;
        std::shared_ptr<SSLModule> ssl_module_;
        std::shared_ptr<ConnectionHandler> self_handler_holder_{ make_non_owning_handler(*this) };

        ConnectedCallback connected_cb_;
        ReadCallback read_cb_;
        WriteCallback write_cb_;
        CloseCallback close_cb_;
        ErrorCallback error_cb_;
    };

} // namespace yuan::net

#endif
