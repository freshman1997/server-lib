#ifndef __YUAN_REDIS_MULTI_CMD_H__
#define __YUAN_REDIS_MULTI_CMD_H__
#include "cmd/default_cmd.h"

namespace yuan::redis 
{
    class MultiCmd : public DefaultCmd
    {
    public:
        void add_command(std::shared_ptr<Command> cmd)
        {
            cmds_.push_back(cmd);
        }

        const std::vector<std::shared_ptr<Command>> & get_commands() const
        {
            return cmds_;
        }

        virtual std::string pack() const;

        virtual int unpack(const unsigned char *begin, const unsigned char *end);
    
    private:
        std::vector<std::shared_ptr<Command>> cmds_;
    };
}

#endif // __YUAN_REDIS_MULTI_CMD_H__