#ifndef __YUAN_REDIS_INT_CMD_H__
#define __YUAN_REDIS_INT_CMD_H__
#include "command.h"
#include <cstdint>

namespace yuan::redis 
{
    class IntCmd : public Command
    {
    public:
        virtual int on_reply(const unsigned char *begin, const unsigned char *end);

    private:
        uint64_t value_;
    };
}

#endif // __YUAN_REDIS_INT_CMD_H__