#ifndef __NET_FTP_CLIENT_CONFIG_H__
#define __NET_FTP_CLIENT_CONFIG_H__
#include <string_view>
#include "nlohmann/json.hpp"

namespace yuan::net::ftp 
{
    extern std::string_view config_file_path_;
    extern std::string_view config_key_read_amount_;
    extern std::string_view config_key_idle_timeout_;

    class FtpClientConfig
    {
    public:
        std::size_t get_read_amount();

        std::size_t get_idle_timeout();

    private:
        nlohmann::json config_json_;
    };
}

#endif