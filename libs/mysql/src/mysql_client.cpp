#include "yuan/mysql/mysql_client.h"

#include <mysql.h>

#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>
#include <string>
#include <charconv>
#include <cerrno>

namespace yuan::mysql
{
    namespace
    {
        void apply_ssl_mode(MYSQL *mysql, SslMode mode)
        {
#ifdef MYSQL_OPT_SSL_MODE
            unsigned int ssl_mode = SSL_MODE_PREFERRED;
            switch (mode) {
            case SslMode::Disabled:
                ssl_mode = SSL_MODE_DISABLED;
                mysql_options(mysql, MYSQL_OPT_SSL_MODE, &ssl_mode);
                return;
            case SslMode::Preferred:
                ssl_mode = SSL_MODE_PREFERRED;
                mysql_options(mysql, MYSQL_OPT_SSL_MODE, &ssl_mode);
                return;
            case SslMode::Required:
                ssl_mode = SSL_MODE_REQUIRED;
                mysql_options(mysql, MYSQL_OPT_SSL_MODE, &ssl_mode);
                return;
            case SslMode::VerifyCa:
                ssl_mode = SSL_MODE_VERIFY_CA;
                mysql_options(mysql, MYSQL_OPT_SSL_MODE, &ssl_mode);
                return;
            case SslMode::VerifyIdentity:
                ssl_mode = SSL_MODE_VERIFY_IDENTITY;
                mysql_options(mysql, MYSQL_OPT_SSL_MODE, &ssl_mode);
                return;
            }
#else
            if (mode == SslMode::Required || mode == SslMode::VerifyCa || mode == SslMode::VerifyIdentity) {
                mysql_ssl_set(mysql, nullptr, nullptr, nullptr, nullptr, nullptr);
            }
#endif
        }

        struct BoundParams
        {
            std::vector<MYSQL_BIND> binds;
            std::vector<std::unique_ptr<bool>> is_null;
            std::vector<unsigned long> lengths;

            explicit BoundParams(const std::vector<Value> &params)
                : binds(params.size()), is_null(params.size()), lengths(params.size(), 0)
            {
                for (std::size_t i = 0; i < params.size(); ++i) {
                    MYSQL_BIND &bind = binds[i];
                    std::memset(&bind, 0, sizeof(bind));

                    if (std::holds_alternative<std::monostate>(params[i])) {
                        is_null[i] = std::make_unique<bool>(true);
                        bind.buffer_type = MYSQL_TYPE_NULL;
                        bind.is_null = is_null[i].get();
                    } else if (auto value = std::get_if<std::int64_t>(&params[i])) {
                        bind.buffer_type = MYSQL_TYPE_LONGLONG;
                        bind.buffer = const_cast<std::int64_t *>(value);
                        bind.is_unsigned = 0;
                    } else if (auto value = std::get_if<std::uint64_t>(&params[i])) {
                        bind.buffer_type = MYSQL_TYPE_LONGLONG;
                        bind.buffer = const_cast<std::uint64_t *>(value);
                        bind.is_unsigned = 1;
                    } else if (auto value = std::get_if<double>(&params[i])) {
                        bind.buffer_type = MYSQL_TYPE_DOUBLE;
                        bind.buffer = const_cast<double *>(value);
                    } else if (auto value = std::get_if<std::string>(&params[i])) {
                        lengths[i] = static_cast<unsigned long>(value->size());
                        bind.buffer_type = MYSQL_TYPE_STRING;
                        bind.buffer = const_cast<char *>(value->data());
                        bind.buffer_length = lengths[i];
                        bind.length = &lengths[i];
                    }
                }
            }
        };

        void ensure_library_initialized()
        {
            static const bool initialized = [] {
                if (mysql_library_init(0, nullptr, nullptr) != 0) {
                    throw MysqlError(0, "HY000", "mysql_library_init failed");
                }
                std::atexit(mysql_library_end);
                return true;
            }();
            (void)initialized;
        }
    }

    MysqlError::MysqlError(unsigned int code, std::string sql_state, std::string message)
        : std::runtime_error(std::move(message)), code_(code), sql_state_(std::move(sql_state))
    {
    }

