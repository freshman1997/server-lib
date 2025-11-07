#ifndef __YUAN_REDIS_DEFAULT_CMD_H__
#define __YUAN_REDIS_DEFAULT_CMD_H__
#include "../command.h"
#include "buffer/buffer_reader.h"
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

        // empty args
        DefaultCmd(const std::string &cmdName) : cmd_string_(cmdName)
        {
        }

        DefaultCmd(const DefaultCmd &) = delete;
        DefaultCmd &operator=(const DefaultCmd &) = delete;
        
        DefaultCmd(DefaultCmd &&) = delete;
        DefaultCmd &operator=(DefaultCmd &&) = delete;
        
    public:
        virtual void set_args(const std::string &cmd_name, const std::vector<std::shared_ptr<RedisValue>> &args);

        virtual std::string get_cmd_name() const;

        virtual std::shared_ptr<RedisValue> get_result() const;

        virtual void set_result(std::shared_ptr<RedisValue> result)
        {
            result_ = result;
        }
        
        virtual std::string pack() const;

        virtual int unpack(buffer::BufferReader& reader);

        static int unpack_result(std::shared_ptr<RedisValue> &result, buffer::BufferReader& reader, bool unpack_to_map = false);

    public:
        void add_arg(std::shared_ptr<RedisValue> arg)
        {
            args_.push_back(arg);
        }

        void set_unpack_to_map(bool unpack_to_map)
        {
            unpack_to_map_ = unpack_to_map;
        }

    protected:
        bool unpack_to_map_ = false;
        std::string cmd_string_;
        std::vector<std::shared_ptr<RedisValue>> args_;
        std::shared_ptr<RedisValue> result_;
    };

}

#endif // __YUAN_REDIS_DEFAULT_CMD_H__