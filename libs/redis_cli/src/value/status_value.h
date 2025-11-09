#ifndef __YUAN_REDIS_STATUS_VALUE_H__
#define __YUAN_REDIS_STATUS_VALUE_H__
#include <string>

#include "../redis_value.h"
#include "../internal/def.h"

namespace yuan::redis 
{
    class StatusValue final : public RedisValue
    {
    public:
        explicit StatusValue(const bool status) : status_(status) {}
        std::string to_string() const override { return status_ ? "OK" : "FAIL"; }
        char get_type() const override { return resp_status; }

        bool is_ok() const { return status_; }

        bool is_fail() const { return !status_; }

        bool is_error() const { return !status_; }

        bool is_true() const { return status_; }

        bool is_false() const { return !status_; }
        
        bool get_status() const { return status_; }

        void set_status(const bool status) { status_ = status; }

    private:
        bool status_;
    };

}

#endif // __YUAN_REDIS_STATUS_VALUE_H__
