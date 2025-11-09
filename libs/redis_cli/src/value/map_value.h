#ifndef __YUAN_REDIS_MAP_VALUE_H__
#define __YUAN_REDIS_MAP_VALUE_H__
#include "internal/def.h"
#include "redis_value.h"
#include <memory>
#include <sstream>
#include <unordered_map>

namespace yuan::redis 
{
    class MapValue final : public RedisValue
    {
    public:
        MapValue() = default;
        explicit MapValue(const std::unordered_map<std::string, std::shared_ptr<RedisValue>> &map) : map_val_(map) {}
        ~MapValue() override = default;

        std::string to_string() const override
        {
            std::ostringstream oss;
            
            oss << "{ ";
            for (const auto &[key, val] : map_val_)
            {
                oss << key << ":" << val->to_string() << ", ";
            }
            oss << "}";
            
            return oss.str();
        }

        char get_type() const override
        {
            return resp_map;
        }
        
        std::unordered_map<std::string, std::shared_ptr<RedisValue>> & get_map_value() { return map_val_; }
        const std::unordered_map<std::string, std::shared_ptr<RedisValue>> & get_map_value() const { return map_val_; }
        
    private:
        std::unordered_map<std::string, std::shared_ptr<RedisValue>> map_val_;
    };
}

#endif // __YUAN_REDIS_MAP_VALUE_H__