#include "yuan/mysql/mysql_orm.h"

#include <algorithm>
#include <stdexcept>

namespace yuan::mysql::orm
{
    namespace
    {
        void require_identifier(std::string_view identifier)
        {
            if (identifier.empty()) {
                throw std::invalid_argument("mysql identifier is empty");
            }
        }

        void append_placeholders(std::string &sql, std::size_t count)
        {
            for (std::size_t i = 0; i < count; ++i) {
                if (i != 0) {
                    sql += ", ";
                }
                sql += "?";
            }
        }
    }

    FieldValue field(std::string name, Value value)
    {
        return FieldValue{std::move(name), std::move(value)};
    }

    std::string quote_identifier(std::string_view identifier)
    {
        require_identifier(identifier);
        std::string quoted;
        quoted.reserve(identifier.size() + 2);
        quoted += '`';
        for (const char ch : identifier) {
            if (ch == '`') {
                quoted += "``";
            } else {
                quoted += ch;
            }
        }
        quoted += '`';
        return quoted;
    }

    Where &Where::eq(std::string field, Value value)
    {
        expressions_.emplace_back(quote_identifier(field) + " = ?");
        params_.emplace_back(std::move(value));
        return *this;
    }

    Where &Where::raw(std::string expression, std::vector<Value> params)
    {
        if (expression.empty()) {
            throw std::invalid_argument("mysql where expression is empty");
        }
        expressions_.emplace_back(std::move(expression));
        params_.insert(params_.end(), std::make_move_iterator(params.begin()), std::make_move_iterator(params.end()));
        return *this;
    }

    bool Where::empty() const noexcept
    {
        return expressions_.empty();
    }

    void Where::append_to(std::string &sql, std::vector<Value> &params) const
    {
        if (expressions_.empty()) {
            return;
        }
        sql += " WHERE ";
        for (std::size_t i = 0; i < expressions_.size(); ++i) {
            if (i != 0) {
                sql += " AND ";
            }
            sql += '(';
            sql += expressions_[i];
            sql += ')';
        }
        params.insert(params.end(), params_.begin(), params_.end());
    }

    InsertBuilder::InsertBuilder(std::string table) : table_(std::move(table))
    {
    }

    InsertBuilder &InsertBuilder::value(std::string field, Value value)
    {
        values_.push_back({std::move(field), std::move(value)});
        return *this;
    }

