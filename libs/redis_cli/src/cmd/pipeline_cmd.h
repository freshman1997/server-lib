#ifndef __YUAN_REDIS_PIPELINE_CMD_H__
#define __YUAN_REDIS_PIPELINE_CMD_H__

#include "command.h"

#include <vector>

namespace yuan::redis
{
    class PipelineCmd final : public Command
    {
    public:
        void add_command(const std::shared_ptr<Command> &cmd)
        {
            if (cmd) {
                cmds_.push_back(cmd);
            }
        }

        const std::vector<std::shared_ptr<Command> > &get_commands() const
        {
            return cmds_;
        }

        void set_args(const std::string &, const std::vector<std::shared_ptr<RedisValue> > &) override
        {
        }

        std::string get_cmd_name() const override
        {
            return "pipeline";
        }

        std::shared_ptr<RedisValue> get_result() const override
        {
            return result_;
        }

        void set_result(std::shared_ptr<RedisValue> result) override
        {
            result_ = std::move(result);
        }

        std::string pack() const override;
        int unpack(buffer::ByteBufferReader &reader) override;

    private:
        std::vector<std::shared_ptr<Command> > cmds_;
        std::shared_ptr<RedisValue> result_;
    };
}

#endif // __YUAN_REDIS_PIPELINE_CMD_H__
