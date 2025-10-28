#ifndef __YUAN_REDIS_STRING_CMD_H__
#define __YUAN_REDIS_STRING_CMD_H__

#include "default_cmd.h"
#include "../value/string_value.h"
#include <memory>

namespace yuan::redis 
{
    class StringCmd : public DefaultCmd
    {
    public:
        StringCmd() = default;

    public:
        virtual int unpack(const unsigned char *begin, const unsigned char *end) override;

        virtual std::shared_ptr<RedisValue> get_result() const override
        {
            return value_;
        }

    private:
        std::shared_ptr<StringValue> value_;
    };
}

#endif // __YUAN_REDIS_STRING_CMD_H__