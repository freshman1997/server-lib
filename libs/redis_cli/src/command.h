#ifndef __YUAN_REDIS_COMMAND_H__
#define __YUAN_REDIS_COMMAND_H__

namespace yuan::redis 
{
    class Command 
    {
    public:
        virtual ~Command() = default;
        
    public:
        virtual int on_reply(const unsigned char *begin, const unsigned char *end) = 0;
    };
}

#endif // __YUAN_REDIS_COMMAND_H__
