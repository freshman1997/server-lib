#ifndef __YUAN_REDIS_STRING_VALUE_H__
#define __YUAN_REDIS_STRING_VALUE_H__

#include <memory>
#include <string>
#include "../redis_value.h"
#include "../internal/def.h"

namespace yuan::redis
{
    class StringValue : public RedisValue
    {
    public:
        StringValue(const std::string &value) : value_(value) {}
        StringValue(std::string &&value) : value_(std::move(value)) {} 
        ~StringValue() = default;
        std::string to_string() const override { return value_; }
        virtual char get_type() const override { return resp_string; }
        void set_value(const std::string &value) { value_ = value; }
        void set_value(std::string &&value) { value_ = std::move(value); }

        static std::shared_ptr<StringValue> from_string(const std::string &str)
        {
            return std::make_shared<StringValue>(str);
        }

    private:
        std::string value_;
    };
}

#endif // __STRING_VALUE_H__
