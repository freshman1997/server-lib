#include "common/storage/redis_orm_store.h"

#include "value/array_value.h"
#include "value/null_value.h"

namespace yuan::game::server::storage
{
    RedisOrmStore::RedisOrmStore(std::shared_ptr<yuan::redis::RedisClient> redis, std::string key_prefix)
        : redis_(std::move(redis)), key_prefix_(std::move(key_prefix))
    {
    }

    OrmResult RedisOrmStore::query(std::string table, std::string key, std::uint32_t limit)
    {
        (void)limit;
        if (!ensure_redis()) {
            return OrmResult{false, "redis unavailable"};
        }
        const auto value = redis_->command("HGETALL", {redis_key(table, key)});
        if (!value || value->get_type() == yuan::redis::resp_null) {
            return OrmResult{true, "ok"};
        }
        if (value->get_type() != yuan::redis::resp_array) {
            return OrmResult{false, "unexpected redis response"};
        }
        const auto *array = dynamic_cast<yuan::redis::ArrayValue *>(value.get());
        OrmRow row;
        const auto &values = array->get_values();
        for (std::size_t index = 0; index + 1 < values.size(); index += 2) {
            row.fields[values[index]->to_string()] = values[index + 1]->to_string();
        }
        if (row.fields.empty()) {
            return OrmResult{true, "ok"};
        }
        return OrmResult{true, "ok", {std::move(row)}, 0};
    }

    OrmResult RedisOrmStore::insert(std::string table, std::string key, OrmFields fields)
    {
        if (!ensure_redis()) {
            return OrmResult{false, "redis unavailable"};
        }
        if (fields.empty()) {
            return OrmResult{false, "fields are required"};
        }
        const auto exists = redis_->command("EXISTS", {redis_key(table, key)});
        if (exists && exists->to_string() != "0") {
            return OrmResult{false, "row already exists"};
        }
        return update(std::move(table), std::move(key), std::move(fields));
    }

    OrmResult RedisOrmStore::update(std::string table, std::string key, OrmFields fields)
    {
        if (!ensure_redis()) {
            return OrmResult{false, "redis unavailable"};
        }
        if (fields.empty()) {
            return OrmResult{false, "fields are required"};
        }
        std::vector<std::string> args;
        args.reserve(1 + fields.size() * 2);
        args.push_back(redis_key(table, key));
        for (const auto &[field, value] : fields) {
            args.push_back(field);
            args.push_back(value);
        }
        const auto saved = redis_->command("HSET", args);
        return saved ? OrmResult{true, "ok", {}, static_cast<std::uint64_t>(std::stoull(saved->to_string()))} : OrmResult{false, "redis hset failed"};
    }

    OrmResult RedisOrmStore::delete_(std::string table, std::string key)
    {
        if (!ensure_redis()) {
            return OrmResult{false, "redis unavailable"};
        }
        const auto removed = redis_->command("DEL", {redis_key(table, key)});
        return removed ? OrmResult{true, "ok", {}, static_cast<std::uint64_t>(std::stoull(removed->to_string()))} : OrmResult{false, "redis del failed"};
    }

    std::string RedisOrmStore::redis_key(const std::string &table, const std::string &key) const
    {
        return key_prefix_ + table + ":" + key;
    }

    bool RedisOrmStore::ensure_redis() const
    {
        return redis_ && redis_->ensure_connected();
    }
}
