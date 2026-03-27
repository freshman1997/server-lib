#include "server/context.h"

#include <filesystem>

namespace yuan::net::ftp
{
    ServerContext::ServerContext() : init_port_(20000), max_port_(21000)
    {
        work_dir_ = std::filesystem::current_path().lexically_normal().generic_string();
    }

    std::string ServerContext::get_server_work_dir() { return work_dir_; }

    void ServerContext::set_server_work_dir(const std::string &dir)
    {
        work_dir_ = dir.empty() ? std::filesystem::current_path().lexically_normal().generic_string() : std::filesystem::path(dir).lexically_normal().generic_string();
    }

    short ServerContext::get_next_stream_port()
    {
        for (short port = init_port_; port <= max_port_; ++port) {
            if (stream_ports_.find(port) == stream_ports_.end()) {
                stream_ports_.insert(port);
                init_port_ = port == max_port_ ? 20000 : static_cast<short>(port + 1);
                return port;
            }
        }
        return 0;
    }

    void ServerContext::add_stream_port(short port) { stream_ports_.insert(port); }
    void ServerContext::remove_stream_port(short port) { stream_ports_.erase(port); }
}
