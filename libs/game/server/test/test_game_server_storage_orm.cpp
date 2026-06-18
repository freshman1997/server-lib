#include "common/storage/entity_store.h"
#include "common/storage/redis_orm_store.h"

#include "option.h"
#include "redis_client.h"

#include <chrono>
#include <iostream>
#include <memory>

namespace
{
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
    option.host_ = "127.0.0.1";
    option.port_ = 6379;
    option.db_ = 0;
    option.timeout_ms_ = 500;
    option.connect_timeout_ms_ = 500;
    option.command_timeout_ms_ = 500;
    option.name_ = "game-storage-orm-test";

    auto redis = std::make_shared<yuan::redis::RedisClient>(option);
    if (!redis->ensure_connected() || !redis->ping()) {
        std::cerr << "game_server_storage_orm skipped: local Redis 127.0.0.1:6379 unavailable\n";
        return 0;
    }

    const auto suffix = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
    const auto key = "row:" + std::to_string(suffix);
    const auto batch_key = "batch:" + std::to_string(suffix);
    storage::RedisOrmStore orm(redis, "game:test_orm:");
    storage::EntityStore entities(orm);

    auto insert_result = entities.insert(storage::EntityRecord{"entity", key, {{"name", "first"}}, 1, yuan::rpc::Bytes{0x01, 0x02}});
    if (!require(insert_result.ok && insert_result.affected_rows == 1, "entity insert should create row atomically")) {
        return 1;
    }

    auto duplicate_insert = entities.insert(storage::EntityRecord{"entity", key, {{"name", "duplicate"}}, 1});
    if (!require(!duplicate_insert.ok, "entity insert should reject existing row")) {
        return 2;
    }

    auto loaded = entities.load("entity", key);
    if (!require(loaded && loaded->version == 1 && loaded->fields["name"] == "first" && loaded->object_blob == yuan::rpc::Bytes{0x01, 0x02},
                 "entity load should roundtrip fields version and blob")) {
        return 3;
    }

    auto save_result = entities.save(storage::EntityRecord{"entity", key, {{"name", "second"}}, 2, yuan::rpc::Bytes{0x03}});
    if (!require(save_result.ok, "entity save should upsert without pre-query")) {
        return 4;
    }

    auto cas_mismatch = entities.compare_and_save(storage::EntityRecord{"entity", key, {{"name", "bad"}}, 3}, 1);
    if (!require(!cas_mismatch.ok, "compare_and_save should reject stale version")) {
        return 5;
    }

    auto cas_ok = entities.compare_and_save(storage::EntityRecord{"entity", key, {{"name", "third"}}, 3}, 2);
    if (!require(cas_ok.ok && cas_ok.affected_rows == 1, "compare_and_save should update matching version")) {
        return 6;
    }

    loaded = entities.load("entity", key);
    if (!require(loaded && loaded->version == 3 && loaded->fields["name"] == "third", "CAS update should persist next version")) {
        return 7;
    }

    std::vector<DbOrmOperation> operations;
    operations.push_back(DbOrmOperation{static_cast<std::uint32_t>(DbOrmOpType::update), "entity", batch_key, storage::fields_to_proto({{"name", "batch"}, {"data_version", "1"}}), 0});
    operations.push_back(DbOrmOperation{static_cast<std::uint32_t>(DbOrmOpType::query), "entity", batch_key, {}, 1});
    operations.push_back(DbOrmOperation{static_cast<std::uint32_t>(DbOrmOpType::delete_), "entity", batch_key, {}, 0});
    auto batch = orm.batch(operations);
    if (!require(batch.size() == 3 && batch[0].ok && batch[1].ok && !batch[1].rows.empty() && batch[2].ok,
                 "redis orm batch should pipeline update query delete")) {
        return 8;
    }

    auto exists = orm.exists("entity", key);
    if (!require(exists.ok && exists.affected_rows == 1, "exists should return affected_rows 1 for present row")) {
        return 9;
    }

    (void)redis->command("DEL", {"game:test_orm:entity:" + key, "game:test_orm:entity:" + batch_key});
    return 0;
}
