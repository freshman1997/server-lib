#ifndef YUAN_GAME_SERVER_COMMON_STORAGE_REDIS_ORM_STORE_H
#define YUAN_GAME_SERVER_COMMON_STORAGE_REDIS_ORM_STORE_H

#include "common/storage/orm.h"

#include "redis_client.h"
#include "redis_client_pool.h"

#include <memory>

namespace yuan::game::server::storage
{
    class RedisOrmStore final : public OrmStore
    {
    public:
        RedisOrmStore(std::shared_ptr<yuan::redis::RedisClient> redis, std::string key_prefix = "game:orm:");
        RedisOrmStore(std::shared_ptr<yuan::redis::RedisClientPool> redis_pool, std::string key_prefix = "game:orm:");

        OrmResult query(std::string table, std::string key, std::uint32_t limit = 0) override;
        OrmResult insert(std::string table, std::string key, OrmFields fields) override;
        OrmResult update(std::string table, std::string key, OrmFields fields) override;
        OrmResult delete_(std::string table, std::string key) override;
        OrmResult exists(std::string table, std::string key) override;
        OrmResult upsert(std::string table, std::string key, OrmFields fields) override;
        OrmResult compare_and_update(std::string table, std::string key, std::string version_field, std::uint64_t expected_version, OrmFields fields) override;
        std::vector<OrmResult> batch(const std::vector<DbOrmOperation> &operations, bool transactional = false) override;

    private:
        [[nodiscard]] std::string redis_key(const std::string &table, const std::string &key) const;
        [[nodiscard]] std::shared_ptr<yuan::redis::RedisClient> redis_client() const;

        std::shared_ptr<yuan::redis::RedisClient> redis_;
        std::shared_ptr<yuan::redis::RedisClientPool> redis_pool_;
        std::string key_prefix_;
    };
}

#endif
