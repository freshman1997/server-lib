#include "error_value.h"
#include "internal/def.h"

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> ErrorValue::to_error(const unsigned char *begin, const unsigned char *end)
    {
        char type = *begin;
        switch (type)
        {
        case resp_string:
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
                return nullptr;
            }

            ++ptr;

            int len = std::atoi(str_value.c_str());
            if (len < 0)
            {
                return std::make_shared<ErrorValue>("null");
            }

            return nullptr;
        }
        case resp_null:
        {
            return std::make_shared<ErrorValue>("null");
        }
        }
        return nullptr;
    }

}