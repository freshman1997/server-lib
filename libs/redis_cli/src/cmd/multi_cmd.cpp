#include "multi_cmd.h"
#include "cmd/default_cmd.h"
#include "internal/def.h"
#include "redis_value.h"
#include "value/error_value.h"
#include "value/status_value.h"
#include <memory>
#include <sstream>

namespace yuan::redis 
{
    std::string MultiCmd::pack() const
    {
        std::stringstream ss;

        ss << DefaultCmd::pack();

        for (auto &cmd : cmds_)
        {
            ss << cmd->pack();
        }

        return ss.str();
    }

    int MultiCmd::unpack(const unsigned char *begin, const unsigned char *end)
    {
        if (cmds_.empty())
        {
            result_ = ErrorValue::from_string("ERR: EXEC without MULTI");
            return -1;
        }
        
        auto cmd_size = cmds_.size();
        auto idx = 0;
        auto ptr = begin;
        while (ptr < end && idx < cmd_size)
        {
            std::shared_ptr<RedisValue> cmdResult;
            int ret = DefaultCmd::unpack_result(cmdResult, ptr, end);
            if (ret < 0 || !cmdResult)
            {
                result_ = ErrorValue::from_string("ERR: run multi cmd failed, cmdName: " + cmds_[idx]->get_cmd_name() + ", idx: " + std::to_string(idx - 1));
                return -1;
            }

            if (cmdResult->get_type() != resp_status)
            {
                result_ = ErrorValue::from_string("ERR: internal err, run multi cmd failed");
                return -1;
            }

            StatusValue *status = dynamic_cast<StatusValue *>(cmdResult.get());
            if (!status || !status->get_status())
            {
                result_ = ErrorValue::from_string("ERR: run multi cmd failed");
                return -1;
            }

            ++idx;
            ptr += ret;
        }

        std::shared_ptr<RedisValue> resultList;
        int ret = DefaultCmd::unpack_result(resultList, ptr, end);
        if (ret < 0 || !resultList)
        {
            result_ = ErrorValue::from_string("ERR: run multi cmd failed, cmdName: " + cmds_[idx]->get_cmd_name() + ", idx: " + std::to_string(idx - 1));
            return -1;
        }

        if (resultList->get_type() != resp_array)
        {
            result_ = ErrorValue::from_string("ERR: internal err, run multi cmd failed");
            return -1;
        }

        if (idx != cmd_size)
        {
            result_ = ErrorValue::from_string("ERR: unpack" + std::to_string(idx) + "cmd failed");
            return -1;
        }

        result_ = resultList;

        return 0;
    }
}