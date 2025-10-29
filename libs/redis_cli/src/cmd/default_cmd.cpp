#include "default_cmd.h"
#include "internal/def.h"
#include "value/int_value.h"
#include "value/status_value.h"
#include "value/string_value.h"
#include <sstream>
#include <string>

namespace yuan::redis 
{
    void DefaultCmd::set_args(const std::string &cmd_name, const std::vector<std::shared_ptr<RedisValue>> &args)
    {
        result_ = nullptr;
        cmd_string_ = cmd_name;
        args_.clear();
        args_ = args;
    }

    std::shared_ptr<RedisValue> DefaultCmd::get_result() const
    {
        return result_;
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
        if (end - begin < 1)
        {
            return -1;
        }
        
        char type = *begin;
        switch (type)
        {
        case resp_status:
        {
            const unsigned char *ptr = begin + 1;
            std::string str_value;
            while (ptr < end && *ptr != '\r')
            {
                str_value += static_cast<char>(*ptr);
                ++ptr;
            }
            
            ++ptr;
            if (ptr == end || *ptr != '\n')
            {
                return -1;
            }
            
            ++ptr;
            
            result_ = std::make_shared<StatusValue>(str_value == "OK" ? true : false);
            
            return ptr - end;
        }
        case resp_error:
        case resp_string:
        {
            const unsigned char *ptr = begin + 1;
            std::string str_value;
            while (ptr < end && *ptr != '\r')
            {
                str_value += static_cast<char>(*ptr);
                ++ptr;
            }

            int len = std::atoi(str_value.c_str());
            if (len < 0)
            {
                return ptr - end;;
            }

            str_value.clear();
            ptr += 2; // skip \r\n
            
            while (ptr < end && len > 0)
            {
                str_value += static_cast<char>(*ptr);
                ++ptr;
                --len;
            }

            if (len != 0)
            {
                return -1;
            }

            if (ptr + 2 > end)
            {
                return -1;
            }
            
            ptr += 2; // skip \r\n

            result_ = std::make_shared<StringValue>(str_value);

            return ptr - end;
        }
        case resp_int:
        {
            auto result = std::make_shared<IntValue>();
            const unsigned char *ptr = begin + 1;
            std::string int_str;
            while (ptr < end && *ptr != '\r')
            {
                int_str += static_cast<char>(*ptr);
                ++ptr;
            }

            if (ptr == end || *ptr != '\n')
            {
                return -1;
            }

            ++ptr;

            try 
            { 
                result->set_value(std::stoll(int_str)); 
                result_ = result;
            }
            catch (const std::exception &ignore) 
            { return -1; }
            
            return ptr - begin;
        }
        case resp_null:
        case resp_float:
        case resp_bool:
        case resp_blob_error:
        case resp_verbatim:
        case resp_bigInt:
        case resp_array:
        case resp_map:
        case resp_set:
        case resp_attr:
        case resp_push:
            break;
        }
        return 0;
    }
}