    InsertBuilder &InsertBuilder::values(std::vector<FieldValue> values)
    {
        values_.insert(values_.end(), std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
        return *this;
    }

    SqlCommand InsertBuilder::build() const
    {
        require_identifier(table_);
        if (values_.empty()) {
            throw std::invalid_argument("mysql insert requires at least one value");
        }

        SqlCommand command;
        command.sql = "INSERT INTO " + quote_identifier(table_) + " (";
        for (std::size_t i = 0; i < values_.size(); ++i) {
            if (i != 0) {
                command.sql += ", ";
            }
            command.sql += quote_identifier(values_[i].field);
            command.params.emplace_back(values_[i].value);
        }
        command.sql += ") VALUES (";
        append_placeholders(command.sql, values_.size());
        command.sql += ')';
        return command;
    }

    UpdateBuilder::UpdateBuilder(std::string table) : table_(std::move(table))
    {
    }

    UpdateBuilder &UpdateBuilder::set(std::string field, Value value)
    {
        values_.push_back({std::move(field), std::move(value)});
        return *this;
    }

    UpdateBuilder &UpdateBuilder::where_eq(std::string field, Value value)
    {
        where_.eq(std::move(field), std::move(value));
        return *this;
    }

    UpdateBuilder &UpdateBuilder::where_raw(std::string expression, std::vector<Value> params)
    {
        where_.raw(std::move(expression), std::move(params));
        return *this;
    }

    SqlCommand UpdateBuilder::build() const
    {
        require_identifier(table_);
        if (values_.empty()) {
            throw std::invalid_argument("mysql update requires at least one set value");
        }

        SqlCommand command;
        command.sql = "UPDATE " + quote_identifier(table_) + " SET ";
        for (std::size_t i = 0; i < values_.size(); ++i) {
            if (i != 0) {
                command.sql += ", ";
            }
            command.sql += quote_identifier(values_[i].field);
            command.sql += " = ?";
            command.params.emplace_back(values_[i].value);
        }
        where_.append_to(command.sql, command.params);
        return command;
    }

    DeleteBuilder::DeleteBuilder(std::string table) : table_(std::move(table))
    {
    }

    DeleteBuilder &DeleteBuilder::where_eq(std::string field, Value value)
    {
        where_.eq(std::move(field), std::move(value));
        return *this;
    }

    DeleteBuilder &DeleteBuilder::where_raw(std::string expression, std::vector<Value> params)
    {
        where_.raw(std::move(expression), std::move(params));
        return *this;
    }

    SqlCommand DeleteBuilder::build() const
    {
        require_identifier(table_);
        SqlCommand command;
        command.sql = "DELETE FROM " + quote_identifier(table_);
        where_.append_to(command.sql, command.params);
        return command;
    }

    SelectBuilder::SelectBuilder(std::string table) : table_(std::move(table))
    {
    }

    SelectBuilder &SelectBuilder::columns(std::vector<std::string> columns)
    {
        columns_ = std::move(columns);
        return *this;
    }

    SelectBuilder &SelectBuilder::where_eq(std::string field, Value value)
    {
        where_.eq(std::move(field), std::move(value));
        return *this;
    }

    SelectBuilder &SelectBuilder::where_raw(std::string expression, std::vector<Value> params)
    {
        where_.raw(std::move(expression), std::move(params));
        return *this;
    }

    SelectBuilder &SelectBuilder::order_by(std::string expression)
    {
        order_by_ = std::move(expression);
        return *this;
    }

    SelectBuilder &SelectBuilder::limit(std::uint64_t count)
    {
        limit_ = count;
        return *this;
    }

    SelectBuilder &SelectBuilder::offset(std::uint64_t count)
    {
        offset_ = count;
        return *this;
    }

    SqlCommand SelectBuilder::build() const
    {
        require_identifier(table_);
        SqlCommand command;
        command.sql = "SELECT ";
        if (columns_.empty()) {
            command.sql += '*';
        } else {
            for (std::size_t i = 0; i < columns_.size(); ++i) {
                if (i != 0) {
                    command.sql += ", ";
                }
                command.sql += quote_identifier(columns_[i]);
            }
        }
        command.sql += " FROM ";
        command.sql += quote_identifier(table_);
        where_.append_to(command.sql, command.params);
        if (!order_by_.empty()) {
            command.sql += " ORDER BY ";
            command.sql += order_by_;
        }
        if (limit_) {
            command.sql += " LIMIT ";
            command.sql += std::to_string(*limit_);
        }
        if (offset_) {
            command.sql += " OFFSET ";
            command.sql += std::to_string(*offset_);
        }
        return command;
    }

    InsertBuilder insert_into(std::string table)
    {
        return InsertBuilder(std::move(table));
    }

    UpdateBuilder update(std::string table)
    {
        return UpdateBuilder(std::move(table));
    }

    DeleteBuilder remove_from(std::string table)
    {
        return DeleteBuilder(std::move(table));
    }

    SelectBuilder select_from(std::string table)
    {
        return SelectBuilder(std::move(table));
    }

    SqlGenerator::SqlGenerator(TableSchema schema) : schema_(std::move(schema))
    {
        require_identifier(schema_.table);
        if (schema_.columns.empty()) {
            throw std::invalid_argument("mysql table schema requires at least one column");
        }
        for (const TableColumn &column : schema_.columns) {
            require_identifier(column.name);
        }
        if (primary_key_fields().empty()) {
            throw std::invalid_argument("mysql table schema requires at least one primary key column");
        }
    }

    const TableSchema &SqlGenerator::schema() const noexcept
    {
        return schema_;
    }

    std::vector<std::string> SqlGenerator::primary_key_fields() const
    {
        std::vector<std::string> fields;
        for (const TableColumn &column : schema_.columns) {
            if (column.primary_key) {
                fields.push_back(column.name);
            }
        }
        return fields;
    }

    SqlCommand SqlGenerator::insert(std::vector<FieldValue> values) const
    {
        validate_values(values, false);
        return insert_into(schema_.table).values(std::move(values)).build();
    }

    SqlCommand SqlGenerator::update_by_primary_key(std::vector<FieldValue> values, std::vector<FieldValue> primary_key) const
    {
        validate_values(values, false);
        validate_values(primary_key, true);
        UpdateBuilder builder(schema_.table);
        for (FieldValue &value : values) {
            builder.set(std::move(value.field), std::move(value.value));
        }
        SqlCommand command = builder.build();
        append_primary_key_where(command, primary_key);
        return command;
    }

    SqlCommand SqlGenerator::delete_by_primary_key(std::vector<FieldValue> primary_key) const
    {
        validate_values(primary_key, true);
        SqlCommand command;
        command.sql = "DELETE FROM " + quote_identifier(schema_.table);
        append_primary_key_where(command, primary_key);
        return command;
    }

    SqlCommand SqlGenerator::select_by_primary_key(std::vector<FieldValue> primary_key,
                                                   std::vector<std::string> columns,
                                                   std::optional<std::uint64_t> limit) const
    {
        validate_values(primary_key, true);
        for (const std::string &column : columns) {
            validate_field(column);
        }

        SqlCommand command;
        command.sql = "SELECT ";
        if (columns.empty()) {
            command.sql += '*';
        } else {
            for (std::size_t i = 0; i < columns.size(); ++i) {
                if (i != 0) {
                    command.sql += ", ";
                }
                command.sql += quote_identifier(columns[i]);
            }
        }
        command.sql += " FROM ";
        command.sql += quote_identifier(schema_.table);
        append_primary_key_where(command, primary_key);
        if (limit) {
            command.sql += " LIMIT ";
            command.sql += std::to_string(*limit);
        }
        return command;
    }

    SqlCommand SqlGenerator::select_count_by_primary_key(std::vector<FieldValue> primary_key) const
    {
        validate_values(primary_key, true);
        SqlCommand command;
        command.sql = "SELECT COUNT(*) FROM " + quote_identifier(schema_.table);
        append_primary_key_where(command, primary_key);
        return command;
    }

    SqlCommand SqlGenerator::upsert(std::vector<FieldValue> values) const
    {
        validate_values(values, true);
        if (values.empty()) {
            throw std::invalid_argument("mysql upsert requires at least one value");
        }

        SqlCommand command;
        command.sql = "INSERT INTO " + quote_identifier(schema_.table) + " (";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                command.sql += ", ";
            }
            command.sql += quote_identifier(values[i].field);
            command.params.emplace_back(values[i].value);
        }
        command.sql += ") VALUES (";
        append_placeholders(command.sql, values.size());
        command.sql += ") ON DUPLICATE KEY UPDATE ";

