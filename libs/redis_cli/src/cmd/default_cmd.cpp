#include "default_cmd.h"
#include "internal/def.h"
#include "redis_value.h"
#include "value/array_value.h"
#include "value/error_value.h"
#include "value/int_value.h"
#include "value/status_value.h"
#include "value/string_value.h"
#include "value/float_value.h"
#include "value/map_value.h"

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::redis 
{
    void DefaultCmd::set_args(const std::string &cmd_name, const std::vector<std::shared_ptr<RedisValue>> &args)
    {
        result_ = nullptr;
        cmd_string_ = cmd_name;
        args_.clear();
        args_ = args;
    }

    std::string DefaultCmd::get_cmd_name() const
    {
        return cmd_string_;
    }

    std::shared_ptr<RedisValue> DefaultCmd::get_result() const
    {
        return result_;
    }
    
    std::string DefaultCmd::pack() const
    {
        std::stringstream ss;
        ss << "*" << (args_.size() + 1) << "\r\n";
        ss << "$" << cmd_string_.size() << "\r\n";
        ss << cmd_string_ << "\r\n";
        for (auto &arg : args_)
        {
            const auto &tmp = arg->to_string();
            ss << "$" << tmp.size() << "\r\n";
            ss << tmp << "\r\n";
        }
        
        return ss.str();
    }

    int DefaultCmd::unpack(const unsigned char *begin, const unsigned char *end)
    {
        return DefaultCmd::unpack_result(result_, begin, end, unpack_to_map_);
    }

    int DefaultCmd::unpack_result(std::shared_ptr<RedisValue> &result, const unsigned char *begin, const unsigned char *end, bool toMap)
    {
        if (end - begin < 1)
        {
            return -1;
        }
        
        char type = *begin;
        if (toMap && type == resp_array)
        {
            type = resp_map;
        }

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
            
            auto status = std::make_shared<StatusValue>(true);
            if (str_value == "OK" || str_value == "QUEUED")
            {
                status->set_status(true);
            }

            status->set_msg(str_value);
            result = status;

            return ptr - begin;
        }
        case resp_error:
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
            result = std::make_shared<ErrorValue>(str_value);
            
            return ptr - begin;
        }
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

            result = std::make_shared<StringValue>(str_value);

            return ptr - begin;
        }
        case resp_int:
        {
            const unsigned char *ptr = begin + 1;
            std::string int_str;
            while (ptr < end && *ptr != '\r')
            {
                int_str += static_cast<char>(*ptr);
                ++ptr;
            }

            ++ptr;
            if (ptr == end || *ptr != '\n')
            {
                return -1;
            }

            ++ptr;

            auto intResult = std::make_shared<IntValue>();
            try 
            { 
                intResult->set_value(std::stoll(int_str)); 
                result = intResult;
            }
            catch (const std::exception &ignore) 
            { return -1; }
            
            return ptr - begin;
        }

        case resp_null:
        {
            result = ErrorValue::from_string("null");
            ++begin;
            if (begin + 2 > end)
            {
                return -1;
            }
            
            if (begin[0] != '\r' || begin[1] != '\n')
            {
                return -1;
            }
            
            return 3;
        }

        case resp_float:
        {
            const unsigned char *ptr = begin + 1;
            std::string float_str;
            while (ptr < end && *ptr != '\r')
            {
                float_str += static_cast<char>(*ptr);
                ++ptr;
            }
            
            ++ptr;
            if (ptr == end || *ptr != '\n')
            {
                return -1;
            }
            
            ++ptr;
            
            result = std::make_shared<FloatValue>(float_str);

            return ptr - begin;
        }
        case resp_bool:
        {
            char res = *(begin + 1);
            ++begin;
            if (begin + 2 > end)
            {
                return -1;
            }
            
            if (begin[0] != '\r' || begin[1] != '\n')
            {
                return -1;
            }

            result = std::make_shared<StatusValue>(res == 't');

            return 3;
        }
        case resp_blob_error:
        case resp_verbatim:
        case resp_bigInt:
        case resp_array:
        {
            const unsigned char *ptr = begin + 1;
            std::string int_str;
            while (ptr < end && *ptr != '\r')
            {
                int_str += static_cast<char>(*ptr);
                ++ptr;
            }
            
            ++ptr;
            if (ptr == end || *ptr != '\n')
            {
                return -1;
            }
            
            ++ptr;
            
            int len = std::atoi(int_str.c_str());
            if (len < 0)
            {
                return -1;
            }

            auto arrResult = std::make_shared<ArrayValue>();
            for (int i = 0; i < len; ++i)
            {
                std::shared_ptr<RedisValue> res = nullptr;
                int ret = unpack_result(res, ptr, end);
                if (ret < 0)
                {
                    return -1;
                }

                arrResult->add_value(res);
                ptr += ret;
            }

            result = arrResult;
            return ptr - begin;
        }
        case resp_map:
        {
            const unsigned char *ptr = begin + 1;

            std::string len_str;
            while (ptr < end && *ptr != '\r')
            {
                len_str += static_cast<char>(*ptr);
                ++ptr;
            }

            ++ptr;

            if (ptr == end || *ptr != '\n')
            {
                return -1;
            }
            ++ptr;

            int len = std::atoi(len_str.c_str());
            if (len < 0 || len % 2 != 0)
            {
                return -1;
            }

            std::unordered_map<std::string, std::shared_ptr<RedisValue>> res;
            while (ptr < end)
            {
                if (*ptr != resp_string) {
                    return -1;
                }

                std::shared_ptr<RedisValue> key = nullptr;
                int ret = unpack_result(key, ptr, end);
                if (ret < 0)
                {
                    return -1;
                }

                ptr += ret;
                if (ptr == end) {
                    return -1;
                }

                std::shared_ptr<RedisValue> value = nullptr;
                ret = unpack_result(value, ptr, end);
                if (ret < 0)
                {
                    return -1;
                }
                
                if (key->get_type() != resp_string) {
                    return -1;
                }

                const std::string &key_str = key->to_string();
                if (res.find(key_str) != res.end()) {
                    return -1;
                }

                res[key_str] = value;
                ptr += ret;
            }

            if (len / 2 != res.size()) {
                return -1;
            }
            
            result = std::make_shared<MapValue>(res);

            return ptr - begin;
        }
        case resp_set:
        case resp_attr:
        case resp_push:
            break;
        default:
            result = ErrorValue::from_string("unknown type");
            return -1;
        }

        return 0;
    }
}