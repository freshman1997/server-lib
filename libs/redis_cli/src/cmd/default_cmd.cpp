#include "default_cmd.h"
#include <sstream>
#include <string>

namespace yuan::redis 
{
    void DefaultCmd::set_args(const std::string &cmd_name, const std::vector<std::shared_ptr<RedisValue>> &args)
    {
        cmd_string_ = cmd_name;
        args_.clear();
        args_ = args;
    }

    std::shared_ptr<RedisValue> DefaultCmd::get_result() const
    {
        return nullptr;
    }
    
    std::string DefaultCmd::pack() const
    {
        std::stringstream ss;
        ss << cmd_string_;
        for (const auto &arg : args_)
        {
            ss << " " << arg->to_string();
        }
        ss << "\r\n";
        return ss.str();
    }

    int DefaultCmd::unpack(const unsigned char *begin, const unsigned char *end)
    {
        return 0;
    }
}