#include "common/storage/redis_orm_store.h"

#include "value/array_value.h"
#include <charconv>

namespace yuan::game::server::storage
{
    namespace
    {
        std::optional<std::uint64_t> parse_u64(const std::string &value)
        {
            std::uint64_t parsed = 0;
            const auto *begin = value.data();
            const auto *end = value.data() + value.size();

            const auto [ptr, ec] = std::from_chars(begin, end, parsed);
            if (ec != std::errc{} || ptr != end) {
                return std::nullopt;
            }

            return parsed;
        }

        std::vector<std::string> hset_args(std::string key, const OrmFields &fields)
        {
            std::vector<std::string> args;
            args.reserve(1 + fields.size() * 2);
            args.push_back(std::move(key));

            for (const auto &[field, value] : fields) {
                args.push_back(field);
                args.push_back(value);
            }

            return args;
        }

        std::vector<std::string> eval_hset_args(std::string script, std::string key, const OrmFields &fields)
        {
            std::vector<std::string> args;
            args.reserve(3 + fields.size() * 2);
            args.push_back(std::move(script));
            args.push_back("1");
            args.push_back(std::move(key));

            for (const auto &[field, value] : fields) {
                args.push_back(field);
                args.push_back(value);
            }

            return args;
        }

        OrmResult row_from_hgetall(const std::shared_ptr<yuan::redis::RedisValue> &value)
        {
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

        OrmResult affected_result(const std::shared_ptr<yuan::redis::RedisValue> &value, const char *error)
        {
            if (!value) {
                return OrmResult{false, error};
            }

            const auto parsed = parse_u64(value->to_string());
            return parsed ? OrmResult{true, "ok", {}, *parsed} : OrmResult{false, "unexpected redis integer response"};
        }
    }

    RedisOrmStore::RedisOrmStore(std::shared_ptr<yuan::redis::RedisClient> redis, std::string key_prefix)
        : redis_(std::move(redis)), key_prefix_(std::move(key_prefix))
    {
    }

    RedisOrmStore::RedisOrmStore(std::shared_ptr<yuan::redis::RedisClientPool> redis_pool, std::string key_prefix)
        : redis_pool_(std::move(redis_pool)), key_prefix_(std::move(key_prefix))
    {
    }

    OrmResult RedisOrmStore::query(std::string table, std::string key, std::uint32_t limit)
    {
        (void)limit;
        auto redis = redis_client();
        if (!redis) {
            return OrmResult{false, "redis unavailable"};
        }

        return row_from_hgetall(redis->command("HGETALL", {redis_key(table, key)}));
    }

    OrmResult RedisOrmStore::insert(std::string table, std::string key, OrmFields fields)
    {
        auto redis = redis_client();
        if (!redis) {
            return OrmResult{false, "redis unavailable"};
        }

        if (fields.empty()) {
            return OrmResult{false, "fields are required"};
        }

        static constexpr const char *kInsertIfAbsentScript =
            "if redis.call('EXISTS', KEYS[1]) ~= 0 then return 0 end "
            "redis.call('HSET', KEYS[1], unpack(ARGV)) return 1";
        
        const auto saved = redis->command("EVAL", eval_hset_args(kInsertIfAbsentScript, redis_key(table, key), fields));
        if (!saved) {
            return OrmResult{false, "redis insert failed"};
        }

        const auto inserted = parse_u64(saved->to_string()).value_or(0);
        return inserted == 1 ? OrmResult{true, "ok", {}, 1} : OrmResult{false, "row already exists"};
    }

    OrmResult RedisOrmStore::update(std::string table, std::string key, OrmFields fields)
    {
        auto redis = redis_client();
        if (!redis) {
            return OrmResult{false, "redis unavailable"};
        }

        if (fields.empty()) {
            return OrmResult{false, "fields are required"};
        }

        return affected_result(redis->command("HSET", hset_args(redis_key(table, key), fields)), "redis hset failed");
    }

    OrmResult RedisOrmStore::delete_(std::string table, std::string key)
    {
        auto redis = redis_client();
        if (!redis) {
            return OrmResult{false, "redis unavailable"};
        }

        return affected_result(redis->command("DEL", {redis_key(table, key)}), "redis del failed");
    }

    OrmResult RedisOrmStore::exists(std::string table, std::string key)
    {
        auto redis = redis_client();
        if (!redis) {
            return OrmResult{false, "redis unavailable"};
        }

        return affected_result(redis->command("EXISTS", {redis_key(table, key)}), "redis exists failed");
    }

    OrmResult RedisOrmStore::upsert(std::string table, std::string key, OrmFields fields)
    {
        return update(std::move(table), std::move(key), std::move(fields));
    }

