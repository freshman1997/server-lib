#ifndef __YUAN_REDIS_OPTION_H__
#define __YUAN_REDIS_OPTION_H__

#include <cstddef>
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
        int timeout_ms_ = 0; // milliseconds
        std::size_t max_buffered_response_bytes_ = 16 * 1024 * 1024;
        std::size_t max_pubsub_pending_messages_ = 65536;
        std::string name_;
    };
}

#endif // __YUAN_REDIS_OPTION_H__
