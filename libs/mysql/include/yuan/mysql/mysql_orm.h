#ifndef YUAN_MYSQL_MYSQL_ORM_H
#define YUAN_MYSQL_MYSQL_ORM_H

#include "yuan/mysql/mysql_client.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace yuan::mysql::orm
{
    struct SqlCommand
    {
        std::string sql;
        std::vector<Value> params;
    };

    struct FieldValue
    {
        std::string field;
        Value value;
    };

    struct TableColumn
    {
        std::string name;
        bool primary_key = false;
        bool auto_increment = false;
    };

    struct TableSchema
    {
        std::string table;
        std::vector<TableColumn> columns;
    };

    struct RecordField
    {
        std::string name;
        Value value;
    };

    struct Record
    {
        std::string schema_name;
        std::uint32_t schema_version = 0;
        std::vector<RecordField> fields;
    };

    class RecordSchemaMapper
    {
    public:
        virtual ~RecordSchemaMapper() = default;
        virtual TableSchema table_schema(const Record &record) const = 0;
        virtual std::vector<FieldValue> values(const Record &record) const = 0;
        virtual std::vector<FieldValue> primary_key_values(const Record &record) const = 0;
    };

    FieldValue field(std::string name, Value value);
    std::string quote_identifier(std::string_view identifier);

    class Where
    {
    public:
        Where &eq(std::string field, Value value);
        Where &raw(std::string expression, std::vector<Value> params = {});

        bool empty() const noexcept;
        void append_to(std::string &sql, std::vector<Value> &params) const;

    private:
        std::vector<std::string> expressions_;
        std::vector<Value> params_;
    };

    class InsertBuilder
    {
    public:
        explicit InsertBuilder(std::string table);
        InsertBuilder &value(std::string field, Value value);
        InsertBuilder &values(std::vector<FieldValue> values);
        SqlCommand build() const;

    private:
        std::string table_;
        std::vector<FieldValue> values_;
    };

    class UpdateBuilder
    {
    public:
        explicit UpdateBuilder(std::string table);
        UpdateBuilder &set(std::string field, Value value);
        UpdateBuilder &where_eq(std::string field, Value value);
        UpdateBuilder &where_raw(std::string expression, std::vector<Value> params = {});
        SqlCommand build() const;

    private:
        std::string table_;
        std::vector<FieldValue> values_;
        Where where_;
    };

    class DeleteBuilder
    {
    public:
        explicit DeleteBuilder(std::string table);
        DeleteBuilder &where_eq(std::string field, Value value);
        DeleteBuilder &where_raw(std::string expression, std::vector<Value> params = {});
        SqlCommand build() const;

    private:
        std::string table_;
        Where where_;
    };

    class SelectBuilder
    {
    public:
        explicit SelectBuilder(std::string table);
        SelectBuilder &columns(std::vector<std::string> columns);
        SelectBuilder &where_eq(std::string field, Value value);
        SelectBuilder &where_raw(std::string expression, std::vector<Value> params = {});
        SelectBuilder &order_by(std::string expression);
        SelectBuilder &limit(std::uint64_t count);
        SelectBuilder &offset(std::uint64_t count);
        SqlCommand build() const;

    private:
        std::string table_;
        std::vector<std::string> columns_;
        Where where_;
        std::string order_by_;
        std::optional<std::uint64_t> limit_;
        std::optional<std::uint64_t> offset_;
    };

    InsertBuilder insert_into(std::string table);
    UpdateBuilder update(std::string table);
    DeleteBuilder remove_from(std::string table);
    SelectBuilder select_from(std::string table);

    class SqlGenerator
    {
    public:
        explicit SqlGenerator(TableSchema schema);

        const TableSchema &schema() const noexcept;
        std::vector<std::string> primary_key_fields() const;

        SqlCommand insert(std::vector<FieldValue> values) const;
        SqlCommand update_by_primary_key(std::vector<FieldValue> values, std::vector<FieldValue> primary_key) const;
        SqlCommand delete_by_primary_key(std::vector<FieldValue> primary_key) const;
        SqlCommand select_by_primary_key(std::vector<FieldValue> primary_key,
                                         std::vector<std::string> columns = {},
                                         std::optional<std::uint64_t> limit = {}) const;
        SqlCommand select_count_by_primary_key(std::vector<FieldValue> primary_key) const;
        SqlCommand upsert(std::vector<FieldValue> values) const;

    private:
        void validate_field(std::string_view field) const;
        void validate_values(const std::vector<FieldValue> &values, bool allow_auto_increment) const;
        void append_primary_key_where(SqlCommand &command, const std::vector<FieldValue> &primary_key) const;

        TableSchema schema_;
    };

    class RecordSqlGenerator
    {
    public:
        explicit RecordSqlGenerator(const RecordSchemaMapper &mapper);

        SqlCommand insert(const Record &record) const;
        SqlCommand update_by_primary_key(const Record &record) const;
        SqlCommand delete_by_primary_key(const Record &record) const;
        SqlCommand select_by_primary_key(const Record &record,
                                         std::vector<std::string> columns = {},
                                         std::optional<std::uint64_t> limit = {}) const;
        SqlCommand select_count_by_primary_key(const Record &record) const;
        SqlCommand upsert(const Record &record) const;

    private:
        const RecordSchemaMapper *mapper_ = nullptr;
    };

    template <typename T>
    std::vector<T> map_rows(const Result &result, const std::function<T(const Row &)> &mapper)
    {
        std::vector<T> values;
        values.reserve(result.row_count());
        for (const Row &row : result.rows()) {
            values.emplace_back(mapper(row));
        }
        return values;
    }
}

#endif // YUAN_MYSQL_MYSQL_ORM_H
