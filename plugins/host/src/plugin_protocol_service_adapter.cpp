#include "plugin_protocol_service_adapter.h"

#include "buffer/byte_buffer.h"
#include "coroutine/io_result.h"
#include "logger.h"
#include "net/async/async_listener_host.h"
#include "net/socket/listen_options.h"
#include "plugin/plugin.h"
#include "plugin_host_service.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yuan::app
{
    struct PluginProtocolServiceRuntimeStats
    {
        std::atomic<std::uint64_t> framing_error_count{0};
        std::atomic<std::uint64_t> handler_error_count{0};
    };

    struct PluginProtocolActiveConnectionTracker
    {
        struct TrackedConnection
        {
            std::weak_ptr<net::Connection> connection;
            uint64_t resource_guard_id = 0;
        };

        std::mutex mutex;
        std::condition_variable cv;
        std::unordered_map<std::uintptr_t, TrackedConnection> connections;
        bool stopping = false;
    };

    namespace
    {
        constexpr auto kProtocolConnectionDrainTimeout = std::chrono::seconds(3);
        constexpr std::size_t kLengthPrefixedHeaderBytes = sizeof(std::uint32_t);

        std::string protocol_listener_resource_description(
            const plugin::ProtocolServiceDescriptor &protocol_service)
        {
            return "protocol-listener:" + protocol_service.plugin_id + "." + protocol_service.name;
        }

        std::string protocol_connection_resource_description(
            const plugin::ProtocolServiceDescriptor &protocol_service,
            std::uintptr_t connection_id)
        {
            return "protocol-connection:" + protocol_service.plugin_id + "." +
                   protocol_service.name + "#" + std::to_string(connection_id);
        }

        bool is_valid_protocol_service(const plugin::ProtocolServiceDescriptor &protocol_service)
        {
            return !protocol_service.plugin_id.empty() &&
                   !protocol_service.name.empty() &&
                   !protocol_service.contract_id.empty() &&
                   protocol_service.port >= 0 &&
                   protocol_service.port <= 65535 &&
                   protocol_service.read_timeout_ms >= 0 &&
                   protocol_service.write_timeout_ms >= 0 &&
                   protocol_service.idle_timeout_ms >= 0 &&
                   protocol_service.max_connections >= 0 &&
                   protocol_service.max_frame_bytes > 0;
        }

        std::string lower_copy(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        std::string effective_transport(const plugin::ProtocolServiceDescriptor &protocol_service)
        {
            if (!protocol_service.transport.empty()) {
                return lower_copy(protocol_service.transport);
            }
            return lower_copy(protocol_service.protocol.empty() ? "tcp" : protocol_service.protocol);
        }

        std::string effective_framing(const plugin::ProtocolServiceDescriptor &protocol_service)
        {
            return lower_copy(protocol_service.framing.empty() ? "raw" : protocol_service.framing);
        }

        std::size_t decode_length_prefixed_frame_size(std::span<const std::byte> header)
        {
            if (header.size() < kLengthPrefixedHeaderBytes) {
                return 0;
            }

            return (static_cast<std::size_t>(std::to_integer<std::uint32_t>(header[0])) << 24u) |
                   (static_cast<std::size_t>(std::to_integer<std::uint32_t>(header[1])) << 16u) |
                   (static_cast<std::size_t>(std::to_integer<std::uint32_t>(header[2])) << 8u) |
                   static_cast<std::size_t>(std::to_integer<std::uint32_t>(header[3]));
        }

        void reset_runtime_stats(const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats)
        {
            if (!runtime_stats) {
                return;
            }

            runtime_stats->framing_error_count.store(0, std::memory_order_relaxed);
            runtime_stats->handler_error_count.store(0, std::memory_order_relaxed);
        }

        void record_framing_error(const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats)
        {
            if (!runtime_stats) {
                return;
            }

            runtime_stats->framing_error_count.fetch_add(1, std::memory_order_relaxed);
        }

        void record_handler_error(const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats)
        {
            if (!runtime_stats) {
                return;
            }

            runtime_stats->handler_error_count.fetch_add(1, std::memory_order_relaxed);
        }

        bool is_builtin_echo_demo(const plugin::ProtocolServiceDescriptor &protocol_service)
        {
            return lower_copy(protocol_service.type.empty()
                       ? protocol_service.name
                       : protocol_service.type) == "echo" &&
                   protocol_service.handler.empty();
        }

        std::string resolve_handler_name(const plugin::ProtocolServiceDescriptor &protocol_service)
        {
            if (!protocol_service.handler.empty()) {
                return protocol_service.handler;
            }
            if (is_builtin_echo_demo(protocol_service)) {
                return "builtin.echo";
            }
            return {};
        }

        class HostStreamConnectionAdapter final : public plugin::HostStreamConnection
        {
        public:
            explicit HostStreamConnectionAdapter(net::AsyncConnectionContext &ctx)
                : ctx_(ctx)
            {
            }

            std::uintptr_t id() const override
            {
                return ctx_.connection_id();
            }

            std::string peer_address() const override
            {
                auto *conn = ctx_.native_handle();
                return conn ? conn->get_remote_address().to_address_key() : std::string{};
            }

            std::string local_address() const override
            {
                auto *conn = ctx_.native_handle();
                return conn ? conn->get_local_address().to_address_key() : std::string{};
            }

            bool write(std::span<const std::byte> bytes) override
            {
                if (!is_open() || bytes.empty()) {
                    return is_open();
                }
                pending_output_.append(bytes.data(), bytes.size());
                return true;
            }

            bool flush() override
            {
                flush_requested_ = true;
                return is_open();
            }

            void close() override
            {
                close_requested_ = true;
            }

            bool is_open() const override
            {
                return !close_requested_ && !ctx_.is_closed() && ctx_.is_connected();
            }

            bool close_requested() const noexcept
            {
                return close_requested_;
            }

            bool has_pending_output() const noexcept
            {
                return pending_output_.readable_bytes() > 0;
            }

            bool consume_flush_request() noexcept
            {
                const bool requested = flush_requested_;
                flush_requested_ = false;
                return requested;
            }

            ::yuan::buffer::ByteBuffer take_pending_output()
            {
                auto pending = std::move(pending_output_);
                pending_output_ = ::yuan::buffer::ByteBuffer{};
                return pending;
            }

        private:
            net::AsyncConnectionContext &ctx_;
            ::yuan::buffer::ByteBuffer pending_output_;
            bool flush_requested_ = false;
            bool close_requested_ = false;
        };

        class ActiveConnectionGuard
        {
        public:
            explicit ActiveConnectionGuard(std::shared_ptr<std::atomic<int> > count)
                : count_(std::move(count))
            {
            }

            ~ActiveConnectionGuard()
            {
                if (count_) {
                    count_->fetch_sub(1, std::memory_order_relaxed);
                }
            }

            ActiveConnectionGuard(const ActiveConnectionGuard &) = delete;
            ActiveConnectionGuard &operator=(const ActiveConnectionGuard &) = delete;

        private:
            std::shared_ptr<std::atomic<int> > count_;
        };

        class TrackedConnectionGuard
        {
        public:
            TrackedConnectionGuard(std::shared_ptr<PluginProtocolActiveConnectionTracker> tracker,
                                   PluginHostService *host,
                                   std::uintptr_t connection_id,
                                   uint64_t resource_guard_id)
                : tracker_(std::move(tracker)),
                  host_(host),
                  connection_id_(connection_id),
                  resource_guard_id_(resource_guard_id)
            {
            }

            ~TrackedConnectionGuard()
            {
                if (!tracker_ || connection_id_ == 0) {
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(tracker_->mutex);
                    tracker_->connections.erase(connection_id_);
                }
                if (host_ && resource_guard_id_ != 0) {
                    (void)host_->untrack_plugin_resource(resource_guard_id_);
                }
                tracker_->cv.notify_all();
            }

            TrackedConnectionGuard(const TrackedConnectionGuard &) = delete;
            TrackedConnectionGuard &operator=(const TrackedConnectionGuard &) = delete;

        private:
            std::shared_ptr<PluginProtocolActiveConnectionTracker> tracker_;
            PluginHostService *host_ = nullptr;
            std::uintptr_t connection_id_ = 0;
            uint64_t resource_guard_id_ = 0;
        };

        bool track_stream_connection(
            const std::shared_ptr<PluginProtocolActiveConnectionTracker> &tracker,
            const net::AsyncConnectionContext &ctx,
            PluginHostService *host,
            const plugin::ProtocolServiceDescriptor &protocol_service,
            uint64_t &resource_guard_id)
        {
            if (!tracker) {
                return true;
            }

            auto conn = ctx.connection();
            if (!conn) {
                return false;
            }

            resource_guard_id = 0;
            if (host) {
                resource_guard_id = host->track_plugin_resource(
                    protocol_service.plugin_id,
                    plugin::PluginResourceType::network_connection,
                    [weak = std::weak_ptr<net::Connection>(conn)]() {
                        if (auto locked = weak.lock()) {
                            locked->close();
                        }
                    },
                    protocol_connection_resource_description(protocol_service, ctx.connection_id()));
            }

            std::lock_guard<std::mutex> lock(tracker->mutex);
            if (tracker->stopping) {
                if (host && resource_guard_id != 0) {
                    (void)host->untrack_plugin_resource(resource_guard_id);
                    resource_guard_id = 0;
                }
                return false;
            }
            tracker->connections[ctx.connection_id()] = PluginProtocolActiveConnectionTracker::TrackedConnection{
                conn,
                resource_guard_id
            };
            return true;
        }

        void request_tracked_connection_shutdown(
            const std::shared_ptr<PluginProtocolActiveConnectionTracker> &tracker,
            net::NetworkRuntime *runtime)
        {
            if (!tracker) {
                return;
            }

            std::vector<std::shared_ptr<net::Connection> > connections;
            {
                std::lock_guard<std::mutex> lock(tracker->mutex);
                tracker->stopping = true;
                connections.reserve(tracker->connections.size());
                for (const auto &entry : tracker->connections) {
                    if (auto conn = entry.second.connection.lock()) {
                        connections.push_back(std::move(conn));
                    }
                }
            }

            if (connections.empty()) {
                tracker->cv.notify_all();
                return;
            }

            auto close_connections = [connections = std::move(connections)]() {
                for (const auto &conn : connections) {
                    if (conn) {
                        conn->close();
                    }
                }
            };

            if (runtime) {
                runtime->dispatch(std::move(close_connections));
            } else {
                close_connections();
            }
        }

        bool wait_for_tracked_connections(
            const std::shared_ptr<PluginProtocolActiveConnectionTracker> &tracker,
            std::chrono::steady_clock::duration timeout)
        {
            if (!tracker) {
                return true;
            }

            std::unique_lock<std::mutex> lock(tracker->mutex);
            return tracker->cv.wait_for(lock, timeout, [&]() {
                return tracker->connections.empty();
            });
        }

        void apply_recorded_fault_policy(PluginHostService *host,
                                         const std::string &plugin_name)
        {
            if (!host || plugin_name.empty()) {
                return;
            }

            auto &lifecycle = host->lifecycle_manager();
            auto current_state = lifecycle.state(plugin_name);
            const auto suggested_state = lifecycle.call_guard().suggested_state(plugin_name);

            if (suggested_state == plugin::PluginState::degraded) {
                if (current_state == plugin::PluginState::active) {
                    lifecycle.degrade(plugin_name);
                }
                return;
            }

            if (suggested_state == plugin::PluginState::faulted ||
                suggested_state == plugin::PluginState::quarantined) {
                if (current_state == plugin::PluginState::initialized ||
                    current_state == plugin::PluginState::active ||
                    current_state == plugin::PluginState::degraded) {
                    lifecycle.transition(plugin_name, plugin::PluginState::faulted);
                    current_state = lifecycle.state(plugin_name);
                }

                if (suggested_state == plugin::PluginState::quarantined &&
                    current_state == plugin::PluginState::faulted) {
                    lifecycle.quarantine(plugin_name);
                }
            }
        }

        struct GuardedBoolHandlerResult
        {
            bool dispatched = false;
            bool threw = false;
            bool value = false;
            std::string error_message;
        };

        template <typename Fn>
        GuardedBoolHandlerResult invoke_guarded_bool_handler(PluginHostService *host,
                                                             const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats,
                                                             const plugin::ProtocolServiceDescriptor &protocol_service,
                                                             const char *call_site,
                                                             const char *callback_name,
                                                             Fn &&fn)
        {
            if (!host) {
                return GuardedBoolHandlerResult{ true, false, std::forward<Fn>(fn)() };
            }

            auto &lifecycle = host->lifecycle_manager();
            const auto state = lifecycle.state(protocol_service.plugin_id);
            if (!plugin::accepts_callbacks(state)) {
                return {};
            }

            try {
                return GuardedBoolHandlerResult{
                    true,
                    false,
                    lifecycle.call_guard().guarded_call(
                        protocol_service.plugin_id,
                        state,
                        call_site,
                        std::forward<Fn>(fn)),
                    {}
                };
            } catch (const std::exception &ex) {
                LOG_ERROR("plugin protocol service '{}.{}' handler threw from {}: {}",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          callback_name,
                          ex.what());
                record_handler_error(runtime_stats);
                apply_recorded_fault_policy(host, protocol_service.plugin_id);
                return GuardedBoolHandlerResult{ true, true, false, ex.what() };
            } catch (...) {
                LOG_ERROR("plugin protocol service '{}.{}' handler threw from {}",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          callback_name);
                record_handler_error(runtime_stats);
                apply_recorded_fault_policy(host, protocol_service.plugin_id);
                return GuardedBoolHandlerResult{ true, true, false, "unknown handler exception" };
            }
        }

        template <typename Fn>
        void invoke_guarded_void_handler(PluginHostService *host,
                                         const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats,
                                         const plugin::ProtocolServiceDescriptor &protocol_service,
                                         const char *call_site,
                                         const char *callback_name,
                                         Fn &&fn)
        {
            if (!host) {
                std::forward<Fn>(fn)();
                return;
            }

            auto &lifecycle = host->lifecycle_manager();
            const auto state = lifecycle.state(protocol_service.plugin_id);
            if (!plugin::accepts_callbacks(state)) {
                return;
            }

            try {
                (void)lifecycle.call_guard().guarded_call(
                    protocol_service.plugin_id,
                    state,
                    call_site,
                    [&]() -> bool {
                        std::forward<Fn>(fn)();
                        return true;
                    });
            } catch (const std::exception &ex) {
                LOG_ERROR("plugin protocol service '{}.{}' handler threw from {}: {}",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          callback_name,
                          ex.what());
                record_handler_error(runtime_stats);
                apply_recorded_fault_policy(host, protocol_service.plugin_id);
            } catch (...) {
                LOG_ERROR("plugin protocol service '{}.{}' handler threw from {}",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          callback_name);
                record_handler_error(runtime_stats);
                apply_recorded_fault_policy(host, protocol_service.plugin_id);
            }
        }

        void report_handler_failure(PluginHostService *host,
                                    const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats,
                                    const plugin::ProtocolServiceDescriptor &protocol_service,
                                    const char *call_site,
                                    const char *reason)
        {
            record_handler_error(runtime_stats);
            if (!host) {
                return;
            }

            host->lifecycle_manager().fault(
                protocol_service.plugin_id,
                std::string(call_site) + ": " + reason);
        }

        void notify_handler_error(PluginHostService *host,
                                  const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats,
                                  plugin::PluginStreamProtocolHandler &handler,
                                  HostStreamConnectionAdapter &connection,
                                  const plugin::ProtocolServiceDescriptor &protocol_service,
                                  std::string message,
                                  int code = 0)
        {
            plugin::ProtocolHandlerErrorInfo error;
            error.code = code;
            error.message = std::move(message);

            invoke_guarded_void_handler(
                host,
                runtime_stats,
                protocol_service,
                "protocol.on_error",
                "on_error",
                [&]() {
                    handler.on_error(connection, error);
                });
        }

        bool call_handler_on_accept(PluginHostService *host,
                                    const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats,
                                    plugin::PluginStreamProtocolHandler &handler,
                                    HostStreamConnectionAdapter &connection,
                                    const plugin::ProtocolServiceDescriptor &protocol_service)
        {
            const auto result = invoke_guarded_bool_handler(
                host,
                runtime_stats,
                protocol_service,
                "protocol.on_accept",
                "on_accept",
                [&]() {
                    return handler.on_accept(connection);
                });

            if (result.threw) {
                notify_handler_error(host, runtime_stats, handler, connection, protocol_service, result.error_message, 1);
                return false;
            }

            if (!result.dispatched) {
                return false;
            }

            if (!result.value) {
                report_handler_failure(host, runtime_stats, protocol_service, "protocol.on_accept", "handler returned false");
                notify_handler_error(host, runtime_stats, handler, connection, protocol_service, "handler returned false", 1);
                return false;
            }

            return true;
        }

        bool call_handler_on_data(PluginHostService *host,
                                  const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats,
                                  plugin::PluginStreamProtocolHandler &handler,
                                  HostStreamConnectionAdapter &connection,
                                  std::span<const std::byte> bytes,
                                  const plugin::ProtocolServiceDescriptor &protocol_service)
        {
            const auto result = invoke_guarded_bool_handler(
                host,
                runtime_stats,
                protocol_service,
                "protocol.on_data",
                "on_data",
                [&]() {
                    return handler.on_data(connection, bytes);
                });

            if (result.threw) {
                notify_handler_error(host, runtime_stats, handler, connection, protocol_service, result.error_message, 1);
                return false;
            }

            if (!result.dispatched) {
                return false;
            }

            if (!result.value) {
                report_handler_failure(host, runtime_stats, protocol_service, "protocol.on_data", "handler returned false");
                return false;
            }

            return true;
        }

        bool idle_timeout_expired(std::chrono::steady_clock::time_point last_activity,
                                  uint32_t idle_timeout_ms)
        {
            if (idle_timeout_ms == 0) {
                return false;
            }

            return std::chrono::steady_clock::now() - last_activity >=
                   std::chrono::milliseconds(idle_timeout_ms);
        }

        uint32_t effective_read_timeout_ms(uint32_t read_timeout_ms,
                                           std::chrono::steady_clock::time_point last_activity,
                                           uint32_t idle_timeout_ms)
        {
            if (idle_timeout_ms == 0) {
                return read_timeout_ms;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto idle_timeout = std::chrono::milliseconds(idle_timeout_ms);
            if (now - last_activity >= idle_timeout) {
                return 1;
            }

            const auto remaining_idle =
                std::chrono::duration_cast<std::chrono::milliseconds>(idle_timeout - (now - last_activity));
            const uint32_t remaining_idle_ms =
                remaining_idle.count() <= 0 ? 1u : static_cast<uint32_t>(remaining_idle.count());

            if (read_timeout_ms == 0) {
                return remaining_idle_ms;
            }
            return std::min(read_timeout_ms, remaining_idle_ms);
        }

        coroutine::Task<bool> flush_pending_writes(net::AsyncConnectionContext &ctx,
                                                  HostStreamConnectionAdapter &connection,
                                                  uint32_t write_timeout_ms)
        {
            const bool flush_requested = connection.consume_flush_request();
            auto pending = connection.take_pending_output();
            const bool has_pending = pending.readable_bytes() > 0;
            if (has_pending) {
                auto write = co_await ctx.write_async(pending, write_timeout_ms);
                if (write.status != coroutine::IoStatus::success) {
                    co_return false;
                }
            }

            if (has_pending || flush_requested) {
                auto flush = co_await ctx.flush_async(write_timeout_ms);
                if (flush.status != coroutine::IoStatus::success) {
                    co_return false;
                }
            }

            co_return true;
        }

        coroutine::Task<void> run_stream_protocol(
            yuan::net::AsyncConnectionContext ctx,
            plugin::PluginStreamProtocolHandlerFactory handler_factory,
            plugin::ProtocolServiceDescriptor protocol_service,
            std::shared_ptr<std::atomic<int> > active_connections,
            std::shared_ptr<PluginProtocolServiceRuntimeStats> runtime_stats,
            PluginHostService *host)
        {
            ActiveConnectionGuard active_guard(std::move(active_connections));
            ctx.install_default_handler();
            if (!handler_factory) {
                (void)co_await ctx.close_async();
                co_return;
            }

            std::unique_ptr<plugin::PluginStreamProtocolHandler> handler;
            bool handler_factory_failed = false;
            try {
                handler = handler_factory(protocol_service);
            } catch (const std::exception &ex) {
                LOG_ERROR("plugin protocol service '{}.{}' handler factory threw: {}",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          ex.what());
                handler_factory_failed = true;
            } catch (...) {
                LOG_ERROR("plugin protocol service '{}.{}' handler factory threw",
                          protocol_service.plugin_id,
                          protocol_service.name);
                handler_factory_failed = true;
            }
            if (handler_factory_failed) {
                (void)co_await ctx.close_async();
                co_return;
            }
            if (!handler) {
                LOG_ERROR("plugin protocol service '{}.{}' handler factory returned null",
                          protocol_service.plugin_id,
                          protocol_service.name);
                (void)co_await ctx.close_async();
                co_return;
            }

            HostStreamConnectionAdapter connection(ctx);
            const auto write_timeout_ms = static_cast<uint32_t>(protocol_service.write_timeout_ms);
            const bool accepted = call_handler_on_accept(host, runtime_stats, *handler, connection, protocol_service);
            if (!accepted) {
                (void)co_await ctx.close_async();
                co_return;
            }
            const bool accept_writes_flushed = co_await flush_pending_writes(ctx, connection, write_timeout_ms);
            if (!accept_writes_flushed || connection.close_requested()) {
                (void)co_await ctx.close_async();
                co_return;
            }

            const auto framing = effective_framing(protocol_service);
            const auto read_timeout_ms = static_cast<uint32_t>(protocol_service.read_timeout_ms);
            const auto idle_timeout_ms = static_cast<uint32_t>(protocol_service.idle_timeout_ms);
            const auto max_frame_bytes = static_cast<std::size_t>(protocol_service.max_frame_bytes);
            std::vector<std::byte> frame_buffer;
            auto last_activity = std::chrono::steady_clock::now();
            bool deliver_partial_line = false;

            while (ctx) {
                if (idle_timeout_expired(last_activity, idle_timeout_ms)) {
                    break;
                }

                auto read = co_await ctx.read_async(
                    effective_read_timeout_ms(read_timeout_ms, last_activity, idle_timeout_ms),
                    true);
                if (read.status != coroutine::IoStatus::success || read.data.readable_bytes() == 0) {
                    deliver_partial_line = read.status == coroutine::IoStatus::connection_closed;
                    break;
                }
                last_activity = std::chrono::steady_clock::now();

                const auto *read_ptr = reinterpret_cast<const std::byte *>(read.data.read_ptr());
                const auto readable = read.data.readable_bytes();
                if (framing == "raw") {
                    if (readable > max_frame_bytes) {
                        record_framing_error(runtime_stats);
                        notify_handler_error(host, runtime_stats, *handler, connection, protocol_service, "frame too large", 2);
                        break;
                    }
                    if (!call_handler_on_data(
                            host,
                            runtime_stats,
                            *handler,
                            connection,
                            std::span<const std::byte>(read_ptr, readable),
                            protocol_service)) {
                        break;
                    }
                    const bool flushed = co_await flush_pending_writes(ctx, connection, write_timeout_ms);
                    if (!flushed) {
                        break;
                    }
                    if (connection.close_requested()) {
                        break;
                    }
                    continue;
                }

                if (framing == "line") {
                    frame_buffer.insert(frame_buffer.end(), read_ptr, read_ptr + readable);
                    if (frame_buffer.size() > max_frame_bytes) {
                        record_framing_error(runtime_stats);
                        notify_handler_error(host, runtime_stats, *handler, connection, protocol_service, "line frame too large", 2);
                        break;
                    }

                    bool keep_open = true;
                    auto search_begin = frame_buffer.begin();
                    while (keep_open) {
                        auto it = std::find(
                            search_begin,
                            frame_buffer.end(),
                            std::byte{ static_cast<unsigned char>('\n') });
                        if (it == frame_buffer.end()) {
                            break;
                        }

                        auto frame_end = it;
                        if (frame_end != frame_buffer.begin() &&
                            *(frame_end - 1) == std::byte{ static_cast<unsigned char>('\r') }) {
                            --frame_end;
                        }

                        std::vector<std::byte> frame(frame_buffer.begin(), frame_end);
                        keep_open = call_handler_on_data(
                            host,
                            runtime_stats,
                            *handler,
                            connection,
                            std::span<const std::byte>(frame.data(), frame.size()),
                            protocol_service);
                        frame_buffer.erase(frame_buffer.begin(), it + 1);
                        search_begin = frame_buffer.begin();

                        const bool flushed = co_await flush_pending_writes(ctx, connection, write_timeout_ms);
                        if (!flushed) {
                            keep_open = false;
                        }
                        if (connection.close_requested()) {
                            keep_open = false;
                        }
                    }

                    if (!keep_open) {
                        break;
                    }
                    continue;
                }

                if (framing == "length_prefixed") {
                    frame_buffer.insert(frame_buffer.end(), read_ptr, read_ptr + readable);

                    bool keep_open = true;
                    while (keep_open) {
                        if (frame_buffer.size() < kLengthPrefixedHeaderBytes) {
                            break;
                        }

                        const auto frame_size = decode_length_prefixed_frame_size(
                            std::span<const std::byte>(frame_buffer.data(), kLengthPrefixedHeaderBytes));
                        if (frame_size > max_frame_bytes) {
                            record_framing_error(runtime_stats);
                            notify_handler_error(
                                host,
                                runtime_stats,
                                *handler,
                                connection,
                                protocol_service,
                                "length-prefixed frame too large",
                                2);
                            keep_open = false;
                            break;
                        }

                        const auto total_frame_bytes = kLengthPrefixedHeaderBytes + frame_size;
                        if (frame_buffer.size() < total_frame_bytes) {
                            break;
                        }

                        keep_open = call_handler_on_data(
                            host,
                            runtime_stats,
                            *handler,
                            connection,
                            std::span<const std::byte>(
                                frame_buffer.data() + kLengthPrefixedHeaderBytes,
                                frame_size),
                            protocol_service);
                        frame_buffer.erase(
                            frame_buffer.begin(),
                            frame_buffer.begin() + static_cast<std::ptrdiff_t>(total_frame_bytes));

                        const bool flushed = co_await flush_pending_writes(ctx, connection, write_timeout_ms);
                        if (!flushed) {
                            keep_open = false;
                        }
                        if (connection.close_requested()) {
                            keep_open = false;
                        }
                    }

                    if (!keep_open) {
                        break;
                    }
                    continue;
                }

                record_framing_error(runtime_stats);
                notify_handler_error(host, runtime_stats, *handler, connection, protocol_service, "unsupported framing", 3);
                break;
            }

            if (framing == "line" && deliver_partial_line &&
                !frame_buffer.empty() && frame_buffer.size() <= max_frame_bytes) {
                (void)call_handler_on_data(
                    host,
                    runtime_stats,
                    *handler,
                    connection,
                    std::span<const std::byte>(frame_buffer.data(), frame_buffer.size()),
                    protocol_service);
                (void)co_await flush_pending_writes(ctx, connection, write_timeout_ms);
            }

            invoke_guarded_void_handler(
                host,
                runtime_stats,
                protocol_service,
                "protocol.on_close",
                "on_close",
                [&]() {
                    handler->on_close(connection);
                });

            if (!ctx.is_closed()) {
                (void)co_await ctx.close_async();
            }
        }

        coroutine::Task<void> reject_stream_connection(yuan::net::AsyncConnectionContext ctx)
        {
            ctx.install_default_handler();
            if (!ctx.is_closed()) {
                (void)co_await ctx.close_async();
            }
        }

        bool framing_is_supported(const std::string &framing)
        {
            return framing == "raw" || framing == "line" || framing == "length_prefixed";
        }

        bool reserve_connection_slot(std::shared_ptr<std::atomic<int> > active_connections,
                                     int max_connections)
        {
            if (!active_connections) {
                return true;
            }
            const int previous = active_connections->fetch_add(1, std::memory_order_relaxed);
            if (max_connections <= 0 || previous < max_connections) {
                return true;
            }
            active_connections->fetch_sub(1, std::memory_order_relaxed);
            return false;
        }

        coroutine::Task<void> run_limited_stream_protocol(
            yuan::net::AsyncConnectionContext ctx,
            plugin::PluginStreamProtocolHandlerFactory handler_factory,
            plugin::ProtocolServiceDescriptor protocol_service,
            std::shared_ptr<std::atomic<int> > active_connections,
            std::shared_ptr<PluginProtocolActiveConnectionTracker> connection_tracker,
            std::shared_ptr<PluginProtocolServiceRuntimeStats> runtime_stats,
            PluginHostService *host)
        {
            const auto connection_id = ctx.connection_id();
            uint64_t connection_resource_id = 0;
            if (!track_stream_connection(connection_tracker, ctx, host, protocol_service, connection_resource_id)) {
                co_await reject_stream_connection(std::move(ctx));
                co_return;
            }
            TrackedConnectionGuard tracked_guard(
                std::move(connection_tracker),
                host,
                connection_id,
                connection_resource_id);

            if (!reserve_connection_slot(active_connections, protocol_service.max_connections)) {
                LOG_WARN("plugin protocol service '{}.{}' rejected connection: active connection limit {} reached",
                         protocol_service.plugin_id,
                         protocol_service.name,
                         protocol_service.max_connections);
                co_await reject_stream_connection(std::move(ctx));
                co_return;
            }

            co_await run_stream_protocol(
                std::move(ctx),
                std::move(handler_factory),
                std::move(protocol_service),
                std::move(active_connections),
                std::move(runtime_stats),
                host);
        }

        const char *manifest_reason(const plugin::ProtocolServiceDescriptor &protocol_service)
        {
            if (protocol_service.plugin_id.empty()) {
                return "missing plugin id";
            }
            if (protocol_service.name.empty()) {
                return "missing service name";
            }
            if (protocol_service.contract_id.empty()) {
                return "missing contract id";
            }
            if (protocol_service.port < 0 || protocol_service.port > 65535) {
                return "port out of range";
            }
            if (protocol_service.max_frame_bytes <= 0) {
                return "max_frame_bytes must be positive";
            }
            if (protocol_service.max_connections < 0) {
                return "max_connections must be non-negative";
            }
            if (protocol_service.read_timeout_ms < 0 ||
                protocol_service.write_timeout_ms < 0 ||
                protocol_service.idle_timeout_ms < 0) {
                return "timeouts must be non-negative";
            }
            return "unknown manifest validation failure";
        }

    } // namespace

    PluginProtocolServiceAdapter::PluginProtocolServiceAdapter(
        std::string plugin_path,
        plugin::ProtocolServiceDescriptor protocol_service)
        : plugin_path_(std::move(plugin_path)),
          protocol_service_(std::move(protocol_service))
    {
    }

    PluginProtocolServiceAdapter::~PluginProtocolServiceAdapter()
    {
        stop();
    }

    void PluginProtocolServiceAdapter::set_runtime_context(const RuntimeContext &context)
    {
        runtime_context_ = context;
        if (host_) {
            host_->set_runtime_context(runtime_context_);
        }
    }

    bool PluginProtocolServiceAdapter::init()
    {
        if (initialized_) {
            return true;
        }
        if (plugin_path_.empty() || !is_valid_protocol_service(protocol_service_)) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' manifest validation failed: {}",
                protocol_service_.plugin_id,
                protocol_service_.name,
                manifest_reason(protocol_service_));
            return false;
        }

        handler_registry_.clear();
        plugin::register_builtin_protocol_handlers(handler_registry_);

        host_ = std::make_unique<PluginHostService>(
            plugin_path_,
            std::vector<std::string>{ protocol_service_.plugin_id });
        host_->set_runtime_context(runtime_context_);

        if (!host_->init()) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' failed to initialize plugin runtime",
                protocol_service_.plugin_id,
                protocol_service_.name);
            host_.reset();
            return false;
        }

        if (auto *plugin = host_->get_plugin(protocol_service_.plugin_id)) {
            try {
                plugin->register_protocol_handlers(handler_registry_);
            } catch (const std::exception &ex) {
                LOG_ERROR(
                    "plugin protocol service '{}.{}' failed while registering protocol handlers: {}",
                    protocol_service_.plugin_id,
                    protocol_service_.name,
                    ex.what());
                handler_registry_.clear();
                host_.reset();
                return false;
            } catch (...) {
                LOG_ERROR(
                    "plugin protocol service '{}.{}' failed while registering protocol handlers",
                    protocol_service_.plugin_id,
                    protocol_service_.name);
                handler_registry_.clear();
                host_.reset();
                return false;
            }
        }

        initialized_ = true;
        return true;
    }

    void PluginProtocolServiceAdapter::start()
    {
        if (started_) {
            return;
        }
        if (!initialized_ && !init()) {
            return;
        }
        if (host_) {
            host_->start();
        }
        if (!start_protocol_listener()) {
            handler_registry_.clear();
            if (host_) {
                host_->stop();
            }
            return;
        }
        started_ = true;
    }

    void PluginProtocolServiceAdapter::stop()
    {
        stop_protocol_listener();
        handler_registry_.clear();
        if (host_) {
            host_->stop();
            host_.reset();
        }
        started_ = false;
        initialized_ = false;
    }

    const std::string &PluginProtocolServiceAdapter::plugin_path() const noexcept
    {
        return plugin_path_;
    }

    const plugin::ProtocolServiceDescriptor &PluginProtocolServiceAdapter::protocol_service() const noexcept
    {
        return protocol_service_;
    }

    bool PluginProtocolServiceAdapter::initialized() const noexcept
    {
        return initialized_;
    }

    bool PluginProtocolServiceAdapter::started() const noexcept
    {
        return started_;
    }

    std::string PluginProtocolServiceAdapter::resource_leak_report() const
    {
        if (!host_) {
            return {};
        }
        return host_->resource_leak_report(protocol_service_.plugin_id);
    }

    ProtocolServiceRuntimeStatsSnapshot PluginProtocolServiceAdapter::runtime_stats() const noexcept
    {
        ProtocolServiceRuntimeStatsSnapshot snapshot;
        if (!runtime_stats_) {
            return snapshot;
        }

        snapshot.framing_error_count =
            runtime_stats_->framing_error_count.load(std::memory_order_relaxed);
        snapshot.handler_error_count =
            runtime_stats_->handler_error_count.load(std::memory_order_relaxed);
        return snapshot;
    }

    bool PluginProtocolServiceAdapter::start_protocol_listener()
    {
        if (listener_) {
            return true;
        }

        if (!is_valid_protocol_service(protocol_service_)) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' manifest validation failed: {}",
                protocol_service_.plugin_id,
                protocol_service_.name,
                manifest_reason(protocol_service_));
            return false;
        }

        const auto transport = effective_transport(protocol_service_);
        if (transport != "tcp") {
            LOG_ERROR(
                "plugin protocol service '{}.{}' has unsupported transport '{}'",
                protocol_service_.plugin_id,
                protocol_service_.name,
                transport);
            return false;
        }

        const auto framing = effective_framing(protocol_service_);
        if (!framing_is_supported(framing)) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' has unsupported framing '{}'",
                protocol_service_.plugin_id,
                protocol_service_.name,
                framing);
            return false;
        }

        const auto handler_name = resolve_handler_name(protocol_service_);
        if (handler_name.empty()) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' has no handler; set 'handler' for custom services or use type='echo' for the built-in demo",
                protocol_service_.plugin_id,
                protocol_service_.name);
            return false;
        }

        auto handler_factory = handler_registry_.find_stream_handler(handler_name);
        if (!handler_factory) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' handler '{}' was not registered",
                protocol_service_.plugin_id,
                protocol_service_.name,
                handler_name);
            return false;
        }

        if (is_builtin_echo_demo(protocol_service_)) {
            LOG_INFO(
                "plugin protocol service '{}.{}' uses built-in demo protocol handler '{}'; this is not custom plugin protocol logic",
                protocol_service_.plugin_id,
                protocol_service_.name,
                handler_name);
        }

        auto *runtime = runtime_context_.shared_runtime;
        if (!runtime) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' requires a worker-owned NetworkRuntime",
                protocol_service_.plugin_id,
                protocol_service_.name);
            return false;
        }

        net::ListenOptions options;
        options.reuse_port = runtime_context_.listener_reuse_port;

        auto active_connections = std::make_shared<std::atomic<int> >(0);
        connection_tracker_ = std::make_shared<PluginProtocolActiveConnectionTracker>();
        if (!runtime_stats_) {
            runtime_stats_ = std::make_shared<PluginProtocolServiceRuntimeStats>();
        }
        reset_runtime_stats(runtime_stats_);
        auto captured_service = protocol_service_;
        auto captured_handler_factory = std::move(handler_factory);
        auto captured_connection_tracker = connection_tracker_;
        auto captured_runtime_stats = runtime_stats_;
        auto *captured_host = host_.get();
        auto listener = std::make_unique<net::AsyncListenerHost>();
        listener->set_connection_handler(
            [captured_handler_factory, captured_service, active_connections, captured_connection_tracker, captured_runtime_stats, captured_host](
                net::AsyncConnectionContext ctx) {
                return run_limited_stream_protocol(
                    std::move(ctx),
                    captured_handler_factory,
                    captured_service,
                    active_connections,
                    captured_connection_tracker,
                    captured_runtime_stats,
                    captured_host);
            });

        const auto host = protocol_service_.host.empty() ? "0.0.0.0" : protocol_service_.host;
        if (!listener->bind(host, static_cast<uint16_t>(protocol_service_.port), *runtime, options)) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' failed to bind {}://{}:{}",
                protocol_service_.plugin_id,
                protocol_service_.name,
                transport,
                host,
                protocol_service_.port);
            connection_tracker_.reset();
            return false;
        }

        auto task = listener->run_async();
        task.resume();
        task.detach();

        listener_ = std::move(listener);
        if (host_) {
            listener_resource_id_ = host_->track_plugin_resource(
                protocol_service_.plugin_id,
                plugin::PluginResourceType::network_listener,
                [this]() {
                    if (listener_) {
                        listener_->close();
                    }
                },
                protocol_listener_resource_description(protocol_service_));
        }
        return true;
    }

    void PluginProtocolServiceAdapter::stop_protocol_listener()
    {
        if (listener_resource_id_ != 0 && host_) {
            (void)host_->untrack_plugin_resource(listener_resource_id_);
            listener_resource_id_ = 0;
        }

        auto tracker = connection_tracker_;
        if (listener_) {
            listener_->close();
            listener_.reset();
        }
        if (tracker) {
            request_tracked_connection_shutdown(tracker, runtime_context_.shared_runtime);
            if (!wait_for_tracked_connections(tracker, kProtocolConnectionDrainTimeout)) {
                LOG_WARN(
                    "plugin protocol service '{}.{}' still has active connections after shutdown drain timeout",
                    protocol_service_.plugin_id,
                    protocol_service_.name);
            }
            connection_tracker_.reset();
        }
    }

    std::optional<ServiceDescriptor> make_plugin_protocol_service_descriptor(
        const plugin::ProtocolServiceDescriptor &protocol_service,
        ServicePlacement placement)
    {
        if (!is_valid_protocol_service(protocol_service)) {
            return std::nullopt;
        }

        ServiceDescriptor descriptor;
        descriptor.name = protocol_service.plugin_id + "." + protocol_service.name;
        descriptor.type_name = protocol_service.type.empty()
            ? "plugin.protocol"
            : "plugin.protocol." + protocol_service.type;
        descriptor.contract_id = protocol_service.contract_id;
        descriptor.contract_version = protocol_service.contract_version;
        descriptor.placement = placement;
        descriptor.endpoints.push_back(ServiceEndpoint{
            protocol_service.name,
            protocol_service.host.empty() ? "0.0.0.0" : protocol_service.host,
            protocol_service.port,
            effective_transport(protocol_service)
        });
        return descriptor;
    }

    std::vector<ServiceDescriptor> make_plugin_protocol_service_descriptors(
        const std::vector<plugin::ProtocolServiceDescriptor> &protocol_services,
        ServicePlacement placement)
    {
        std::vector<ServiceDescriptor> descriptors;
        descriptors.reserve(protocol_services.size());
        for (const auto &protocol_service : protocol_services) {
            auto descriptor = make_plugin_protocol_service_descriptor(protocol_service, placement);
            if (descriptor) {
                descriptors.push_back(std::move(*descriptor));
            }
        }
        return descriptors;
    }

    bool add_plugin_protocol_service(
        Application &application,
        std::string plugin_path,
        const plugin::ProtocolServiceDescriptor &protocol_service,
        ServicePlacement placement)
    {
        auto descriptor = make_plugin_protocol_service_descriptor(protocol_service, placement);
        if (!descriptor || plugin_path.empty()) {
            return false;
        }

        auto captured_path = std::move(plugin_path);
        auto captured_service = protocol_service;
        return application.add_service(
            std::move(*descriptor),
            [captured_path = std::move(captured_path), captured_service]() {
                return std::make_shared<PluginProtocolServiceAdapter>(
                    captured_path,
                    captured_service);
            });
    }

    std::size_t add_plugin_protocol_services(
        Application &application,
        std::string plugin_path,
        const std::vector<plugin::ProtocolServiceDescriptor> &protocol_services,
        ServicePlacement placement)
    {
        std::size_t added = 0;
        for (const auto &protocol_service : protocol_services) {
            if (add_plugin_protocol_service(application, plugin_path, protocol_service, placement)) {
                ++added;
            }
        }
        return added;
    }

} // namespace yuan::app
