#include "pipeline_cmd.h"

#include "default_cmd.h"
#include "internal/def.h"
#include "value/array_value.h"
#include "value/error_value.h"

namespace yuan::redis
{
    std::string PipelineCmd::pack() const
    {
        if (!packed_payload_.empty()) {
            return packed_payload_;
        }

        std::string out;
        for (const auto &cmd : cmds_) {
            out.append(cmd->pack());
        }
        return out;
    }

    int PipelineCmd::unpack(buffer::ByteBufferReader &reader)
    {
        const auto checkpoint = reader.position();
        const std::size_t command_count = packed_command_count_ > 0 ? packed_command_count_ : cmds_.size();
        auto values = std::vector<std::shared_ptr<RedisValue> >();
        values.reserve(command_count);
        for (std::size_t i = 0; i < command_count; ++i) {
            std::shared_ptr<RedisValue> value;
            const int ret = DefaultCmd::unpack_result(value, reader);
            if (ret < 0) {
                if (ret == UnpackCode::need_more_bytes) {
                    reader.restore(checkpoint);
                    result_ = nullptr;
                    return ret;
                }
                result_ = ErrorValue::from_string("ERR: pipeline unpack failed");
                return ret;
            }
            values.push_back(value);
        }

        auto arr = std::make_shared<ArrayValue>();
        arr->set_values(values);
        result_ = arr;
        return 0;
    }
}
