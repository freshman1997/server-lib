#ifndef __YUAN_REDIS_CLIENT_H__
#define __YUAN_REDIS_CLIENT_H__

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "option.h"
#include "redis_value.h"

namespace yuan::redis
{
    struct SubMessage
    {
        std::shared_ptr<RedisValue> channel;
        std::shared_ptr<RedisValue> message;
    };

    struct PSubMessage
    {
        std::shared_ptr<RedisValue> pattern;
        std::shared_ptr<RedisValue> channel;
        std::shared_ptr<RedisValue> message;
    };

    struct PipelineCommand
    {
        std::string name;
        std::vector<std::string> args;

        PipelineCommand() = default;
        PipelineCommand(std::string command_name, std::vector<std::string> command_args = {});
        PipelineCommand(std::string command_name, std::initializer_list<std::string_view> command_args);

        static PipelineCommand get(std::string key);
        static PipelineCommand set(std::string key, std::string value);
        static PipelineCommand del(std::vector<std::string> keys);
        static PipelineCommand incr(std::string key);
        static PipelineCommand expire(std::string key, int seconds);
        static PipelineCommand hset(std::string key, std::string field, std::string value);
        static PipelineCommand hget(std::string key, std::string field);
        static PipelineCommand lpush(std::string key, std::vector<std::string> values);
        static PipelineCommand rpush(std::string key, std::vector<std::string> values);
    };

    class RedisClient : public std::enable_shared_from_this<RedisClient>
    {
    public:
        RedisClient();
        explicit RedisClient(const Option &opt);
        RedisClient(const Option &opt, std::shared_ptr<void> registry);
        ~RedisClient();

        RedisClient(const RedisClient &) = delete;
        RedisClient &operator=(const RedisClient &) = delete;
        RedisClient(RedisClient &&) = delete;
        RedisClient &operator=(RedisClient &&) = delete;

        void set_option(const Option &opt);

        bool is_connected() const;
        bool is_closed() const;
        bool is_timeout() const;
        void close();
        void disconnect();

        std::shared_ptr<RedisValue> get_last_error() const;
        void set_last_error(std::shared_ptr<RedisValue> error);
        const std::string &get_name() const;

        int receive(int timeout);
        int receive();
        bool is_subscribing() const;
        void unsubscribe_channel(const std::string &channel);

        std::shared_ptr<RedisValue> pipeline(const std::vector<PipelineCommand> &commands);

        bool ensure_connected();
        bool wait_in_flight(uint32_t timeout_ms);

    public: // key commands
#include "api/redis_client_key_commands.inc"

    public: // string commands
#include "api/redis_client_string_commands.inc"

    public: // hash commands
#include "api/redis_client_hash_commands.inc"

    public: // list commands
#include "api/redis_client_list_commands.inc"

    public: // set commands
#include "api/redis_client_set_commands.inc"

    public: // sorted-set commands
#include "api/redis_client_zset_commands.inc"

    public: // bitmap and geo commands
#include "api/redis_client_advanced_commands.inc"

    public: // pub/sub commands
#include "api/redis_client_pubsub_commands.inc"

    public: // transaction and scripting commands
#include "api/redis_client_tx_script_commands.inc"

    public: // server and stream commands
#include "api/redis_client_server_stream_commands.inc"

    private:
        int connect();

    private:
        class Impl;
        std::shared_ptr<Impl> impl_;
    };
}

#endif // __YUAN_REDIS_CLIENT_H__
