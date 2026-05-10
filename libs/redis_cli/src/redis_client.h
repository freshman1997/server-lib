#ifndef __YUAN_REDIS_CLIENT_H__
#define __YUAN_REDIS_CLIENT_H__

#include <functional>
#include <memory>
#include <string>
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

    class RedisClient : public std::enable_shared_from_this<RedisClient>
    {
    public:
        RedisClient();
        explicit RedisClient(const Option &opt);
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

        std::shared_ptr<RedisValue> get_last_error() const;
        void set_last_error(std::shared_ptr<RedisValue> error);
        const std::string &get_name() const;

        int receive(int timeout);
        int receive();
        bool is_subscribing() const;
        void unsubscribe_channel(const std::string &channel);

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
        std::unique_ptr<Impl> impl_;
    };
}

#endif // __YUAN_REDIS_CLIENT_H__
