#ifndef NET_FTP_SERVER_CONTEXT_H
#define NET_FTP_SERVER_CONTEXT_H
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "singleton/singleton.h"

namespace yuan::net::ftp
{
    class ServerContext : public singleton::Singleton<ServerContext>, public std::enable_shared_from_this<ServerContext>
    {
    public:
        ServerContext();
        std::string get_server_work_dir();
        void set_server_work_dir(const std::string &dir);
        void set_stream_port_range(short start_port, short end_port);
        short get_next_stream_port();
        void add_stream_port(short port);
        void remove_stream_port(short port);

        void set_auth_credential(const std::string &username, const std::string &password);
        void clear_auth_credential();
        bool is_auth_required() const;
        bool verify_user(const std::string &username, const std::string &password) const;

    private:
        mutable std::mutex mutex_;
        std::string work_dir_;
        short min_port_;
        short init_port_;
        short max_port_;
        std::set<short> stream_ports_;
        bool auth_required_ = false;
        std::string username_;
        std::string password_;
    };
}

#endif
