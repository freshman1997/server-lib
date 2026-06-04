#ifndef __YUAN_REDIS_NULL_VALUE_H__
#define __YUAN_REDIS_NULL_VALUE_H__

#include "../redis_value.h"
#include "../internal/def.h"
#include <memory>

namespace yuan::redis
{
    class NullValue final : public RedisValue
    {
    public:
        NullValue() = default;
        std::string to_string() const override
        {
            return "null";
        }
        
        char get_type() const override
        {
            return resp_null;
        }

        static std::shared_ptr<NullValue> null()
        {
            static auto instance = std::make_shared<NullValue>();
            return instance;
        }
    };
}

#endif // __YUAN_REDIS_NULL_VALUE_H__