    unsigned int MysqlError::code() const noexcept
    {
        return code_;
    }

    const std::string &MysqlError::sql_state() const noexcept
    {
        return sql_state_;
    }

    Value null_value()
    {
        return std::monostate{};
    }

    Value string_value(std::string value)
    {
        return Value(std::move(value));
    }

    namespace
    {
        Value decode_mysql_text(const char *data, unsigned long length, enum_field_types type, unsigned int flags)
        {
            if (!data) return std::monostate{};
            const std::string text(data, length);
            switch (type) {
                case MYSQL_TYPE_TINY:
                case MYSQL_TYPE_SHORT:
                case MYSQL_TYPE_LONG:
                case MYSQL_TYPE_INT24:
                case MYSQL_TYPE_LONGLONG:
                    if (flags & UNSIGNED_FLAG) {
                        std::uint64_t value = 0;
                        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return value;
                    } else {
                        std::int64_t value = 0;
                        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return value;
                    }
                    break;
                case MYSQL_TYPE_FLOAT:
                case MYSQL_TYPE_DOUBLE: {
                    char *end = nullptr;
                    errno = 0;
                    const auto value = std::strtod(text.c_str(), &end);
                    if (errno == 0 && end == text.c_str() + text.size()) return value;
                    break;
                }
                default:
                    break;
            }
            return text;
        }
    }

    ThreadGuard::ThreadGuard()
    {
        ensure_library_initialized();
        if (mysql_thread_init() != 0) {
            throw MysqlError(0, "HY000", "mysql_thread_init failed");
        }
    }

    ThreadGuard::~ThreadGuard()
    {
        mysql_thread_end();
    }

    Row::Row(std::vector<Value> values) : values_(std::move(values))
    {
    }

    std::size_t Row::size() const noexcept
    {
        return values_.size();
    }

    bool Row::is_null(std::size_t index) const
    {
        return std::holds_alternative<std::monostate>(values_.at(index));
    }

    const Value &Row::operator[](std::size_t index) const
    {
        return values_.at(index);
    }

    const std::vector<Value> &Row::values() const noexcept
    {
        return values_;
    }

    Result::Result(std::vector<std::string> columns, std::vector<Row> rows)
        : columns_(std::move(columns)), rows_(std::move(rows))
    {
    }

    bool Result::empty() const noexcept
    {
        return rows_.empty();
    }

    std::size_t Result::row_count() const noexcept
    {
        return rows_.size();
    }

    std::size_t Result::column_count() const noexcept
    {
        return columns_.size();
    }

    const std::vector<std::string> &Result::columns() const noexcept
    {
        return columns_;
    }

    const std::vector<Row> &Result::rows() const noexcept
    {
        return rows_;
    }

    Connection::Connection()
    {
        ensure_library_initialized();
        mysql_ = mysql_init(nullptr);
        if (!mysql_) {
            throw MysqlError(0, "HY000", "mysql_init failed");
        }
    }

    Connection::Connection(ConnectionOptions options) : Connection()
    {
        options_ = std::move(options);
        connect();
    }

    Connection::~Connection()
    {
        close();
    }

    Connection::Connection(Connection &&other) noexcept
        : mysql_(std::exchange(other.mysql_, nullptr)),
          connected_(std::exchange(other.connected_, false)),
          options_(std::move(other.options_))
    {
    }

    Connection &Connection::operator=(Connection &&other) noexcept
    {
        if (this != &other) {
            close();
            mysql_ = std::exchange(other.mysql_, nullptr);
            connected_ = std::exchange(other.connected_, false);
            options_ = std::move(other.options_);
        }
        return *this;
    }

    void Connection::connect()
    {
        if (!mysql_) {
            mysql_ = mysql_init(nullptr);
            if (!mysql_) {
                throw MysqlError(0, "HY000", "mysql_init failed");
            }
        }
        connected_ = false;
        apply_options();

        const char *unix_socket = options_.unix_socket.empty() ? nullptr : options_.unix_socket.c_str();
        const char *database = options_.database.empty() ? nullptr : options_.database.c_str();
        MYSQL *ret = mysql_real_connect(mysql_, options_.host.c_str(), options_.user.c_str(), options_.password.c_str(),
                                        database, options_.port, unix_socket, options_.client_flags);
        if (!ret) {
            throw_last_error("connect");
        }
        connected_ = true;
    }

