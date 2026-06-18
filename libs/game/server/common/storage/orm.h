#ifndef YUAN_GAME_SERVER_COMMON_STORAGE_ORM_H
#define YUAN_GAME_SERVER_COMMON_STORAGE_ORM_H

#include "common/proto/db_proxy_proto.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace yuan::game::server::storage
{
    using OrmFields = std::unordered_map<std::string, std::string>;

    struct OrmRow
    {
        OrmFields fields;
    };

    struct OrmResult
    {
        bool ok = false;
        std::string message;
        std::vector<OrmRow> rows;
        std::uint64_t affected_rows = 0;
    };

    class OrmStore
    {
    public:
        virtual ~OrmStore() = default;

        virtual OrmResult query(std::string table, std::string key, std::uint32_t limit = 0) = 0;
        virtual OrmResult insert(std::string table, std::string key, OrmFields fields) = 0;
        virtual OrmResult update(std::string table, std::string key, OrmFields fields) = 0;
        virtual OrmResult delete_(std::string table, std::string key) = 0;

        virtual std::vector<OrmResult> batch(const std::vector<DbOrmOperation> &operations, bool transactional = false);
    };

    OrmFields fields_from_proto(const std::vector<DbOrmField> &fields);
    std::vector<DbOrmField> fields_to_proto(const OrmFields &fields);
    DbOrmOperationResult result_to_proto(const OrmResult &result);
    OrmResult execute_operation(OrmStore &store, const DbOrmOperation &operation);
}

#endif
