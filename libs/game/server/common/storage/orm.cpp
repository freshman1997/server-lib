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
