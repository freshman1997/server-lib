#ifndef __YUAN_REDIS_FLOAT_VALUE_H__
#define __YUAN_REDIS_FLOAT_VALUE_H__

#include "internal/def.h"
#include "redis_value.h"
#include "internal/utils.h"
#include <string>

namespace yuan::redis 
{
    class FloatValue : public RedisValue
    {
    public:
        FloatValue() : value_(0.0)
        {
        }

        FloatValue(double value) : value_(value)
        {
            raw_str_ = serializeDouble(value_);
        }

        FloatValue(const std::string &str)
        {
            raw_str_ = str;
            value_ = RedisDoubleConverter::convertSafe(str);
        }
        
        virtual std::string to_string() const override
        {
            return raw_str_.empty() ? serializeDouble(value_) : raw_str_;
        }


        virtual char get_type() const override
        {
            return resp_float;
        }
        
        virtual void set_raw_str(const std::string &str)
        {
            raw_str_ = str;
            value_ = RedisDoubleConverter::convertSafe(str);
        }

    private:
        double value_;
    };
        
}

#endif // __YUAN_REDIS_FLOAT_VALUE_H__