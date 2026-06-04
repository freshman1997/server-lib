#ifndef __YUAN_NET_ASYNC_ASYNC_LISTENER_HOST_H__
#define __YUAN_NET_ASYNC_ASYNC_LISTENER_HOST_H__

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "coroutine/accept_awaitable.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/stream_acceptor.h"
#include "net/async/async_connection_context.h"
#include "net/async/iocp_sharded_async_listener.h"
#include "net/async/sharded_async_listener.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/listen_options.h"
#include "net/socket/socket.h"
#include "net/security/ssl_module.h"

namespace yuan::net
{

    class AsyncListenerHost
    {
    public:
        using AsyncConnectionHandler = std::function<coroutine::Task<void>(AsyncConnectionContext)>;

        struct Metrics
        {
            std::size_t active_connections = 0;
            std::size_t accepted_connections = 0;
            std::size_t rejected_connections = 0;
            std::size_t completed_handlers = 0;
            std::size_t backpressure_closes = 0;
        };

        AsyncListenerHost() = default;

        ~AsyncListenerHost()
        {
            close();
        }

        AsyncListenerHost(const AsyncListenerHost &) = delete;
        AsyncListenerHost &operator=(const AsyncListenerHost &) = delete;

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
            if (state_) {
                state_->runtime = nullptr;
            }
            if (sharded_listener_) {
                sharded_listener_->close();
                sharded_listener_.reset();
            }
            if (generic_sharded_listener_) {
                generic_sharded_listener_->close();
                generic_sharded_listener_.reset();
            }
            affinity_pending_ = false;
            if (acceptor_) {
                acceptor_->close();
                acceptor_.reset();
            }
            runtime_ = nullptr;
        }

        void set_ssl_module(std::shared_ptr<SSLModule> ssl_module)
        {
            ssl_module_ = std::move(ssl_module);
        }

        void set_connection_handler(AsyncConnectionHandler handler)
        {
            state_->conn_handler = std::move(handler);
        }

        coroutine::Task<std::shared_ptr<Connection>> accept_async()
        {
            auto acceptor = acceptor_;
            if (!acceptor || !runtime_) {
                co_return std::shared_ptr<Connection>{};
            }

            auto rv = runtime_->runtime_view();
            auto conn = co_await coroutine::async_accept(rv, acceptor.get());
            co_return conn;
        }

        coroutine::Task<void> run_async()
        {
            if (affinity_pending_) {
                start_affinity_listener();
                co_return;
            }
            if (acceptor_) {
                acceptor_->set_queue_pending_connections(false);
                acceptor_->set_connection_handler(dispatch_handler_holder_);
            }
            co_return;
        }

        bool is_listening() const noexcept
        {
            return acceptor_ != nullptr ||
                   (sharded_listener_ && sharded_listener_->running()) ||
                   (generic_sharded_listener_ && generic_sharded_listener_->running());
        }

        NetworkRuntime *runtime() const noexcept
        {
            return state_ ? state_->runtime : nullptr;
        }

        std::shared_ptr<StreamAcceptor> acceptor() const noexcept
        {
            return acceptor_;
        }

        Metrics metrics() const
        {
            Metrics out;
            if (!state_) {
                return out;
            }
            out.active_connections = state_->active_connections.load(std::memory_order_acquire);
            out.accepted_connections = state_->accepted_connections.load(std::memory_order_acquire);
            out.rejected_connections = state_->rejected_connections.load(std::memory_order_acquire);
            out.completed_handlers = state_->completed_handlers.load(std::memory_order_acquire);
            out.backpressure_closes = state_->backpressure_closes.load(std::memory_order_acquire);
            return out;
        }

