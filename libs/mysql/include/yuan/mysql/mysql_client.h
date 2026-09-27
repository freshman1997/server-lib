#ifndef YUAN_MYSQL_MYSQL_CLIENT_H
#define YUAN_MYSQL_MYSQL_CLIENT_H

#include <cstdint>
#include <cstddef>
#include <optional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "coroutine/runtime_view.h"
#include "coroutine/task.h"

struct MYSQL;
struct MYSQL_RES;
struct MYSQL_STMT;

namespace yuan::mysql
{
    class Statement;
    class AsyncConnection;
    class AsyncTransaction;
    struct AsyncConnectionState;

    class MysqlError : public std::runtime_error
    {
    public:
        MysqlError(unsigned int code, std::string sql_state, std::string message);

        unsigned int code() const noexcept;
        const std::string &sql_state() const noexcept;

    private:
        unsigned int code_ = 0;
        std::string sql_state_;
    };

    enum class SslMode
    {
        Disabled,
        Preferred,
        Required,
        VerifyCa,
        VerifyIdentity,
    };

    struct ConnectionOptions
    {
        std::string host = "127.0.0.1";
        std::string user;
        std::string password;
        std::string database;
        unsigned int port = 3306;
        std::string unix_socket;
        unsigned long client_flags = 0;
        std::optional<unsigned int> connect_timeout_seconds;
        std::optional<unsigned int> read_timeout_seconds;
        std::optional<unsigned int> write_timeout_seconds;
        std::optional<std::string> charset;
        std::optional<SslMode> ssl_mode;
    };

    using Value = std::variant<std::monostate, std::int64_t, std::uint64_t, double, std::string>;

    Value null_value();
    Value string_value(std::string value);

    class ThreadGuard
    {
    public:
        ThreadGuard();
        ~ThreadGuard();

        ThreadGuard(const ThreadGuard &) = delete;
        ThreadGuard &operator=(const ThreadGuard &) = delete;
    };

    class Row
    {
    public:
        Row() = default;
        explicit Row(std::vector<Value> values);

        std::size_t size() const noexcept;
        bool is_null(std::size_t index) const;
        const Value &operator[](std::size_t index) const;
        const std::vector<Value> &values() const noexcept;
        const Value *data() const noexcept { return values_.data(); }

    private:
        std::vector<Value> values_;
    };

    class Result
    {
    public:
        Result() = default;
        Result(std::vector<std::string> columns, std::vector<Row> rows);

        bool empty() const noexcept;
        std::size_t row_count() const noexcept;
        std::size_t column_count() const noexcept;
        const std::vector<std::string> &columns() const noexcept;
        const std::vector<Row> &rows() const noexcept;

    private:
        std::vector<std::string> columns_;
        std::vector<Row> rows_;
    };

    class Connection
    {
    public:
        Connection();
        explicit Connection(ConnectionOptions options);
        ~Connection();

        Connection(const Connection &) = delete;
        Connection &operator=(const Connection &) = delete;
        Connection(Connection &&other) noexcept;
        Connection &operator=(Connection &&other) noexcept;

        void connect();
        void connect(ConnectionOptions options);
        void close() noexcept;

        bool is_connected() const noexcept;
        void ping();
        void select_database(std::string_view database);

        void execute(std::string_view sql);
        void execute(std::string_view sql, const std::vector<Value> &params);
        Result query(std::string_view sql);
        Result query(std::string_view sql, const std::vector<Value> &params);

        Statement prepare(std::string_view sql);

        void begin_transaction();
        void commit();
        void rollback();

        std::uint64_t affected_rows() const;
        std::uint64_t last_insert_id() const;
        std::string escape(std::string_view value) const;

    private:
        void apply_options();
        void throw_last_error(std::string_view operation) const;
        static Result read_result(MYSQL_RES *result);

        MYSQL *mysql_ = nullptr;
        bool connected_ = false;
        ConnectionOptions options_;
        std::optional<std::uint64_t> last_statement_affected_rows_;
        std::optional<std::uint64_t> last_statement_insert_id_;
    };

    class Statement
    {
    public:
        Statement() = default;
        Statement(MYSQL *mysql, std::string sql);
        ~Statement();

        Statement(const Statement &) = delete;
        Statement &operator=(const Statement &) = delete;
        Statement(Statement &&other) noexcept;
        Statement &operator=(Statement &&other) noexcept;

        void execute(const std::vector<Value> &params = {});
        Result query(const std::vector<Value> &params = {});

        std::uint64_t affected_rows() const;
        std::uint64_t last_insert_id() const;
        unsigned long parameter_count() const;

    private:
        void close() noexcept;
        void validate_params(const std::vector<Value> &params) const;
        void throw_last_error(std::string_view operation) const;
        Result read_result();

        MYSQL_STMT *stmt_ = nullptr;
    };

    class AsyncConnection
    {
    public:
        AsyncConnection(coroutine::RuntimeView runtime, ConnectionOptions options);
        ~AsyncConnection();

        AsyncConnection(const AsyncConnection &) = delete;
        AsyncConnection &operator=(const AsyncConnection &) = delete;
        AsyncConnection(AsyncConnection &&other) noexcept;
        AsyncConnection &operator=(AsyncConnection &&other) noexcept;

        coroutine::Task<void> connect_async();
        coroutine::Task<void> close_async();
        coroutine::Task<void> execute_async(std::string sql);
        coroutine::Task<void> execute_async(std::string sql, std::vector<Value> params);
        coroutine::Task<Result> query_async(std::string sql);
        coroutine::Task<Result> query_async(std::string sql, std::vector<Value> params);
        coroutine::Task<void> begin_transaction_async();
        coroutine::Task<AsyncTransaction> transaction_async();
        coroutine::Task<void> commit_async();
        coroutine::Task<void> rollback_async();

        bool is_connected() const noexcept;

    private:
        void close_now() noexcept;

        std::shared_ptr<AsyncConnectionState> state_;
        bool connected_ = false;
    };

    class AsyncTransaction
    {
    public:
        AsyncTransaction() = default;
        explicit AsyncTransaction(std::shared_ptr<AsyncConnectionState> state);
        ~AsyncTransaction();

        AsyncTransaction(const AsyncTransaction &) = delete;
        AsyncTransaction &operator=(const AsyncTransaction &) = delete;
        AsyncTransaction(AsyncTransaction &&other) noexcept;
        AsyncTransaction &operator=(AsyncTransaction &&other) noexcept;

        coroutine::Task<void> execute_async(std::string sql);
        coroutine::Task<void> execute_async(std::string sql, std::vector<Value> params);
        coroutine::Task<Result> query_async(std::string sql);
        coroutine::Task<Result> query_async(std::string sql, std::vector<Value> params);
        coroutine::Task<void> commit_async();
        coroutine::Task<void> rollback_async();

    private:
        void rollback_on_destroy() noexcept;

        std::shared_ptr<AsyncConnectionState> state_;
        bool completed_ = true;
    };

    using AsyncMysqlExecutor = AsyncConnection;
}

#endif // YUAN_MYSQL_MYSQL_CLIENT_H
