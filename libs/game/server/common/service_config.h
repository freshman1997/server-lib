#ifndef YUAN_GAME_SERVER_COMMON_SERVICE_CONFIG_H
#define YUAN_GAME_SERVER_COMMON_SERVICE_CONFIG_H

#include "common/codec/game_binary_codec.h"
#include "common/db_proxy_routing.h"
#include "common/rpc_network.h"
#include "common/service_id.h"
#include "common/world_routing.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace yuan::game::server
{
    struct ServiceServerConfig
    {
        GameServiceId service_id;
        std::string listen_host;
        std::uint16_t listen_port = 0;
        std::uint16_t http_port = 0;
        std::string tunnel_host;
        std::uint16_t tunnel_port = 0;
        std::vector<rpc_network::RpcEndpoint> tunnel_endpoints;
        std::string public_host;
        std::string target_world_host;
        std::uint16_t target_world_port = 0;
        std::vector<std::uint16_t> target_world_ports;
        std::vector<WorldEndpointConfig> world_endpoints;
        std::string redis_host = "127.0.0.1";
        std::uint16_t redis_port = 6379;
        std::uint16_t redis_db = 0;
        std::string redis_username;
        std::string redis_password;
        std::uint16_t redis_connect_timeout_ms = 1000;
        std::uint16_t redis_command_timeout_ms = 1000;
        std::uint16_t redis_flush_interval_ms = 5000;
        std::string world_ownership_store = "memory";
        std::uint16_t zone_load_sync_interval_ms = 1000;
        std::uint32_t zone_max_players = 0;
        std::uint64_t login_reservation_ttl_ms = 3000;
        std::uint64_t zone_report_ttl_ms = 3000;
        std::uint64_t tunnel_heartbeat_interval_ms = 5000;
        std::uint64_t metrics_log_interval_ms = 0;
        std::uint64_t login_token_secret = kDefaultLoginTokenSecret;
        std::uint64_t gateway_internal_secret = kDefaultLoginTokenSecret;
        WorldRoutingConfig world_routing;
        std::uint64_t rpc_max_connections = 0;
        std::uint64_t rpc_max_buffered_bytes = 1024 * 1024;
        std::uint64_t rpc_idle_timeout_ms = 0;
        std::uint64_t gateway_drain_timeout_ms = 3000;
        GameServiceId target_world_id;
        GameServiceId target_player_db_proxy_id;
        DbProxyRoutingConfig player_db_proxy_routing;
        DbProxyRoutingConfig world_db_proxy_routing;
        DbProxyRoutingConfig global_db_proxy_routing;
        std::vector<std::pair<PackedGameServiceId, std::string>> zone_endpoints;
        std::vector<SSGatewayInfo> gateway_endpoints;
    };

    inline std::optional<GameServiceType> parse_service_type(const std::string &value)
    {
        if (value == "tunnel" || value == "1") {
            return GameServiceType::tunnel;
        }
        if (value == "zone" || value == "2") {
            return GameServiceType::zone;
        }
        if (value == "global" || value == "3") {
            return GameServiceType::global;
        }
        if (value == "gateway" || value == "4") {
            return GameServiceType::gateway;
        }
        if (value == "login" || value == "5") {
            return GameServiceType::login;
        }
        if (value == "match" || value == "6") {
            return GameServiceType::match;
        }
        if (value == "battle" || value == "7") {
            return GameServiceType::battle;
        }
        if (value == "chat" || value == "8") {
            return GameServiceType::chat;
        }
        if (value == "world" || value == "9") {
            return GameServiceType::world;
        }
        if (value == "web" || value == "10") {
            return GameServiceType::web;
        }
        if (value == "rank" || value == "11") {
            return GameServiceType::rank;
        }
        if (value == "player_db_proxy" || value == "12") {
            return GameServiceType::player_db_proxy;
        }
        if (value == "world_db_proxy" || value == "13") {
            return GameServiceType::world_db_proxy;
        }
        return std::nullopt;
    }

    inline std::string trim_config_value(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    inline void apply_secret_env_overrides(ServiceServerConfig &config)
    {
        try {
            if (const char *value = std::getenv("GAME_LOGIN_TOKEN_SECRET")) {
                if (*value != '\0') {
                    config.login_token_secret = static_cast<std::uint64_t>(std::stoull(value));
                }
            }
            if (const char *value = std::getenv("GAME_GATEWAY_INTERNAL_SECRET")) {
                if (*value != '\0') {
                    config.gateway_internal_secret = static_cast<std::uint64_t>(std::stoull(value));
                }
            }
        } catch (...) {
        }
    }

    inline std::optional<ServiceServerConfig> load_service_server_config(const std::string &path)
    {
        std::ifstream input(path);
        if (!input) {
            return std::nullopt;
        }

        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto first_non_space = text.find_first_not_of(" \t\r\n");
        if (first_non_space != std::string::npos && text[first_non_space] == '{') {
            nlohmann::json root;
            try {
                root = nlohmann::json::parse(text);
            } catch (...) {
                return std::nullopt;
            }

            auto get_string = [&](const char *key, std::string fallback = {}) -> std::string {
                return root.contains(key) && root[key].is_string() ? root[key].get<std::string>() : std::move(fallback);
            };
            auto get_u16_json = [&](const char *key, std::uint16_t fallback = 0) -> std::uint16_t {
                return root.contains(key) && root[key].is_number_unsigned() ? static_cast<std::uint16_t>(root[key].get<std::uint64_t>()) : fallback;
            };
            auto get_u64_json = [&](const char *key, std::uint64_t fallback = 0) -> std::uint64_t {
                return root.contains(key) && root[key].is_number_unsigned() ? root[key].get<std::uint64_t>() : fallback;
            };
            auto get_tunnel_endpoints_json = [&]() -> std::vector<rpc_network::RpcEndpoint> {
                std::vector<rpc_network::RpcEndpoint> result;
                if (!root.contains("tunnel_endpoints") || !root["tunnel_endpoints"].is_array()) {
                    return result;
                }
                for (const auto &item : root["tunnel_endpoints"]) {
                    if (!item.is_object()) {
                        result.clear();
                        return result;
                    }
                    const auto host = item.value("host", std::string{});
                    const auto port = item.value("port", static_cast<std::uint16_t>(0));
                    if (host.empty() || port == 0) {
                        result.clear();
                        return result;
                    }
                    result.push_back(rpc_network::RpcEndpoint{host, port});
                }
                return result;
            };
            auto get_zone_endpoints_json = [&]() -> std::vector<std::pair<PackedGameServiceId, std::string>> {
                std::vector<std::pair<PackedGameServiceId, std::string>> result;
                if (!root.contains("zone_endpoints") || !root["zone_endpoints"].is_array()) {
                    return result;
                }
                for (const auto &item : root["zone_endpoints"]) {
                    if (!item.is_object()) {
                        result.clear();
                        return result;
                    }
                    const auto service_id = item.value("service_id", static_cast<PackedGameServiceId>(0));
                    const auto host = item.value("host", std::string{});
                    const auto port = item.value("port", static_cast<std::uint16_t>(0));
                    if (service_id == 0 || host.empty() || port == 0) {
                        result.clear();
                        return result;
                    }
                    result.emplace_back(service_id, host + ":" + std::to_string(port));
                }
                return result;
            };
            auto get_gateway_endpoints_json = [&]() -> std::vector<SSGatewayInfo> {
                std::vector<SSGatewayInfo> result;
                if (!root.contains("gateway_endpoints") || !root["gateway_endpoints"].is_array()) {
                    return result;
                }
                for (const auto &item : root["gateway_endpoints"]) {
                    if (!item.is_object()) {
                        result.clear();
                        return result;
                    }
                    SSGatewayInfo gateway;
                    gateway.service_id = item.value("service_id", static_cast<PackedGameServiceId>(0));
                    gateway.host = item.value("host", std::string{});
                    gateway.port = item.value("port", static_cast<std::uint16_t>(0));
                    gateway.name = item.value("name", std::string{"gateway"});
                    if (gateway.service_id == 0 || gateway.host.empty() || gateway.port == 0) {
                        result.clear();
                        return result;
                    }
                    result.push_back(std::move(gateway));
                }
                return result;
            };
            auto get_world_endpoints_json = [&]() -> std::vector<WorldEndpointConfig> {
                std::vector<WorldEndpointConfig> result;
                if (!root.contains("world_endpoints") || !root["world_endpoints"].is_array()) {
                    return result;
                }
                for (const auto &item : root["world_endpoints"]) {
                    if (!item.is_object()) {
                        result.clear();
                        return result;
                    }
                    WorldEndpointConfig endpoint;
                    endpoint.world = item.value("world", static_cast<std::uint16_t>(0));
                    endpoint.host = item.value("host", std::string{});
                    endpoint.port = item.value("port", static_cast<std::uint16_t>(0));
                    endpoint.state = item.value("state", std::string{"open"});
                    if (endpoint.world == 0 || endpoint.host.empty() || endpoint.port == 0) {
                        result.clear();
                        return result;
                    }
                    result.push_back(std::move(endpoint));
                }
                return result;
            };
            auto get_db_proxy_routing_json = [&](const char *key) -> DbProxyRoutingConfig {
                DbProxyRoutingConfig routing;
                if (!root.contains(key) || !root[key].is_object()) {
                    return routing;
                }
                const auto &node = root[key];
                routing.strategy = node.value("strategy", routing.strategy);
                routing.version = node.value("version", routing.version);
                routing.shard_count = node.value("shard_count", routing.shard_count);
                if (node.contains("endpoints") && node["endpoints"].is_array()) {
                    for (const auto &item : node["endpoints"]) {
                        if (!item.is_object()) {
                            routing.endpoints.clear();
                            return routing;
                        }
                        DbProxyEndpointConfig endpoint;
                        endpoint.service_id = item.value("service_id", static_cast<PackedGameServiceId>(0));
                        endpoint.shard = item.value("shard", static_cast<std::uint32_t>(0));
                        endpoint.state = item.value("state", std::string{"open"});
                        if (endpoint.service_id == 0) {
                            routing.endpoints.clear();
                            return routing;
                        }
                        routing.endpoints.push_back(std::move(endpoint));
                    }
                }
                return routing;
            };

            const auto type_value = root.contains("type") ? (root["type"].is_string() ? root["type"].get<std::string>() : std::to_string(root["type"].get<std::uint64_t>())) : std::string{"web"};
            const auto type = parse_service_type(type_value);
            if (!type) {
                return std::nullopt;
            }

            ServiceServerConfig config;
            config.service_id.region = get_u16_json("region");
            config.service_id.world = get_u16_json("world");
            config.service_id.type = *type;
            config.service_id.shard = config.service_id.world;
            config.service_id.instance = get_u64_json("instance");
            config.listen_host = get_string("listen_host");
            config.listen_port = get_u16_json("listen_port");
            config.http_port = get_u16_json("http_port");
            config.tunnel_host = get_string("tunnel_host");
            config.tunnel_port = get_u16_json("tunnel_port");
            config.tunnel_endpoints = get_tunnel_endpoints_json();
            if (config.tunnel_endpoints.empty() && !config.tunnel_host.empty() && config.tunnel_port != 0) {
                config.tunnel_endpoints.push_back(rpc_network::RpcEndpoint{config.tunnel_host, config.tunnel_port});
            }
            config.public_host = get_string("public_host");
            config.target_world_host = get_string("target_world_host");
            config.target_world_port = get_u16_json("target_world_port");
            if (root.contains("target_world_ports") && root["target_world_ports"].is_array()) {
                for (const auto &port : root["target_world_ports"]) {
                    if (port.is_number_unsigned()) {
                        config.target_world_ports.push_back(static_cast<std::uint16_t>(port.get<std::uint64_t>()));
                    }
                }
            }
            if (config.target_world_ports.empty() && config.target_world_port != 0) {
                config.target_world_ports.push_back(config.target_world_port);
            }
            config.world_endpoints = get_world_endpoints_json();
            config.player_db_proxy_routing = get_db_proxy_routing_json("player_db_proxies");
            config.world_db_proxy_routing = get_db_proxy_routing_json("world_db_proxies");
            config.global_db_proxy_routing = get_db_proxy_routing_json("global_db_proxies");
            config.redis_host = get_string("redis_host", "127.0.0.1");
            config.redis_port = get_u16_json("redis_port", 6379);
            config.redis_db = get_u16_json("redis_db", 0);
            config.redis_username = get_string("redis_username");
            config.redis_password = get_string("redis_password");
            config.redis_connect_timeout_ms = get_u16_json("redis_connect_timeout_ms", 1000);
            config.redis_command_timeout_ms = get_u16_json("redis_command_timeout_ms", 1000);
            config.redis_flush_interval_ms = get_u16_json("redis_flush_interval_ms", 5000);
            config.world_ownership_store = get_string("world_ownership_store", "memory");
            config.zone_load_sync_interval_ms = get_u16_json("zone_load_sync_interval_ms", 1000);
            config.zone_max_players = static_cast<std::uint32_t>(get_u64_json("zone_max_players", 0));
            config.login_reservation_ttl_ms = get_u64_json("login_reservation_ttl_ms", 3000);
            config.zone_report_ttl_ms = get_u64_json("zone_report_ttl_ms", 3000);
            config.tunnel_heartbeat_interval_ms = get_u64_json("tunnel_heartbeat_interval_ms", 5000);
            config.metrics_log_interval_ms = get_u64_json("metrics_log_interval_ms", 0);
            config.login_token_secret = get_u64_json("login_token_secret", kDefaultLoginTokenSecret);
            config.gateway_internal_secret = get_u64_json("gateway_internal_secret", kDefaultLoginTokenSecret);
            config.world_routing.strategy = get_string("world_routing_strategy", config.world_routing.strategy);
            config.world_routing.version = get_u64_json("world_routing_version", config.world_routing.version);
            config.world_routing.world_count = get_u16_json("world_count", config.world_routing.world_count);
            config.rpc_max_connections = get_u64_json("rpc_max_connections", 0);
            config.rpc_max_buffered_bytes = get_u64_json("rpc_max_buffered_bytes", 1024 * 1024);
            config.rpc_idle_timeout_ms = get_u64_json("rpc_idle_timeout_ms", 0);
            config.gateway_drain_timeout_ms = get_u64_json("gateway_drain_timeout_ms", config.gateway_drain_timeout_ms);
            config.target_world_id.region = get_u16_json("target_world_region", config.service_id.region);
            config.target_world_id.world = get_u16_json("target_world_world", config.service_id.world);
            config.target_world_id.type = GameServiceType::world;
            config.target_world_id.shard = config.target_world_id.world;
            config.target_world_id.instance = get_u64_json("target_world_instance", 1);
            config.target_player_db_proxy_id.region = get_u16_json("target_player_db_proxy_region", config.service_id.region);
            config.target_player_db_proxy_id.world = get_u16_json("target_player_db_proxy_world", config.service_id.world);
            config.target_player_db_proxy_id.type = GameServiceType::player_db_proxy;
            config.target_player_db_proxy_id.shard = config.target_player_db_proxy_id.world;
            config.target_player_db_proxy_id.instance = get_u64_json("target_player_db_proxy_instance", 0);
            config.zone_endpoints = get_zone_endpoints_json();
            config.gateway_endpoints = get_gateway_endpoints_json();

            if (config.listen_host.empty() || config.listen_port == 0) {
                return std::nullopt;
            }
            if (*type == GameServiceType::global || *type == GameServiceType::zone || *type == GameServiceType::world || *type == GameServiceType::rank || *type == GameServiceType::player_db_proxy || *type == GameServiceType::world_db_proxy) {
                if (config.tunnel_endpoints.empty()) {
                    return std::nullopt;
                }
            }
            if (*type == GameServiceType::world && config.http_port == 0) {
                return std::nullopt;
            }
            if (*type == GameServiceType::zone && config.gateway_endpoints.size() != 1) {
                return std::nullopt;
            }
            if (*type == GameServiceType::gateway && (config.public_host.empty() || config.zone_endpoints.empty())) {
                return std::nullopt;
            }
            if (*type == GameServiceType::web && config.world_endpoints.empty() && (config.target_world_host.empty() || config.target_world_ports.empty())) {
                return std::nullopt;
            }
            if (config.world_routing.strategy != "fixed" && config.world_routing.strategy != "modulo") {
                return std::nullopt;
            }
            if (config.world_routing.world_count == 0) {
                return std::nullopt;
            }
            if (*type == GameServiceType::web && config.world_endpoints.empty() && config.target_world_ports.size() < config.world_routing.world_count) {
                return std::nullopt;
            }
            if (*type == GameServiceType::web && !config.world_endpoints.empty()) {
                for (std::uint16_t index = 0; index < config.world_routing.world_count; ++index) {
                    const auto expected_world = static_cast<std::uint16_t>(1 + index);
                    bool found = false;
                    for (const auto &endpoint : config.world_endpoints) {
                        if (endpoint.world == expected_world && !endpoint.host.empty() && endpoint.port != 0) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        return std::nullopt;
                    }
                }
            }
            apply_secret_env_overrides(config);
            return config;
        }

        std::unordered_map<std::string, std::string> values;
        std::stringstream text_stream(text);
        std::string line;
        while (std::getline(text_stream, line)) {
            const auto comment = line.find('#');
            if (comment != std::string::npos) {
                line.resize(comment);
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            auto key = trim_config_value(line.substr(0, eq));
            auto value = trim_config_value(line.substr(eq + 1));
            if (!key.empty()) {
                values[std::move(key)] = std::move(value);
            }
        }

        auto get = [&](const std::string &key) -> std::optional<std::string> {
            const auto it = values.find(key);
            if (it == values.end()) {
                return std::nullopt;
            }
            return it->second;
        };

        auto get_u16 = [&](const std::string &key, std::uint16_t fallback = 0) -> std::uint16_t {
            const auto value = get(key);
            return value ? static_cast<std::uint16_t>(std::stoul(*value)) : fallback;
        };
        auto get_u64 = [&](const std::string &key, std::uint64_t fallback = 0) -> std::uint64_t {
            const auto value = get(key);
            return value ? static_cast<std::uint64_t>(std::stoull(*value)) : fallback;
        };
        auto get_u16_list = [&](const std::string &key) -> std::vector<std::uint16_t> {
            std::vector<std::uint16_t> result;
            const auto value = get(key);
            if (!value) {
                return result;
            }
            std::stringstream stream(*value);
            std::string item;
            while (std::getline(stream, item, ',')) {
                item = trim_config_value(item);
                if (!item.empty()) {
                    result.push_back(static_cast<std::uint16_t>(std::stoul(item)));
                }
            }
            return result;
        };
        auto get_zone_endpoints = [&](const std::string &key) -> std::vector<std::pair<PackedGameServiceId, std::string>> {
            std::vector<std::pair<PackedGameServiceId, std::string>> result;
            const auto value = get(key);
            if (!value) {
                return result;
            }
            std::stringstream stream(*value);
            std::string item;
            while (std::getline(stream, item, ',')) {
                item = trim_config_value(item);
                if (item.empty()) {
                    continue;
                }
                const auto at = item.find('@');
                if (at == std::string::npos || at == 0 || at + 1 >= item.size()) {
                    result.clear();
                    return result;
                }
                result.emplace_back(static_cast<PackedGameServiceId>(std::stoull(item.substr(0, at))), trim_config_value(item.substr(at + 1)));
            }
            return result;
        };
        auto get_gateway_endpoints = [&](const std::string &key) -> std::vector<SSGatewayInfo> {
            std::vector<SSGatewayInfo> result;
            const auto value = get(key);
            if (!value) {
                return result;
            }
            std::stringstream stream(*value);
            std::string item;
            while (std::getline(stream, item, ',')) {
                item = trim_config_value(item);
                if (item.empty()) {
                    continue;
                }
                const auto at = item.find('@');
                const auto colon = item.rfind(':');
                if (at == std::string::npos || colon == std::string::npos || at == 0 || colon <= at + 1 || colon + 1 >= item.size()) {
                    result.clear();
                    return result;
                }
                SSGatewayInfo gateway;
                gateway.service_id = static_cast<PackedGameServiceId>(std::stoull(item.substr(0, at)));
                gateway.host = trim_config_value(item.substr(at + 1, colon - at - 1));
                gateway.port = static_cast<std::uint16_t>(std::stoul(item.substr(colon + 1)));
                gateway.name = "gateway";
                if (gateway.service_id == 0 || gateway.host.empty() || gateway.port == 0) {
                    result.clear();
                    return result;
                }
                result.push_back(std::move(gateway));
            }
            return result;
        };
        auto get_world_endpoints = [&]() -> std::vector<WorldEndpointConfig> {
            std::vector<WorldEndpointConfig> result;
            const auto value = get("world_endpoints");
            if (!value) {
                return result;
            }
            std::stringstream stream(*value);
            std::string item;
            while (std::getline(stream, item, ';')) {
                std::stringstream endpoint_stream(item);
                std::string world;
                std::string host;
                std::string port;
                std::string state;
                if (!std::getline(endpoint_stream, world, ',') || !std::getline(endpoint_stream, host, ',') || !std::getline(endpoint_stream, port, ',')) {
                    result.clear();
                    return result;
                }
                std::getline(endpoint_stream, state, ',');
                try {
                    WorldEndpointConfig endpoint;
                    endpoint.world = static_cast<std::uint16_t>(std::stoul(trim_config_value(world)));
                    endpoint.host = trim_config_value(host);
                    endpoint.port = static_cast<std::uint16_t>(std::stoul(trim_config_value(port)));
                    endpoint.state = state.empty() ? std::string{"open"} : trim_config_value(state);
                    if (endpoint.world == 0 || endpoint.host.empty() || endpoint.port == 0) {
                        result.clear();
                        return result;
                    }
                    result.push_back(std::move(endpoint));
                } catch (...) {
                    result.clear();
                    return result;
                }
            }
            return result;
        };

        const auto type_value = get("type");
        if (!type_value) {
            return std::nullopt;
        }
        const auto type = parse_service_type(*type_value);
        if (!type) {
            return std::nullopt;
        }

        ServiceServerConfig config;
        config.service_id.region = get_u16("region");
        config.service_id.world = get_u16("world");
        config.service_id.type = *type;
        config.service_id.shard = config.service_id.world;
        config.service_id.instance = get_u64("instance");
        if (const auto listen_host = get("listen_host")) {
            config.listen_host = *listen_host;
        }
        config.listen_port = get_u16("listen_port");
        config.http_port = get_u16("http_port");
        if (const auto tunnel_host = get("tunnel_host")) {
            config.tunnel_host = *tunnel_host;
        }
        config.tunnel_port = get_u16("tunnel_port");
        if (!config.tunnel_host.empty() && config.tunnel_port != 0) {
            config.tunnel_endpoints.push_back(rpc_network::RpcEndpoint{config.tunnel_host, config.tunnel_port});
        }
        if (const auto public_host = get("public_host")) {
            config.public_host = *public_host;
        }
        if (const auto target_world_host = get("target_world_host")) {
            config.target_world_host = *target_world_host;
        }
        config.target_world_port = get_u16("target_world_port");
        config.target_world_ports = get_u16_list("target_world_ports");
        if (config.target_world_ports.empty() && config.target_world_port != 0) {
            config.target_world_ports.push_back(config.target_world_port);
        }
        config.world_endpoints = get_world_endpoints();
        if (const auto redis_host = get("redis_host")) {
            config.redis_host = *redis_host;
        }
        config.redis_port = get_u16("redis_port", 6379);
        config.redis_db = get_u16("redis_db", 0);
        if (const auto redis_username = get("redis_username")) {
            config.redis_username = *redis_username;
        }
        if (const auto redis_password = get("redis_password")) {
            config.redis_password = *redis_password;
        }
            config.redis_connect_timeout_ms = get_u16("redis_connect_timeout_ms", 1000);
            config.redis_command_timeout_ms = get_u16("redis_command_timeout_ms", 1000);
            config.redis_flush_interval_ms = get_u16("redis_flush_interval_ms", 5000);
            if (const auto world_ownership_store = get("world_ownership_store")) {
                config.world_ownership_store = *world_ownership_store;
            }
            config.zone_load_sync_interval_ms = get_u16("zone_load_sync_interval_ms", 1000);
        config.zone_max_players = static_cast<std::uint32_t>(get_u64("zone_max_players", 0));
        config.login_reservation_ttl_ms = get_u64("login_reservation_ttl_ms", 3000);
        config.zone_report_ttl_ms = get_u64("zone_report_ttl_ms", 3000);
        config.tunnel_heartbeat_interval_ms = get_u64("tunnel_heartbeat_interval_ms", 5000);
        config.metrics_log_interval_ms = get_u64("metrics_log_interval_ms", 0);
        config.login_token_secret = get_u64("login_token_secret", kDefaultLoginTokenSecret);
        config.gateway_internal_secret = get_u64("gateway_internal_secret", kDefaultLoginTokenSecret);
        if (const auto strategy = get("world_routing_strategy")) {
            config.world_routing.strategy = *strategy;
        }
        config.world_routing.version = get_u64("world_routing_version", config.world_routing.version);
        config.world_routing.world_count = get_u16("world_count", config.world_routing.world_count);
        config.rpc_max_connections = get_u64("rpc_max_connections", 0);
        config.rpc_max_buffered_bytes = get_u64("rpc_max_buffered_bytes", 1024 * 1024);
        config.rpc_idle_timeout_ms = get_u64("rpc_idle_timeout_ms", 0);
        config.gateway_drain_timeout_ms = get_u64("gateway_drain_timeout_ms", config.gateway_drain_timeout_ms);

        config.target_world_id.region = get_u16("target_world_region", config.service_id.region);
        config.target_world_id.world = get_u16("target_world_world", config.service_id.world);
        config.target_world_id.type = GameServiceType::world;
        config.target_world_id.shard = config.target_world_id.world;
        config.target_world_id.instance = get_u64("target_world_instance", 1);
        config.target_player_db_proxy_id.region = get_u16("target_player_db_proxy_region", config.service_id.region);
        config.target_player_db_proxy_id.world = get_u16("target_player_db_proxy_world", config.service_id.world);
        config.target_player_db_proxy_id.type = GameServiceType::player_db_proxy;
        config.target_player_db_proxy_id.shard = config.target_player_db_proxy_id.world;
        config.target_player_db_proxy_id.instance = get_u64("target_player_db_proxy_instance", 0);
        config.zone_endpoints = get_zone_endpoints("zone_endpoints");
        config.gateway_endpoints = get_gateway_endpoints("gateway_endpoints");

        if (config.listen_host.empty() || config.listen_port == 0) {
            return std::nullopt;
        }
        if (*type == GameServiceType::global || *type == GameServiceType::zone || *type == GameServiceType::world || *type == GameServiceType::rank || *type == GameServiceType::player_db_proxy || *type == GameServiceType::world_db_proxy) {
            if (config.tunnel_endpoints.empty()) {
                return std::nullopt;
            }
        }
        if (*type == GameServiceType::world && config.http_port == 0) {
            return std::nullopt;
        }
        if (*type == GameServiceType::zone && config.gateway_endpoints.size() != 1) {
            return std::nullopt;
        }
        if (*type == GameServiceType::gateway && config.public_host.empty()) {
            return std::nullopt;
        }
        if (*type == GameServiceType::gateway && config.zone_endpoints.empty()) {
            return std::nullopt;
        }
        if (*type == GameServiceType::web) {
            if (config.world_endpoints.empty() && (config.target_world_host.empty() || config.target_world_ports.empty())) {
                return std::nullopt;
            }
            if (config.world_endpoints.empty() && config.target_world_ports.size() < config.world_routing.world_count) {
                return std::nullopt;
            }
            if (config.world_endpoints.empty()) {
                for (const auto port : config.target_world_ports) {
                    if (port == 0) {
                        return std::nullopt;
                    }
                }
            } else {
                std::unordered_map<std::uint16_t, bool> worlds;
                for (const auto &endpoint : config.world_endpoints) {
                    if (endpoint.world == 0 || endpoint.host.empty() || endpoint.port == 0) {
                        return std::nullopt;
                    }
                    worlds[endpoint.world] = true;
                }
                for (std::uint16_t i = 0; i < config.world_routing.world_count; ++i) {
                    if (!worlds.contains(static_cast<std::uint16_t>(1 + i))) {
                        return std::nullopt;
                    }
                }
            }
        }
        if (config.world_routing.strategy != "fixed" && config.world_routing.strategy != "modulo") {
            return std::nullopt;
        }
        if (config.world_routing.world_count == 0) {
            return std::nullopt;
        }
        apply_secret_env_overrides(config);
        return config;
    }
}

#endif
