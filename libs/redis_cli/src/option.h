#ifndef __YUAN_REDIS_OPTION_H__
#define __YUAN_REDIS_OPTION_H__

#include <string>
namespace yuan::redis 
{
    struct Option 
    {
        std::string host_ = std::string("localhost");
        int port_ = 6379;
        std::string username_;
        std::string password_;
        int db_ = 0;
        int timeout_ms_ = 2000; // milliseconds
        std::string name_;
    };
}

#endif // __YUAN_REDIS_OPTION_H__