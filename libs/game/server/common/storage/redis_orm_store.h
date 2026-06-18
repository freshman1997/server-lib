#ifndef YUAN_GAME_SERVER_COMMON_STORAGE_REDIS_ORM_STORE_H
#define YUAN_GAME_SERVER_COMMON_STORAGE_REDIS_ORM_STORE_H

#include "common/storage/orm.h"

#include "redis_client.h"

#include <memory>

namespace yuan::game::server::storage
{
    class RedisOrmStore final : public OrmStore
    {
    public:
        RedisOrmStore(std::shared_ptr<yuan::redis::RedisClient> redis, std::string key_prefix = "game:orm:");

        OrmResult query(std::string table, std::string key, std::uint32_t limit = 0) override;
        OrmResult insert(std::string table, std::string key, OrmFields fields) override;
        OrmResult update(std::string table, std::string key, OrmFields fields) override;
        OrmResult delete_(std::string table, std::string key) override;

    private:
        [[nodiscard]] std::string redis_key(const std::string &table, const std::string &key) const;
        [[nodiscard]] bool ensure_redis() const;

        std::shared_ptr<yuan::redis::RedisClient> redis_;
        std::string key_prefix_;
    };
}

#endif
