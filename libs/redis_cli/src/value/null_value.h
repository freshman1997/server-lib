#ifndef __YUAN_REDIS_NULL_VALUE_H__
#define __YUAN_REDIS_NULL_VALUE_H__

#include "../redis_value.h"
#include "../internal/def.h"

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
    };
}

#endif // __YUAN_REDIS_NULL_VALUE_H__
