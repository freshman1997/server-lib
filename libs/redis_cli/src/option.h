#ifndef __YUAN_REDIS_OPTION_H__
#define __YUAN_REDIS_OPTION_H__

#include <string>
namespace yuan::redis 
{
    struct Option 
    {
        std::string host_;
        int port_ = 6379;
        std::string username_;
        std::string password_;
        int db_ = 0;
        int timeout_ms_ = 2000; // milliseconds
    };

    struct ClusterOption 
    {
        std::string startup_node_host_;
        int startup_node_port_ = 6379;
        std::string username_;
        std::string password_;
        int timeout_ms_ = 2000; // milliseconds
    };

    
}

#endif // __YUAN_REDIS_OPTION_H__