#ifndef YUAN_MYSQL_MYSQL_DB_PROXY_H
#define YUAN_MYSQL_MYSQL_DB_PROXY_H

#include "yuan/mysql/mysql_client.h"
#include "yuan/mysql/mysql_orm.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yuan::mysql::proxy
{
    struct DbQueryEvent
    {
        std::string worker_name;
        std::string sql;
        std::chrono::milliseconds elapsed{0};
        bool success = false;
        bool slow = false;
        std::string error;
    };

    using DbQueryEventCallback = std::function<void(const DbQueryEvent &)>;

    struct DbWorkerOptions
    {
        std::string name;
        ConnectionOptions connection;
        std::chrono::milliseconds slow_query_threshold{600};
        bool connect_on_start = true;
        bool reconnect_before_request = true;
        DbQueryEventCallback on_query;
    };

    struct ExecuteResult
    {
        std::uint64_t affected_rows = 0;
        std::uint64_t last_insert_id = 0;
    };

    struct DbWorkerStats
    {
        std::uint64_t total_requests = 0;
        std::uint64_t successful_requests = 0;
        std::uint64_t failed_requests = 0;
        std::uint64_t slow_requests = 0;
        std::uint64_t total_elapsed_ms = 0;
        std::uint64_t max_elapsed_ms = 0;
        std::uint64_t min_elapsed_ms = 0;
        std::size_t max_pending_jobs = 0;
        std::string last_error;
        std::string last_slow_sql;
    };

    struct DbShardRouterOptions
    {
        std::vector<DbWorkerOptions> workers;
    };

    struct DbShardRoute
    {
        std::size_t worker_index = 0;
    };

    struct DbShardRouterStats
    {
        std::vector<DbWorkerStats> workers;
        DbWorkerStats total;
    };

    class DbWorker
    {
    public:
        explicit DbWorker(DbWorkerOptions options);
        ~DbWorker();

        DbWorker(const DbWorker &) = delete;
        DbWorker &operator=(const DbWorker &) = delete;
        DbWorker(DbWorker &&) noexcept;
        DbWorker &operator=(DbWorker &&) noexcept;

        void start();
        void stop() noexcept;

        bool is_running() const noexcept;
        bool is_connected() const noexcept;
        std::size_t pending_jobs() const;

        std::future<void> connect();
        std::future<void> close();
        std::future<ExecuteResult> execute(std::string sql, std::vector<Value> params = {});
        std::future<Result> query(std::string sql, std::vector<Value> params = {});
        std::future<void> transaction(std::function<void(Connection &)> operation);

        DbWorkerStats stats() const;
        void reset_stats();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };

    class DbShardRouter
    {
    public:
        explicit DbShardRouter(DbShardRouterOptions options);
        ~DbShardRouter();

        DbShardRouter(const DbShardRouter &) = delete;
        DbShardRouter &operator=(const DbShardRouter &) = delete;
        DbShardRouter(DbShardRouter &&) noexcept;
        DbShardRouter &operator=(DbShardRouter &&) noexcept;

        std::size_t worker_count() const noexcept;
        DbShardRoute route(std::uint64_t shard_key) const;

        DbWorker &worker(std::uint64_t shard_key);
        const DbWorker &worker(std::uint64_t shard_key) const;

        std::future<ExecuteResult> execute(std::uint64_t shard_key, std::string sql, std::vector<Value> params = {});
        std::future<Result> query(std::uint64_t shard_key, std::string sql, std::vector<Value> params = {});
        std::future<void> transaction(std::uint64_t shard_key, std::function<void(Connection &)> operation);

        DbShardRouterStats stats() const;
        void stop() noexcept;

    private:
        DbShardRouterOptions options_;
        std::vector<DbWorker> workers_;
    };

    struct RecordRepositoryOptions
    {
        const orm::RecordSchemaMapper *mapper = nullptr;
        DbShardRouter *router = nullptr;
    };

    class RecordRepository
    {
    public:
        explicit RecordRepository(RecordRepositoryOptions options);

        std::future<ExecuteResult> insert(std::uint64_t shard_key, const orm::Record &record) const;
        std::future<ExecuteResult> update_by_primary_key(std::uint64_t shard_key, const orm::Record &record) const;
        std::future<ExecuteResult> delete_by_primary_key(std::uint64_t shard_key, const orm::Record &record) const;
        std::future<Result> select_by_primary_key(std::uint64_t shard_key,
                                                  const orm::Record &record,
                                                  std::vector<std::string> columns = {},
                                                  std::optional<std::uint64_t> limit = {}) const;
        std::future<Result> select_count_by_primary_key(std::uint64_t shard_key, const orm::Record &record) const;
        std::future<ExecuteResult> upsert(std::uint64_t shard_key, const orm::Record &record) const;

    private:
        orm::SqlCommand command_for_shard(std::uint64_t shard_key, const orm::Record &record,
                                          const std::function<orm::SqlCommand(const orm::RecordSqlGenerator &)> &build) const;

        const orm::RecordSchemaMapper *mapper_ = nullptr;
        DbShardRouter *router_ = nullptr;
    };
}

#endif // YUAN_MYSQL_MYSQL_DB_PROXY_H
