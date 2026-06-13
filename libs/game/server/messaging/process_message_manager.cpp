#include "messaging/process_message_manager.h"

#include <algorithm>
#include <chrono>

namespace yuan::game::server
{
    ProcessMessageManager::ProcessMessageManager() = default;

    ProcessMessageManager::ProcessMessageManager(std::vector<rpc_network::RpcEndpoint> tunnel_endpoints)
    {
        set_tunnel_endpoints(std::move(tunnel_endpoints));
    }

    ProcessMessageManager::ProcessMessageManager(std::vector<std::uint16_t> tunnel_ports)
    {
        set_tunnel_ports(std::move(tunnel_ports));
    }

    ProcessMessageManager::~ProcessMessageManager()
    {
        stop_heartbeat();
    }

    void ProcessMessageManager::set_tunnel_endpoints(std::vector<rpc_network::RpcEndpoint> tunnel_endpoints)
    {
        std::scoped_lock lock(mutex_);
        tunnels_.clear();
        for (auto &endpoint : tunnel_endpoints) {
            const auto exists = std::find_if(tunnels_.begin(), tunnels_.end(), [&endpoint](const auto &connection) {
                return connection->endpoint().host == endpoint.host && connection->endpoint().port == endpoint.port;
            });
            if (!endpoint.host.empty() && endpoint.port != 0 && exists == tunnels_.end()) {
                tunnels_.push_back(std::make_shared<TunnelConnection>(std::move(endpoint)));
            }
        }
    }

    void ProcessMessageManager::set_tunnel_ports(std::vector<std::uint16_t> tunnel_ports)
    {
        std::vector<rpc_network::RpcEndpoint> endpoints;
        endpoints.reserve(tunnel_ports.size());
        for (const auto port : tunnel_ports) {
            endpoints.push_back(rpc_network::RpcEndpoint{"127.0.0.1", port});
        }
        set_tunnel_endpoints(std::move(endpoints));
    }

    void ProcessMessageManager::add_tunnel_endpoint(rpc_network::RpcEndpoint tunnel_endpoint)
    {
        if (tunnel_endpoint.host.empty() || tunnel_endpoint.port == 0) {
            return;
        }
        std::scoped_lock lock(mutex_);
        const auto exists = std::find_if(tunnels_.begin(), tunnels_.end(), [&tunnel_endpoint](const auto &connection) {
            return connection->endpoint().host == tunnel_endpoint.host && connection->endpoint().port == tunnel_endpoint.port;
        });
        if (exists == tunnels_.end()) {
            tunnels_.push_back(std::make_shared<TunnelConnection>(std::move(tunnel_endpoint)));
        }
    }

    void ProcessMessageManager::add_tunnel_port(std::uint16_t tunnel_port)
    {
        add_tunnel_endpoint(rpc_network::RpcEndpoint{"127.0.0.1", tunnel_port});
    }

    void ProcessMessageManager::set_heartbeat_interval_ms(std::uint64_t interval_ms)
    {
        std::scoped_lock lock(mutex_);
        heartbeat_interval_ = std::chrono::milliseconds(interval_ms == 0 ? 5000 : interval_ms);
    }

    void ProcessMessageManager::start_heartbeat()
    {
        if (heartbeat_thread_.joinable()) {
            return;
        }
        heartbeat_thread_ = std::jthread([this](std::stop_token stop_token) {
            heartbeat_loop(stop_token);
        });
    }

    void ProcessMessageManager::stop_heartbeat()
    {
        heartbeat_thread_.request_stop();
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
    }

    bool ProcessMessageManager::empty() const
    {
        std::scoped_lock lock(mutex_);
        return tunnels_.empty();
    }

