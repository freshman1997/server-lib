#ifndef __YUAN_REDIS_CLIENT_H__
#define __YUAN_REDIS_CLIENT_H__
#include "command.h"

namespace yuan::redis 
{
    class RedisClient 
    {
    public:
        virtual int execute_command(Command &cmd, const unsigned char *cmd_data, int cmd_len) = 0;
    };
}

#endif // __YUAN_REDIS_CLIENT_H__