    void Connection::connect(ConnectionOptions options)
    {
        close();
        mysql_ = mysql_init(nullptr);
        if (!mysql_) {
            throw MysqlError(0, "HY000", "mysql_init failed");
        }
        options_ = std::move(options);
        connect();
    }

    void Connection::close() noexcept
    {
        if (mysql_) {
            mysql_close(mysql_);
            mysql_ = nullptr;
        }
        connected_ = false;
    }

    bool Connection::is_connected() const noexcept
    {
        return connected_;
    }

    void Connection::ping()
    {
        if (mysql_ping(mysql_) != 0) {
            connected_ = false;
            throw_last_error("ping");
        }
    }

    void Connection::select_database(std::string_view database)
    {
        const std::string db(database);
        if (mysql_select_db(mysql_, db.c_str()) != 0) {
            throw_last_error("select database");
        }
        options_.database = db;
    }

    void Connection::execute(std::string_view sql)
    {
        last_statement_affected_rows_.reset();
        last_statement_insert_id_.reset();
        if (mysql_real_query(mysql_, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
            throw_last_error("execute");
        }

        MYSQL_RES *result = mysql_store_result(mysql_);
        if (result) {
            mysql_free_result(result);
        } else if (mysql_field_count(mysql_) != 0) {
            throw_last_error("store result");
        }
    }

    void Connection::execute(std::string_view sql, const std::vector<Value> &params)
    {
        auto statement = prepare(sql);
        statement.execute(params);
        last_statement_affected_rows_ = statement.affected_rows();
        last_statement_insert_id_ = statement.last_insert_id();
    }

    Result Connection::query(std::string_view sql)
    {
        last_statement_affected_rows_.reset();
        last_statement_insert_id_.reset();
        if (mysql_real_query(mysql_, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
            throw_last_error("query");
        }

        MYSQL_RES *result = mysql_store_result(mysql_);
        if (!result) {
            if (mysql_field_count(mysql_) == 0) {
                return {};
            }
            throw_last_error("store result");
        }

        return read_result(result);
    }

    Result Connection::query(std::string_view sql, const std::vector<Value> &params)
    {
        last_statement_affected_rows_.reset();
        last_statement_insert_id_.reset();
        return prepare(sql).query(params);
    }

    Statement Connection::prepare(std::string_view sql)
    {
        return Statement(mysql_, std::string(sql));
    }

    void Connection::begin_transaction()
    {
        execute("START TRANSACTION");
    }

    void Connection::commit()
    {
        if (mysql_commit(mysql_) != 0) {
            throw_last_error("commit");
        }
    }

    void Connection::rollback()
    {
        if (mysql_rollback(mysql_) != 0) {
            throw_last_error("rollback");
        }
    }

    std::uint64_t Connection::affected_rows() const
    {
        if (last_statement_affected_rows_) {
            return *last_statement_affected_rows_;
        }
        const my_ulonglong value = mysql_affected_rows(mysql_);
        if (value == static_cast<my_ulonglong>(-1)) {
            if (mysql_errno(mysql_) == 0) {
                return 0;
            }
            throw_last_error("affected rows");
        }
        return static_cast<std::uint64_t>(value);
    }

    std::uint64_t Connection::last_insert_id() const
    {
        if (last_statement_insert_id_) {
            return *last_statement_insert_id_;
        }
        return static_cast<std::uint64_t>(mysql_insert_id(mysql_));
    }

    std::string Connection::escape(std::string_view value) const
    {
        if (value.size() > (std::numeric_limits<unsigned long>::max() / 2) - 1) {
            throw MysqlError(0, "HY000", "string too large to escape");
        }

        std::string escaped(value.size() * 2 + 1, '\0');
        unsigned long length = mysql_real_escape_string(mysql_, escaped.data(), value.data(),
                                                       static_cast<unsigned long>(value.size()));
        escaped.resize(length);
        return escaped;
    }

    void Connection::apply_options()
    {
        if (options_.connect_timeout_seconds) {
            unsigned int timeout = *options_.connect_timeout_seconds;
            mysql_options(mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        }
        if (options_.read_timeout_seconds) {
            unsigned int timeout = *options_.read_timeout_seconds;
            mysql_options(mysql_, MYSQL_OPT_READ_TIMEOUT, &timeout);
        }
        if (options_.write_timeout_seconds) {
            unsigned int timeout = *options_.write_timeout_seconds;
            mysql_options(mysql_, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
        }
        if (options_.charset) {
            mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, options_.charset->c_str());
        }
        if (options_.ssl_mode) {
            apply_ssl_mode(mysql_, *options_.ssl_mode);
        }
    }

    void Connection::throw_last_error(std::string_view operation) const
    {
        std::string message(operation);
        message += " failed: ";
        message += mysql_ ? mysql_error(mysql_) : "mysql handle is null";
        throw MysqlError(mysql_ ? mysql_errno(mysql_) : 0, mysql_ ? mysql_sqlstate(mysql_) : "HY000", std::move(message));
    }

    Result Connection::read_result(MYSQL_RES *result)
    {
        std::vector<std::string> columns;
        std::vector<Row> rows;

        const unsigned int field_count = mysql_num_fields(result);
        MYSQL_FIELD *fields = mysql_fetch_fields(result);
        columns.reserve(field_count);
        for (unsigned int i = 0; i < field_count; ++i) {
            columns.emplace_back(fields[i].name ? fields[i].name : "");
        }

        MYSQL_ROW mysql_row = nullptr;
        while ((mysql_row = mysql_fetch_row(result)) != nullptr) {
            unsigned long *lengths = mysql_fetch_lengths(result);
            std::vector<Value> values;
            values.reserve(field_count);
            for (unsigned int i = 0; i < field_count; ++i) {
                if (!mysql_row[i]) {
                    values.emplace_back(std::monostate{});
                } else {
                    values.emplace_back(decode_mysql_text(mysql_row[i], lengths[i], fields[i].type, fields[i].flags));
                }
            }
            rows.emplace_back(std::move(values));
        }

        mysql_free_result(result);
        return Result(std::move(columns), std::move(rows));
    }

    Statement::Statement(MYSQL *mysql, std::string sql)
    {
        stmt_ = mysql_stmt_init(mysql);
        if (!stmt_) {
            throw MysqlError(0, "HY000", "mysql_stmt_init failed");
        }
        if (mysql_stmt_prepare(stmt_, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
            throw_last_error("prepare");
        }

        bool update_max_length = true;
        mysql_stmt_attr_set(stmt_, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length);
    }

    Statement::~Statement()
    {
        close();
    }

    Statement::Statement(Statement &&other) noexcept : stmt_(std::exchange(other.stmt_, nullptr))
    {
    }

    Statement &Statement::operator=(Statement &&other) noexcept
    {
        if (this != &other) {
            close();
            stmt_ = std::exchange(other.stmt_, nullptr);
        }
        return *this;
    }

    void Statement::execute(const std::vector<Value> &params)
    {
        validate_params(params);
        BoundParams bound_params(params);
        if (!bound_params.binds.empty() && mysql_stmt_bind_param(stmt_, bound_params.binds.data()) != 0) {
            throw_last_error("bind statement parameters");
        }
        if (mysql_stmt_execute(stmt_) != 0) {
            throw_last_error("execute statement");
        }
        if (mysql_stmt_field_count(stmt_) != 0) {
            if (mysql_stmt_store_result(stmt_) != 0) {
                throw_last_error("store statement result");
            }
            mysql_stmt_free_result(stmt_);
        }
    }

    Result Statement::query(const std::vector<Value> &params)
    {
        validate_params(params);
        BoundParams bound_params(params);
        if (!bound_params.binds.empty() && mysql_stmt_bind_param(stmt_, bound_params.binds.data()) != 0) {
            throw_last_error("bind statement parameters");
        }
        if (mysql_stmt_execute(stmt_) != 0) {
            throw_last_error("query statement");
        }
        if (mysql_stmt_store_result(stmt_) != 0) {
            throw_last_error("store statement result");
        }
        return read_result();
    }

    std::uint64_t Statement::affected_rows() const
    {
        return static_cast<std::uint64_t>(mysql_stmt_affected_rows(stmt_));
    }

    std::uint64_t Statement::last_insert_id() const
    {
        return static_cast<std::uint64_t>(mysql_stmt_insert_id(stmt_));
    }

    unsigned long Statement::parameter_count() const
    {
        return mysql_stmt_param_count(stmt_);
    }

    void Statement::close() noexcept
    {
        if (stmt_) {
            mysql_stmt_close(stmt_);
            stmt_ = nullptr;
        }
    }

    void Statement::validate_params(const std::vector<Value> &params) const
    {
        if (params.size() != parameter_count()) {
            throw MysqlError(0, "HY000", "statement parameter count mismatch");
        }
    }

    void Statement::throw_last_error(std::string_view operation) const
    {
        std::string message(operation);
        message += " failed: ";
        message += stmt_ ? mysql_stmt_error(stmt_) : "statement handle is null";
        throw MysqlError(stmt_ ? mysql_stmt_errno(stmt_) : 0, stmt_ ? mysql_stmt_sqlstate(stmt_) : "HY000",
                         std::move(message));
    }

    Result Statement::read_result()
    {
        MYSQL_RES *metadata = mysql_stmt_result_metadata(stmt_);
        if (!metadata) {
            if (mysql_stmt_field_count(stmt_) == 0) {
                return {};
            }
            throw_last_error("read statement metadata");
        }

        const unsigned int field_count = mysql_num_fields(metadata);
        MYSQL_FIELD *fields = mysql_fetch_fields(metadata);
        std::vector<std::string> columns;
        std::vector<unsigned long> max_lengths;
        std::vector<enum_field_types> field_types;
        std::vector<unsigned int> field_flags;
        columns.reserve(field_count);
        max_lengths.reserve(field_count);
        field_types.reserve(field_count);
        field_flags.reserve(field_count);
        for (unsigned int i = 0; i < field_count; ++i) {
            columns.emplace_back(fields[i].name ? fields[i].name : "");
            max_lengths.emplace_back(fields[i].max_length);
            field_types.emplace_back(fields[i].type);
            field_flags.emplace_back(fields[i].flags);
        }
        mysql_free_result(metadata);

        std::vector<MYSQL_BIND> binds(field_count);
        std::vector<std::vector<char>> buffers(field_count);
        std::vector<unsigned long> lengths(field_count, 0);
        std::vector<std::unique_ptr<bool>> is_null(field_count);

        for (unsigned int i = 0; i < field_count; ++i) {
            buffers[i].resize(max_lengths[i] + 1);
            std::memset(&binds[i], 0, sizeof(MYSQL_BIND));
            binds[i].buffer_type = MYSQL_TYPE_STRING;
            binds[i].buffer = buffers[i].data();
            binds[i].buffer_length = static_cast<unsigned long>(buffers[i].size());
            binds[i].length = &lengths[i];
            is_null[i] = std::make_unique<bool>(false);
            binds[i].is_null = is_null[i].get();
        }

        if (mysql_stmt_bind_result(stmt_, binds.data()) != 0) {
            throw_last_error("bind statement result");
        }

        std::vector<Row> rows;
        while (true) {
            int status = mysql_stmt_fetch(stmt_);
            if (status == MYSQL_NO_DATA) {
                break;
            }
            if (status == 1) {
                throw_last_error("fetch statement result");
            }

            std::vector<Value> values;
            values.reserve(field_count);
            for (unsigned int i = 0; i < field_count; ++i) {
                if (*is_null[i]) {
                    values.emplace_back(std::monostate{});
                } else {
                    values.emplace_back(decode_mysql_text(buffers[i].data(), lengths[i], field_types[i], field_flags[i]));
                }
            }
            rows.emplace_back(std::move(values));
        }

        mysql_stmt_free_result(stmt_);
        return Result(std::move(columns), std::move(rows));
    }
}