    OrmResult RedisOrmStore::compare_and_update(std::string table, std::string key, std::string version_field, std::uint64_t expected_version, OrmFields fields)
    {
        auto redis = redis_client();
        if (!redis) {
            return OrmResult{false, "redis unavailable"};
        }

        if (fields.empty() || version_field.empty()) {
            return OrmResult{false, "fields and version_field are required"};
        }

        static constexpr const char *kCompareAndUpdateScript =
            "local current = redis.call('HGET', KEYS[1], ARGV[1]) "
            "if not current then return -1 end "
            "if tostring(current) ~= tostring(ARGV[2]) then return 0 end "
            "redis.call('HSET', KEYS[1], unpack(ARGV, 3)) return 1";

        std::vector<std::string> args;
        args.reserve(5 + fields.size() * 2);
        args.push_back(kCompareAndUpdateScript);
        args.push_back("1");
        args.push_back(redis_key(table, key));
        args.push_back(std::move(version_field));
        args.push_back(std::to_string(expected_version));
        for (const auto &[field, value] : fields) {
            args.push_back(field);
            args.push_back(value);
        }

        const auto result = redis->command("EVAL", args);
        if (!result) {
            return OrmResult{false, "redis compare_and_update failed"};
        }

        const auto status = result->to_string();
        if (status == "1") {
            return OrmResult{true, "ok", {}, 1};
        }

        if (status == "0") {
            return OrmResult{false, "version mismatch"};
        }

        return OrmResult{false, "row not found"};
    }

    std::vector<OrmResult> RedisOrmStore::batch(const std::vector<DbOrmOperation> &operations, bool transactional)
    {
        if (transactional) {
            return {OrmResult{false, "redis orm transactional batch is not supported"}};
        }

        auto redis = redis_client();
        if (!redis) {
            return {OrmResult{false, "redis unavailable"}};
        }

        std::vector<yuan::redis::PipelineCommand> commands;
        commands.reserve(operations.size());
        for (const auto &operation : operations) {
            const auto key = redis_key(operation.table, operation.key);
            switch (static_cast<DbOrmOpType>(operation.op_type)) {
                case DbOrmOpType::query:
                    commands.push_back(yuan::redis::PipelineCommand{"HGETALL", {key}});
                    break;
                case DbOrmOpType::insert:
                    commands.push_back(yuan::redis::PipelineCommand{"EVAL", eval_hset_args("if redis.call('EXISTS', KEYS[1]) ~= 0 then return 0 end redis.call('HSET', KEYS[1], unpack(ARGV)) return 1", key, fields_from_proto(operation.fields))});
                    break;
                case DbOrmOpType::update:
                    commands.push_back(yuan::redis::PipelineCommand{"HSET", hset_args(key, fields_from_proto(operation.fields))});
                    break;
                case DbOrmOpType::delete_:
                    commands.push_back(yuan::redis::PipelineCommand{"DEL", {key}});
                    break;
            }
        }

        const auto value = redis->pipeline(commands);
        std::vector<OrmResult> results;
        results.reserve(operations.size());
        if (!value || value->get_type() != yuan::redis::resp_array) {
            results.push_back(OrmResult{false, "redis pipeline failed"});
            return results;
        }

        const auto *array = dynamic_cast<yuan::redis::ArrayValue *>(value.get());
        const auto &values = array->get_values();
        for (std::size_t index = 0; index < operations.size(); ++index) {
            if (index >= values.size()) {
                results.push_back(OrmResult{false, "missing redis pipeline response"});
                continue;
            }
            
            switch (static_cast<DbOrmOpType>(operations[index].op_type)) {
                case DbOrmOpType::query:
                    results.push_back(row_from_hgetall(values[index]));
                    break;
                case DbOrmOpType::insert: {
                    const auto inserted = parse_u64(values[index]->to_string()).value_or(0);
                    results.push_back(inserted == 1 ? OrmResult{true, "ok", {}, 1} : OrmResult{false, "row already exists"});
                    break;
                }
                case DbOrmOpType::update:
                case DbOrmOpType::delete_:
                    results.push_back(affected_result(values[index], "redis pipeline command failed"));
                    break;
            }
        }

        return results;
    }

    std::string RedisOrmStore::redis_key(const std::string &table, const std::string &key) const
    {
        return key_prefix_ + table + ":" + key;
    }

    std::shared_ptr<yuan::redis::RedisClient> RedisOrmStore::redis_client() const
    {
        if (redis_pool_) {
            return redis_pool_->get_client_with_wait(50);
        }

        if (redis_ && redis_->ensure_connected()) {
            return redis_;
        }

        return nullptr;
    }
}
