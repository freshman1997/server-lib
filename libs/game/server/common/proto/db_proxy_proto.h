#ifndef YUAN_GAME_SERVER_COMMON_PROTO_DB_PROXY_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_DB_PROXY_PROTO_H

#include "common/codec/binary_codec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::game::server
{
    enum class DbOrmOpType : std::uint32_t
    {
        query = 1,
        insert = 2,
        update = 3,
        delete_ = 4
    };

    struct DbOrmField
    {
        std::string name;
        std::string value;

        YUAN_GAME_BINARY_FIELDS(name, value)
    };

    struct DbOrmOperation
    {
        std::uint32_t op_type = static_cast<std::uint32_t>(DbOrmOpType::query);
        std::string table;
        std::string key;
        std::vector<DbOrmField> fields;
        std::uint32_t limit = 0;

        YUAN_GAME_BINARY_FIELDS(op_type, table, key, fields, limit)
    };

    struct DbOrmRequest
    {
        DbOrmOperation operation;

        YUAN_GAME_BINARY_FIELDS(operation)
    };

    struct DbOrmBatchRequest
    {
        std::vector<DbOrmOperation> operations;
        bool transactional = false;

        YUAN_GAME_BINARY_FIELDS(operations, transactional)
    };

    struct DbOrmRow
    {
        std::vector<DbOrmField> fields;

        YUAN_GAME_BINARY_FIELDS(fields)
    };

    struct DbOrmOperationResult
    {
        bool ok = false;
        std::string message;
        std::vector<DbOrmRow> rows;
        std::uint64_t affected_rows = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, rows, affected_rows)
    };

    struct DbOrmResponse
    {
        DbOrmOperationResult result;

        YUAN_GAME_BINARY_FIELDS(result)
    };

    struct DbOrmBatchResponse
    {
        bool ok = false;
        std::string message;
        std::vector<DbOrmOperationResult> results;

        YUAN_GAME_BINARY_FIELDS(ok, message, results)
    };
}

#endif
