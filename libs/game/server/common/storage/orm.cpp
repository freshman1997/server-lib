#include "common/storage/orm.h"

namespace yuan::game::server::storage
{
    std::vector<OrmResult> OrmStore::batch(const std::vector<DbOrmOperation> &operations, bool transactional)
    {
        (void)transactional;
        std::vector<OrmResult> results;
        results.reserve(operations.size());
        for (const auto &operation : operations) {
            results.push_back(execute_operation(*this, operation));
        }

        return results;
    }

    OrmResult OrmStore::exists(std::string table, std::string key)
    {
        auto result = query(std::move(table), std::move(key), 1);
        if (!result.ok) {
            return result;
        }

        result.affected_rows = result.rows.empty() ? 0 : 1;
        result.rows.clear();
        return result;
    }

    OrmResult OrmStore::upsert(std::string table, std::string key, OrmFields fields)
    {
        return update(std::move(table), std::move(key), std::move(fields));
    }

    OrmResult OrmStore::compare_and_update(std::string table, std::string key, std::string version_field, std::uint64_t expected_version, OrmFields fields)
    {
        auto current = query(table, key, 1);
        if (!current.ok) {
            return current;
        }

        if (current.rows.empty()) {
            return OrmResult{false, "row not found"};
        }

        const auto it = current.rows.front().fields.find(version_field);
        std::uint64_t actual_version = 0;
        if (it != current.rows.front().fields.end()) {
            try {
                actual_version = static_cast<std::uint64_t>(std::stoull(it->second));
            } catch (...) {
                return OrmResult{false, "invalid version field"};
            }
        }

        if (actual_version != expected_version) {
            return OrmResult{false, "version mismatch"};
        }

        return update(std::move(table), std::move(key), std::move(fields));
    }

    OrmFields fields_from_proto(const std::vector<DbOrmField> &fields)
    {
        OrmFields result;
        for (const auto &field : fields) {
            result[field.name] = field.value;
        }
        return result;
    }

    std::vector<DbOrmField> fields_to_proto(const OrmFields &fields)
    {
        std::vector<DbOrmField> result;
        result.reserve(fields.size());

        for (const auto &[name, value] : fields) {
            result.push_back(DbOrmField{name, value});
        }

        return result;
    }

    DbOrmOperationResult result_to_proto(const OrmResult &result)
    {
        DbOrmOperationResult proto;
        proto.ok = result.ok;
        proto.message = result.message;
        proto.affected_rows = result.affected_rows;
        proto.rows.reserve(result.rows.size());
        for (const auto &row : result.rows) {
            proto.rows.push_back(DbOrmRow{fields_to_proto(row.fields)});
        }

        return proto;
    }

    OrmResult execute_operation(OrmStore &store, const DbOrmOperation &operation)
    {
        switch (static_cast<DbOrmOpType>(operation.op_type)) {
            case DbOrmOpType::query:
                return store.query(operation.table, operation.key, operation.limit);
            case DbOrmOpType::insert:
                return store.insert(operation.table, operation.key, fields_from_proto(operation.fields));
            case DbOrmOpType::update:
                return store.update(operation.table, operation.key, fields_from_proto(operation.fields));
            case DbOrmOpType::delete_:
                return store.delete_(operation.table, operation.key);
        }
        return OrmResult{false, "unknown orm operation"};
    }
}