    std::optional<rpc_network::RpcEndpoint> ProcessMessageManager::select_tunnel(PackedGameServiceId route_key, TunnelSelectMode mode)
    {
        std::scoped_lock lock(mutex_);
        std::vector<std::size_t> live_indexes;
        for (std::size_t index = 0; index < tunnels_.size(); ++index) {
            if (tunnels_[index]->alive()) {
                live_indexes.push_back(index);
            }
        }
        if (live_indexes.empty()) {
            return std::nullopt;
        }
        std::size_t index = 0;
        if (mode == TunnelSelectMode::random) {
            std::uniform_int_distribution<std::size_t> dist(0, live_indexes.size() - 1);
            index = live_indexes[dist(random_)];
        } else {
            index = live_indexes[static_cast<std::size_t>(route_key % live_indexes.size())];
        }
        return tunnels_[index]->endpoint();
    }

    namespace
    {
        std::vector<std::size_t> tunnel_attempt_order(const std::vector<std::shared_ptr<TunnelConnection>> &tunnels,
                                                      PackedGameServiceId route_key,
                                                      TunnelSelectMode mode,
                                                      std::mt19937 &random)
        {
            std::vector<std::size_t> live;
            std::vector<std::size_t> dead;
            for (std::size_t index = 0; index < tunnels.size(); ++index) {
                if (tunnels[index]->alive()) {
                    live.push_back(index);
                } else {
                    dead.push_back(index);
                }
            }
            if (mode == TunnelSelectMode::random) {
                std::shuffle(live.begin(), live.end(), random);
                std::shuffle(dead.begin(), dead.end(), random);
            } else if (!live.empty()) {
                const auto offset = static_cast<std::size_t>(route_key % live.size());
                std::rotate(live.begin(), live.begin() + static_cast<std::ptrdiff_t>(offset), live.end());
            }
            live.insert(live.end(), dead.begin(), dead.end());
            return live;
        }
    }

    std::optional<yuan::rpc::Response> ProcessMessageManager::register_service(TunnelRegistration registration,
                                                                                PackedGameServiceId route_key,
                                                                                TunnelSelectMode mode)
    {
        yuan::rpc::Bytes payload;
        if (!encode_tunnel_registration(registration, payload)) {
            return std::nullopt;
        }
        yuan::rpc::Message message;
        message.route = game_route::tunnel_register();
        message.payload = std::move(payload);
        return call_tunnel(std::move(message), route_key == 0 ? registration.service_id : route_key, mode);
    }

    std::optional<yuan::rpc::Response> ProcessMessageManager::forward(TunnelEnvelope envelope, TunnelSelectMode mode)
    {
        const auto route_key = envelope.target_service_id != 0 ? envelope.target_service_id : envelope.source_service_id;
        yuan::rpc::Bytes payload;
        if (!encode_tunnel_envelope(envelope, payload)) {
            return std::nullopt;
        }
        yuan::rpc::Message message;
        message.request_id = envelope.request_id;
        message.set_continuation_id(envelope.continuation_id);
        message.route = game_route::tunnel_forward();
        message.payload = std::move(payload);
        return call_tunnel(std::move(message), route_key, mode);
    }

    std::optional<yuan::rpc::Response> ProcessMessageManager::send_to_service(PackedGameServiceId source_service_id,
                                                                               PackedGameServiceId target_service_id,
                                                                               yuan::rpc::Route route,
                                                                               yuan::rpc::Bytes payload,
                                                                               yuan::rpc::Metadata metadata,
                                                                               TunnelSelectMode mode)
    {
        TunnelEnvelope envelope;
        envelope.source_service_id = source_service_id;
        envelope.target_service_id = target_service_id;
        envelope.source = std::to_string(source_service_id);
        envelope.target = std::to_string(target_service_id);
        envelope.route = std::move(route);
        envelope.metadata = std::move(metadata);
        envelope.payload = std::move(payload);
        return forward(std::move(envelope), mode);
    }

