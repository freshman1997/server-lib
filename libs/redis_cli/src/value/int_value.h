#ifndef __YUAN_REDIS_INT_VALUE_H__
#define __YUAN_REDIS_INT_VALUE_H__

#include <cstdint>
#include "../redis_value.h"
#include "../internal/def.h"

namespace yuan::redis 
{
    class IntValue final : public RedisValue
    {
    public:
        IntValue() : value_(0) {}
        explicit IntValue(int64_t value) : value_(value) {}
        std::string to_string() const override { return std::to_string(value_); }

        char get_type() const override { return resp_int; }

        int64_t get_value() const { return value_; }
        void set_value(const int64_t value) { value_ = value; }

    private:
        int64_t value_;
    };
}

#endif // __YUAN_REDIS_INT_VALUE_H__
