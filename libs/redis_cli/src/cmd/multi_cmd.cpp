#include "multi_cmd.h"
#include "cmd/default_cmd.h"
#include "internal/def.h"
#include "redis_value.h"
#include "value/error_value.h"
#include "value/status_value.h"
#include <memory>

namespace yuan::redis 
{
    std::string MultiCmd::pack() const
    {
        std::string out = DefaultCmd::pack();
        for (const auto &cmd : cmds_) {
            if (!cmd) {
                continue;
            }
            out.append(cmd->pack());
        }
        return out;
    }

    int MultiCmd::unpack(buffer::ByteBufferReader& reader)
    {
        const auto checkpoint = reader.position();
        auto need_more = [&]() {
            reader.restore(checkpoint);
            result_ = nullptr;
            return UnpackCode::need_more_bytes;
        };

        if (cmds_.empty())
        {
            result_ = ErrorValue::from_string("ERR: EXEC without MULTI");
            return -1;
        }
        
        auto cmd_size = cmds_.size();
        auto idx = 0;
        while (reader.get_remain_bytes() > 0 && idx < cmd_size)
        {
            std::shared_ptr<RedisValue> cmdResult;
            int ret = DefaultCmd::unpack_result(cmdResult, reader);
            if (ret == UnpackCode::need_more_bytes)
            {
                return need_more();
            }
            if (ret < 0 || !cmdResult)
            {
                result_ = ErrorValue::from_string("ERR: run multi cmd failed, cmdName: " + cmds_[idx]->get_cmd_name() + ", idx: " + std::to_string(idx));
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
        }

        if (reader.get_remain_bytes() <= 0) {
            return need_more();
        }

        std::shared_ptr<RedisValue> resultList;
        int ret = DefaultCmd::unpack_result(resultList, reader);
        if (ret == UnpackCode::need_more_bytes)
        {
            return need_more();
        }
        if (ret < 0 || !resultList)
        {
            const auto cmd_idx = idx < cmd_size ? idx : cmd_size - 1;
            result_ = ErrorValue::from_string("ERR: run multi cmd failed, cmdName: " + cmds_[cmd_idx]->get_cmd_name() + ", idx: " + std::to_string(idx));
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
