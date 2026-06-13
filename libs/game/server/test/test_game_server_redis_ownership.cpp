#include "internal/def.h"
#include "option.h"
#include "redis_client.h"
#include "world/model/world_ownership_store.h"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace
{
    int env_int(const char *name, int fallback)
    {
        const char *value = std::getenv(name);
        if (!value) {
            return fallback;
        }
        int parsed = fallback;
        const std::string text(value);
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? parsed : fallback;
    }

    std::string env_string(const char *name, std::string fallback)
    {
        const char *value = std::getenv(name);
        return value ? std::string(value) : std::move(fallback);
    }

    std::string test_prefix()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#ifndef _WIN32
        return "game:world:owner:test:" + std::to_string(::getpid()) + ":" + std::to_string(now) + ":";
#else
        return "game:world:owner:test:" + std::to_string(now) + ":";
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
    using namespace yuan::game::server;

    yuan::redis::Option option;
    option.host_ = env_string("REDIS_HOST", "127.0.0.1");
    option.port_ = env_int("REDIS_PORT", 6379);
    option.db_ = env_int("REDIS_DB", 1);
    option.timeout_ms_ = env_int("REDIS_TIMEOUT_MS", 1000);
    option.command_timeout_ms_ = env_int("REDIS_TIMEOUT_MS", 1000);
    option.connect_timeout_ms_ = env_int("REDIS_TIMEOUT_MS", 1000);
    option.name_ = "game-world-ownership-test";

    auto redis = std::make_shared<yuan::redis::RedisClient>(option);
    if (!redis->ping()) {
        std::cerr << "Redis ownership test skipped: Redis unavailable\n";
        return EXIT_SUCCESS;
    }

    const auto prefix = test_prefix();
    RedisWorldOwnershipStore store(redis, prefix);
    const RoleId role_id = 10001;
    const auto zone_a = pack_game_service_id(1, 1, GameServiceType::zone, 1);
    const auto zone_b = pack_game_service_id(1, 1, GameServiceType::zone, 2);

    if (!require(store.compare_and_set(role_id, 0, 0, WorldOwnershipRecord{zone_a, 10}), "redis initial owner should set")) {
        return 1;
    }
    if (!require(store.compare_and_set(role_id, zone_a, 10, WorldOwnershipRecord{zone_b, 20}), "redis newer owner should set")) {
        return 2;
    }
    if (!require(!store.compare_and_set(role_id, zone_a, 10, WorldOwnershipRecord{0, 0}), "redis old zone logout should be rejected")) {
        return 3;
    }
    if (!require(store.get(role_id) && store.get(role_id)->zone_service_id == zone_b, "redis owner should remain zone b")) {
        return 4;
    }
    if (!require(!store.compare_and_set(role_id, zone_b, 10, WorldOwnershipRecord{0, 0}), "redis old session logout should be rejected")) {
        return 5;
    }
    if (!require(store.compare_and_set(role_id, zone_b, 20, WorldOwnershipRecord{0, 0}), "redis current session logout should clear")) {
        return 6;
    }
    if (!require(!store.get(role_id), "redis owner should be cleared")) {
        return 7;
    }
    return EXIT_SUCCESS;
}