    private:
        struct State
        {
            NetworkRuntime *runtime = nullptr;
            AsyncConnectionHandler conn_handler{};
            std::shared_ptr<ConnectionHandler> default_handler{};
            ListenOptions options{};
            std::atomic_size_t active_connections{0};
            std::atomic_size_t accepted_connections{0};
            std::atomic_size_t rejected_connections{0};
            std::atomic_size_t completed_handlers{0};
            std::atomic_size_t backpressure_closes{0};
            mutable std::mutex per_ip_mutex;
            std::unordered_map<std::string, std::size_t> active_per_ip;
        };

        static void apply_connection_limits(const std::shared_ptr<Connection> &conn, const ListenOptions &options)
        {
            if (!conn) {
                return;
            }
            if (options.max_input_buffer_bytes > 0) {
                conn->set_max_packet_size(options.max_input_buffer_bytes);
            }
            if (options.max_output_buffer_bytes > 0) {
                conn->set_max_output_buffer_size(options.max_output_buffer_bytes);
            }
        }

        static bool acquire_global_connection_slot(const std::shared_ptr<State> &state)
        {
            const auto limit = state->options.max_connections;
            if (limit == 0) {
                state->active_connections.fetch_add(1, std::memory_order_acq_rel);
                return true;
            }

            auto current = state->active_connections.load(std::memory_order_acquire);
            while (current < limit) {
                if (state->active_connections.compare_exchange_weak(current,
                                                                    current + 1,
                                                                    std::memory_order_acq_rel,
                                                                    std::memory_order_acquire)) {
                    return true;
                }
            }
            return false;
        }

        static bool admit_connection(const std::shared_ptr<State> &state,
                                     const std::shared_ptr<Connection> &conn,
                                     std::string &ip_key)
        {
            if (!state || !conn) {
                return false;
            }

            const auto &options = state->options;
            if (!acquire_global_connection_slot(state)) {
                state->rejected_connections.fetch_add(1, std::memory_order_release);
                return false;
            }

            ip_key = conn->get_remote_address().get_ip();
            if (options.max_connections_per_ip > 0) {
                std::lock_guard<std::mutex> lock(state->per_ip_mutex);
                const auto current = state->active_per_ip[ip_key];
                if (current >= options.max_connections_per_ip) {
                    state->rejected_connections.fetch_add(1, std::memory_order_release);
                    state->active_connections.fetch_sub(1, std::memory_order_acq_rel);
                    return false;
                }
                state->active_per_ip[ip_key] = current + 1;
            }

            state->accepted_connections.fetch_add(1, std::memory_order_release);
            return true;
        }

        static void release_connection_admission(const std::shared_ptr<State> &state,
                                                 const std::string &ip_key,
                                                 bool counted_per_ip)
        {
            if (!state) {
                return;
            }
            state->active_connections.fetch_sub(1, std::memory_order_release);
            state->completed_handlers.fetch_add(1, std::memory_order_release);
            if (counted_per_ip && !ip_key.empty()) {
                std::lock_guard<std::mutex> lock(state->per_ip_mutex);
                auto it = state->active_per_ip.find(ip_key);
                if (it != state->active_per_ip.end()) {
                    if (it->second <= 1) {
                        state->active_per_ip.erase(it);
                    } else {
                        --it->second;
                    }
                }
            }
        }

        static coroutine::Task<void> run_guarded_handler(std::shared_ptr<State> state,
                                                         AsyncConnectionContext ctx,
                                                         std::string ip_key,
                                                         bool counted_per_ip)
        {
            struct AdmissionGuard
            {
                std::shared_ptr<State> state;
                std::shared_ptr<Connection> conn;
                std::string ip_key;
                bool counted_per_ip = false;
                bool active = true;

                ~AdmissionGuard()
                {
                    if (!active) {
                        return;
                    }
                    if (state && conn && conn->output_limit_exceeded()) {
                        state->backpressure_closes.fetch_add(1, std::memory_order_release);
                    }
                    AsyncListenerHost::release_connection_admission(state, ip_key, counted_per_ip);
                }
            };

            AdmissionGuard guard{state, ctx.connection(), std::move(ip_key), counted_per_ip, true};
            co_await state->conn_handler(std::move(ctx));
            co_return;
        }

