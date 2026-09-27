#include "yuan/mysql/mysql_db_proxy.h"
#include "yuan/mysql/mysql_orm.h"

#include "mysql_smoke_options.h"

#include <exception>
#include <future>
#include <iostream>
#include <mutex>

namespace
{
    class SampleRecordMapper final : public yuan::mysql::orm::RecordSchemaMapper
    {
    public:
        yuan::mysql::orm::TableSchema table_schema(const yuan::mysql::orm::Record &record) const override
        {
            if (record.schema_name != "sample_record") {
                throw std::invalid_argument("unexpected sample record schema");
            }
            return {
                "sample_record",
                {
                    {"id", true, true},
                    {"bucket_id", true, false},
                    {"label", false, false},
                    {"level", false, false},
                },
            };
        }

        std::vector<yuan::mysql::orm::FieldValue> values(const yuan::mysql::orm::Record &record) const override
        {
            std::vector<yuan::mysql::orm::FieldValue> values;
            values.reserve(record.fields.size());
            for (const yuan::mysql::orm::RecordField &field : record.fields) {
                values.push_back({field.name, field.value});
            }
            return values;
        }

        std::vector<yuan::mysql::orm::FieldValue> primary_key_values(const yuan::mysql::orm::Record &record) const override
        {
            std::vector<yuan::mysql::orm::FieldValue> values;
            for (const yuan::mysql::orm::RecordField &field : record.fields) {
                if (field.name == "id" || field.name == "bucket_id") {
                    values.push_back({field.name, field.value});
                }
            }
            return values;
        }
    };

    bool check_sql(const yuan::mysql::orm::SqlCommand &command, const std::string &sql, std::size_t param_count)
    {
        return command.sql == sql && command.params.size() == param_count;
    }
}

