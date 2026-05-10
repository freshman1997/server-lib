#include "redis_cli_manager.h"
#include "logger.h"
#include "value/int_value.h"

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

#ifdef _WIN32
    WSADATA wsa;
    if (const int result = WSAStartup(MAKEWORD(2, 2), &wsa); result != NO_ERROR) {
        std::cerr << "WSAStartup failed: " << result << std::endl;
        return 1;
    }
#endif

    using namespace yuan::redis;

    Option option;
    option.host_ = env_string("REDIS_HOST", "localhost");
    option.port_ = env_int("REDIS_PORT", 6378);
    option.db_ = env_int("REDIS_DB", 1);
    option.timeout_ms_ = env_int("REDIS_TIMEOUT_MS", 5000);
    option.max_buffered_response_bytes_ = 64 * 1024 * 1024;
    option.name_ = "redis-stress";

    const int command_ops = std::max(1, env_int("REDIS_STRESS_COMMANDS", 300));
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
#ifdef _WIN32
        WSACleanup();
#endif
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
    RedisCliManager::get_instance()->release_all();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
