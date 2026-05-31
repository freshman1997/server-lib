#include "net/acceptor/iocp_stream_acceptor.h"

#include "net/handler/connection_handler.h"

#include <algorithm>
#include <utility>

namespace yuan::net
{
    struct IocpStreamAcceptor::State
    {
        mutable std::mutex mutex;
        NetworkRuntime *runtime = nullptr;
        std::shared_ptr<ConnectionHandler> handler;
        bool closed = false;
    };

    class IocpStreamAcceptor::RuntimeDispatchHandler final : public ConnectionHandler
    {
    public:
        explicit RuntimeDispatchHandler(std::weak_ptr<State> state) noexcept
            : state_(std::move(state))
        {
        }

        void on_connected(const std::shared_ptr<Connection> &conn) override
        {
            dispatch([conn](ConnectionHandler &handler) {
                handler.on_connected(conn);
            });
        }

        void on_error(const std::shared_ptr<Connection> &conn) override
        {
            dispatch([conn](ConnectionHandler &handler) {
                handler.on_error(conn);
            });
        }

        void on_read(const std::shared_ptr<Connection> &conn) override
        {
            auto iocp_conn = std::dynamic_pointer_cast<IocpTcpConnection>(conn);
            if (iocp_conn && !iocp_conn->try_mark_read_dispatch_pending()) {
                return;
            }
            if (iocp_conn) {
                iocp_conn->mark_defer_close_on_unconsumed_input();
            }

            const bool queued = dispatch([conn, iocp_conn](ConnectionHandler &handler) {
                struct PendingReadGuard
                {
                    std::shared_ptr<IocpTcpConnection> connection;
                    ~PendingReadGuard()
                    {
                        if (connection) {
                            connection->clear_read_dispatch_pending();
                        }
                    }
                } guard{ iocp_conn };
                handler.on_read(conn);
            });
            if (!queued && iocp_conn) {
                iocp_conn->clear_read_dispatch_pending();
            }
        }

        void on_write(const std::shared_ptr<Connection> &conn) override
        {
            dispatch([conn](ConnectionHandler &handler) {
                handler.on_write(conn);
            });
        }

        void on_close(const std::shared_ptr<Connection> &conn) override
        {
            dispatch([conn](ConnectionHandler &handler) {
                handler.on_close(conn);
            });
        }

        void on_input_shutdown(const std::shared_ptr<Connection> &conn) override
        {
            dispatch([conn](ConnectionHandler &handler) {
                handler.on_input_shutdown(conn);
            });
        }

    private:
        template<typename Fn>
        bool dispatch(Fn fn)
        {
            auto state = state_.lock();
            if (!state) {
                return false;
            }

            NetworkRuntime *runtime = nullptr;
            std::shared_ptr<ConnectionHandler> handler;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->closed || !state->handler) {
                    return false;
                }
                runtime = state->runtime;
                handler = state->handler;
            }

            if (runtime) {
                runtime->dispatch([handler = std::move(handler), fn = std::move(fn)]() mutable {
                    if (handler) {
                        fn(*handler);
                    }
                });
                return true;
            }

            fn(*handler);
            return true;
        }

        std::weak_ptr<State> state_;
    };

    IocpStreamAcceptor::IocpStreamAcceptor(std::string host,
                                           uint16_t port,
                                           NetworkRuntime &runtime,
                                           ListenOptions options)
        : host_(std::move(host)),
          port_(port),
          runtime_(&runtime),
          options_(options),
          state_(std::make_shared<State>())
    {
        state_->runtime = &runtime;
        dispatch_handler_ = std::make_shared<RuntimeDispatchHandler>(state_);
    }

    IocpStreamAcceptor::~IocpStreamAcceptor()
    {
        close();
    }

    bool IocpStreamAcceptor::listen()
    {
        return listen(options_.backlog);
    }

    bool IocpStreamAcceptor::listen(int backlog)
    {
#ifdef _WIN32
        if (!runtime_ || ssl_module_) {
            return false;
        }

        options_.backlog = backlog > 0 ? backlog : options_.backlog;
        IocpTcpEngineCallbacks callbacks;
        callbacks.on_accept = [this](const std::shared_ptr<IocpTcpConnection> &connection) {
            if (!connection) {
                return;
            }
            connection->set_connection_handler(dispatch_handler_);
            notify_accept_waiters(connection);
        };

        const auto worker_count = (std::max<std::size_t>)(1, options_.iocp_worker_count);
        const bool started = engine_.listen(host_,
                                            port_,
                                            worker_count,
                                            std::move(callbacks),
                                            0,
                                            options_.backlog,
                                            options_.iocp_completion_batch_size);
        if (started) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->closed = false;
            state_->runtime = runtime_;
        }
        return started;
#else
        (void)backlog;
        return false;
#endif
    }

    void IocpStreamAcceptor::close()
    {
        NetworkRuntime *runtime = nullptr;
        std::shared_ptr<ConnectionHandler> handler;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->closed) {
                return;
            }
            state_->closed = true;
            runtime = state_->runtime;
            handler = state_->handler;
            state_->runtime = nullptr;
        }

        notify_accept_waiters(std::shared_ptr<Connection>{});
        engine_.stop();
        runtime_ = nullptr;

        if (handler) {
            if (runtime) {
                runtime->dispatch([handler = std::move(handler)]() {
                    handler->on_close(std::shared_ptr<Connection>{});
                });
            } else {
                handler->on_close(std::shared_ptr<Connection>{});
            }
        }
    }

    void IocpStreamAcceptor::update_channel()
    {
    }

    Channel *IocpStreamAcceptor::listener_channel() const
    {
        return nullptr;
    }

    void IocpStreamAcceptor::on_read_event()
    {
    }

    void IocpStreamAcceptor::on_write_event()
    {
    }

    void IocpStreamAcceptor::set_event_handler(EventHandler *eventHandler)
    {
        event_handler_ = eventHandler;
        (void)event_handler_;
    }

    void IocpStreamAcceptor::set_connection_handler(std::shared_ptr<ConnectionHandler> connHandler)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->handler = std::move(connHandler);
    }

    ConnectionHandler *IocpStreamAcceptor::connection_handler() const
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->handler ? &*state_->handler : nullptr;
    }

    std::shared_ptr<ConnectionHandler> IocpStreamAcceptor::connection_handler_owner() const
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->handler;
    }

    void IocpStreamAcceptor::set_ssl_module(std::shared_ptr<SSLModule> module)
    {
        ssl_module_ = std::move(module);
    }

    uint16_t IocpStreamAcceptor::local_port() const noexcept
    {
        return engine_.local_port();
    }
}