int main(int argc, char **argv)
{
    try {
        yuan::mysql::orm::SqlGenerator generator({
            "sample_record",
            {
                {"id", true, true},
                {"bucket_id", true, false},
                {"label", false, false},
                {"level", false, false},
            },
        });

        if (!check_sql(generator.insert({yuan::mysql::orm::field("label", yuan::mysql::string_value("a")),
                                         yuan::mysql::orm::field("level", std::uint64_t{1})}),
                       "INSERT INTO `sample_record` (`label`, `level`) VALUES (?, ?)", 2) ||
            !check_sql(generator.update_by_primary_key({yuan::mysql::orm::field("level", std::uint64_t{2})},
                                                       {yuan::mysql::orm::field("id", std::uint64_t{1}),
                                                        yuan::mysql::orm::field("bucket_id", std::uint64_t{10})}),
                       "UPDATE `sample_record` SET `level` = ? WHERE `id` = ? AND `bucket_id` = ?", 3) ||
            !check_sql(generator.delete_by_primary_key({yuan::mysql::orm::field("id", std::uint64_t{1}),
                                                        yuan::mysql::orm::field("bucket_id", std::uint64_t{10})}),
                       "DELETE FROM `sample_record` WHERE `id` = ? AND `bucket_id` = ?", 2) ||
            !check_sql(generator.select_by_primary_key({yuan::mysql::orm::field("id", std::uint64_t{1}),
                                                        yuan::mysql::orm::field("bucket_id", std::uint64_t{10})},
                                                       {"label", "level"}, 1),
                       "SELECT `label`, `level` FROM `sample_record` WHERE `id` = ? AND `bucket_id` = ? LIMIT 1", 2) ||
            !check_sql(generator.select_count_by_primary_key({yuan::mysql::orm::field("id", std::uint64_t{1}),
                                                              yuan::mysql::orm::field("bucket_id", std::uint64_t{10})}),
                       "SELECT COUNT(*) FROM `sample_record` WHERE `id` = ? AND `bucket_id` = ?", 2) ||
            !check_sql(generator.upsert({yuan::mysql::orm::field("id", std::uint64_t{1}),
                                         yuan::mysql::orm::field("bucket_id", std::uint64_t{10}),
                                         yuan::mysql::orm::field("level", std::uint64_t{3})}),
                       "INSERT INTO `sample_record` (`id`, `bucket_id`, `level`) VALUES (?, ?, ?) ON DUPLICATE KEY UPDATE `level` = VALUES(`level`)", 3)) {
            std::cerr << "unexpected schema sql generator output\n";
            return 1;
        }

        const SampleRecordMapper mapper;
        const yuan::mysql::orm::RecordSqlGenerator record_generator(mapper);
        const yuan::mysql::orm::Record record{
            "sample_record",
            1,
            {
                {"id", std::uint64_t{1}},
                {"bucket_id", std::uint64_t{10}},
                {"label", yuan::mysql::string_value("a")},
                {"level", std::uint64_t{3}},
            },
        };
        if (!check_sql(record_generator.update_by_primary_key(record),
                       "UPDATE `sample_record` SET `label` = ?, `level` = ? WHERE `id` = ? AND `bucket_id` = ?", 4) ||
            !check_sql(record_generator.select_count_by_primary_key(record),
                       "SELECT COUNT(*) FROM `sample_record` WHERE `id` = ? AND `bucket_id` = ?", 2)) {
            std::cerr << "unexpected record sql generator output\n";
            return 1;
        }

        yuan::mysql::proxy::DbWorkerOptions worker_options;
        std::mutex event_mutex;
        std::uint64_t event_count = 0;
        std::uint64_t slow_event_count = 0;
        std::uint64_t failed_event_count = 0;
        std::uint64_t invalid_event_count = 0;
        std::string last_event_error;
        std::string last_event_worker;
        std::string last_event_sql;
        worker_options.name = "smoke-worker";
        worker_options.connection = yuan::mysql::test::parse_connection_options(argc, argv);
        worker_options.slow_query_threshold = std::chrono::milliseconds(0);
        worker_options.connect_on_start = false;
        worker_options.on_query = [&](const yuan::mysql::proxy::DbQueryEvent &event) {
            std::lock_guard lock(event_mutex);
            event_count++;
            if (event.slow) {
                slow_event_count++;
            }
            if (!event.success) {
                failed_event_count++;
                last_event_error = event.error;
            }
            if (event.worker_name.empty() || event.sql.empty()) {
                invalid_event_count++;
                return;
            }
            last_event_worker = event.worker_name;
            last_event_sql = event.sql;
        };

        yuan::mysql::proxy::DbWorker worker(worker_options);
        worker.connect().get();

        const auto execute_result = worker.execute(
            "SET @yuan_mysql_proxy_smoke = ?", {yuan::mysql::string_value("db-proxy")}).get();
        if (execute_result.affected_rows != 0) {
            std::cerr << "unexpected affected rows for session variable SET\n";
            return 1;
        }

        const auto result = worker.query("SELECT @yuan_mysql_proxy_smoke").get();
        if (result.row_count() != 1 || result.column_count() != 1 || !result.rows()[0][0] ||
            *result.rows()[0][0] != "db-proxy") {
            std::cerr << "unexpected db proxy smoke query result\n";
            return 1;
        }

        worker.transaction([](yuan::mysql::Connection &connection) {
            connection.execute("SET @yuan_mysql_proxy_tx = ?", {yuan::mysql::string_value("committed")});
        }).get();

        const auto tx_result = worker.query("SELECT @yuan_mysql_proxy_tx").get();
        if (tx_result.row_count() != 1 || tx_result.column_count() != 1 || !tx_result.rows()[0][0] ||
            *tx_result.rows()[0][0] != "committed") {
            std::cerr << "unexpected db proxy transaction result\n";
            return 1;
        }

        try {
            worker.query("SELECT * FROM yuan_mysql_missing_table_for_smoke").get();
            std::cerr << "expected db proxy failed query\n";
            return 1;
        } catch (const std::exception &) {
        }

        const auto stats = worker.stats();
        if (stats.total_requests < 5 || stats.successful_requests + stats.failed_requests != stats.total_requests ||
            stats.failed_requests != 1 || stats.last_error.empty() || stats.slow_requests == 0) {
            std::cerr << "unexpected db proxy stats: total=" << stats.total_requests
                      << " success=" << stats.successful_requests << " failed=" << stats.failed_requests
                      << " slow=" << stats.slow_requests << "\n";
            return 1;
        }
        {
            std::lock_guard lock(event_mutex);
            if (event_count != stats.total_requests || slow_event_count != stats.slow_requests ||
                failed_event_count != 1 || invalid_event_count != 0 || last_event_error.empty() ||
                last_event_worker != "smoke-worker" || last_event_sql.empty()) {
                std::cerr << "unexpected db query event stats\n";
                return 1;
            }
        }

        worker.reset_stats();
        const auto reset_stats = worker.stats();
        if (reset_stats.total_requests != 0 || reset_stats.successful_requests != 0 ||
            reset_stats.failed_requests != 0 || reset_stats.slow_requests != 0 ||
            !reset_stats.last_error.empty() || !reset_stats.last_slow_sql.empty()) {
            std::cerr << "unexpected db proxy reset stats result\n";
            return 1;
        }

        worker.query("SELECT 1").get();
        const auto stats_after_reset = worker.stats();
        if (stats_after_reset.total_requests != 1 || stats_after_reset.successful_requests != 1 ||
            stats_after_reset.failed_requests != 0) {
            std::cerr << "unexpected db proxy stats after reset\n";
            return 1;
        }

        yuan::mysql::proxy::DbShardRouterOptions router_options;
        router_options.workers.push_back(worker_options);
        router_options.workers.push_back(worker_options);

        yuan::mysql::proxy::DbShardRouter router(std::move(router_options));
        const auto route = router.route(9);
        if (route.worker_index != 1) {
            std::cerr << "unexpected db shard route\n";
            return 1;
        }

        router.execute(0, "SET @yuan_mysql_router_smoke = ?", {yuan::mysql::string_value("router-0")}).get();
        router.execute(1, "SET @yuan_mysql_router_smoke = ?", {yuan::mysql::string_value("router-1")}).get();

        const auto router_result0 = router.query(0, "SELECT @yuan_mysql_router_smoke").get();
        const auto router_result1 = router.query(1, "SELECT @yuan_mysql_router_smoke").get();
        if (!router_result0.rows()[0][0] || *router_result0.rows()[0][0] != "router-0" ||
            !router_result1.rows()[0][0] || *router_result1.rows()[0][0] != "router-1") {
            std::cerr << "unexpected db shard router query result\n";
            return 1;
        }

        const auto router_stats = router.stats();
        if (router_stats.workers.size() != 2 || router_stats.total.total_requests < 4 ||
            router_stats.total.successful_requests != router_stats.total.total_requests) {
            std::cerr << "unexpected db shard router stats\n";
            return 1;
        }

        class RepositoryMapper final : public yuan::mysql::orm::RecordSchemaMapper
        {
        public:
            yuan::mysql::orm::TableSchema table_schema(const yuan::mysql::orm::Record &) const override
            {
                return {
                    "yuan_mysql_repository_smoke",
                    {
                        {"id", true, false},
                        {"bucket_id", true, false},
                        {"label", false, false},
                        {"level", false, false},
                    },
                };
            }

            std::vector<yuan::mysql::orm::FieldValue> values(const yuan::mysql::orm::Record &record) const override
            {
                std::vector<yuan::mysql::orm::FieldValue> values;
                for (const yuan::mysql::orm::RecordField &field : record.fields) {
                    values.push_back({field.name, field.value});
                }
                return values;
            }

            std::vector<yuan::mysql::orm::FieldValue> primary_key_values(const yuan::mysql::orm::Record &record) const override
            {
                std::vector<yuan::mysql::orm::FieldValue> values;
                for (const yuan::mysql::orm::RecordField &field : record.fields) {
                    if (field.name == "id" || field.name == "bucket_id") {
                        values.push_back({field.name, field.value});
                    }
                }
                return values;
            }
        } repository_mapper;

        if (!worker_options.connection.database.empty()) {
            router.execute(0, "DROP TEMPORARY TABLE IF EXISTS yuan_mysql_repository_smoke").get();
            router.execute(0,
                "CREATE TEMPORARY TABLE yuan_mysql_repository_smoke ("
                "id BIGINT UNSIGNED NOT NULL, "
                "bucket_id BIGINT UNSIGNED NOT NULL, "
                "label VARCHAR(64) NOT NULL, "
                "level BIGINT UNSIGNED NOT NULL, "
                "PRIMARY KEY(id, bucket_id)) ENGINE=InnoDB").get();

            try {
                router.transaction(0, [](yuan::mysql::Connection &connection) {
                    connection.execute("INSERT INTO yuan_mysql_repository_smoke (id, bucket_id, label, level) VALUES (?, ?, ?, ?)",
                        {std::uint64_t{99}, std::uint64_t{0}, yuan::mysql::string_value("rollback"), std::uint64_t{1}});
                    throw std::runtime_error("force rollback");
                }).get();
                std::cerr << "expected db proxy transaction rollback failure\n";
                return 1;
            } catch (const std::future_error &) {
                throw;
            } catch (const std::exception &) {
            }

            const auto rollback_count = router.query(0,
                "SELECT COUNT(*) FROM yuan_mysql_repository_smoke WHERE id = ? AND bucket_id = ?",
                {std::uint64_t{99}, std::uint64_t{0}}).get();
            if (rollback_count.row_count() != 1 || !rollback_count.rows()[0][0] || *rollback_count.rows()[0][0] != "0") {
                std::cerr << "unexpected db proxy transaction rollback result\n";
                return 1;
            }

            yuan::mysql::proxy::RecordRepository repository({&repository_mapper, &router, false});
            const yuan::mysql::orm::Record repository_record{
                "yuan_mysql_repository_smoke",
                1,
                {
                    {"id", std::uint64_t{7}},
                    {"bucket_id", std::uint64_t{0}},
                    {"label", yuan::mysql::string_value("created")},
                    {"level", std::uint64_t{1}},
                },
            };
            repository.insert(0, repository_record).get();

            const yuan::mysql::orm::Record repository_update_record{
                "yuan_mysql_repository_smoke",
                1,
                {
                    {"id", std::uint64_t{7}},
                    {"bucket_id", std::uint64_t{0}},
                    {"label", yuan::mysql::string_value("updated")},
                    {"level", std::uint64_t{2}},
                },
            };
            repository.update_by_primary_key(0, repository_update_record).get();

            const auto repository_select = repository.select_by_primary_key(0, repository_update_record, {"label", "level"}, 1).get();
            if (repository_select.row_count() != 1 || !repository_select.rows()[0][0] ||
                *repository_select.rows()[0][0] != "updated" || !repository_select.rows()[0][1] ||
                *repository_select.rows()[0][1] != "2") {
                std::cerr << "unexpected record repository select result\n";
                return 1;
            }

            const auto repository_count = repository.select_count_by_primary_key(0, repository_update_record).get();
            if (repository_count.row_count() != 1 || !repository_count.rows()[0][0] || *repository_count.rows()[0][0] != "1") {
                std::cerr << "unexpected record repository count result\n";
                return 1;
            }

            repository.upsert(0, repository_update_record).get();
            repository.delete_by_primary_key(0, repository_update_record).get();
        }

        router.stop();

        worker.stop();
        std::cout << "YuanMysql db proxy smoke test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
