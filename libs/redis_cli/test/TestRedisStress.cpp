#include "redis_client.h"
#include "redis_client_pool.h"
#include "logger.h"
#include "value/int_value.h"
#include "platform/native_platform.h"

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#endif

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#undef assert
#define assert(expr)                                                                                                   \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "assertion failed: " << #expr << " at " << __FILE__ << ":" << __LINE__ << std::endl;          \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

namespace
{
    using Clock = std::chrono::steady_clock;

    int env_int(const char *name, int fallback)
    {
        const char *value = std::getenv(name);
        if (!value) {
            return fallback;
        }

        int parsed = fallback;
        const std::string text(value);
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        return ec == std::errc() && ptr == text.data() + text.size() ? parsed : fallback;
    }

    bool env_exists(const char *name)
    {
        return std::getenv(name) != nullptr;
    }

    std::string env_string(const char *name, const std::string &fallback)
    {
        const char *value = std::getenv(name);
        return value ? std::string(value) : fallback;
    }

    std::string test_key_prefix()
    {
#ifdef _WIN32
        return "yuan:redis:stress:" + std::to_string(GetCurrentProcessId()) + ":";
#else
        const auto now = Clock::now().time_since_epoch().count();
        return "yuan:redis:stress:" + std::to_string(now) + ":";
#endif
    }

    double elapsed_seconds(Clock::time_point start, Clock::time_point end)
    {
        return std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    }

    std::size_t current_rss_kb()
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                sizeof(counters))) {
            return static_cast<std::size_t>(counters.WorkingSetSize / 1024);
        }
#endif
        return 0;
    }

    void assert_ok(const std::shared_ptr<yuan::redis::RedisValue> &value, const char *message)
    {
        if (!value) {
            std::cerr << message << " returned null" << std::endl;
            assert(false);
        }
    }

    std::shared_ptr<yuan::redis::RedisClient> connect_client(yuan::redis::Option &option)
    {
        auto client = std::make_shared<yuan::redis::RedisClient>(option);
        if (client->ping()) {
            return client;
        }

        if (!env_exists("REDIS_PORT") && option.port_ != 6379) {
            option.port_ = 6379;
            option.name_ += "-fallback";
            auto fallback_client = std::make_shared<yuan::redis::RedisClient>(option);
            if (fallback_client->ping()) {
                return fallback_client;
            }
        }

        return client;
    }
}

