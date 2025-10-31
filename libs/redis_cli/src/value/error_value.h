#ifndef __YUAN_REDIS_ERROR_VALUE_H__
#define __YUAN_REDIS_ERROR_VALUE_H__

#include <memory>
#include <string>
#include "../redis_value.h"
#include "../internal/def.h"

namespace yuan::redis 
{
    class ErrorValue : public RedisValue
    {
    public:
        ErrorValue(const std::string &error) : error_(error) {}
        std::string to_string() const override { return error_; }
        char get_type() const override { return resp_error; }

        static std::shared_ptr<RedisValue> to_error(const unsigned char *begin, const unsigned char *end);

        static std::shared_ptr<RedisValue> from_string(const std::string &str)
        {
            return std::make_shared<ErrorValue>(str);
        }

    private:
        std::string error_;
    };

}

#endif // __YUAN_REDIS_ERROR_VALUE_H__