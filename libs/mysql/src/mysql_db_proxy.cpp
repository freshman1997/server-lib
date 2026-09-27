#include "yuan/mysql/mysql_db_proxy.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace yuan::mysql::proxy
{
    namespace
    {
        bool is_connection_loss(const std::exception &error)
        {
            const auto *mysql_error = dynamic_cast<const MysqlError *>(&error);
            if (!mysql_error) {
                return false;
            }
            return mysql_error->code() == 2006 || mysql_error->code() == 2013;
        }
    }

    struct DbWorker::State
    {
        explicit State(DbWorkerOptions worker_options)
            : options(std::move(worker_options))
        {
        }

        ~State()
        {
            stop();
        }

        void start()
        {
            std::lock_guard lock(mutex);
            if (running) {
                return;
            }
            stopping = false;
            running = true;
            worker = std::thread([this] { run(); });
        }

        void stop() noexcept
        {
            {
                std::lock_guard lock(mutex);
                if (!running && !worker.joinable()) {
                    return;
                }
                stopping = true;
            }
            condition.notify_all();
            if (worker.joinable()) {
                worker.join();
            }
            std::lock_guard lock(mutex);
            running = false;
        }

        template <typename T, typename Fn>
        std::future<T> submit(std::string sql, bool track_query, Fn &&fn)
        {
            auto promise = std::make_shared<std::promise<T>>();
            auto future = promise->get_future();
            {
                std::lock_guard lock(mutex);
                if (stopping) {
                    throw std::runtime_error("db worker is stopping");
                }
                jobs.emplace([this, promise, sql = std::move(sql), track_query, fn = std::forward<Fn>(fn)]() mutable {
                    execute_job<T>(std::move(sql), track_query, *promise, fn);
                });
                stats_data.max_pending_jobs = std::max(stats_data.max_pending_jobs, jobs.size());
            }
            condition.notify_one();
            return future;
        }

        void run()
        {
            ThreadGuard mysql_thread;
            connection.emplace(options.connection);
            if (options.connect_on_start) {
                try {
                    connection->connect();
                } catch (const std::exception &error) {
                    std::lock_guard lock(mutex);
                    stats_data.last_error = error.what();
                }
            }

            while (true) {
                std::function<void()> job;
                {
                    std::unique_lock lock(mutex);
                    condition.wait(lock, [this] { return stopping || !jobs.empty(); });
                    if (stopping && jobs.empty()) {
                        break;
                    }
                    job = std::move(jobs.front());
                    jobs.pop();
                }
                job();
            }

            if (connection) {
                connection->close();
                connection.reset();
            }
        }

        template <typename T, typename Fn>
        void execute_job(std::string sql, bool track_query, std::promise<T> &promise, Fn &fn)
        {
            const auto start_time = std::chrono::steady_clock::now();
            bool success = false;
            try {
                auto &db = active_connection();
                if (options.reconnect_before_request) {
                    if (db.is_connected()) {
                        try {
                            db.ping();
                        } catch (const std::exception &error) {
                            if (!is_connection_loss(error)) {
                                throw;
                            }
                            close_connection();
                        }
                    }
                    if (!db.is_connected()) {
                        db.connect();
                    }
                }

                if constexpr (std::is_void_v<T>) {
                    fn(db);
                    promise.set_value();
                } else {
                    promise.set_value(fn(db));
                }
                success = true;
            } catch (const std::exception &error) {
                if (is_connection_loss(error)) {
                    close_connection();
                }
                record_error(error.what());
                promise.set_exception(std::current_exception());
            } catch (...) {
                record_error("unknown db worker error");
                promise.set_exception(std::current_exception());
            }

            if (track_query) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time);
                record_query(std::move(sql), elapsed, success);
            }
        }

        void record_query(std::string sql, std::chrono::milliseconds elapsed, bool success)
        {
            if (sql.empty()) {
                return;
            }
            const auto elapsed_ms = static_cast<std::uint64_t>(elapsed.count());
            const bool slow = elapsed >= options.slow_query_threshold;
            std::string error;
            DbQueryEventCallback callback;
            {
                std::lock_guard lock(mutex);
                stats_data.total_requests++;
                if (success) {
                    stats_data.successful_requests++;
                } else {
                    stats_data.failed_requests++;
                }
                stats_data.total_elapsed_ms += elapsed_ms;
                stats_data.max_elapsed_ms = std::max(stats_data.max_elapsed_ms, elapsed_ms);
                if (stats_data.min_elapsed_ms == 0 || elapsed_ms < stats_data.min_elapsed_ms) {
                    stats_data.min_elapsed_ms = elapsed_ms;
                }
                if (slow) {
                    stats_data.slow_requests++;
                    stats_data.last_slow_sql = sql;
                }
                error = stats_data.last_error;
                callback = options.on_query;
            }

            if (callback) {
                callback(DbQueryEvent{options.name, std::move(sql), elapsed, success, slow, success ? std::string{} : error});
            }
        }

        void record_error(std::string error)
        {
            std::lock_guard lock(mutex);
            stats_data.last_error = std::move(error);
        }

        Connection &active_connection()
        {
            if (!connection) {
                connection.emplace(options.connection);
            }
            return *connection;
        }

        void close_connection() noexcept
        {
            if (!connection) {
                return;
            }
            try {
                connection->close();
            } catch (...) {
            }
        }

        DbWorkerOptions options;
        std::optional<Connection> connection;
        mutable std::mutex mutex;
        std::condition_variable condition;
        std::queue<std::function<void()>> jobs;
        std::thread worker;
        bool running = false;
        bool stopping = false;
        DbWorkerStats stats_data;
    };

    DbWorker::DbWorker(DbWorkerOptions options) : state_(std::make_unique<State>(std::move(options)))
    {
        start();
    }

    DbWorker::~DbWorker() = default;

    DbWorker::DbWorker(DbWorker &&) noexcept = default;

    DbWorker &DbWorker::operator=(DbWorker &&) noexcept = default;

    void DbWorker::start()
    {
        state_->start();
    }

    void DbWorker::stop() noexcept
    {
        if (state_) {
            state_->stop();
        }
    }

    bool DbWorker::is_running() const noexcept
    {
        std::lock_guard lock(state_->mutex);
        return state_->running;
    }

    bool DbWorker::is_connected() const noexcept
    {
        return state_->connection && state_->connection->is_connected();
    }

    std::size_t DbWorker::pending_jobs() const
    {
        std::lock_guard lock(state_->mutex);
        return state_->jobs.size();
    }

    std::future<void> DbWorker::connect()
    {
        return state_->submit<void>({}, false, [](Connection &connection) {
            if (!connection.is_connected()) {
                connection.connect();
            }
        });
    }

    std::future<void> DbWorker::close()
    {
        return state_->submit<void>({}, false, [](Connection &connection) {
            connection.close();
        });
    }

    std::future<ExecuteResult> DbWorker::execute(std::string sql, std::vector<Value> params)
    {
        std::string query_sql = sql;
        return state_->submit<ExecuteResult>(std::move(query_sql), true,
            [sql = std::move(sql), params = std::move(params)](Connection &connection) mutable {
                if (params.empty()) {
                    connection.execute(sql);
                } else {
                    connection.execute(sql, params);
                }
                std::uint64_t affected_rows = 0;
                try {
                    affected_rows = connection.affected_rows();
                } catch (const MysqlError &error) {
                    if (error.code() != 0) {
                        throw;
                    }
                }
                return ExecuteResult{affected_rows, connection.last_insert_id()};
            });
    }

    std::future<Result> DbWorker::query(std::string sql, std::vector<Value> params)
    {
        std::string query_sql = sql;
        return state_->submit<Result>(std::move(query_sql), true,
            [sql = std::move(sql), params = std::move(params)](Connection &connection) mutable {
                return params.empty() ? connection.query(sql) : connection.query(sql, params);
            });
    }

    std::future<void> DbWorker::transaction(std::function<void(Connection &)> operation)
    {
        return state_->submit<void>("transaction", true, [operation = std::move(operation)](Connection &connection) mutable {
            connection.begin_transaction();
            try {
                operation(connection);
                connection.commit();
            } catch (...) {
                try {
                    connection.rollback();
                } catch (...) {
                    connection.close();
                }
                throw;
            }
        });
    }

    DbWorkerStats DbWorker::stats() const
    {
        std::lock_guard lock(state_->mutex);
        return state_->stats_data;
    }

    void DbWorker::reset_stats()
    {
        std::lock_guard lock(state_->mutex);
        state_->stats_data = {};
    }

    DbShardRouter::DbShardRouter(DbShardRouterOptions options) : options_(std::move(options))
    {
        if (options_.workers.empty()) {
            throw std::invalid_argument("db shard router requires at least one worker");
        }
        workers_.reserve(options_.workers.size());
        for (const DbWorkerOptions &worker_options : options_.workers) {
            workers_.emplace_back(worker_options);
        }
    }

    DbShardRouter::~DbShardRouter() = default;

    DbShardRouter::DbShardRouter(DbShardRouter &&) noexcept = default;

    DbShardRouter &DbShardRouter::operator=(DbShardRouter &&) noexcept = default;

    std::size_t DbShardRouter::worker_count() const noexcept
    {
        return workers_.size();
    }

    DbShardRoute DbShardRouter::route(std::uint64_t shard_key) const
    {
        if (workers_.empty()) {
            throw std::runtime_error("db shard router has no workers");
        }

        DbShardRoute route;
        route.worker_index = static_cast<std::size_t>(shard_key % workers_.size());
        return route;
    }

    DbWorker &DbShardRouter::worker(std::uint64_t shard_key)
    {
        return workers_.at(route(shard_key).worker_index);
    }

    const DbWorker &DbShardRouter::worker(std::uint64_t shard_key) const
    {
        return workers_.at(route(shard_key).worker_index);
    }

    std::future<ExecuteResult> DbShardRouter::execute(std::uint64_t shard_key, std::string sql, std::vector<Value> params)
    {
        return worker(shard_key).execute(std::move(sql), std::move(params));
    }

    std::future<Result> DbShardRouter::query(std::uint64_t shard_key, std::string sql, std::vector<Value> params)
    {
        return worker(shard_key).query(std::move(sql), std::move(params));
    }

    std::future<void> DbShardRouter::transaction(std::uint64_t shard_key, std::function<void(Connection &)> operation)
    {
        return worker(shard_key).transaction(std::move(operation));
    }

    DbShardRouterStats DbShardRouter::stats() const
    {
        DbShardRouterStats stats;
        stats.workers.reserve(workers_.size());
        for (const DbWorker &worker : workers_) {
            DbWorkerStats worker_stats = worker.stats();
            stats.workers.push_back(worker_stats);
            stats.total.total_requests += worker_stats.total_requests;
            stats.total.successful_requests += worker_stats.successful_requests;
            stats.total.failed_requests += worker_stats.failed_requests;
            stats.total.slow_requests += worker_stats.slow_requests;
            stats.total.total_elapsed_ms += worker_stats.total_elapsed_ms;
            stats.total.max_elapsed_ms = std::max(stats.total.max_elapsed_ms, worker_stats.max_elapsed_ms);
            if (worker_stats.min_elapsed_ms != 0 &&
                (stats.total.min_elapsed_ms == 0 || worker_stats.min_elapsed_ms < stats.total.min_elapsed_ms)) {
                stats.total.min_elapsed_ms = worker_stats.min_elapsed_ms;
            }
            stats.total.max_pending_jobs = std::max(stats.total.max_pending_jobs, worker_stats.max_pending_jobs);
            if (!worker_stats.last_error.empty()) {
                stats.total.last_error = worker_stats.last_error;
            }
            if (!worker_stats.last_slow_sql.empty()) {
                stats.total.last_slow_sql = worker_stats.last_slow_sql;
            }
        }
        return stats;
    }

    void DbShardRouter::stop() noexcept
    {
        for (DbWorker &worker : workers_) {
            worker.stop();
        }
    }

    RecordRepository::RecordRepository(RecordRepositoryOptions options)
        : mapper_(options.mapper), router_(options.router)
    {
        if (!mapper_) {
            throw std::invalid_argument("record repository requires a schema mapper");
        }
        if (!router_) {
            throw std::invalid_argument("record repository requires a shard router");
        }
    }

    std::future<ExecuteResult> RecordRepository::insert(std::uint64_t shard_key, const orm::Record &record) const
    {
        orm::SqlCommand command = command_for_shard(shard_key, record, [&](const orm::RecordSqlGenerator &generator) {
            return generator.insert(record);
        });
        return router_->execute(shard_key, std::move(command.sql), std::move(command.params));
    }

    std::future<ExecuteResult> RecordRepository::update_by_primary_key(std::uint64_t shard_key, const orm::Record &record) const
    {
        orm::SqlCommand command = command_for_shard(shard_key, record, [&](const orm::RecordSqlGenerator &generator) {
            return generator.update_by_primary_key(record);
        });
        return router_->execute(shard_key, std::move(command.sql), std::move(command.params));
    }

    std::future<ExecuteResult> RecordRepository::delete_by_primary_key(std::uint64_t shard_key, const orm::Record &record) const
    {
        orm::SqlCommand command = command_for_shard(shard_key, record, [&](const orm::RecordSqlGenerator &generator) {
            return generator.delete_by_primary_key(record);
        });
        return router_->execute(shard_key, std::move(command.sql), std::move(command.params));
    }

    std::future<Result> RecordRepository::select_by_primary_key(std::uint64_t shard_key,
                                                                const orm::Record &record,
                                                                std::vector<std::string> columns,
                                                                std::optional<std::uint64_t> limit) const
    {
        orm::SqlCommand command = command_for_shard(shard_key, record, [&](const orm::RecordSqlGenerator &generator) {
            return generator.select_by_primary_key(record, std::move(columns), limit);
        });
        return router_->query(shard_key, std::move(command.sql), std::move(command.params));
    }

    std::future<Result> RecordRepository::select_count_by_primary_key(std::uint64_t shard_key, const orm::Record &record) const
    {
        orm::SqlCommand command = command_for_shard(shard_key, record, [&](const orm::RecordSqlGenerator &generator) {
            return generator.select_count_by_primary_key(record);
        });
        return router_->query(shard_key, std::move(command.sql), std::move(command.params));
    }

    std::future<ExecuteResult> RecordRepository::upsert(std::uint64_t shard_key, const orm::Record &record) const
    {
        orm::SqlCommand command = command_for_shard(shard_key, record, [&](const orm::RecordSqlGenerator &generator) {
            return generator.upsert(record);
        });
        return router_->execute(shard_key, std::move(command.sql), std::move(command.params));
    }

    orm::SqlCommand RecordRepository::command_for_shard(
        std::uint64_t shard_key, const orm::Record &record,
        const std::function<orm::SqlCommand(const orm::RecordSqlGenerator &)> &build) const
    {
        const orm::TableSchema schema = mapper_->table_schema(record);
        class MappedSchema final : public orm::RecordSchemaMapper
        {
        public:
            MappedSchema(const orm::RecordSchemaMapper &mapper, orm::TableSchema schema)
                : mapper_(mapper), schema_(std::move(schema))
            {
            }

            orm::TableSchema table_schema(const orm::Record &) const override
            {
                return schema_;
            }

            std::vector<orm::FieldValue> values(const orm::Record &record) const override
            {
                return mapper_.values(record);
            }

            std::vector<orm::FieldValue> primary_key_values(const orm::Record &record) const override
            {
                return mapper_.primary_key_values(record);
            }

        private:
            const orm::RecordSchemaMapper &mapper_;
            orm::TableSchema schema_;
        };

        const MappedSchema mapped_schema(*mapper_, schema);
        const orm::RecordSqlGenerator generator(mapped_schema);
        return build(generator);
    }
}
