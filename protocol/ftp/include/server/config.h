#ifndef __NET_FTP_SERVER_SERVER_CONFIG_H__
#define __NET_FTP_SERVER_SERVER_CONFIG_H__
#include <string_view>

#include "nlohmann/json.hpp"

namespace net::ftp
{
    extern std::string_view ss;

    class FtpServerConfig
    {
    public:
        FtpServerConfig();

    public:
        const std::string & get_work_dir() const;

    private:
        nlohmann::json config_json_;
    };
}

#endif