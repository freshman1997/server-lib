#ifndef __YUAN_REDIS_REDIS_VALUE_H__
#define __YUAN_REDIS_REDIS_VALUE_H__
#include <string>

namespace yuan::redis 
{
    class RedisValue 
    {
    public:
        virtual ~RedisValue() = default;

    public:
        virtual std::string to_string() const = 0;

        virtual char get_type() const = 0;
    };
}

#endif // __YUAN_REDIS_REDIS_VALUE_H__
