#ifndef __YUAN_REDIS_STATUS_VALUE_H__
#define __YUAN_REDIS_STATUS_VALUE_H__
#include <string>

#include "../redis_value.h"
#include "../internal/def.h"

namespace yuan::redis 
{
    class StatusValue : public RedisValue
    {
    public:
        StatusValue(const std::string &status) : status_(status) {}
        std::string to_string() const override { return status_; }
        virtual char get_type() const override { return resp_string; }
        
        std::string get_status() const { return status_; }

    private:
        std::string status_;
    };

}

#endif // __YUAN_REDIS_STATUS_VALUE_H__
