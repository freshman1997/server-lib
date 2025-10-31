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
        StatusValue(bool status) : status_(status) {}
        std::string to_string() const override { return status_ ? "OK" : "FAIL"; }
        virtual char get_type() const override { return resp_status; }
        
        bool get_status() const { return status_; }

        void set_status(bool status) { status_ = status; }

        void set_msg(std::string msg) { msg_ = msg; }

        std::string get_msg() const { return msg_; }

    private:
        bool status_;
        std::string msg_;
    };

}

#endif // __YUAN_REDIS_STATUS_VALUE_H__
