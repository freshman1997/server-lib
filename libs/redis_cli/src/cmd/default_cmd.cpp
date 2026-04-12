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
#include <string>
#include <vector>

namespace yuan::redis 
{
    namespace
    {
        void append_bulk_string(std::string &out, std::string_view value)
        {
            out.push_back('$');
            out.append(std::to_string(value.size()));
            out.append("\r\n");
            out.append(value.data(), value.size());
            out.append("\r\n");
        }
    }

    void DefaultCmd::set_args(const std::string &cmd_name, const std::vector<std::shared_ptr<RedisValue>> &args)
    {
        result_ = nullptr;
        cmd_string_ = cmd_name;
        args_.clear();
        args_.reserve(args.size());
        for (const auto &arg : args) {
            args_.push_back(arg ? arg->to_string() : std::string());
        }
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
        std::size_t estimated_size = 16 + cmd_string_.size();

        for (const auto &arg : args_) {
            estimated_size += 16 + arg.size();
        }

        std::string out;
        out.reserve(estimated_size);
        out.push_back('*');
        out.append(std::to_string(args_.size() + 1));
        out.append("\r\n");
        append_bulk_string(out, cmd_string_);
        for (const auto &arg : args_) {
            append_bulk_string(out, arg);
        }

        return out;
    }

    int DefaultCmd::unpack(buffer::ByteBufferReader& reader)
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

    static int unpack_result_impl(std::shared_ptr<RedisValue> &result, buffer::ByteBufferReader& reader, bool toMap)
    {
        if (reader.get_remain_bytes() < 2)
        {
            return UnpackCode::need_more_bytes;
        }
        
        char type = reader.read_char();
        const bool array_as_map = toMap && type == resp_array;
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
            if (len < 0)
            {
                result = ErrorValue::from_string("null");
                return 0;
            }

            if (reader.get_remain_bytes() < static_cast<std::size_t>(len) + 2)
            {
                return UnpackCode::need_more_bytes;
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

            const char cr = reader.read_char();
            const char lf = reader.read_char();
            if (cr != '\r' || lf != '\n')
            {
                return UnpackCode::format_error;
            }

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
                int ret = DefaultCmd::unpack_result(res, reader, toMap);
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
            if (len < 0)
            {
                return UnpackCode::format_error;
            }

            if (array_as_map && len % 2 != 0)
            {
                return UnpackCode::format_error;
            }

            const int pair_count = array_as_map ? len / 2 : len;
            std::unordered_map<std::string, std::shared_ptr<RedisValue>> res;
            for (int i = 0; i < pair_count; ++i)
            {
                if (reader.get_remain_bytes() <= 0) {
                    return UnpackCode::need_more_bytes;
                }

                char subType = reader.peek_char();
                if (subType != resp_string) {
                    return UnpackCode::format_error;
                }

                std::shared_ptr<RedisValue> key = nullptr;
                ret = DefaultCmd::unpack_result(key, reader);
                if (ret < 0)
                {
                    return ret;
                }

                if (reader.get_remain_bytes() <= 0) {
                    return UnpackCode::need_more_bytes;
                }

                std::shared_ptr<RedisValue> value = nullptr;
                ret = DefaultCmd::unpack_result(value, reader);
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

            if (static_cast<std::size_t>(pair_count) != res.size()) {
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

    int DefaultCmd::unpack_result(std::shared_ptr<RedisValue> &result, buffer::ByteBufferReader& reader, bool toMap)
    {
        const auto checkpoint = reader.position();
        int ret = unpack_result_impl(result, reader, toMap);
        if (ret == UnpackCode::need_more_bytes)
        {
            reader.restore(checkpoint);
            result = nullptr;
        }
        return ret;
    }
}
