#ifndef YUAN_GAME_SERVER_WORLD_WORLD_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_WORLD_WORLD_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "messaging/process_message_manager.h"
#include "world/world_service.h"

#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace yuan::redis
{
    class RedisClient;
}

namespace yuan::game::server
{
    class WorldServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        WorldServerService(GameServiceId service_id,
                           std::uint16_t port,
                           std::uint16_t tunnel_port,
                           std::string redis_host,
                           std::uint16_t redis_port,
                           std::uint16_t redis_db,
                           std::string redis_username,
                           std::string redis_password,
                           std::uint16_t redis_connect_timeout_ms,
                           std::uint16_t redis_command_timeout_ms,
                           std::uint16_t redis_flush_interval_ms,
                           std::size_t expected_requests = 1);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        bool register_to_tunnel();
        void ensure_player_roles(PlayerUid player_uid);
        [[nodiscard]] std::optional<PlayerRoleInfo> load_role(RoleId role_id) const;
        [[nodiscard]] PlayerRoleInfo create_role(PlayerUid player_uid) const;
        [[nodiscard]] std::string random_role_name() const;
        void mark_role_dirty(PlayerId player_id, PackedGameServiceId zone_service_id);
        [[nodiscard]] std::optional<GmCommandResponse> forward_gm(GmCommandRequest request) const;
        void flush_dirty_roles();
        void flush_loop(std::stop_token stop_token);

        std::uint16_t port_ = 0;
        std::uint16_t tunnel_port_ = 0;
        std::size_t expected_requests_ = 1;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        std::string redis_host_;
        std::uint16_t redis_port_ = 6379;
        std::uint16_t redis_db_ = 0;
        std::string redis_username_;
        std::string redis_password_;
        std::uint16_t redis_connect_timeout_ms_ = 1000;
        std::uint16_t redis_command_timeout_ms_ = 1000;
        std::uint16_t redis_flush_interval_ms_ = 5000;
        WorldService world_;
        std::shared_ptr<yuan::redis::RedisClient> redis_;
        mutable std::mutex cache_mutex_;
        std::unordered_set<PlayerUid> loaded_players_;
        std::unordered_map<PlayerUid, std::vector<RoleId>> role_ids_by_player_uid_;
        std::unordered_map<RoleId, PlayerRoleInfo> roles_by_id_;
        std::unordered_set<PlayerUid> dirty_players_;
        std::unordered_set<RoleId> dirty_roles_;
        std::jthread flush_thread_;
        mutable ProcessMessageManager messaging_;
        rpc_network::RpcNetworkServer rpc_server_;
    };
}

#endif
