#ifndef __YUAN_APP_PLUGIN_REDIS_STORAGE_H__
#define __YUAN_APP_PLUGIN_REDIS_STORAGE_H__

#include "plugin/host_storage.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace yuan::redis
{
    class RedisClient;
}

namespace yuan::app
{

    class PluginRedisStorage : public plugin::HostStorage
    {
    public:
        struct Config
        {
            std::string host{ "127.0.0.1" };
            int port = 6379;
            std::string password;
            int db = 0;
            std::string key_prefix{ "yuan:plugin:" };
        };

        explicit PluginRedisStorage(const std::string &plugin_name);
        ~PluginRedisStorage() override;

        bool init();

        bool set(const std::string &key, const std::string &value) override;
        bool set(const std::string &key, const std::string &value,
                 std::chrono::milliseconds ttl) override;
        std::optional<std::string> get(const std::string &key) const override;
        bool del(const std::string &key) override;
        bool exists(const std::string &key) const override;

        bool hset(const std::string &key, const std::string &field, const std::string &value) override;
        std::optional<std::string> hget(const std::string &key, const std::string &field) const override;
        bool hdel(const std::string &key, const std::string &field) override;
        std::unordered_map<std::string, std::string> hgetall(const std::string &key) const override;

        bool is_available() const override;
        const char *backend_name() const override
        {
            return "redis";
        }

    private:
        std::string make_key(const std::string &key) const;

        std::string plugin_name_;
        Config config_;
        std::shared_ptr<yuan::redis::RedisClient> client_;
        bool initialized_ = false;
    };

} // namespace yuan::app

#endif
