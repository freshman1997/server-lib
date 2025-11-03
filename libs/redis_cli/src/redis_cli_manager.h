#ifndef __YUAN_REDIS_REDIS_CLI_MANAGER_H__
#define __YUAN_REDIS_REDIS_CLI_MANAGER_H__
#include "option.h"
#include "redis_client.h"
#include "singleton/singleton.h"
#include <utility>
#include <vector>

namespace yuan::redis 
{
    class RedisCliManager : public singleton::Singleton<RedisCliManager>
    {
    public:
        int init(const std::vector<Option> &options);

        std::shared_ptr<RedisClient> get_random_redis_client();

        std::shared_ptr<RedisClient> get_round_robin_redis_client();
        
        RedisCliManager() = default;
        ~RedisCliManager() = default;
        
    private:
        int m_redis_cli_idx_ = 0;
        std::vector<std::pair<Option, std::shared_ptr<RedisClient>>> m_redis_cli_map;
    };
}

#endif // __YUAN_REDIS_REDIS_CLI_MANAGER_H__
