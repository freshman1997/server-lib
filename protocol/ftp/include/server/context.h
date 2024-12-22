#ifndef __NET_FTP_SERVER_CONTEXT_H__
#define __NET_FTP_SERVER_CONTEXT_H__
#include <set>
#include <string>
#include <memory>

#include "singleton/singleton.h"

namespace yuan::net::ftp 
{
    class ServerContext : public singleton::Singleton<ServerContext>, public std::enable_shared_from_this<ServerContext>
    {
    public:
        /**
         * @brief 获取服务工作目录
         * @return std::string 
         */
        std::string get_server_work_dir();

        /**
         * @brief 设置服务工作目录
         */
        void set_server_work_dir(const std::string &dir);

        short get_next_stream_port();

        void add_stream_port(short port);

        void remove_stream_port(short port);
    
    private:
        std::string work_dir_;
        short init_port_;
        std::set<short> stream_ports_;
    };
}

#endif