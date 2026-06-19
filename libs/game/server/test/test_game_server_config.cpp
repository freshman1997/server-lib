#include "common/service_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace
{
    void set_env_value(const char *key, const char *value)
    {
#ifdef _WIN32
        _putenv_s(key, value);
#else
        setenv(key, value, 1);
#endif
    }

    void unset_env_value(const char *key)
    {
#ifdef _WIN32
        _putenv_s(key, "");
#else
        unsetenv(key);
#endif
    }

    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main()
{
    namespace fs = std::filesystem;
    using namespace yuan::game::server;

    const auto path = fs::temp_directory_path() / "game_server_world_config_test.json";
    {
        std::ofstream out(path);
        out << R"json({
  "type": 9,
  "region": 1,
  "world": 1,
  "instance": 1,
  "listen_host": "127.0.0.1",
  "listen_port": 25003,
  "websocket_port": 25013,
  "kcp_port": 25023,
  "kcp_update_interval_ms": 20,
  "kcp_cleanup_interval_ms": 2000,
  "kcp_idle_timeout_ms": 70000,
  "kcp_mtu": 1300,
  "kcp_send_window": 64,
  "kcp_receive_window": 96,
  "kcp_resend": 2,
  "kcp_nodelay": false,
  "kcp_no_congestion_control": false,
  "kcp_max_sessions": 100,
  "kcp_max_sessions_per_ip": 3,
  "kcp_allow_migration": true,
  "kcp_max_handshakes_per_address_per_window": 5,
  "kcp_handshake_rate_window_ms": 3000,
  "kcp_max_malformed_packets_per_address": 4,
  "kcp_require_login_token": true,
  "http_port": 25103,
  "tunnel_endpoints": [
    { "host": "127.0.0.1", "port": 25000 }
  ],
  "world_ownership_store": "redis",
  "rpc_max_connections": 2000,
  "rpc_max_buffered_bytes": 131072,
  "rpc_idle_timeout_ms": 30000,
  "gateway_drain_timeout_ms": 4000,
  "login_token_secret": 123456789,
  "gateway_internal_secret": 987654321,
  "world_routing_strategy": "modulo",
  "world_routing_version": 9,
  "world_count": 4
})json";
    }

    const auto config = load_service_server_config(path.string());
    std::error_code ignored;
    fs::remove(path, ignored);

    if (!require(config.has_value(), "world config should parse")) {
        return 1;
    }
    if (!require(config->world_ownership_store == "redis", "world ownership store mode should parse")) {
        return 2;
    }
    if (!require(config->tunnel_endpoints.size() == 1, "world tunnel endpoint should parse")) {
        return 3;
    }
    if (!require(config->rpc_max_connections == 2000 && config->rpc_max_buffered_bytes == 131072 && config->rpc_idle_timeout_ms == 30000,
                 "rpc lifecycle policy should parse")) {
        return 4;
    }
    if (!require(config->websocket_port == 25013 && config->kcp_port == 25023,
                 "gateway transport ports should parse")) {
        return 13;
    }
    if (!require(config->kcp_update_interval_ms == 20 && config->kcp_cleanup_interval_ms == 2000 &&
                     config->kcp_idle_timeout_ms == 70000 && config->kcp_mtu == 1300 &&
                     config->kcp_send_window == 64 && config->kcp_receive_window == 96 &&
                     config->kcp_resend == 2 && !config->kcp_nodelay && !config->kcp_no_congestion_control &&
                     config->kcp_max_sessions == 100 && config->kcp_max_sessions_per_ip == 3 && config->kcp_allow_migration &&
                     config->kcp_max_handshakes_per_address_per_window == 5 &&
                     config->kcp_handshake_rate_window_ms == 3000 && config->kcp_max_malformed_packets_per_address == 4 &&
                     config->kcp_require_login_token,
                 "gateway kcp config should parse")) {
        return 14;
    }
    if (!require(config->gateway_drain_timeout_ms == 4000, "gateway drain timeout should parse")) {
        return 12;
    }
    if (!require(config->login_token_secret == 123456789, "login token secret should parse")) {
        return 5;
    }
    if (!require(config->gateway_internal_secret == 987654321, "gateway internal secret should parse")) {
        return 10;
    }
    if (!require(config->world_routing.strategy == "modulo" && config->world_routing.version == 9 && config->world_routing.world_count == 4,
                 "world routing config should parse")) {
        return 6;
    }
    if (!require(route_world_number_by_player_uid(5, config->world_routing).value_or(0) == 2,
                 "modulo world routing should use configured world count")) {
        return 7;
    }

    const auto web_path = fs::temp_directory_path() / "game_server_web_config_test.json";
    {
        std::ofstream out(web_path);
        out << R"json({
  "listen_host": "127.0.0.1",
  "listen_port": 25005,
  "world_routing_strategy": "modulo",
  "world_routing_version": 10,
  "world_count": 2,
  "world_endpoints": [
    { "world": 1, "host": "127.0.0.1", "port": 25103, "state": "open" },
    { "world": 2, "host": "127.0.0.1", "port": 25104, "state": "open" }
  ]
})json";
    }
    const auto web_config = load_service_server_config(web_path.string());
    fs::remove(web_path, ignored);
    if (!require(web_config.has_value() && web_config->world_endpoints.size() == 2 && web_config->world_endpoints[1].world == 2,
                 "web explicit world endpoints should parse")) {
        return 8;
    }

    const auto legacy_web_path = fs::temp_directory_path() / "game_server_legacy_web_config_test.conf";
    {
        std::ofstream out(legacy_web_path);
        out << R"conf(type=web
listen_host=127.0.0.1
listen_port=25005
world_routing_strategy=modulo
world_routing_version=10
world_count=2
world_endpoints=1,127.0.0.1,25103,open;2,127.0.0.1,25104,open
)conf";
    }
    const auto legacy_web_config = load_service_server_config(legacy_web_path.string());
    fs::remove(legacy_web_path, ignored);
    if (!require(legacy_web_config.has_value() && legacy_web_config->world_endpoints.size() == 2 && legacy_web_config->world_endpoints[0].port == 25103,
                 "legacy web explicit world endpoints should parse")) {
        return 9;
    }

    const auto env_path = fs::temp_directory_path() / "game_server_env_secret_config_test.json";
    {
        std::ofstream out(env_path);
        out << R"json({
  "type": 9,
  "region": 1,
  "world": 1,
  "instance": 1,
  "listen_host": "127.0.0.1",
  "listen_port": 25003,
  "http_port": 25103,
  "tunnel_endpoints": [
    { "host": "127.0.0.1", "port": 25000 }
  ],
  "login_token_secret": 123,
  "gateway_internal_secret": 456
})json";
    }
    set_env_value("GAME_LOGIN_TOKEN_SECRET", "111");
    set_env_value("GAME_GATEWAY_INTERNAL_SECRET", "222");
    const auto env_config = load_service_server_config(env_path.string());
    fs::remove(env_path, ignored);
    unset_env_value("GAME_LOGIN_TOKEN_SECRET");
    unset_env_value("GAME_GATEWAY_INTERNAL_SECRET");
    if (!require(env_config.has_value() && env_config->login_token_secret == 111 && env_config->gateway_internal_secret == 222,
                 "secret environment overrides should parse")) {
        return 11;
    }

    const auto rank_path = fs::temp_directory_path() / "game_server_rank_config_test.json";
    {
        std::ofstream out(rank_path);
        out << R"json({
  "type": 11,
  "region": 1,
  "world": 1,
  "instance": 1,
  "listen_host": "127.0.0.1",
  "listen_port": 25006,
  "tunnel_endpoints": [
    { "host": "127.0.0.1", "port": 25000 }
  ],
  "redis_host": "127.0.0.1",
  "redis_port": 6379,
  "redis_pool_size": 8
})json";
    }
    const auto rank_config = load_service_server_config(rank_path.string());
    fs::remove(rank_path, ignored);
    if (!require(rank_config.has_value() && rank_config->service_id.type == GameServiceType::rank,
                 "rank config should parse")) {
        return 13;
    }

    const auto chat_path = fs::temp_directory_path() / "game_server_chat_web_config_test.json";
    {
        std::ofstream out(chat_path);
        out << R"json({
  "type": "chat",
  "region": 1,
  "world": 1,
  "instance": 1,
  "listen_host": "127.0.0.1",
  "listen_port": 25007,
  "redis_host": "127.0.0.1",
  "redis_port": 6379,
  "redis_pool_size": 8
})json";
    }
    const auto chat_config = load_service_server_config(chat_path.string());
    fs::remove(chat_path, ignored);
    if (!require(chat_config.has_value() && chat_config->service_id.type == GameServiceType::chat,
                 "chat web config should parse")) {
        return 14;
    }

    const auto player_db_path = fs::temp_directory_path() / "game_server_player_db_proxy_config_test.json";
    {
        std::ofstream out(player_db_path);
        out << R"json({
  "type": "player_db_proxy",
  "region": 1,
  "world": 1,
  "instance": 1,
  "listen_host": "127.0.0.1",
  "listen_port": 25008,
  "tunnel_endpoints": [
    { "host": "127.0.0.1", "port": 25000 }
  ],
  "redis_host": "127.0.0.1",
  "redis_port": 6379,
  "redis_pool_size": 8
})json";
    }
    const auto player_db_config = load_service_server_config(player_db_path.string());
    fs::remove(player_db_path, ignored);
    if (!require(player_db_config.has_value() && player_db_config->service_id.type == GameServiceType::player_db_proxy && player_db_config->redis_pool_size == 8,
                 "player db proxy config should parse")) {
        return 15;
    }

    const auto zone_proxy_path = fs::temp_directory_path() / "game_server_zone_proxy_group_config_test.json";
    {
        std::ofstream out(zone_proxy_path);
        out << R"json({
  "type": 2,
  "region": 1,
  "world": 1,
  "instance": 1,
  "listen_host": "127.0.0.1",
  "listen_port": 25002,
  "tunnel_endpoints": [
    { "host": "127.0.0.1", "port": 25000 }
  ],
  "gateway_endpoints": [
    { "service_id": 2252349813686273, "host": "127.0.0.1", "port": 25004, "name": "gateway" }
  ],
  "player_db_proxies": {
    "strategy": "modulo",
    "version": 3,
    "shard_count": 2,
    "endpoints": [
      { "service_id": 4504702360223745, "shard": 0, "state": "open" },
      { "service_id": 4504702360223746, "shard": 1, "state": "open" }
    ]
  }
})json";
    }
    const auto zone_proxy_config = load_service_server_config(zone_proxy_path.string());
    fs::remove(zone_proxy_path, ignored);
    if (!require(zone_proxy_config.has_value() && zone_proxy_config->player_db_proxy_routing.endpoints.size() == 2,
                 "zone player db proxy group should parse")) {
        return 16;
    }
    if (!require(select_db_proxy(3, zone_proxy_config->player_db_proxy_routing).value_or(0) == 4504702360223746ULL,
                 "player db proxy group should route by modulo shard")) {
        return 17;
    }

    const auto world_db_path = fs::temp_directory_path() / "game_server_world_db_proxy_config_test.json";
    {
        std::ofstream out(world_db_path);
        out << R"json({
  "type": "world_db_proxy",
  "region": 1,
  "world": 1,
  "instance": 1,
  "listen_host": "127.0.0.1",
  "listen_port": 25009,
  "tunnel_endpoints": [
    { "host": "127.0.0.1", "port": 25000 }
  ],
  "redis_host": "127.0.0.1",
  "redis_port": 6379
})json";
    }
    const auto world_db_config = load_service_server_config(world_db_path.string());
    fs::remove(world_db_path, ignored);
    if (!require(world_db_config.has_value() && world_db_config->service_id.type == GameServiceType::world_db_proxy,
                 "world db proxy config should parse")) {
        return 18;
    }
    const auto global_db_path = fs::temp_directory_path() / "game_server_global_db_proxy_config_test.json";
    {
        std::ofstream out(global_db_path);
        out << R"json({
  "type": "global_db_proxy",
  "region": 1,
  "world": 1,
  "instance": 1,
  "listen_host": "127.0.0.1",
  "listen_port": 25010,
  "tunnel_endpoints": [
    { "host": "127.0.0.1", "port": 25000 }
  ],
  "redis_host": "127.0.0.1",
  "redis_port": 6379
})json";
    }
    const auto global_db_config = load_service_server_config(global_db_path.string());
    fs::remove(global_db_path, ignored);
    if (!require(global_db_config.has_value() && global_db_config->service_id.type == GameServiceType::global_db_proxy,
                 "global db proxy config should parse")) {
        return 19;
    }
    return EXIT_SUCCESS;
}
