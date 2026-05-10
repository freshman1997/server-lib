#include "internal/def.h"
#include "redis_cli_manager.h"
#include "value/array_value.h"
#include "value/int_value.h"
#include "value/map_value.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
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
        return "yuan:redis:e2e:" + std::to_string(GetCurrentProcessId()) + ":";
#else
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return "yuan:redis:e2e:" + std::to_string(now) + ":";
#endif
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

    bool array_contains_pair(
        const std::shared_ptr<yuan::redis::ArrayValue> &array,
        const std::string &field,
        const std::string &expected)
    {
        const auto &values = array->get_values();
        for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
            if (values[i] && values[i + 1] &&
                values[i]->to_string() == field &&
                values[i + 1]->to_string() == expected) {
                return true;
            }
        }
        return false;
    }
}

int main()
{
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
    option.timeout_ms_ = env_int("REDIS_TIMEOUT_MS", 3000);
    option.name_ = "redis-e2e";

    auto client = connect_client(option);
    if (!client->is_connected()) {
        std::cerr << "Redis e2e skipped: no client connected to "
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
    const std::string string_key = prefix + "string";
    const std::string list_key = prefix + "list";
    const std::string hash_key = prefix + "hash";
    const std::string set_key = prefix + "set";
    const std::string zset_key = prefix + "zset";

    (void)client->del({string_key, list_key, hash_key, set_key, zset_key});

    assert_ok(client->set(string_key, "hello"), "SET");
    auto value = client->get(string_key);
    assert_ok(value, "GET");
    assert(value->to_string() == "hello");

    assert_ok(client->lpush(list_key, {"a", "b", "c"}), "LPUSH");
    value = client->lrange(list_key, 0, -1);
    assert_ok(value, "LRANGE");
    assert(value->get_type() == resp_array);
    assert(value->as<ArrayValue>()->get_values().size() == 3);

    assert_ok(client->hset(hash_key, "field", "value"), "HSET");
    value = client->hgetall(hash_key);
    assert_ok(value, "HGETALL");
    if (value->get_type() == resp_map) {
        const auto &map = value->as<MapValue>()->get_map_value();
        assert(map.contains("field"));
        assert(map.at("field")->to_string() == "value");
    } else {
        assert(value->get_type() == resp_array);
        assert(array_contains_pair(value->as<ArrayValue>(), "field", "value"));
    }

    assert_ok(client->sadd(set_key, {"m1", "m2"}), "SADD");
    value = client->sismember(set_key, "m1");
    assert_ok(value, "SISMEMBER");
    assert(value->get_type() == resp_int);
    assert(value->as<IntValue>()->get_value() == 1);

    assert_ok(client->zadd(zset_key, {{"alice", 1.5}, {"bob", 2.5}}), "ZADD");
    value = client->zrange(zset_key, 0, -1);
    assert_ok(value, "ZRANGE");
    assert(value->get_type() == resp_array);
    assert(value->as<ArrayValue>()->get_values().size() == 2);

    value = client->eval("return ARGV[1]", {}, {"script-ok"});
    assert_ok(value, "EVAL");
    assert(value->to_string() == "script-ok");

    const std::string channel = prefix + "channel";
    Option sub_option = option;
    sub_option.name_ = "redis-e2e-sub";
    Option pub_option = option;
    pub_option.name_ = "redis-e2e-pub";
    auto subscriber = connect_client(sub_option);
    auto publisher = connect_client(pub_option);
    assert(subscriber->is_connected());
    assert(publisher->is_connected());

    std::vector<std::string> messages;
    assert_ok(subscriber->subscribe({channel}, [&messages](const std::vector<SubMessage> &received) {
        for (const auto &message : received) {
            messages.push_back(message.channel->to_string() + ":" + message.message->to_string());
        }
    }), "SUBSCRIBE");

    assert_ok(publisher->publish(channel, "payload-1"), "PUBLISH");
    assert(subscriber->receive(2000) == 0);
    assert(messages.size() == 1);
    assert(messages[0] == channel + ":payload-1");

    assert_ok(subscriber->unsubscribe({channel}), "UNSUBSCRIBE");

    const std::string mixed_channel = prefix + "mixed-channel";
    const std::string mixed_pattern = prefix + "mixed-*";
    const std::string pattern_channel = prefix + "mixed-target";
    std::vector<std::string> pattern_messages;
    assert_ok(subscriber->psubscribe({mixed_pattern}, [&pattern_messages](const std::vector<PSubMessage> &received) {
        for (const auto &message : received) {
            pattern_messages.push_back(message.channel->to_string() + ":" + message.message->to_string());
        }
    }), "PSUBSCRIBE");
    assert_ok(subscriber->subscribe({mixed_channel}, [](const std::vector<SubMessage> &) {}), "SUBSCRIBE mixed");
    assert_ok(subscriber->unsubscribe(std::vector<std::string>{}), "UNSUBSCRIBE channels only");
    assert(subscriber->is_subscribing());
    assert_ok(publisher->publish(pattern_channel, "pattern-payload"), "PUBLISH pattern");
    assert(subscriber->receive(2000) == 0);
    assert(pattern_messages.size() == 1);
    assert(pattern_messages[0] == pattern_channel + ":pattern-payload");
    assert_ok(subscriber->punsubscribe(std::vector<std::string>{}), "PUNSUBSCRIBE all");

    subscriber->close();
    publisher->close();

    assert_ok(client->del({string_key, list_key, hash_key, set_key, zset_key}), "DEL cleanup");
    client->close();
    RedisCliManager::get_instance()->release_all();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
