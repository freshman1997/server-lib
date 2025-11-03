#ifndef __YUAN_REDIS_REDIS_VALUE_H__
#define __YUAN_REDIS_REDIS_VALUE_H__
#include <memory>
#include <string>

namespace yuan::redis 
{
    class RedisValue : public std::enable_shared_from_this<RedisValue>
    {
    public:
        virtual ~RedisValue() = default;

    public:
        virtual std::string to_string() const = 0;

        virtual char get_type() const = 0;

        virtual const std::string & get_raw_str() const
        {
            return raw_str_;
        }

        virtual void set_raw_str(const std::string &str)
        {
            raw_str_ = str;
        }

        template <typename T>
        std::shared_ptr<T> as()
        {
            auto result = std::dynamic_pointer_cast<T>(shared_from_this());
            return !result ? nullptr : result;
        }

    protected:
        std::string raw_str_;
    };
}

#endif // __YUAN_REDIS_REDIS_VALUE_H__
