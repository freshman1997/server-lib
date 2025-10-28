#ifndef __YUAN_REDIS_CLIENT_H__
#define __YUAN_REDIS_CLIENT_H__
#include "command.h"
#include <memory>

namespace yuan::redis 
{
    class RedisClient 
    {
    public:
        RedisClient();
        ~RedisClient();

    public:
        int connect(const std::string &host, int port);
        int connect(const std::string &host, int port, const std::string &password);
        int connect(const std::string &host, int port, const std::string &password, int db);
        int connect(const std::string &host, int port, const std::string &password, int db, int timeout);

        int execute_command(std::shared_ptr<Command> cmd);

    public:
        bool is_connected() const;

        void disconnect();

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}

#endif // __YUAN_REDIS_CLIENT_H__