    std::optional<yuan::rpc::Response> ProcessMessageManager::send_to_random_service(PackedGameServiceId source_service_id,
                                                                                      GameServiceType target_type,
                                                                                      yuan::rpc::Route route,
                                                                                      yuan::rpc::Bytes payload,
                                                                                      yuan::rpc::Metadata metadata,
                                                                                      TunnelSelectMode mode)
    {
        TunnelEnvelope envelope;
        envelope.source_service_id = source_service_id;
        envelope.source = std::to_string(source_service_id);
        envelope.target = std::to_string(static_cast<ServiceTypeId>(target_type));
        envelope.target_type = target_type;
        envelope.mode = TunnelEnvelope::ForwardMode::random_one;
        envelope.route = std::move(route);
        envelope.metadata = std::move(metadata);
        envelope.payload = std::move(payload);
        return forward(std::move(envelope), mode);
    }

    std::optional<yuan::rpc::Response> ProcessMessageManager::broadcast_to_service_type(PackedGameServiceId source_service_id,
                                                                                        GameServiceType target_type,
                                                                                        yuan::rpc::Route route,
                                                                                        yuan::rpc::Bytes payload,
                                                                                        yuan::rpc::Metadata metadata,
                                                                                        TunnelSelectMode mode)
    {
        TunnelEnvelope envelope;
        envelope.source_service_id = source_service_id;
        envelope.source = std::to_string(source_service_id);
        envelope.target = std::to_string(static_cast<ServiceTypeId>(target_type));
        envelope.target_type = target_type;
        envelope.mode = TunnelEnvelope::ForwardMode::all_of_type;
        envelope.route = std::move(route);
        envelope.metadata = std::move(metadata);
        envelope.payload = std::move(payload);
        return forward(std::move(envelope), mode);
    }

    std::optional<yuan::rpc::Response> ProcessMessageManager::call_tunnel(yuan::rpc::Message message,
                                                                           PackedGameServiceId route_key,
                                                                           TunnelSelectMode mode)
    {
        std::vector<std::shared_ptr<TunnelConnection>> tunnels;
        std::vector<std::size_t> order;
        {
            std::scoped_lock lock(mutex_);
            if (tunnels_.empty()) {
                return std::nullopt;
            }
            tunnels = tunnels_;
            order = tunnel_attempt_order(tunnels_, route_key, mode, random_);
        }
        std::optional<yuan::rpc::Response> last_response;
        int attempt = 0;
        for (const auto index : order) {
            tunnel_call_attempts_.fetch_add(1, std::memory_order_relaxed);
            last_response = tunnels[index]->send_and_update_health(message);
            if (last_response && last_response->status != yuan::rpc::RpcStatus::unavailable) {
                if (attempt > 0) {
                    tunnel_call_recoveries_.fetch_add(1, std::memory_order_relaxed);
                }
                return last_response;
            }
            ++attempt;
            if (attempt < static_cast<int>(order.size())) {
                tunnel_call_retries_.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds{10 * attempt});
            }
        }
        tunnel_call_failures_.fetch_add(1, std::memory_order_relaxed);
        return last_response;
    }

    ProcessMessageManager::Metrics ProcessMessageManager::metrics() const
    {
        return Metrics{tunnel_call_attempts_.load(std::memory_order_relaxed),
                       tunnel_call_retries_.load(std::memory_order_relaxed),
                       tunnel_call_recoveries_.load(std::memory_order_relaxed),
                       tunnel_call_failures_.load(std::memory_order_relaxed)};
    }

    void ProcessMessageManager::heartbeat_loop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            std::chrono::milliseconds interval;
            {
                std::scoped_lock lock(mutex_);
                interval = heartbeat_interval_;
            }
            std::this_thread::sleep_for(interval);
            if (stop_token.stop_requested()) {
                break;
            }
            std::vector<std::shared_ptr<TunnelConnection>> tunnels;
            {
                std::scoped_lock lock(mutex_);
                tunnels = tunnels_;
            }
            for (auto &tunnel : tunnels) {
                (void)tunnel->heartbeat();
            }
        }
    }
}
