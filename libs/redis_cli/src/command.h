#ifndef __YUAN_REDIS_COMMAND_H__
#define __YUAN_REDIS_COMMAND_H__
#include <string>
#include <vector>
#include <memory>

#include "buffer/buffer_reader.h"
#include "redis_value.h"

namespace yuan::redis 
{
    class Command 
    {
    public:
        virtual ~Command() = default;
        
    public:
        virtual void set_args(const std::string &cmd_name, const std::vector<std::shared_ptr<RedisValue>> &args) = 0;

        virtual std::string get_cmd_name() const  = 0;

        virtual std::shared_ptr<RedisValue> get_result() const = 0;

        virtual void set_result(std::shared_ptr<RedisValue> result) = 0;

        virtual std::string pack() const = 0;

        virtual int unpack(buffer::BufferReader& reader) = 0;
    };
}

#endif // __YUAN_REDIS_COMMAND_H__
