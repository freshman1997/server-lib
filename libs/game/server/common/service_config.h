#ifndef YUAN_GAME_SERVER_COMMON_SERVICE_CONFIG_H
#define YUAN_GAME_SERVER_COMMON_SERVICE_CONFIG_H

#include "common/service_id.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::game::server
{
    struct ServiceServerConfig
    {
        GameServiceId service_id;
        std::uint16_t listen_port = 0;
        std::uint16_t tunnel_port = 0;
        std::uint16_t target_world_port = 0;
        std::vector<std::uint16_t> target_world_ports;
        std::string redis_host = "127.0.0.1";
        std::uint16_t redis_port = 6379;
        std::uint16_t redis_db = 0;
        std::string redis_username;
        std::string redis_password;
        std::uint16_t redis_connect_timeout_ms = 1000;
        std::uint16_t redis_command_timeout_ms = 1000;
        std::uint16_t redis_flush_interval_ms = 5000;
        std::size_t expected_requests = 1;
        GameServiceId target_global_id;
        GameServiceId target_world_id;
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

    inline std::optional<ServiceServerConfig> load_service_server_config(const std::string &path)
    {
        std::ifstream input(path);
        if (!input) {
            return std::nullopt;
        }

        std::unordered_map<std::string, std::string> values;
        std::string line;
        while (std::getline(input, line)) {
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
        auto get_size = [&](const std::string &key, std::size_t fallback = 0) -> std::size_t {
            const auto value = get(key);
            return value ? static_cast<std::size_t>(std::stoull(*value)) : fallback;
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
        config.listen_port = get_u16("listen_port");
        config.tunnel_port = get_u16("tunnel_port");
        config.target_world_port = get_u16("target_world_port");
        config.target_world_ports = get_u16_list("target_world_ports");
        if (config.target_world_ports.empty() && config.target_world_port != 0) {
            config.target_world_ports.push_back(config.target_world_port);
        }
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
        config.expected_requests = get_size("expected_requests", 1);

        config.target_global_id.region = get_u16("target_global_region", config.service_id.region);
        config.target_global_id.world = get_u16("target_global_world", config.service_id.world);
        config.target_global_id.type = GameServiceType::global;
        config.target_global_id.shard = config.target_global_id.world;
        config.target_global_id.instance = get_u64("target_global_instance", 1);
        config.target_world_id.region = get_u16("target_world_region", config.service_id.region);
        config.target_world_id.world = get_u16("target_world_world", config.service_id.world);
        config.target_world_id.type = GameServiceType::world;
        config.target_world_id.shard = config.target_world_id.world;
        config.target_world_id.instance = get_u64("target_world_instance", 1);
        return config;
    }
}

#endif
