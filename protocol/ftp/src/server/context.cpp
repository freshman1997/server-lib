#include "server/context.h"

namespace yuan::net::ftp 
{
    std::string ServerContext::get_server_work_dir()
    {
        return work_dir_;
    }

    void ServerContext::set_server_work_dir(const std::string &dir)
    {
        work_dir_ = dir;
    }
    
    short ServerContext::get_next_stream_port()
    {
        return 0;
    }

    void ServerContext::add_stream_port(short port)
    {
        stream_ports_.insert(port);
    }

    void ServerContext::remove_stream_port(short port)
    {
        stream_ports_.erase(port);
    }
}