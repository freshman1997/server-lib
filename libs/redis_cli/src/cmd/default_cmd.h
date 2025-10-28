#ifndef __YUAN_REDIS_DEFAULT_CMD_H__
#define __YUAN_REDIS_DEFAULT_CMD_H__
#include "../command.h"

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

        virtual void set_callback(std::function<void (std::shared_ptr<RedisValue>)> callback) { callback_ = callback;}

        virtual void on_executed()
        {
            (*this)();
        }

        virtual std::shared_ptr<RedisValue> get_result() const;
        
        virtual std::string pack() const;

        virtual int unpack(const unsigned char *begin, const unsigned char *end);

    public:
        void operator()()
        {
            if (callback_)
            {
                callback_(get_result());
            }
        }

    protected:
        std::function<void (std::shared_ptr<RedisValue>)> callback_;
        std::string cmd_string_;
        std::vector<std::shared_ptr<RedisValue>> args_;
    };

}

#endif // __YUAN_REDIS_DEFAULT_CMD_H__