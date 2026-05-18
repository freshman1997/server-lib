#include "plugin_protocol_service_adapter.h"

#include "buffer/byte_buffer.h"
#include "coroutine/io_result.h"
#include "eventbus/event_bus.h"
#include "logger.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/async/async_listener_host.h"
#include "net/handler/connection_handler.h"
#include "net/session/connection_context.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "net/socket/listen_options.h"
#include "plugin/plugin.h"
#include "plugin/plugin_events.h"
#include "plugin_host_service.h"
#include "timer/timer_handle.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
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
        std::atomic<std::uint64_t> active_connection_count{0};
        std::atomic<std::uint64_t> accepted_connection_count{0};
        std::atomic<std::uint64_t> closed_connection_count{0};
        std::atomic<std::uint64_t> bytes_received{0};
        std::atomic<std::uint64_t> bytes_sent{0};
        std::atomic<std::uint64_t> framing_error_count{0};
        std::atomic<std::uint64_t> handler_error_count{0};
        std::atomic<std::uint64_t> backpressure_drop_count{0};
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
        constexpr auto kProtocolStopDispatchTimeout = std::chrono::seconds(1);
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

        std::string protocol_udp_peer_resource_description(
            const plugin::ProtocolServiceDescriptor &protocol_service,
            std::string_view peer)
        {
            return "protocol-udp-peer:" + protocol_service.plugin_id + "." +
                   protocol_service.name + "#" + std::string(peer);
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
                   protocol_service.max_frame_bytes > 0 &&
                   protocol_service.max_write_buffer_bytes > 0;
        }

        std::string lower_copy(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        std::string bool_text(bool value)
        {
            return value ? "true" : "false";
        }

        std::string runtime_identity_text(const RuntimeContext &runtime_context)
        {
            return "worker=" + std::to_string(runtime_context.worker_index) +
                   ",service_instance=" + std::to_string(runtime_context.service_instance_index) +
                   ",active_service=" + runtime_context.active_service_name;
        }

        std::string runtime_identity_text(const RuntimeContext *runtime_context)
        {
            if (!runtime_context) {
                return "worker=unknown,service_instance=unknown,active_service=unknown";
            }
            return runtime_identity_text(*runtime_context);
        }

        std::string join_permission_names(plugin::PluginPermission permissions)
        {
            const auto names = plugin::PluginPermissionNames::to_names(permissions);
            if (names.empty()) {
                return "none";
            }

            std::string joined;
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i > 0) {
                    joined += ",";
                }
                joined += names[i];
            }
            return joined;
        }

        bool dispatch_to_runtime_and_wait(net::NetworkRuntime *runtime,
                                          std::function<void()> callback,
                                          std::chrono::steady_clock::duration timeout = kProtocolStopDispatchTimeout)
        {
            if (!callback) {
                return true;
            }
            if (!runtime) {
                callback();
                return true;
            }

            struct DispatchState
            {
                std::mutex mutex;
                std::condition_variable cv;
                bool completed = false;
            };

            auto state = std::make_shared<DispatchState>();
            runtime->dispatch([callback = std::move(callback), state]() mutable {
                callback();
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->completed = true;
                }
                state->cv.notify_one();
            });

            std::unique_lock<std::mutex> lock(state->mutex);
            return state->cv.wait_for(lock, timeout, [&]() {
                return state->completed;
            });
        }

        plugin::PluginRunMode to_plugin_run_mode(RunMode mode)
        {
            switch (mode) {
            case RunMode::single_thread:
                return plugin::PluginRunMode::single_thread;
            case RunMode::multi_thread:
                return plugin::PluginRunMode::multi_thread;
            case RunMode::multi_process:
                return plugin::PluginRunMode::multi_process;
            default:
                return plugin::PluginRunMode::unknown;
            }
        }

        plugin::PluginProtocolServiceEvent make_protocol_service_event(
            const RuntimeContext &runtime_context,
            const plugin::ProtocolServiceDescriptor &protocol_service,
            std::string reason = {})
        {
            plugin::PluginProtocolServiceEvent event;
            event.app_name = runtime_context.app_name;
            event.plugin_name = protocol_service.plugin_id;
            event.run_mode = to_plugin_run_mode(runtime_context.run_mode);
            event.worker_threads = runtime_context.worker_threads;
            event.runtime_worker_count = runtime_context.runtime_worker_count == 0
                ? runtime_context.worker_threads
                : runtime_context.runtime_worker_count;
            event.worker_index = runtime_context.worker_index;
            event.is_worker_process = runtime_context.is_worker_process;
            event.active_service_name = runtime_context.active_service_name;
            event.service_index = runtime_context.service_index;
            event.service_instance_index = runtime_context.service_instance_index;
            event.service_instance_count = runtime_context.service_instance_count == 0
                ? 1
                : runtime_context.service_instance_count;
            event.listener_reuse_port = runtime_context.listener_reuse_port;
            event.protocol_service_name = protocol_service.name;
            event.transport = lower_copy(
                !protocol_service.transport.empty()
                    ? protocol_service.transport
                    : (protocol_service.protocol.empty() ? "tcp" : protocol_service.protocol));
            event.host = protocol_service.host.empty() ? "0.0.0.0" : protocol_service.host;
            event.port = protocol_service.port;
            event.reason = std::move(reason);
            return event;
        }

        plugin::PluginProtocolConnectionEvent make_protocol_connection_event(
            const RuntimeContext &runtime_context,
            const plugin::ProtocolServiceDescriptor &protocol_service,
            std::uintptr_t connection_id,
            std::string peer_address,
            std::string local_address,
            std::string reason = {},
            std::string reason_code = {})
        {
            plugin::PluginProtocolConnectionEvent event;
            static_cast<plugin::PluginProtocolServiceEvent &>(event) =
                make_protocol_service_event(runtime_context, protocol_service, std::move(reason));
            event.connection_id = connection_id;
            event.peer_address = std::move(peer_address);
            event.local_address = std::move(local_address);
            event.reason_code = std::move(reason_code);
            return event;
        }

        void publish_protocol_connection_event(
            const RuntimeContext &runtime_context,
            const char *event_name,
            const plugin::ProtocolServiceDescriptor &protocol_service,
            std::uintptr_t connection_id,
            std::string peer_address,
            std::string local_address,
            std::string reason = {},
            std::string reason_code = {})
        {
            if (!runtime_context.event_bus || !event_name) {
                return;
            }
            runtime_context.event_bus->publish(
                event_name,
                make_protocol_connection_event(
                    runtime_context,
                    protocol_service,
                    connection_id,
                    std::move(peer_address),
                    std::move(local_address),
                    std::move(reason),
                    std::move(reason_code)));
        }

        struct RuntimeStatsSnapshot
        {
            std::uint64_t framing_error_count = 0;
            std::uint64_t handler_error_count = 0;
            std::uint64_t backpressure_drop_count = 0;
        };

        RuntimeStatsSnapshot snapshot_runtime_stats(
            const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats)
        {
            RuntimeStatsSnapshot snapshot;
            if (!runtime_stats) {
                return snapshot;
            }
            snapshot.framing_error_count =
                runtime_stats->framing_error_count.load(std::memory_order_relaxed);
            snapshot.handler_error_count =
                runtime_stats->handler_error_count.load(std::memory_order_relaxed);
            snapshot.backpressure_drop_count =
                runtime_stats->backpressure_drop_count.load(std::memory_order_relaxed);
            return snapshot;
        }

        std::string runtime_fault_reason_code(const RuntimeStatsSnapshot &before,
                                              const RuntimeStatsSnapshot &after)
        {
            const bool framing_increased = after.framing_error_count > before.framing_error_count;
            const bool handler_increased = after.handler_error_count > before.handler_error_count;
            const bool backpressure_increased =
                after.backpressure_drop_count > before.backpressure_drop_count;

            const int increased_categories =
                (framing_increased ? 1 : 0) +
                (handler_increased ? 1 : 0) +
                (backpressure_increased ? 1 : 0);

            if (increased_categories == 0) {
                return {};
            }
            if (increased_categories > 1) {
                return "runtime_multiple_errors";
            }
            if (handler_increased) {
                return "runtime_handler_error";
            }
            if (framing_increased) {
                return "runtime_framing_error";
            }
            return "runtime_backpressure_drop";
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

            runtime_stats->active_connection_count.store(0, std::memory_order_relaxed);
            runtime_stats->accepted_connection_count.store(0, std::memory_order_relaxed);
            runtime_stats->closed_connection_count.store(0, std::memory_order_relaxed);
            runtime_stats->bytes_received.store(0, std::memory_order_relaxed);
            runtime_stats->bytes_sent.store(0, std::memory_order_relaxed);
            runtime_stats->framing_error_count.store(0, std::memory_order_relaxed);
            runtime_stats->handler_error_count.store(0, std::memory_order_relaxed);
            runtime_stats->backpressure_drop_count.store(0, std::memory_order_relaxed);
        }

        void decrement_active_connection_count(const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats)
        {
            if (!runtime_stats) {
                return;
            }

            auto current = runtime_stats->active_connection_count.load(std::memory_order_relaxed);
            while (current > 0 &&
                   !runtime_stats->active_connection_count.compare_exchange_weak(
                       current,
                       current - 1,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
        }

        void record_connection_accepted(const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats)
        {
            if (!runtime_stats) {
                return;
            }
            runtime_stats->accepted_connection_count.fetch_add(1, std::memory_order_relaxed);
            runtime_stats->active_connection_count.fetch_add(1, std::memory_order_relaxed);
        }

        void record_connection_closed(const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats)
        {
            if (!runtime_stats) {
                return;
            }
            runtime_stats->closed_connection_count.fetch_add(1, std::memory_order_relaxed);
            decrement_active_connection_count(runtime_stats);
        }

        void record_bytes_received(const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats,
                                   std::size_t bytes)
        {
            if (!runtime_stats || bytes == 0) {
                return;
            }
            runtime_stats->bytes_received.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
        }

        void record_bytes_sent(const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats,
                               std::size_t bytes)
        {
            if (!runtime_stats || bytes == 0) {
                return;
            }
            runtime_stats->bytes_sent.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
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

        void record_backpressure_drop(const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats)
        {
            if (!runtime_stats) {
                return;
            }

            runtime_stats->backpressure_drop_count.fetch_add(1, std::memory_order_relaxed);
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

        bool parse_peer_endpoint(std::string_view peer, net::InetAddress &address)
        {
            if (peer.empty()) {
                return false;
            }

            std::string host;
            std::string port_text;
            if (peer.front() == '[') {
                const auto close_bracket = peer.find(']');
                if (close_bracket == std::string_view::npos || close_bracket + 2 > peer.size() || peer[close_bracket + 1] != ':') {
                    return false;
                }
                host = std::string(peer.substr(1, close_bracket - 1));
                port_text = std::string(peer.substr(close_bracket + 2));
            } else {
                const auto colon = peer.rfind(':');
                if (colon == std::string_view::npos || colon == 0 || colon + 1 >= peer.size()) {
                    return false;
                }
                host = std::string(peer.substr(0, colon));
                port_text = std::string(peer.substr(colon + 1));
            }

            char *parse_end = nullptr;
            const long parsed_port = std::strtol(port_text.c_str(), &parse_end, 10);
            if (parse_end == nullptr || *parse_end != '\0' || parsed_port < 0 || parsed_port > 65535) {
                return false;
            }

            address = net::InetAddress(host, static_cast<int>(parsed_port));
            return true;
        }

        class HostDatagramEndpointAdapter final : public plugin::HostDatagramEndpoint
        {
        public:
            HostDatagramEndpointAdapter(net::DatagramAcceptor *acceptor,
                                        std::string local_endpoint,
                                        std::shared_ptr<PluginProtocolServiceRuntimeStats> runtime_stats)
                : acceptor_(acceptor),
                  local_endpoint_(std::move(local_endpoint)),
                  runtime_stats_(std::move(runtime_stats))
            {
            }

            std::string local_address() const override
            {
                return local_endpoint_;
            }

            bool send_to(std::string_view peer, std::span<const std::byte> bytes) override
            {
                if (!acceptor_) {
                    return false;
                }

                net::InetAddress peer_address;
                if (!parse_peer_endpoint(peer, peer_address)) {
                    return false;
                }

                ::yuan::buffer::ByteBuffer packet;
                if (!bytes.empty()) {
                    packet.append(bytes.data(), bytes.size());
                }
                const bool sent = acceptor_->send_datagram(peer_address, packet) >= 0;
                if (sent) {
                    record_bytes_sent(runtime_stats_, bytes.size());
                }
                return sent;
            }

        private:
            net::DatagramAcceptor *acceptor_ = nullptr;
            std::string local_endpoint_;
            std::shared_ptr<PluginProtocolServiceRuntimeStats> runtime_stats_;
        };

        class HostStreamConnectionAdapter final : public plugin::HostStreamConnection
        {
        public:
            HostStreamConnectionAdapter(net::AsyncConnectionContext &ctx,
                                        std::size_t max_pending_write_bytes)
                : ctx_(ctx),
                  max_pending_write_bytes_(max_pending_write_bytes)
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

                const auto buffered = pending_output_.readable_bytes();
                const auto remaining_capacity =
                    max_pending_write_bytes_ > buffered ? (max_pending_write_bytes_ - buffered) : 0;
                if (max_pending_write_bytes_ > 0 && bytes.size() > remaining_capacity) {
                    backpressure_exceeded_ = true;
                    close_requested_ = true;
                    return false;
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

            bool backpressure_exceeded() const noexcept
            {
                return backpressure_exceeded_;
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
            std::size_t max_pending_write_bytes_ = 0;
            ::yuan::buffer::ByteBuffer pending_output_;
            bool backpressure_exceeded_ = false;
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
                                                             const RuntimeContext *runtime_context,
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
                LOG_ERROR("plugin protocol service '{}.{}' handler threw from {}: {} [{}]",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          callback_name,
                          ex.what(),
                          runtime_identity_text(runtime_context));
                record_handler_error(runtime_stats);
                apply_recorded_fault_policy(host, protocol_service.plugin_id);
                return GuardedBoolHandlerResult{ true, true, false, ex.what() };
            } catch (...) {
                LOG_ERROR("plugin protocol service '{}.{}' handler threw from {} [{}]",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          callback_name,
                          runtime_identity_text(runtime_context));
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
                                         const RuntimeContext *runtime_context,
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
                LOG_ERROR("plugin protocol service '{}.{}' handler threw from {}: {} [{}]",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          callback_name,
                          ex.what(),
                          runtime_identity_text(runtime_context));
                record_handler_error(runtime_stats);
                apply_recorded_fault_policy(host, protocol_service.plugin_id);
            } catch (...) {
                LOG_ERROR("plugin protocol service '{}.{}' handler threw from {} [{}]",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          callback_name,
                          runtime_identity_text(runtime_context));
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
                                  const RuntimeContext *runtime_context,
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
                runtime_context,
                [&]() {
                    handler.on_error(connection, error);
                });
        }

        bool call_handler_on_accept(PluginHostService *host,
                                    const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats,
                                    plugin::PluginStreamProtocolHandler &handler,
                                    HostStreamConnectionAdapter &connection,
                                    const plugin::ProtocolServiceDescriptor &protocol_service,
                                    const RuntimeContext *runtime_context)
        {
            const auto result = invoke_guarded_bool_handler(
                host,
                runtime_stats,
                protocol_service,
                "protocol.on_accept",
                "on_accept",
                runtime_context,
                [&]() {
                    return handler.on_accept(connection);
                });

            if (result.threw) {
                notify_handler_error(host, runtime_stats, handler, connection, protocol_service, runtime_context, result.error_message, 1);
                return false;
            }

            if (!result.dispatched) {
                return false;
            }

            if (!result.value) {
                report_handler_failure(host, runtime_stats, protocol_service, "protocol.on_accept", "handler returned false");
                notify_handler_error(host, runtime_stats, handler, connection, protocol_service, runtime_context, "handler returned false", 1);
                return false;
            }

            return true;
        }

        bool call_handler_on_data(PluginHostService *host,
                                  const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats,
                                  plugin::PluginStreamProtocolHandler &handler,
                                  HostStreamConnectionAdapter &connection,
                                  std::span<const std::byte> bytes,
                                  const plugin::ProtocolServiceDescriptor &protocol_service,
                                  const RuntimeContext *runtime_context)
        {
            const auto result = invoke_guarded_bool_handler(
                host,
                runtime_stats,
                protocol_service,
                "protocol.on_data",
                "on_data",
                runtime_context,
                [&]() {
                    return handler.on_data(connection, bytes);
                });

            if (result.threw) {
                notify_handler_error(host, runtime_stats, handler, connection, protocol_service, runtime_context, result.error_message, 1);
                return false;
            }

            if (!result.dispatched) {
                return false;
            }

            if (!result.value) {
                if (connection.backpressure_exceeded()) {
                    record_backpressure_drop(runtime_stats);
                    notify_handler_error(
                        host,
                        runtime_stats,
                        handler,
                        connection,
                        protocol_service,
                        runtime_context,
                        "write buffer limit exceeded",
                        4);
                    return false;
                }
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
                                                   uint32_t write_timeout_ms,
                                                   const std::shared_ptr<PluginProtocolServiceRuntimeStats> &runtime_stats)
        {
            const bool flush_requested = connection.consume_flush_request();
            auto pending = connection.take_pending_output();
            const bool has_pending = pending.readable_bytes() > 0;
            const auto pending_bytes = pending.readable_bytes();
            if (has_pending) {
                auto write = co_await ctx.write_async(pending, write_timeout_ms);
                if (write.status != coroutine::IoStatus::success) {
                    co_return false;
                }
                record_bytes_sent(runtime_stats, pending_bytes);
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
            PluginHostService *host,
            RuntimeContext runtime_context)
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
                LOG_ERROR("plugin protocol service '{}.{}' handler factory threw: {} [{}]",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          ex.what(),
                          runtime_identity_text(runtime_context));
                handler_factory_failed = true;
            } catch (...) {
                LOG_ERROR("plugin protocol service '{}.{}' handler factory threw [{}]",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          runtime_identity_text(runtime_context));
                handler_factory_failed = true;
            }
            if (handler_factory_failed) {
                (void)co_await ctx.close_async();
                co_return;
            }
            if (!handler) {
                LOG_ERROR("plugin protocol service '{}.{}' handler factory returned null [{}]",
                          protocol_service.plugin_id,
                          protocol_service.name,
                          runtime_identity_text(runtime_context));
                (void)co_await ctx.close_async();
                co_return;
            }

            HostStreamConnectionAdapter connection(
                ctx,
                static_cast<std::size_t>(protocol_service.max_write_buffer_bytes));
            const auto write_timeout_ms = static_cast<uint32_t>(protocol_service.write_timeout_ms);
            const bool accepted = call_handler_on_accept(
                host,
                runtime_stats,
                *handler,
                connection,
                protocol_service,
                &runtime_context);
            if (!accepted) {
                (void)co_await ctx.close_async();
                co_return;
            }
            const bool accept_writes_flushed = co_await flush_pending_writes(ctx, connection, write_timeout_ms, runtime_stats);
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
                record_bytes_received(runtime_stats, readable);
                if (framing == "raw") {
                    if (readable > max_frame_bytes) {
                record_framing_error(runtime_stats);
                notify_handler_error(host, runtime_stats, *handler, connection, protocol_service, &runtime_context, "frame too large", 2);
                break;
            }
                    if (!call_handler_on_data(
                            host,
                            runtime_stats,
                            *handler,
                            connection,
                            std::span<const std::byte>(read_ptr, readable),
                            protocol_service,
                            &runtime_context)) {
                        break;
                    }
                    const bool flushed = co_await flush_pending_writes(ctx, connection, write_timeout_ms, runtime_stats);
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
                        notify_handler_error(host, runtime_stats, *handler, connection, protocol_service, &runtime_context, "line frame too large", 2);
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
                            protocol_service,
                            &runtime_context);
                        frame_buffer.erase(frame_buffer.begin(), it + 1);
                        search_begin = frame_buffer.begin();

                        const bool flushed = co_await flush_pending_writes(ctx, connection, write_timeout_ms, runtime_stats);
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
                                &runtime_context,
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
                            protocol_service,
                            &runtime_context);
                        frame_buffer.erase(
                            frame_buffer.begin(),
                            frame_buffer.begin() + static_cast<std::ptrdiff_t>(total_frame_bytes));

                        const bool flushed = co_await flush_pending_writes(ctx, connection, write_timeout_ms, runtime_stats);
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
                notify_handler_error(host, runtime_stats, *handler, connection, protocol_service, &runtime_context, "unsupported framing", 3);
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
                    protocol_service,
                    &runtime_context);
                (void)co_await flush_pending_writes(ctx, connection, write_timeout_ms, runtime_stats);
            }

            invoke_guarded_void_handler(
                host,
                runtime_stats,
                protocol_service,
                "protocol.on_close",
                "on_close",
                &runtime_context,
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
            PluginHostService *host,
            RuntimeContext runtime_context)
        {
            const auto connection_id = ctx.connection_id();
            const auto event_protocol_service = protocol_service;
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
                LOG_WARN("plugin protocol service '{}.{}' rejected connection: active connection limit {} reached [{}]",
                         protocol_service.plugin_id,
                         protocol_service.name,
                         protocol_service.max_connections,
                         runtime_identity_text(runtime_context));
                co_await reject_stream_connection(std::move(ctx));
                co_return;
            }

            const auto connection_id_for_event = connection_id;
            const auto peer_address_for_event = ctx.native_handle()
                ? ctx.native_handle()->get_remote_address().to_address_key()
                : std::string{};
            const auto local_address_for_event = ctx.native_handle()
                ? ctx.native_handle()->get_local_address().to_address_key()
                : std::string{};

            publish_protocol_connection_event(
                runtime_context,
                plugin::events::plugin_protocol_connection_accepted,
                event_protocol_service,
                connection_id_for_event,
                peer_address_for_event,
                local_address_for_event);
            record_connection_accepted(runtime_stats);

            const auto stats_before = snapshot_runtime_stats(runtime_stats);

            co_await run_stream_protocol(
                std::move(ctx),
                std::move(handler_factory),
                std::move(protocol_service),
                std::move(active_connections),
                runtime_stats,
                host,
                runtime_context);

            const auto stats_after = snapshot_runtime_stats(runtime_stats);
            const auto runtime_reason_code = runtime_fault_reason_code(stats_before, stats_after);
            if (!runtime_reason_code.empty()) {
                publish_protocol_connection_event(
                    runtime_context,
                    plugin::events::plugin_protocol_connection_faulted,
                    event_protocol_service,
                    connection_id_for_event,
                    peer_address_for_event,
                    local_address_for_event,
                    "runtime error stats increased",
                    runtime_reason_code);
            }
            record_connection_closed(runtime_stats);
            publish_protocol_connection_event(
                runtime_context,
                plugin::events::plugin_protocol_connection_closed,
                event_protocol_service,
                connection_id_for_event,
                peer_address_for_event,
                local_address_for_event);
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
            if (protocol_service.max_write_buffer_bytes <= 0) {
                return "max_write_buffer_bytes must be positive";
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

        class PluginDatagramConnectionHandler final : public net::ConnectionHandler
                                                    , public std::enable_shared_from_this<PluginDatagramConnectionHandler>
        {
        public:
            struct PeerState
            {
                std::chrono::steady_clock::time_point last_activity;
                uint64_t resource_id = 0;
                std::weak_ptr<net::Connection> connection;
            };

            PluginDatagramConnectionHandler(plugin::ProtocolServiceDescriptor protocol_service,
                                            plugin::PluginDatagramProtocolHandlerFactory handler_factory,
                                            std::shared_ptr<PluginProtocolServiceRuntimeStats> runtime_stats,
                                            PluginHostService *host,
                                            net::DatagramAcceptor *acceptor,
                                            std::string local_endpoint,
                                            RuntimeContext runtime_context)
                : protocol_service_(std::move(protocol_service)),
                  handler_factory_(std::move(handler_factory)),
                  runtime_stats_(std::move(runtime_stats)),
                  host_(host),
                  acceptor_(acceptor),
                  local_endpoint_(std::move(local_endpoint)),
                  runtime_context_(std::move(runtime_context)),
                  idle_timeout_ms_(static_cast<uint32_t>(protocol_service_.idle_timeout_ms))
            {
            }

            ~PluginDatagramConnectionHandler() override
            {
                cleanup_timer_.cancel();
                cleanup_timer_.reset();
                clear_peer_states();
            }

            void start_idle_cleanup_timer(net::NetworkRuntime *runtime)
            {
                if (!runtime || idle_timeout_ms_ == 0) {
                    return;
                }

                runtime_ = runtime;
                cleanup_interval_ms_ = (std::max)(50u, (std::min)(idle_timeout_ms_, 1000u));
                schedule_cleanup_tick();
            }

            void on_connected(const std::shared_ptr<net::Connection> &conn) override
            {
                (void)conn;
            }

            void on_error(const std::shared_ptr<net::Connection> &conn) override
            {
                if (conn) {
                    remove_peer_state(conn->get_remote_address().to_address_key());
                }
            }

            void on_write(const std::shared_ptr<net::Connection> &conn) override
            {
                (void)conn;
            }

            void on_close(const std::shared_ptr<net::Connection> &conn) override
            {
                if (conn) {
                    remove_peer_state(conn->get_remote_address().to_address_key());
                }
            }

            void on_read(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!conn || !acceptor_) {
                    return;
                }

                ensure_handler();
                if (!handler_) {
                    conn->abort();
                    return;
                }

                auto packet = conn->take_input_byte_buffer();
                if (packet.readable_bytes() == 0) {
                    return;
                }
                record_bytes_received(runtime_stats_, packet.readable_bytes());

                const auto peer = conn->get_remote_address().to_address_key();
                touch_peer_state(conn, peer);
                cleanup_idle_peers();

                const auto framing = effective_framing(protocol_service_);
                if (framing != "raw") {
                    record_framing_error(runtime_stats_);
                    notify_error(*handler_, peer, "unsupported udp framing", 3);
                    publish_protocol_connection_event(
                        runtime_context_,
                        plugin::events::plugin_protocol_connection_faulted,
                        protocol_service_,
                        0,
                        std::string(peer),
                        local_endpoint_,
                        "unsupported udp framing",
                        "udp_unsupported_framing");
                    conn->abort();
                    return;
                }

                const auto max_frame_bytes = static_cast<std::size_t>(protocol_service_.max_frame_bytes);
                if (packet.readable_bytes() > max_frame_bytes) {
                    record_framing_error(runtime_stats_);
                    notify_error(*handler_, peer, "udp frame too large", 2);
                    publish_protocol_connection_event(
                        runtime_context_,
                        plugin::events::plugin_protocol_connection_faulted,
                        protocol_service_,
                        0,
                        std::string(peer),
                        local_endpoint_,
                        "udp frame too large",
                        "udp_frame_too_large");
                    conn->abort();
                    return;
                }

                HostDatagramEndpointAdapter endpoint(acceptor_, local_endpoint_, runtime_stats_);
                const auto *ptr = reinterpret_cast<const std::byte *>(packet.read_ptr());
                const auto result = invoke_guarded_bool_handler(
                    host_,
                    runtime_stats_,
                    protocol_service_,
                    "protocol.on_datagram",
                    "on_datagram",
                    &runtime_context_,
                    [&]() {
                        return handler_->on_datagram(endpoint,
                                                     peer,
                                                     std::span<const std::byte>(ptr, packet.readable_bytes()));
                    });

                if (result.threw) {
                    notify_error(*handler_, peer, result.error_message, 1);
                    publish_protocol_connection_event(
                        runtime_context_,
                        plugin::events::plugin_protocol_connection_faulted,
                        protocol_service_,
                        0,
                        std::string(peer),
                        local_endpoint_,
                        result.error_message,
                        "udp_handler_exception");
                    conn->abort();
                    return;
                }

                if (!result.dispatched || !result.value) {
                    if (result.dispatched) {
                        report_handler_failure(host_, runtime_stats_, protocol_service_, "protocol.on_datagram", "handler returned false");
                        publish_protocol_connection_event(
                            runtime_context_,
                            plugin::events::plugin_protocol_connection_faulted,
                            protocol_service_,
                            0,
                            std::string(peer),
                            local_endpoint_,
                            "handler returned false",
                            "udp_handler_rejected");
                    }
                    conn->abort();
                }
            }

        private:
            void touch_peer_state(const std::shared_ptr<net::Connection> &conn, std::string_view peer)
            {
                auto it = peer_states_.find(std::string(peer));
                const bool is_new_peer = it == peer_states_.end();
                auto &state = is_new_peer
                    ? peer_states_[std::string(peer)]
                    : it->second;
                state.last_activity = std::chrono::steady_clock::now();
                state.connection = conn;
                if (state.resource_id == 0 && host_) {
                    state.resource_id = host_->track_plugin_resource(
                        protocol_service_.plugin_id,
                        plugin::PluginResourceType::network_connection,
                        []() {},
                        protocol_udp_peer_resource_description(protocol_service_, peer));
                }
                if (is_new_peer) {
                    record_connection_accepted(runtime_stats_);
                    publish_protocol_connection_event(
                        runtime_context_,
                        plugin::events::plugin_protocol_connection_accepted,
                        protocol_service_,
                        0,
                        std::string(peer),
                        local_endpoint_);
                }
            }

            void remove_peer_state(std::string_view peer)
            {
                const auto it = peer_states_.find(std::string(peer));
                if (it == peer_states_.end()) {
                    return;
                }

                if (host_ && it->second.resource_id != 0) {
                    (void)host_->untrack_plugin_resource(it->second.resource_id);
                }
                peer_states_.erase(it);
                record_connection_closed(runtime_stats_);
                publish_protocol_connection_event(
                    runtime_context_,
                    plugin::events::plugin_protocol_connection_closed,
                    protocol_service_,
                    0,
                    std::string(peer),
                    local_endpoint_);
            }

            void cleanup_idle_peers()
            {
                if (idle_timeout_ms_ == 0 || peer_states_.empty()) {
                    return;
                }

                const auto now = std::chrono::steady_clock::now();
                const auto idle_window = std::chrono::milliseconds(idle_timeout_ms_);
                std::vector<std::shared_ptr<net::Connection> > expired_connections;
                std::vector<std::string> expired_peers_without_connection;
                for (auto it = peer_states_.begin(); it != peer_states_.end(); ++it) {
                    if (now - it->second.last_activity < idle_window) {
                        continue;
                    }
                    if (auto conn = it->second.connection.lock()) {
                        expired_connections.push_back(std::move(conn));
                    } else {
                        expired_peers_without_connection.push_back(it->first);
                    }
                }

                for (const auto &conn : expired_connections) {
                    if (conn && conn->get_connection_state() != net::ConnectionState::closed) {
                        conn->close();
                    }
                }

                for (const auto &peer : expired_peers_without_connection) {
                    remove_peer_state(peer);
                }
            }

            void schedule_cleanup_tick()
            {
                if (!runtime_ || cleanup_interval_ms_ == 0) {
                    return;
                }

                std::weak_ptr<PluginDatagramConnectionHandler> weak_self = weak_from_this();
                cleanup_timer_ = runtime_->schedule_handle(
                    cleanup_interval_ms_,
                    [weak_self]() {
                        if (auto self = weak_self.lock()) {
                            self->on_cleanup_tick();
                        }
                    });
            }

            void on_cleanup_tick()
            {
                cleanup_idle_peers();
                schedule_cleanup_tick();
            }

            void clear_peer_states()
            {
                std::vector<std::string> peers;
                peers.reserve(peer_states_.size());
                for (const auto &entry : peer_states_) {
                    peers.push_back(entry.first);
                }
                for (const auto &peer : peers) {
                    remove_peer_state(peer);
                }
            }

            void ensure_handler()
            {
                if (handler_ || !handler_factory_) {
                    return;
                }

                try {
                    handler_ = handler_factory_(protocol_service_);
                } catch (const std::exception &ex) {
                    LOG_ERROR("plugin protocol service '{}.{}' datagram handler factory threw: {} [{}]",
                              protocol_service_.plugin_id,
                              protocol_service_.name,
                              ex.what(),
                              runtime_identity_text(runtime_context_));
                    handler_.reset();
                } catch (...) {
                    LOG_ERROR("plugin protocol service '{}.{}' datagram handler factory threw [{}]",
                              protocol_service_.plugin_id,
                              protocol_service_.name,
                              runtime_identity_text(runtime_context_));
                    handler_.reset();
                }
            }

            void notify_error(plugin::PluginDatagramProtocolHandler &handler,
                              std::string_view peer,
                              std::string message,
                              int code)
            {
                plugin::ProtocolHandlerErrorInfo error;
                error.code = code;
                error.message = std::move(message);

                HostDatagramEndpointAdapter endpoint(acceptor_, local_endpoint_, runtime_stats_);
                invoke_guarded_void_handler(
                    host_,
                    runtime_stats_,
                    protocol_service_,
                    "protocol.on_error",
                    "on_error",
                    &runtime_context_,
                    [&]() {
                        handler.on_error(endpoint, peer, error);
                    });
            }

            plugin::ProtocolServiceDescriptor protocol_service_;
            plugin::PluginDatagramProtocolHandlerFactory handler_factory_;
            std::shared_ptr<PluginProtocolServiceRuntimeStats> runtime_stats_;
            PluginHostService *host_ = nullptr;
            net::DatagramAcceptor *acceptor_ = nullptr;
            std::string local_endpoint_;
            std::unique_ptr<plugin::PluginDatagramProtocolHandler> handler_;
            uint32_t idle_timeout_ms_ = 0;
            uint32_t cleanup_interval_ms_ = 0;
            timer::TimerHandle cleanup_timer_;
            net::NetworkRuntime *runtime_ = nullptr;
            std::unordered_map<std::string, PeerState> peer_states_;
            RuntimeContext runtime_context_{};
        };

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
                "plugin protocol service '{}.{}' manifest validation failed: {} [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                manifest_reason(protocol_service_),
                runtime_identity_text(runtime_context_));
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
                "plugin protocol service '{}.{}' failed to initialize plugin runtime [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                runtime_identity_text(runtime_context_));
            host_.reset();
            return false;
        }

        if (auto *plugin = host_->get_plugin(protocol_service_.plugin_id)) {
            try {
                plugin->register_protocol_handlers(handler_registry_);
            } catch (const std::exception &ex) {
                LOG_ERROR(
                    "plugin protocol service '{}.{}' failed while registering protocol handlers: {} [{}]",
                    protocol_service_.plugin_id,
                    protocol_service_.name,
                    ex.what(),
                    runtime_identity_text(runtime_context_));
                handler_registry_.clear();
                host_.reset();
                return false;
            } catch (...) {
                LOG_ERROR(
                    "plugin protocol service '{}.{}' failed while registering protocol handlers [{}]",
                    protocol_service_.plugin_id,
                    protocol_service_.name,
                    runtime_identity_text(runtime_context_));
                handler_registry_.clear();
                host_.reset();
                return false;
            }
        }

        const auto context = host_->plugin_context(protocol_service_.plugin_id);
        const auto capabilities = context.capabilities();
        LOG_INFO(
            "plugin protocol service '{}.{}' capability snapshot: permissions=[{}], event_bus={}, logger={}, service_catalog={}, scheduler={}, service_registry={}, permission_guard={}, resource_guard={}, http_interceptor={}, storage={}, network_runtime={}, extension_points={} [{}]",
            protocol_service_.plugin_id,
            protocol_service_.name,
            join_permission_names(capabilities.granted_permissions),
            bool_text(capabilities.event_bus),
            bool_text(capabilities.logger),
            bool_text(capabilities.service_catalog),
            bool_text(capabilities.scheduler),
            bool_text(capabilities.service_registry),
            bool_text(capabilities.permission_guard),
            bool_text(capabilities.resource_guard),
            bool_text(capabilities.http_interceptor),
            bool_text(capabilities.storage),
            bool_text(capabilities.network_runtime),
            bool_text(capabilities.extension_points),
            runtime_identity_text(runtime_context_));

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
            if (runtime_context_.event_bus && last_start_failure_is_bind_) {
                runtime_context_.event_bus->publish(
                    plugin::events::plugin_protocol_service_bind_failed,
                    make_protocol_service_event(
                        runtime_context_,
                        protocol_service_,
                        last_start_failure_reason_.empty()
                            ? "failed to bind protocol listener"
                            : last_start_failure_reason_));
            }
            handler_registry_.clear();
            if (host_) {
                host_->stop();
            }
            return;
        }
        started_ = true;
        if (runtime_context_.event_bus) {
            runtime_context_.event_bus->publish(
                plugin::events::plugin_protocol_service_started,
                make_protocol_service_event(runtime_context_, protocol_service_));
        }
    }

    void PluginProtocolServiceAdapter::stop()
    {
        const bool was_started = started_;
        stop_protocol_listener();
        handler_registry_.clear();
        if (host_) {
            host_->stop();
            host_.reset();
        }
        started_ = false;
        initialized_ = false;
        if (was_started && runtime_context_.event_bus) {
            runtime_context_.event_bus->publish(
                plugin::events::plugin_protocol_service_stopped,
                make_protocol_service_event(runtime_context_, protocol_service_));
        }
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

        snapshot.active_connection_count =
            runtime_stats_->active_connection_count.load(std::memory_order_relaxed);
        snapshot.accepted_connection_count =
            runtime_stats_->accepted_connection_count.load(std::memory_order_relaxed);
        snapshot.closed_connection_count =
            runtime_stats_->closed_connection_count.load(std::memory_order_relaxed);
        snapshot.bytes_received =
            runtime_stats_->bytes_received.load(std::memory_order_relaxed);
        snapshot.bytes_sent =
            runtime_stats_->bytes_sent.load(std::memory_order_relaxed);
        snapshot.framing_error_count =
            runtime_stats_->framing_error_count.load(std::memory_order_relaxed);
        snapshot.handler_error_count =
            runtime_stats_->handler_error_count.load(std::memory_order_relaxed);
        snapshot.backpressure_drop_count =
            runtime_stats_->backpressure_drop_count.load(std::memory_order_relaxed);
        return snapshot;
    }

    ProtocolServiceHealthSnapshot PluginProtocolServiceAdapter::health_snapshot() const noexcept
    {
        ProtocolServiceHealthSnapshot snapshot;
        snapshot.listener_present = listener_ != nullptr || datagram_acceptor_ != nullptr;

        const auto stats = runtime_stats();
        snapshot.active_connection_count = stats.active_connection_count;
        snapshot.max_connection_limit = protocol_service_.max_connections > 0
            ? static_cast<std::uint64_t>(protocol_service_.max_connections)
            : 0;
        snapshot.connection_at_capacity =
            snapshot.max_connection_limit > 0 &&
            snapshot.active_connection_count >= snapshot.max_connection_limit;
        snapshot.connection_over_limit =
            snapshot.max_connection_limit > 0 &&
            snapshot.active_connection_count > snapshot.max_connection_limit;
        snapshot.handler_fault_count = stats.handler_error_count;
        snapshot.total_fault_count =
            stats.handler_error_count +
            stats.framing_error_count +
            stats.backpressure_drop_count;
        snapshot.healthy =
            snapshot.listener_present &&
            !snapshot.connection_at_capacity &&
            !snapshot.connection_over_limit;
        return snapshot;
    }

    bool PluginProtocolServiceAdapter::start_protocol_listener()
    {
        if (listener_ || datagram_acceptor_) {
            return true;
        }

        last_start_failure_reason_.clear();
        last_start_failure_is_bind_ = false;
        auto fail_start = [this](std::string reason, bool bind_failure = false) {
            last_start_failure_reason_ = std::move(reason);
            last_start_failure_is_bind_ = bind_failure;
            return false;
        };

        if (!is_valid_protocol_service(protocol_service_)) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' manifest validation failed: {} [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                manifest_reason(protocol_service_),
                runtime_identity_text(runtime_context_));
            return fail_start("manifest validation failed");
        }

        const auto handler_name = resolve_handler_name(protocol_service_);
        if (handler_name.empty()) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' has no handler; set 'handler' for custom services or use type='echo' for the built-in demo [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                runtime_identity_text(runtime_context_));
            return fail_start("protocol service handler was not configured");
        }

        const auto transport = effective_transport(protocol_service_);
        const auto framing = effective_framing(protocol_service_);
        if (transport == "tcp" && !framing_is_supported(framing)) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' has unsupported framing '{}' [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                framing,
                runtime_identity_text(runtime_context_));
            return fail_start("unsupported tcp framing '" + framing + "'");
        }
        if (transport == "udp" && framing != "raw") {
            LOG_ERROR(
                "plugin protocol service '{}.{}' has unsupported udp framing '{}' [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                framing,
                runtime_identity_text(runtime_context_));
            return fail_start("unsupported udp framing '" + framing + "'");
        }

        if (is_builtin_echo_demo(protocol_service_)) {
            LOG_INFO(
                "plugin protocol service '{}.{}' uses built-in demo protocol handler '{}'; this is not custom plugin protocol logic [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                handler_name,
                runtime_identity_text(runtime_context_));
        }

        auto *runtime = runtime_context_.shared_runtime;
        if (!runtime) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' requires a worker-owned NetworkRuntime [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                runtime_identity_text(runtime_context_));
            return fail_start("worker-owned NetworkRuntime is required");
        }

        net::ListenOptions options;
        options.reuse_port = runtime_context_.listener_reuse_port;

        if (transport == "udp") {
            auto datagram_factory = handler_registry_.find_datagram_handler(handler_name);
            if (!datagram_factory) {
                LOG_ERROR(
                    "plugin protocol service '{}.{}' datagram handler '{}' was not registered [{}]",
                    protocol_service_.plugin_id,
                    protocol_service_.name,
                    handler_name,
                    runtime_identity_text(runtime_context_));
                return fail_start("datagram handler '" + handler_name + "' was not registered");
            }

            auto socket = std::make_unique<net::Socket>(
                protocol_service_.host.empty() ? "0.0.0.0" : protocol_service_.host,
                protocol_service_.port,
                true);
            if (!socket || !socket->valid()) {
                LOG_ERROR(
                    "plugin protocol service '{}.{}' failed to create udp socket [{}]",
                    protocol_service_.plugin_id,
                    protocol_service_.name,
                    runtime_identity_text(runtime_context_));
                return fail_start("failed to create udp socket");
            }

            if (!socket->apply_listen_options(options)) {
                LOG_ERROR(
                    "plugin protocol service '{}.{}' failed to apply udp listen options [{}]",
                    protocol_service_.plugin_id,
                    protocol_service_.name,
                    runtime_identity_text(runtime_context_));
                return fail_start("failed to apply udp listen options");
            }
            socket->set_none_block(true);
            if (!socket->bind()) {
                LOG_ERROR(
                    "plugin protocol service '{}.{}' failed to bind {}://{}:{} [{}]",
                    protocol_service_.plugin_id,
                    protocol_service_.name,
                    transport,
                    protocol_service_.host.empty() ? "0.0.0.0" : protocol_service_.host,
                    protocol_service_.port,
                    runtime_identity_text(runtime_context_));
                return fail_start(
                    "failed to bind " + transport + "://" +
                    (protocol_service_.host.empty() ? std::string("0.0.0.0") : protocol_service_.host) +
                    ":" + std::to_string(protocol_service_.port),
                    true);
            }

            const auto local_endpoint = socket->get_local_address().to_address_key();
            auto datagram_acceptor = std::unique_ptr<net::DatagramAcceptor>(
                net::create_datagram_acceptor(socket.release(), *runtime));
            if (!datagram_acceptor || !datagram_acceptor->listen()) {
                LOG_ERROR(
                    "plugin protocol service '{}.{}' failed to start udp acceptor [{}]",
                    protocol_service_.plugin_id,
                    protocol_service_.name,
                    runtime_identity_text(runtime_context_));
                return fail_start("failed to start udp acceptor");
            }

            if (!runtime_stats_) {
                runtime_stats_ = std::make_shared<PluginProtocolServiceRuntimeStats>();
            }
            reset_runtime_stats(runtime_stats_);

            auto datagram_handler = std::make_shared<PluginDatagramConnectionHandler>(
                protocol_service_,
                std::move(datagram_factory),
                runtime_stats_,
                host_.get(),
                datagram_acceptor.get(),
                local_endpoint,
                runtime_context_);
            datagram_handler->start_idle_cleanup_timer(runtime);

            runtime->register_acceptor(
                datagram_acceptor.get(),
                datagram_handler,
                datagram_acceptor->endpoint_channel());

            datagram_acceptor_ = std::move(datagram_acceptor);
            if (host_) {
                listener_resource_id_ = host_->track_plugin_resource(
                    protocol_service_.plugin_id,
                    plugin::PluginResourceType::network_listener,
                    [this]() {
                        if (datagram_acceptor_) {
                            datagram_acceptor_->close();
                        }
                        if (listener_) {
                            listener_->close();
                        }
                    },
                    protocol_listener_resource_description(protocol_service_));
            }
            return true;
        }

        if (transport != "tcp") {
            LOG_ERROR(
                "plugin protocol service '{}.{}' has unsupported transport '{}' [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                transport,
                runtime_identity_text(runtime_context_));
            return fail_start("unsupported transport '" + transport + "'");
        }

        auto handler_factory = handler_registry_.find_stream_handler(handler_name);
        if (!handler_factory) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' handler '{}' was not registered [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                handler_name,
                runtime_identity_text(runtime_context_));
            return fail_start("stream handler '" + handler_name + "' was not registered");
        }

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
        auto captured_runtime_context = runtime_context_;
        auto listener = std::make_unique<net::AsyncListenerHost>();
        listener->set_connection_handler(
            [captured_handler_factory, captured_service, active_connections, captured_connection_tracker, captured_runtime_stats, captured_host, captured_runtime_context](
                net::AsyncConnectionContext ctx) {
                return run_limited_stream_protocol(
                    std::move(ctx),
                    captured_handler_factory,
                    captured_service,
                    active_connections,
                    captured_connection_tracker,
                    captured_runtime_stats,
                    captured_host,
                    captured_runtime_context);
            });

        const auto host = protocol_service_.host.empty() ? "0.0.0.0" : protocol_service_.host;
        if (!listener->bind(host, static_cast<uint16_t>(protocol_service_.port), *runtime, options)) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' failed to bind {}://{}:{} [{}]",
                protocol_service_.plugin_id,
                protocol_service_.name,
                transport,
                host,
                protocol_service_.port,
                runtime_identity_text(runtime_context_));
            connection_tracker_.reset();
            return fail_start(
                "failed to bind " + transport + "://" + host + ":" + std::to_string(protocol_service_.port),
                true);
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

        auto *runtime = runtime_context_.shared_runtime;
        auto tracker = connection_tracker_;
        if (datagram_acceptor_) {
            std::shared_ptr<net::DatagramAcceptor> datagram_acceptor(datagram_acceptor_.release());
            const bool closed_on_runtime = dispatch_to_runtime_and_wait(
                runtime,
                [datagram_acceptor]() {
                    datagram_acceptor->close();
                });
            if (!closed_on_runtime) {
                datagram_acceptor->close();
            }
            datagram_acceptor.reset();
        }
        if (listener_) {
            std::shared_ptr<net::AsyncListenerHost> listener(listener_.release());
            const bool closed_on_runtime = dispatch_to_runtime_and_wait(
                runtime,
                [listener]() {
                    listener->close();
                });
            if (!closed_on_runtime) {
                listener->close();
            }
            listener.reset();
        }
        if (tracker) {
            request_tracked_connection_shutdown(tracker, runtime);
            if (!wait_for_tracked_connections(tracker, kProtocolConnectionDrainTimeout)) {
                LOG_WARN(
                    "plugin protocol service '{}.{}' still has active connections after shutdown drain timeout [{}]",
                    protocol_service_.plugin_id,
                    protocol_service_.name,
                    runtime_identity_text(runtime_context_));
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
