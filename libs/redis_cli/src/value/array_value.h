#ifndef __YUAN_REDIS_ARRAY_VALUE_H__
#define __YUAN_REDIS_ARRAY_VALUE_H__
#include "internal/def.h"
#include "redis_value.h"
#include <vector>
#include <memory>
#include <sstream>

namespace yuan::redis 
{
    class ArrayValue final : public RedisValue
    {
    public:
        ArrayValue() = default;
        ~ArrayValue() override = default;

    public:
        std::string to_string() const override
        {
            std::stringstream ss;
            
            ss << "[";
            
            for (auto &value : values_)
            {
                ss << value->to_string();
                if (value != values_.back())
                {
                    ss << ",";
                }
            }
            
            ss << "]";
            
            return ss.str();
        }

        char get_type() const override
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