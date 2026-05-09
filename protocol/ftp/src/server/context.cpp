#include "server/context.h"

#include <filesystem>

namespace yuan::net::ftp
{
    ServerContext::ServerContext()
        : min_port_(20000), init_port_(20000), max_port_(21000)
    {
        work_dir_ = std::filesystem::current_path().lexically_normal().generic_string();
    }

    std::string ServerContext::get_server_work_dir()
    {
        return work_dir_;
    }

    void ServerContext::set_server_work_dir(const std::string & dir)
    {
        work_dir_ = dir.empty() ? std::filesystem::current_path().lexically_normal().generic_string() : std::filesystem::path(dir).lexically_normal().generic_string();
    }

    void ServerContext::set_stream_port_range(short start_port, short end_port)
    {
        if (start_port <= 0 || end_port <= 0 || start_port > end_port) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        min_port_ = start_port;
        init_port_ = start_port;
        max_port_ = end_port;
        stream_ports_.clear();
    }

    short ServerContext::get_next_stream_port()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (short port = init_port_; port <= max_port_; ++port) {
            if (stream_ports_.find(port) == stream_ports_.end()) {
                stream_ports_.insert(port);
                init_port_ = port == max_port_ ? min_port_ : static_cast<short>(port + 1);
                return port;
            }
        }
        return 0;
    }

    void ServerContext::add_stream_port(short port)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_ports_.insert(port);
    }

    void ServerContext::remove_stream_port(short port)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_ports_.erase(port);
    }

    void ServerContext::set_auth_credential(const std::string & username, const std::string & password)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        username_ = username;
        password_ = password;
        auth_required_ = !username_.empty();
    }

    void ServerContext::clear_auth_credential()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auth_required_ = false;
        username_.clear();
        password_.clear();
    }

    bool ServerContext::is_auth_required() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return auth_required_;
    }

    bool ServerContext::verify_user(const std::string & username, const std::string & password) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!auth_required_) {
            return true;
        }
        return username == username_ && password == password_;
    }
}