int main()
{
    LOG_GET_REGISTRY()->set_global_level(yuan::log::Level::warn);
    LOG_GET_REGISTRY()->disable_file_log();

   yuan::platform::NativePlatformGuard guard;

    using namespace yuan::redis;

    Option option;
    option.host_ = env_string("REDIS_HOST", "localhost");
    option.port_ = env_int("REDIS_PORT", 6378);
    option.db_ = env_int("REDIS_DB", 1);
    option.connect_timeout_ms_ = env_int("REDIS_CONNECT_TIMEOUT_MS", 5000);
    option.command_timeout_ms_ = env_int("REDIS_COMMAND_TIMEOUT_MS", 0);
    option.timeout_ms_ = env_int("REDIS_TIMEOUT_MS", 0);
    option.max_buffered_response_bytes_ = 64 * 1024 * 1024;
    option.name_ = "redis-stress";

    const int command_ops = std::max(1, env_int("REDIS_STRESS_COMMANDS", 300));
    const int pipeline_batch = std::max(1, env_int("REDIS_STRESS_PIPELINE_BATCH", 32));
    const int pool_size = std::max(1, env_int("REDIS_STRESS_POOL_SIZE", 4));
    const int pubsub_messages = std::max(1, env_int("REDIS_STRESS_PUBSUB", 300));
    const int limited_pubsub_messages = std::max(1, env_int("REDIS_STRESS_LIMITED_PUBSUB", 96));
    const int pending_limit = std::max(1, env_int("REDIS_STRESS_PENDING_LIMIT", 32));

    auto client = connect_client(option);
    if (!client->is_connected()) {
        std::cerr << "Redis stress skipped: no client connected to "
                  << option.host_ << ":" << option.port_;
        if (const auto error = client->get_last_error()) {
            std::cerr << " (" << error->to_string() << ")";
        }
        std::cerr << std::endl;
        return 0;
    }

    const std::string prefix = test_key_prefix();
    const std::string data_key = prefix + "data";
    const std::string counter_key = prefix + "counter";
    assert_ok(client->del({data_key, counter_key}), "DEL setup");

    const auto command_mem_before = current_rss_kb();
    const auto command_start = Clock::now();
    for (int i = 0; i < command_ops; ++i) {
        switch (i % 4) {
        case 0:
            assert_ok(client->set(data_key, "payload-" + std::to_string(i)), "SET");
            break;
        case 1:
            assert_ok(client->get(data_key), "GET");
            break;
        case 2:
            assert_ok(client->incr(counter_key), "INCR");
            break;
        default:
            assert_ok(client->expire(data_key, 60), "EXPIRE");
            break;
        }
    }
    const auto command_end = Clock::now();
    const auto command_mem_after = current_rss_kb();
    const double command_seconds = elapsed_seconds(command_start, command_end);

    std::cout << "STRESS_RESULT command_ops=" << command_ops
              << " seconds=" << command_seconds
              << " ops_per_sec=" << (command_ops / command_seconds)
              << " rss_before_kb=" << command_mem_before
              << " rss_after_kb=" << command_mem_after
              << std::endl;

    const std::string pipeline_key = prefix + "pipe";
    assert_ok(client->del({pipeline_key}), "DEL pipeline setup");

    int pipeline_commands = 0;
    const auto pipeline_mem_before = current_rss_kb();
    const auto pipeline_start = Clock::now();
    for (int i = 0; i < command_ops; i += pipeline_batch) {
        std::vector<std::string> commands;
        commands.reserve(static_cast<std::size_t>(pipeline_batch));
        const int limit = std::min(command_ops, i + pipeline_batch);
        for (int j = i; j < limit; ++j) {
            commands.push_back("INCR " + pipeline_key);
            ++pipeline_commands;
        }
        assert_ok(client->pipeline(commands), "PIPELINE INCR");
    }
    const auto pipeline_end = Clock::now();
    const auto pipeline_mem_after = current_rss_kb();
    const double pipeline_seconds = elapsed_seconds(pipeline_start, pipeline_end);

    std::cout << "STRESS_RESULT pipeline_ops=" << pipeline_commands
              << " batch=" << pipeline_batch
              << " seconds=" << pipeline_seconds
              << " ops_per_sec=" << (pipeline_commands / pipeline_seconds)
              << " rss_before_kb=" << pipeline_mem_before
              << " rss_after_kb=" << pipeline_mem_after
              << std::endl;

    int typed_pipeline_commands = 0;
    const auto typed_pipeline_start = Clock::now();
    for (int i = 0; i < command_ops; i += pipeline_batch) {
        std::vector<PipelineCommand> commands;
        commands.reserve(static_cast<std::size_t>(pipeline_batch));
        const int limit = std::min(command_ops, i + pipeline_batch);
        for (int j = i; j < limit; ++j) {
            commands.emplace_back("incr", std::vector<std::string>{pipeline_key});
            ++typed_pipeline_commands;
        }
        assert_ok(client->pipeline(commands), "TYPED PIPELINE INCR");
    }
    const auto typed_pipeline_end = Clock::now();
    const double typed_pipeline_seconds = elapsed_seconds(typed_pipeline_start, typed_pipeline_end);
    std::cout << "STRESS_RESULT typed_pipeline_ops=" << typed_pipeline_commands
              << " batch=" << pipeline_batch
              << " seconds=" << typed_pipeline_seconds
              << " ops_per_sec=" << (typed_pipeline_commands / typed_pipeline_seconds)
              << std::endl;

    auto concurrent_client = connect_client(option);
    assert(concurrent_client->is_connected());
    std::atomic<bool> guard_detected{false};
    std::thread concurrent_worker([concurrent_client, pipeline_key, &guard_detected]() {
        for (int i = 0; i < 50; ++i) {
            const auto result = concurrent_client->pipeline({PipelineCommand("incr", {pipeline_key})});
            if (!result && concurrent_client->get_last_error()) {
                guard_detected.store(true, std::memory_order_release);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    for (int i = 0; i < 50; ++i) {
        client->pipeline({PipelineCommand("incr", {pipeline_key})});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    concurrent_worker.join();
    concurrent_client->close();
    std::cout << "STRESS_RESULT concurrent_guard=" << (guard_detected.load() ? "ok" : "no_contention") << std::endl;

    RedisClientPool pool;
    Option pool_option = option;
    pool_option.name_ = "redis-stress-pool";
    if (pool.init(pool_option, static_cast<std::size_t>(pool_size))) {
        const auto initial_pool_stats = pool.stats();
        assert(initial_pool_stats.size == static_cast<std::size_t>(pool_size));
        assert(initial_pool_stats.connected == static_cast<std::size_t>(pool_size));

        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(pool_size));
        const int ops_per_worker = command_ops / pool_size;
        const int remainder = command_ops % pool_size;
        const auto pool_start = Clock::now();
        for (int worker = 0; worker < pool_size; ++worker) {
            const int worker_ops = ops_per_worker + (worker < remainder ? 1 : 0);
            const std::string worker_key = prefix + "pool:" + std::to_string(worker);
            auto pool_client = pool.get_round_robin_client();
            workers.emplace_back([worker_ops, pipeline_batch, worker_key, pool_client]() {
                assert(pool_client && pool_client->is_connected());
                for (int i = 0; i < worker_ops; i += pipeline_batch) {
                    std::vector<PipelineCommand> commands;
                    commands.reserve(static_cast<std::size_t>(pipeline_batch));
                    const int limit = std::min(worker_ops, i + pipeline_batch);
                    for (int j = i; j < limit; ++j) {
                        commands.emplace_back("incr", std::vector<std::string>{worker_key});
                    }
                    assert_ok(pool_client->pipeline(commands), "POOL PIPELINE INCR");
                }
            });
        }

        for (auto &worker : workers) {
            worker.join();
        }
        const auto pool_end = Clock::now();
        const double pool_seconds = elapsed_seconds(pool_start, pool_end);
        std::cout << "STRESS_RESULT pool_pipeline_ops=" << command_ops
                  << " pool_size=" << pool_size
                  << " batch=" << pipeline_batch
                  << " seconds=" << pool_seconds
                  << " ops_per_sec=" << (command_ops / pool_seconds)
                  << std::endl;

        const auto final_pool_stats = pool.stats();
        assert(final_pool_stats.size == static_cast<std::size_t>(pool_size));
        std::cout << "STRESS_RESULT pool_stats size=" << final_pool_stats.size
                  << " connected=" << final_pool_stats.connected
                  << " unhealthy=" << final_pool_stats.unhealthy
                  << std::endl;
        pool.close();
    }

    Option sub_option = option;
    sub_option.name_ = "redis-stress-sub";
    sub_option.max_pubsub_pending_messages_ = static_cast<std::size_t>(pubsub_messages + 16);
    Option pub_option = option;
    pub_option.name_ = "redis-stress-pub";

    auto subscriber = connect_client(sub_option);
    auto publisher = connect_client(pub_option);
    assert(subscriber->is_connected());
    assert(publisher->is_connected());

    const std::string channel = prefix + "burst";
    int received = 0;
    assert_ok(subscriber->subscribe({channel}, [&received](const std::vector<SubMessage> &messages) {
        received += static_cast<int>(messages.size());
    }), "SUBSCRIBE burst");

    const auto pubsub_mem_before = current_rss_kb();
    const auto publish_start = Clock::now();
    for (int i = 0; i < pubsub_messages; ++i) {
        assert_ok(publisher->publish(channel, "message-" + std::to_string(i)), "PUBLISH burst");
    }
    const auto publish_end = Clock::now();

    int receive_calls = 0;
    while (received < pubsub_messages && receive_calls < pubsub_messages + 8) {
        assert(subscriber->receive(option.timeout_ms_) == 0);
        ++receive_calls;
    }
    const auto drain_end = Clock::now();
    const auto pubsub_mem_after = current_rss_kb();
    assert(received == pubsub_messages);

    const double publish_seconds = elapsed_seconds(publish_start, publish_end);
    const double drain_seconds = elapsed_seconds(publish_end, drain_end);
    std::cout << "STRESS_RESULT pubsub_messages=" << pubsub_messages
              << " publish_seconds=" << publish_seconds
              << " publish_per_sec=" << (pubsub_messages / publish_seconds)
              << " drain_seconds=" << drain_seconds
              << " receive_calls=" << receive_calls
              << " received=" << received
              << " rss_before_kb=" << pubsub_mem_before
              << " rss_after_kb=" << pubsub_mem_after
              << std::endl;

    assert_ok(subscriber->unsubscribe({channel}), "UNSUBSCRIBE burst");

    Option limited_sub_option = option;
    limited_sub_option.name_ = "redis-stress-limited-sub";
    limited_sub_option.max_pubsub_pending_messages_ = static_cast<std::size_t>(pending_limit);
    auto limited_subscriber = connect_client(limited_sub_option);
    assert(limited_subscriber->is_connected());

    const std::string limited_channel = prefix + "limited";
    int limited_received = 0;
    assert_ok(limited_subscriber->subscribe({limited_channel}, [&limited_received](const std::vector<SubMessage> &messages) {
        limited_received += static_cast<int>(messages.size());
    }), "SUBSCRIBE limited");

    const auto limited_mem_before = current_rss_kb();
    for (int i = 0; i < limited_pubsub_messages; ++i) {
        assert_ok(publisher->publish(limited_channel, "limited-" + std::to_string(i)), "PUBLISH limited");
    }

    int limited_receive_calls = 0;
    while (limited_received < std::min(limited_pubsub_messages, pending_limit) &&
           limited_receive_calls < pending_limit + 8) {
        if (limited_subscriber->receive(50) != 0) {
            break;
        }
        ++limited_receive_calls;
    }
    const auto limited_mem_after = current_rss_kb();
    assert(limited_received > 0);
    assert(limited_received <= pending_limit);
    if (limited_pubsub_messages > pending_limit) {
        assert(limited_received < limited_pubsub_messages);
    }

    std::cout << "STRESS_RESULT limited_pubsub_messages=" << limited_pubsub_messages
              << " pending_limit=" << pending_limit
              << " received_after_trim=" << limited_received
              << " receive_calls=" << limited_receive_calls
              << " rss_before_kb=" << limited_mem_before
              << " rss_after_kb=" << limited_mem_after
              << std::endl;

    assert_ok(limited_subscriber->unsubscribe({limited_channel}), "UNSUBSCRIBE limited");

    subscriber->close();
    limited_subscriber->close();
    publisher->close();
    assert_ok(client->del({data_key, counter_key}), "DEL cleanup");
    client->close();

    {
        Option timeout_option = option;
        timeout_option.name_ = "redis-stress-command-timeout";
        timeout_option.command_timeout_ms_ = 50;
        timeout_option.reconnect_ = true;
        timeout_option.max_reconnect_retries_ = 3;
        timeout_option.reconnect_delay_ms_ = 20;

        auto timeout_client = connect_client(timeout_option);
        if (timeout_client && timeout_client->is_connected()) {
            const std::string missing_list = prefix + "timeout:missing";
            const std::string dest_list = prefix + "timeout:dest";
            (void)timeout_client->del({missing_list, dest_list});

            const auto timed_out = timeout_client->brpoplpush(missing_list, dest_list, 1);
            assert(!timed_out);
            const auto timeout_stats = timeout_client->stats();
            assert(timeout_stats.command_timeouts > 0);
            assert(timeout_stats.in_flight == 0);
            assert(timeout_client->is_closed());

            assert_ok(timeout_client->ping(), "PING after command timeout reconnect");
            const auto recovered_stats = timeout_client->stats();
            assert(recovered_stats.reconnect_attempts > 0);
            assert(recovered_stats.reconnect_successes > 0);
            std::cout << "STRESS_RESULT command_timeout=recovered"
                      << " timeouts=" << recovered_stats.command_timeouts
                      << " reconnect_attempts=" << recovered_stats.reconnect_attempts
                      << " reconnect_successes=" << recovered_stats.reconnect_successes
                      << std::endl;
            timeout_client->close();
        } else {
            std::cout << "STRESS_RESULT command_timeout=skipped_no_connection" << std::endl;
        }
    }

    {
        Option rc_option = option;
        rc_option.name_ = "redis-stress-reconnect";
        rc_option.reconnect_ = true;
        rc_option.max_reconnect_retries_ = 3;
        rc_option.reconnect_delay_ms_ = 50;
        auto rc_client = connect_client(rc_option);
        if (rc_client && rc_client->is_connected()) {
            const std::string rc_key = prefix + "reconnect";
            assert_ok(rc_client->set(rc_key, "before"), "SET before disconnect");
            assert_ok(rc_client->get(rc_key), "GET before disconnect");

            rc_client->disconnect();
            assert(!rc_client->is_connected());

            auto rc_result = rc_client->get(rc_key);
            assert(rc_result || rc_client->get_last_error());
            const auto rc_stats = rc_client->stats();
            assert(rc_stats.reconnect_attempts > 0);
            assert(rc_stats.in_flight == 0);

            if (rc_result) {
                assert(rc_stats.reconnect_successes > 0);
                std::cout << "STRESS_RESULT reconnect=auto_reconnected" << std::endl;
            } else {
                std::cout << "STRESS_RESULT reconnect=attempted_error="
                          << (rc_client->get_last_error() ? rc_client->get_last_error()->to_string() : "null")
                          << std::endl;
            }

            rc_client->close();
        } else {
            std::cout << "STRESS_RESULT reconnect=skipped_no_connection" << std::endl;
        }
    }

    {
        Option hc_option = option;
        hc_option.name_ = "redis-stress-healthcheck";
        RedisClientPool hc_pool;
        if (hc_pool.init(hc_option, 2)) {
            auto hc1 = hc_pool.get_round_robin_client();
            auto hc2 = hc_pool.get_round_robin_client();
            assert(hc1 && hc1->is_connected());
            assert(hc2 && hc2->is_connected());

            hc1->disconnect();
            assert(!hc1->is_connected());
            const auto degraded = hc_pool.stats();
            assert(degraded.size == 2);
            assert(degraded.unhealthy >= 1);

            auto hc_again = hc_pool.get_round_robin_client();
            assert(hc_again);
            if (hc_again->is_connected()) {
                assert_ok(hc_again->ping(), "PING after health check reconnect");
                const auto recovered = hc_pool.stats();
                std::cout << "STRESS_RESULT health_check=reconnected_and_ping_ok" << std::endl;
                std::cout << "STRESS_RESULT health_stats size=" << recovered.size
                          << " connected=" << recovered.connected
                          << " unhealthy=" << recovered.unhealthy
                          << std::endl;
            } else {
                std::cout << "STRESS_RESULT health_check=reconnect_failed" << std::endl;
            }

            hc_pool.close();
        } else {
            std::cout << "STRESS_RESULT health_check=skipped_pool_init_failed" << std::endl;
        }
    }

    {
        const int soak_seconds = env_int("REDIS_SOAK_SECONDS", 0);
        if (soak_seconds > 0) {
            Option soak_option = option;
            soak_option.name_ = "redis-soak";
            RedisClientPool soak_pool;
            if (soak_pool.init(soak_option, 2)) {
                const std::string soak_key = prefix + "soak";
                const auto soak_start = Clock::now();
                const auto soak_deadline = soak_start + std::chrono::seconds(soak_seconds);
                int soak_ops = 0;
                int soak_errors = 0;
                const auto soak_rss_before = current_rss_kb();

                while (Clock::now() < soak_deadline) {
                    auto soak_client = soak_pool.get_round_robin_client();
                    if (!soak_client) {
                        ++soak_errors;
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                    auto res = soak_client->set(soak_key, "v" + std::to_string(soak_ops));
                    if (res) {
                        ++soak_ops;
                    } else {
                        ++soak_errors;
                    }
                }

                const auto soak_rss_after = current_rss_kb();
                const double soak_elapsed = elapsed_seconds(soak_start, Clock::now());
                std::cout << "STRESS_RESULT soak_ops=" << soak_ops
                          << " soak_errors=" << soak_errors
                          << " soak_seconds=" << soak_elapsed
                          << " ops_per_sec=" << (soak_ops / soak_elapsed)
                          << " rss_before_kb=" << soak_rss_before
                          << " rss_after_kb=" << soak_rss_after
                          << std::endl;

                soak_pool.close();
            }
        }
    }

    return 0;
}