        static void on_connection_accepted(const std::shared_ptr<State> &state,
                                           const std::shared_ptr<Connection> &conn)
        {
            if (!state || !conn || !state->runtime) {
                return;
            }

            auto *runtime = state->runtime;
            auto *loop = runtime->event_loop();
            if (!loop) {
                return;
            }

            if (!state->conn_handler) {
                conn->close();
                return;
            }

            conn->set_event_handler(loop);
            apply_connection_limits(conn, state->options);

            std::string ip_key;
            if (!admit_connection(state, conn, ip_key)) {
                conn->close();
                return;
            }
            const bool counted_per_ip = state->options.max_connections_per_ip > 0;

            bool uses_readiness_channel = false;
            if (auto stream = std::dynamic_pointer_cast<StreamTransport>(conn)) {
                if (auto *channel = stream->stream_channel()) {
                    uses_readiness_channel = true;
                    loop->update_channel(channel);
                }
            }
            conn->set_connection_handler(uses_readiness_channel ? state->default_handler : nullptr);

            auto ctx = AsyncConnectionContext(conn, static_cast<coroutine::RuntimeView>(runtime->runtime_view()));
            auto task = run_guarded_handler(state, std::move(ctx), std::move(ip_key), counted_per_ip);
            task.resume();
            task.detach();
        }

        bool setup(const std::string &host, uint16_t port, NetworkRuntime &runtime, const ListenOptions &options)
        {
            if (acceptor_) {
                acceptor_->close();
                acceptor_.reset();
            }
            if (sharded_listener_) {
                sharded_listener_->close();
                sharded_listener_.reset();
            }
            if (generic_sharded_listener_) {
                generic_sharded_listener_->close();
                generic_sharded_listener_.reset();
            }
            affinity_pending_ = false;

            runtime_ = &runtime;
            state_->runtime = &runtime;
            state_->default_handler = default_handler_holder_;
            state_->options = options;

            auto *loop = runtime.event_loop();
            auto *tm = runtime.timer_manager();
            if (!loop || !tm) {
                return false;
            }

#ifdef _WIN32
            if (options.use_iocp && options.scheduling_mode == ListenSchedulingMode::affinity) {
                if (ssl_module_) {
                    return false;
                }
                affinity_host_ = host;
                affinity_port_ = port;
                affinity_options_ = options;
                affinity_pending_ = true;
                return true;
            }
#endif
            if (options.scheduling_mode == ListenSchedulingMode::affinity) {
                if (ssl_module_) {
                    return false;
                }
                affinity_host_ = host;
                affinity_port_ = port;
                affinity_options_ = options;
                affinity_pending_ = true;
                return true;
            }
#ifdef _WIN32
            if (options.use_iocp) {
                acceptor_.reset(create_iocp_stream_acceptor(host, port, runtime, options));
            } else
#endif
            {
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
            }
            if (!acceptor_) {
                return false;
            }
            if (ssl_module_) {
                acceptor_->set_ssl_module(ssl_module_);
            }
            acceptor_->set_connection_handler(default_handler_holder_);
            if (!acceptor_->listen(options.backlog)) {
                acceptor_.reset();
                return false;
            }

            acceptor_->set_event_handler(loop);
            if (auto *channel = acceptor_->listener_channel()) {
                loop->update_channel(channel);
            }
            return true;
        }

