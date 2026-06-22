#include "messaging/tunnel_client_manager.h"

#include "base/time.h"
#include "logger.h"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace yuan::game::server
{
    TunnelClientManager::TunnelClientManager() = default;

    TunnelClientManager::TunnelClientManager(std::vector<rpc_network::RpcEndpoint> tunnel_endpoints)
    {
        set_tunnel_endpoints(std::move(tunnel_endpoints));
    }

    TunnelClientManager::TunnelClientManager(std::vector<std::uint16_t> tunnel_ports)
    {
        set_tunnel_ports(std::move(tunnel_ports));
    }

    TunnelClientManager::~TunnelClientManager()
    {
        stop_registered_service();
    }

    void TunnelClientManager::set_tunnel_endpoints(std::vector<rpc_network::RpcEndpoint> tunnel_endpoints)
    {
        std::scoped_lock lock(mutex_);
        tunnels_.clear();
        for (auto &endpoint : tunnel_endpoints) {
            const auto exists = std::find_if(tunnels_.begin(), tunnels_.end(), [&endpoint](const auto &connection) {
                return connection->endpoint().host == endpoint.host && connection->endpoint().port == endpoint.port;
            });
            
            if (!endpoint.host.empty() && endpoint.port != 0 && exists == tunnels_.end()) {
                tunnels_.push_back(std::make_shared<TunnelClient>(std::move(endpoint)));
            }
        }
    }

    void TunnelClientManager::set_tunnel_ports(std::vector<std::uint16_t> tunnel_ports)
    {
        std::vector<rpc_network::RpcEndpoint> endpoints;
        endpoints.reserve(tunnel_ports.size());
        for (const auto port : tunnel_ports) {
            endpoints.push_back(rpc_network::RpcEndpoint{"127.0.0.1", port});
        }
        set_tunnel_endpoints(std::move(endpoints));
    }

    void TunnelClientManager::add_tunnel_endpoint(rpc_network::RpcEndpoint tunnel_endpoint)
    {
        if (tunnel_endpoint.host.empty() || tunnel_endpoint.port == 0) {
            return;
        }

        std::scoped_lock lock(mutex_);
        const auto exists = std::find_if(tunnels_.begin(), tunnels_.end(), [&tunnel_endpoint](const auto &connection) {
            return connection->endpoint().host == tunnel_endpoint.host && connection->endpoint().port == tunnel_endpoint.port;
        });

        if (exists == tunnels_.end()) {
            tunnels_.push_back(std::make_shared<TunnelClient>(std::move(tunnel_endpoint)));
        }
    }

    void TunnelClientManager::add_tunnel_port(std::uint16_t tunnel_port)
    {
        add_tunnel_endpoint(rpc_network::RpcEndpoint{"127.0.0.1", tunnel_port});
    }

    void TunnelClientManager::set_heartbeat_interval_ms(std::uint64_t interval_ms)
    {
        std::scoped_lock lock(mutex_);
        heartbeat_interval_ = std::chrono::milliseconds(interval_ms == 0 ? 5000 : interval_ms);
    }

    void TunnelClientManager::apply_service_config(const ServiceServerConfig &config, std::string registration_name)
    {
        set_tunnel_endpoints(config.tunnel_endpoints);
        set_heartbeat_interval_ms(config.tunnel_heartbeat_interval_ms);
        if (registration_name.empty()) {
            registration_name = std::string(to_string(config.service_id.type));
        }
        configure_registered_service(TunnelRegistration{config.service_id.pack(), config.listen_host, config.listen_port, registration_name}, registration_name);
    }

    void TunnelClientManager::configure_registered_service(TunnelRegistration registration, std::string service_name)
    {
        std::scoped_lock lock(mutex_);
        configured_registration_ = std::move(registration);
        configured_registration_name_ = std::move(service_name);
    }

    void TunnelClientManager::start_registered_service(rpc_network::RpcNetworkServer &rpc_server)
    {
        std::optional<TunnelRegistration> registration;
        std::string service_name;
        {
            std::scoped_lock lock(mutex_);
            registration = configured_registration_;
            service_name = configured_registration_name_;
        }

        if (registration) {
            start_registered_service(rpc_server, std::move(*registration), std::move(service_name));
        }
    }

    void TunnelClientManager::start_registered_service(rpc_network::RpcNetworkServer &rpc_server, TunnelRegistration registration, std::string service_name)
    {
        stop_registered_service();
        if (service_name.empty()) {
            service_name = registration.name.empty() ? "service" : registration.name;
        }

        {
            std::scoped_lock lock(mutex_);
            configured_registration_ = std::move(registration);
            configured_registration_name_ = std::move(service_name);
            timer_owner_ = &rpc_server;
            next_registration_due_ms_ = 0;
        }

        auto first_response = register_service(*configured_registration_);
        if (!first_response || first_response->status != yuan::rpc::RpcStatus::ok) {
            LOG_ERROR("{} failed to register to tunnel service_id={}", configured_registration_name_, configured_registration_->service_id);
        }

        const auto heartbeat_ms = static_cast<std::uint32_t>(heartbeat_interval_.count() <= 0 ? 5000 : heartbeat_interval_.count());
        heartbeat_timer_ = rpc_server.schedule_periodic(heartbeat_ms, heartbeat_ms, [this] {
            heartbeat_tick();
        });

        registered_service_timer_ = rpc_server.schedule_periodic(1, 500, [this] {
            registered_service_tick();
        });
    }

    void TunnelClientManager::stop_registered_service()
    {
        rpc_network::RpcNetworkServer *timer_owner = nullptr;
        yuan::timer::TimerHandle heartbeat_timer;
        yuan::timer::TimerHandle registered_service_timer;

        {
            std::scoped_lock lock(mutex_);
            timer_owner = timer_owner_;
            timer_owner_ = nullptr;
            heartbeat_timer = heartbeat_timer_;
            registered_service_timer = registered_service_timer_;
            heartbeat_timer_.reset();
            registered_service_timer_.reset();
        }

        if (timer_owner) {
            timer_owner->cancel_timer(heartbeat_timer);
            timer_owner->cancel_timer(registered_service_timer);
        }
    }

    bool TunnelClientManager::empty() const
    {
        std::scoped_lock lock(mutex_);
        return tunnels_.empty();
    }

    namespace
    {
        void rotate_by_offset(std::vector<std::size_t> &indexes, std::size_t offset)
        {
            if (indexes.empty()) {
                return;
            }
            offset %= indexes.size();
            std::rotate(indexes.begin(), indexes.begin() + static_cast<std::ptrdiff_t>(offset), indexes.end());
        }

        std::vector<std::size_t> tunnel_attempt_order(const std::vector<std::shared_ptr<TunnelClient>> &tunnels,
                                                      PackedGameServiceId route_key,
                                                      TunnelSelectMode mode,
                                                      std::mt19937 &random,
                                                      std::size_t &next_tunnel_index)
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
            } else if (mode == TunnelSelectMode::hash_by_service_id) {
                rotate_by_offset(live.empty() ? dead : live, static_cast<std::size_t>(route_key));
            } else {
                auto &primary = live.empty() ? dead : live;
                rotate_by_offset(primary, next_tunnel_index);
                if (!primary.empty()) {
                    next_tunnel_index = (next_tunnel_index + 1) % primary.size();
                }
            }

            live.insert(live.end(), dead.begin(), dead.end());
            return live;
        }
    }

    std::optional<yuan::rpc::Response> TunnelClientManager::register_service(TunnelRegistration registration,
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

    void TunnelClientManager::registered_service_tick()
    {
        std::optional<TunnelRegistration> registration;
        std::string service_name;
        const auto now = yuan::base::time::steady_now_ms();
        {
            std::scoped_lock lock(mutex_);
            if (!configured_registration_ || now < next_registration_due_ms_) {
                return;
            }
            registration = configured_registration_;
            service_name = configured_registration_name_.empty() ? "service" : configured_registration_name_;
        }

        auto response = register_service(*registration);
        const auto delay_ms = response && response->status == yuan::rpc::RpcStatus::ok ? 5000ULL : 500ULL;
        if (!response || response->status != yuan::rpc::RpcStatus::ok) {
            LOG_ERROR("{} failed to register to tunnel service_id={}", service_name, registration->service_id);
        }

        {
            std::scoped_lock lock(mutex_);
            next_registration_due_ms_ = yuan::base::time::steady_now_ms() + delay_ms;
        }
    }

    std::optional<yuan::rpc::Response> TunnelClientManager::forward(TunnelEnvelope envelope, TunnelSelectMode mode)
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

    std::optional<yuan::rpc::Response> TunnelClientManager::send_to_service(PackedGameServiceId source_service_id,
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

    std::optional<yuan::rpc::Response> TunnelClientManager::send_to_random_service(PackedGameServiceId source_service_id,
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

    std::optional<yuan::rpc::Response> TunnelClientManager::broadcast_to_service_type(PackedGameServiceId source_service_id,
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

    std::optional<yuan::rpc::Response> TunnelClientManager::call_tunnel(yuan::rpc::Message message,
                                                                           PackedGameServiceId route_key,
                                                                           TunnelSelectMode mode)
    {
        std::vector<std::shared_ptr<TunnelClient>> tunnels;
        std::vector<std::size_t> order;

        {
            std::scoped_lock lock(mutex_);
            if (tunnels_.empty()) {
                return std::nullopt;
            }
            tunnels = tunnels_;
            order = tunnel_attempt_order(tunnels_, route_key, mode, random_, next_tunnel_index_);
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
            }
        }

        tunnel_call_failures_.fetch_add(1, std::memory_order_relaxed);
        return last_response;
    }

    TunnelClientManager::Metrics TunnelClientManager::metrics() const
    {
        return Metrics{tunnel_call_attempts_.load(std::memory_order_relaxed),
                       tunnel_call_retries_.load(std::memory_order_relaxed),
                       tunnel_call_recoveries_.load(std::memory_order_relaxed),
                       tunnel_call_failures_.load(std::memory_order_relaxed)};
    }

    void TunnelClientManager::heartbeat_tick()
    {
        std::vector<std::shared_ptr<TunnelClient>> tunnels;
        {
            std::scoped_lock lock(mutex_);
            tunnels = tunnels_;
        }

        for (auto &tunnel : tunnels) {
            (void)tunnel->heartbeat();
        }
    }
}
