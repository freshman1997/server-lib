#ifndef __YUAN_REDIS_DEFAULT_CMD_H__
#define __YUAN_REDIS_DEFAULT_CMD_H__
#include "../command.h"
#include "redis_value.h"

#include <memory>
#include <vector>

namespace yuan::redis 
{
    class DefaultCmd : public Command
    {
    public:
        DefaultCmd() = default;
        ~DefaultCmd() = default;

        DefaultCmd(const DefaultCmd &) = delete;
        DefaultCmd &operator=(const DefaultCmd &) = delete;
        
        DefaultCmd(DefaultCmd &&) = delete;
        DefaultCmd &operator=(DefaultCmd &&) = delete;
        
    public:
        virtual void set_args(const std::string &cmd_name, const std::vector<std::shared_ptr<RedisValue>> &args);

        virtual std::string get_cmd_name() const;

        virtual std::shared_ptr<RedisValue> get_result() const;
        
        virtual std::string pack() const;

        virtual int unpack(const unsigned char *begin, const unsigned char *end);

        static int unpack_result(std::shared_ptr<RedisValue> &result, const unsigned char *begin, const unsigned char *end);

    public:
        void add_arg(std::shared_ptr<RedisValue> arg)
        {
            args_.push_back(arg);
        }

    protected:
        std::string cmd_string_;
        std::vector<std::shared_ptr<RedisValue>> args_;
        std::shared_ptr<RedisValue> result_;
    };

}

#endif // __YUAN_REDIS_DEFAULT_CMD_H__