        bool start_affinity_listener()
        {
            if (!affinity_pending_ || !state_ || !state_->conn_handler) {
                return false;
            }

#ifdef _WIN32
            if (affinity_options_.use_iocp) {
                if (!sharded_listener_) {
                    sharded_listener_ = std::make_unique<IocpShardedAsyncListener>();
                }

                const auto shard_count = (std::max<std::size_t>)(1, affinity_options_.shard_count);
                const auto iocp_worker_count = (std::max<std::size_t>)(1, affinity_options_.iocp_worker_count);
                const bool started = sharded_listener_->listen(
                    affinity_host_,
                    affinity_port_,
                    shard_count,
                    iocp_worker_count,
                    affinity_options_.iocp_completion_batch_size,
                    [state = state_](AsyncConnectionContext ctx) -> coroutine::Task<void> {
                        auto conn = ctx.connection();
                        apply_connection_limits(conn, state->options);
                        std::string ip_key;
                        if (!admit_connection(state, conn, ip_key)) {
                            ctx.close();
                            co_return;
                        }
                        co_await run_guarded_handler(state,
                                                     std::move(ctx),
                                                     std::move(ip_key),
                                                     state->options.max_connections_per_ip > 0);
                    },
                    affinity_options_.backlog);
                if (started) {
                    affinity_pending_ = false;
                }
                return started;
            }
#endif

            if (!generic_sharded_listener_) {
                generic_sharded_listener_ = std::make_unique<ShardedAsyncListener>();
            }

            const auto shard_count = (std::max<std::size_t>)(1, affinity_options_.shard_count);
            ListenOptions options = affinity_options_;
            options.use_iocp = false;
            const bool started = generic_sharded_listener_->listen(
                affinity_host_,
                affinity_port_,
                shard_count,
                options,
                [state = state_](AsyncConnectionContext ctx) -> coroutine::Task<void> {
                    auto conn = ctx.connection();
                    apply_connection_limits(conn, state->options);
                    std::string ip_key;
                    if (!admit_connection(state, conn, ip_key)) {
                        ctx.close();
                        co_return;
                    }
                    co_await run_guarded_handler(state,
                                                 std::move(ctx),
                                                 std::move(ip_key),
                                                 state->options.max_connections_per_ip > 0);
                });
            if (started) {
                affinity_pending_ = false;
            }
            return started;
        }

        class DefaultHandler final : public yuan::net::ConnectionHandler
        {
        public:
            void on_connected(Connection &) override
            {
            }
            void on_error(Connection &conn) override
            {
                conn.close();
            }
            void on_read(Connection &) override
            {
            }
            void on_write(Connection &) override
            {
            }
            void on_close(Connection &) override
            {
            }
            void on_input_shutdown(Connection &conn) override
            {
                (void)conn;
            }
        };

        class DispatchHandler final : public yuan::net::ConnectionHandler
        {
        public:
            explicit DispatchHandler(std::weak_ptr<State> state) noexcept
                : state_(std::move(state))
            {
            }

            void on_connected(Connection &conn) override
            {
                AsyncListenerHost::on_connection_accepted(state_.lock(), conn.shared_from_this());
            }
            void on_error(Connection &conn) override
            {
                conn.close();
            }
            void on_read(Connection &) override
            {
            }
            void on_write(Connection &) override
            {
            }
            void on_close(Connection &) override
            {
            }
            void on_input_shutdown(Connection &) override
            {
            }

        private:
            std::weak_ptr<State> state_;
        };

        NetworkRuntime *runtime_ = nullptr;
        std::shared_ptr<State> state_ = std::make_shared<State>();
        std::shared_ptr<StreamAcceptor> acceptor_;
        std::unique_ptr<IocpShardedAsyncListener> sharded_listener_;
        std::unique_ptr<ShardedAsyncListener> generic_sharded_listener_;
        std::shared_ptr<SSLModule> ssl_module_;
        bool affinity_pending_ = false;
        std::string affinity_host_;
        uint16_t affinity_port_ = 0;
        ListenOptions affinity_options_;
        DefaultHandler default_handler_;
        std::shared_ptr<ConnectionHandler> default_handler_holder_{ make_non_owning_handler(default_handler_) };
        DispatchHandler dispatch_handler_{ state_ };
        std::shared_ptr<ConnectionHandler> dispatch_handler_holder_{ make_non_owning_handler(dispatch_handler_) };
    };

} // namespace yuan::net

#endif
