#ifndef __CONNECTION_H__
#define __CONNECTION_H__

namespace yuan::redis 
{
    class RedisConnection 
    {
    public:
        virtual int connect() = 0;
        virtual int close() = 0;
        virtual bool is_connected() const = 0;
        virtual int send(const unsigned char *data, int len) = 0;
        virtual int receive(unsigned char *buffer, int buffer_size, int &received_len) = 0;
    };
}

#endif // __CONNECTION_H__
