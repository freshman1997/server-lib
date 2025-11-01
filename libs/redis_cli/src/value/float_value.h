#ifndef __YUAN_REDIS_FLOAT_VALUE_H__
#define __YUAN_REDIS_FLOAT_VALUE_H__

#include "internal/def.h"
#include "redis_value.h"
#include <string>

namespace yuan::redis 
{
    class FloatValue : public RedisValue
    {
    public:
        FloatValue() : value_(0.0)
        {
        }

        FloatValue(float value) : value_(value)
        {
            raw_str_ = std::to_string(value_);
        }

        FloatValue(const std::string &str)
        {
            raw_str_ = str;
            value_ = std::stod(str);
        }
        
        virtual std::string to_string() const override
        {
            return raw_str_.empty() ? std::to_string(value_) : raw_str_;
        }


        virtual char get_type() const override
        {
            return resp_float;
        }
        
        void set_raw_str(const std::string &str)
        {
            raw_str_ = str;
            value_ = std::stod(str);
        }

        std::string get_raw_str() const
        {
            return raw_str_;
        }
    
    private:
        double value_;
        std::string raw_str_;
    };
        
}

#endif // __YUAN_REDIS_FLOAT_VALUE_H__