        bool has_update = false;
        for (const FieldValue &value : values) {
            const auto column = std::find_if(schema_.columns.begin(), schema_.columns.end(), [&](const TableColumn &candidate) {
                return candidate.name == value.field;
            });
            if (column != schema_.columns.end() && column->primary_key) {
                continue;
            }
            if (has_update) {
                command.sql += ", ";
            }
            command.sql += quote_identifier(value.field);
            command.sql += " = VALUES(";
            command.sql += quote_identifier(value.field);
            command.sql += ')';
            has_update = true;
        }

        if (!has_update) {
            throw std::invalid_argument("mysql upsert requires at least one non-primary-key update column");
        }
        return command;
    }

    void SqlGenerator::validate_field(std::string_view field) const
    {
        require_identifier(field);
        const auto found = std::find_if(schema_.columns.begin(), schema_.columns.end(), [&](const TableColumn &column) {
            return column.name == field;
        });
        if (found == schema_.columns.end()) {
            throw std::invalid_argument("mysql schema field is unknown: " + std::string(field));
        }
    }

    void SqlGenerator::validate_values(const std::vector<FieldValue> &values, bool allow_auto_increment) const
    {
        if (values.empty()) {
            throw std::invalid_argument("mysql schema operation requires at least one value");
        }
        for (const FieldValue &value : values) {
            validate_field(value.field);
            const auto column = std::find_if(schema_.columns.begin(), schema_.columns.end(), [&](const TableColumn &candidate) {
                return candidate.name == value.field;
            });
            if (!allow_auto_increment && column != schema_.columns.end() && column->auto_increment) {
                throw std::invalid_argument("mysql insert/update value cannot include auto increment field: " + value.field);
            }
        }
    }

    void SqlGenerator::append_primary_key_where(SqlCommand &command, const std::vector<FieldValue> &primary_key) const
    {
        const std::vector<std::string> primary_fields = primary_key_fields();
        if (primary_key.size() != primary_fields.size()) {
            throw std::invalid_argument("mysql primary key value count does not match schema");
        }

        command.sql += " WHERE ";
        for (std::size_t i = 0; i < primary_fields.size(); ++i) {
            const auto value = std::find_if(primary_key.begin(), primary_key.end(), [&](const FieldValue &candidate) {
                return candidate.field == primary_fields[i];
            });
            if (value == primary_key.end()) {
                throw std::invalid_argument("mysql primary key value is missing: " + primary_fields[i]);
            }
            if (i != 0) {
                command.sql += " AND ";
            }
            command.sql += quote_identifier(primary_fields[i]);
            command.sql += " = ?";
            command.params.emplace_back(value->value);
        }
    }

    RecordSqlGenerator::RecordSqlGenerator(const RecordSchemaMapper &mapper) : mapper_(&mapper)
    {
    }

    SqlCommand RecordSqlGenerator::insert(const Record &record) const
    {
        SqlGenerator generator(mapper_->table_schema(record));
        return generator.insert(mapper_->values(record));
    }

    SqlCommand RecordSqlGenerator::update_by_primary_key(const Record &record) const
    {
        SqlGenerator generator(mapper_->table_schema(record));
        std::vector<FieldValue> values = mapper_->values(record);
        const std::vector<std::string> primary_key_fields = generator.primary_key_fields();
        values.erase(std::remove_if(values.begin(), values.end(), [&](const FieldValue &value) {
            return std::find(primary_key_fields.begin(), primary_key_fields.end(), value.field) != primary_key_fields.end();
        }), values.end());
        return generator.update_by_primary_key(std::move(values), mapper_->primary_key_values(record));
    }

    SqlCommand RecordSqlGenerator::delete_by_primary_key(const Record &record) const
    {
        SqlGenerator generator(mapper_->table_schema(record));
        return generator.delete_by_primary_key(mapper_->primary_key_values(record));
    }

    SqlCommand RecordSqlGenerator::select_by_primary_key(const Record &record,
                                                         std::vector<std::string> columns,
                                                         std::optional<std::uint64_t> limit) const
    {
        SqlGenerator generator(mapper_->table_schema(record));
        return generator.select_by_primary_key(mapper_->primary_key_values(record), std::move(columns), limit);
    }

    SqlCommand RecordSqlGenerator::select_count_by_primary_key(const Record &record) const
    {
        SqlGenerator generator(mapper_->table_schema(record));
        return generator.select_count_by_primary_key(mapper_->primary_key_values(record));
    }

    SqlCommand RecordSqlGenerator::upsert(const Record &record) const
    {
        SqlGenerator generator(mapper_->table_schema(record));
        return generator.upsert(mapper_->values(record));
    }
}
