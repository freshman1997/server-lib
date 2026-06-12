#ifndef YUAN_GAME_SERVER_COMMON_SERVICE_CONFIG_H
#define YUAN_GAME_SERVER_COMMON_SERVICE_CONFIG_H

#include "common/service_id.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

namespace yuan::game::server
{
    struct ServiceProcessConfig
    {
        GameServiceId service_id;
        std::uint16_t listen_port = 0;
        std::uint16_t tunnel_port = 0;
        std::size_t expected_requests = 1;
        GameServiceId target_global_id;
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

    inline std::optional<ServiceProcessConfig> load_service_process_config(const std::string &path)
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

        const auto type_value = get("type");
        if (!type_value) {
            return std::nullopt;
        }
        const auto type = parse_service_type(*type_value);
        if (!type) {
            return std::nullopt;
        }

        ServiceProcessConfig config;
        config.service_id.region = get_u16("region");
        config.service_id.world = get_u16("world");
        config.service_id.type = *type;
        config.service_id.shard = config.service_id.world;
        config.service_id.instance = get_u64("instance");
        config.listen_port = get_u16("listen_port");
        config.tunnel_port = get_u16("tunnel_port");
        config.expected_requests = get_size("expected_requests", 1);

        config.target_global_id.region = get_u16("target_global_region", config.service_id.region);
        config.target_global_id.world = get_u16("target_global_world", config.service_id.world);
        config.target_global_id.type = GameServiceType::global;
        config.target_global_id.shard = config.target_global_id.world;
        config.target_global_id.instance = get_u64("target_global_instance", 1);
        return config;
    }
}

#endif
