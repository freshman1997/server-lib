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
#include "internal/utils.h"

#include <memory>
#include <sstream>
#include <string>
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

    int DefaultCmd::unpack(buffer::BufferReader& reader)
    {
        int ret = DefaultCmd::unpack_result(result_, reader, unpack_to_map_);
        if (ret >= 0) {
            if (reader.get_remain_bytes() <= 0) {
                return ret;
            }

            const auto arr = std::make_shared<ArrayValue>();
            arr->add_value(result_);
            while  (reader.get_remain_bytes() > 0) {
                std::shared_ptr<RedisValue> result;
                ret = unpack_result(result, reader, unpack_to_map_);
                if (ret < 0) {
                    return ret;
                }
                
                arr->add_value(result);
            }

            result_ = arr;
            
            return 0;
        }

        return ret;
    }

    int DefaultCmd::unpack_result(std::shared_ptr<RedisValue> &result, buffer::BufferReader& reader, bool toMap)
    {
        if (reader.get_remain_bytes() < 2)
        {
            return UnpackCode::need_more_bytes;
        }
        
        char type = reader.read_char();
        if (toMap && type == resp_array)
        {
            type = resp_map;
        }

        switch (type)
        {
        case resp_status:
        {
            std::string str_value;
            if (int ret = reader.read_line(str_value); ret < 0)
            {
                return ret;
            }

            auto status = std::make_shared<StatusValue>(false);
            if (str_value == "OK" || str_value == "QUEUED" || str_value == "SUCCESS" || str_value == "PONG")
            {
                status->set_status(true);
            }

            status->set_raw_str(str_value);
            result = status;

            return 0;
        }
        case resp_error:
        {
            std::string str_value;
            if (int ret = reader.read_line(str_value); ret < 0)
            {
                return ret;
            }

            result = std::make_shared<ErrorValue>(str_value);
            return 0;
        }
        case resp_string:
        {
            std::string str_value;
            int ret = reader.read_line(str_value);
            if (ret < 0)
            {
                return ret;
            }

            int len = std::atoi(str_value.c_str());
            if (len < 0 || reader.get_remain_bytes() < len)
            {
                return 0;
            }

            auto pstr = std::make_shared<StringValue>(str_value);
            auto &str = pstr->get_value(); 
            str.resize(len);
            ret = reader.read(str.data(), len);
            if (ret < 0)
            {
                return ret;
            }

            result = pstr;

            // skip \r\n
            str_value.clear();
            reader.read_line(str_value);

            return 0;
        }
        case resp_int:
        {
            std::string str_value;
            if (int ret = reader.read_line(str_value); ret < 0)
            {
                return ret;
            }
            
            try 
            { 
                auto intResult = std::make_shared<IntValue>(std::stoll(str_value));
                result = intResult;
            }
            catch (const std::exception &ignore) 
            { 
                return UnpackCode::format_error; 
            }
            
            return 0;
        }

        case resp_null:
        {
            result = ErrorValue::from_string("null");
            std::string str_value;
            return reader.read_line(str_value);
        }

        case resp_float:
        {
            std::string str_value;
            if (int ret = reader.read_line(str_value); ret < 0)
            {
                return ret;
            }
            
            result = std::make_shared<FloatValue>(RedisDoubleConverter::convertSafe(str_value));
            result->set_raw_str(str_value);

            return 0;
        }
        case resp_bool:
        {
            std::string str_value;
            if (int ret = reader.read_line(str_value); ret < 0)
            {
                return ret;
            }

            if (str_value.empty()) {
                return UnpackCode::format_error;
            }

            result = std::make_shared<StatusValue>(str_value[0] == 't');
            result->set_raw_str(str_value);

            return 0;
        }
        case resp_blob_error:
        case resp_verbatim:
        case resp_bigInt:
            return -2;
        
        case resp_push:
        case resp_array:
        {
            std::string str_value;
            if (int ret = reader.read_line(str_value); ret < 0)
            {
                return ret;
            }
            
            int len = std::atoi(str_value.c_str());
            if (len < 0)
            {
                return UnpackCode::format_error;
            }

            auto arrResult = std::make_shared<ArrayValue>();
            for (int i = 0; i < len; ++i)
            {
                std::shared_ptr<RedisValue> res = nullptr;
                int ret = unpack_result(res, reader, toMap);
                if (ret < 0)
                {
                    return ret;
                }

                arrResult->add_value(res);
            }

            result = arrResult;

            return 0;
        }
        case resp_map:
        {
            std::string str_value;
            int ret = reader.read_line(str_value);
            if (ret < 0)
            {
                return ret;
            }

            int len = std::atoi(str_value.c_str());
            if (len < 0 || len % 2 != 0)
            {
                return UnpackCode::format_error;
            }

            std::unordered_map<std::string, std::shared_ptr<RedisValue>> res;
            while (reader.get_remain_bytes() > 0)
            {
                char subType = reader.peek_char();
                if (subType != resp_string) {
                    return UnpackCode::format_error;
                }

                std::shared_ptr<RedisValue> key = nullptr;
                ret = unpack_result(key, reader);
                if (ret < 0)
                {
                    return ret;
                }

                if (reader.get_remain_bytes() <= 0) {
                    return UnpackCode::need_more_bytes;
                }

                std::shared_ptr<RedisValue> value = nullptr;
                ret = unpack_result(value, reader);
                if (ret < 0)
                {
                    return ret;
                }
                
                if (key->get_type() != resp_string) {
                    return UnpackCode::format_error;
                }

                const std::string &key_str = key->to_string();
                if (res.contains(key_str)) {
                    return UnpackCode::format_error;
                }

                res[key_str] = value;
            }

            if (len / 2 != res.size()) {
                return UnpackCode::format_error;
            }
            
            result = std::make_shared<MapValue>(res);

            return 0;
        }
        case resp_set:
        case resp_attr:
        default:
            result = ErrorValue::from_string("unknown type");
            return -1;
        }

        return 0;
    }
}