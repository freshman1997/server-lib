#ifndef __YUAN_REDIS_ARRAY_VALUE_H__
#define __YUAN_REDIS_ARRAY_VALUE_H__
#include "internal/def.h"
#include "redis_value.h"
#include <vector>
#include <memory>
#include <sstream>

namespace yuan::redis 
{
    class ArrayValue : public RedisValue
    {
    public:
        ArrayValue() = default;
        ~ArrayValue() = default;
    public:
        virtual std::string to_string() const
        {
            std::stringstream ss;
            
            ss << "[";
            
            for (auto &value : values_)
            {
                ss << value->to_string() << ",";
            }
            
            ss << "]";
            
            return ss.str();
        }

        virtual char get_type() const
        {
            return resp_array;
        }

    public:
        const std::vector<std::shared_ptr<RedisValue>> & get_values()
        {
            return values_;
        }

        void set_values(const std::vector<std::shared_ptr<RedisValue>> &values)
        {
            values_ = values;
        }

        void add_value(const std::shared_ptr<RedisValue> &value)
        {
            values_.push_back(value);
        }

    private:
        std::vector<std::shared_ptr<RedisValue>> values_;
    };
}

#endif // __YUAN_REDIS_ARRAY_VALUE_H__