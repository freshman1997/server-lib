#include "client/config.h"

namespace yuan::net::ftp 
{
    std::string_view config_file_path_ = "./ftp_cli.json";
    std::string_view config_key_read_amount_ = "read_amount";
    std::string_view config_key_idle_timeout_ = "idle_timeout";

    std::size_t FtpClientConfig::get_read_amount()
    {
        auto it = config_json_.find(config_key_read_amount_);
        return it != config_json_.end() && it.value().is_number_integer() ? static_cast<std::size_t>(it.value()) : 1024 * 1024;
    }

    std::size_t FtpClientConfig::get_idle_timeout()
    {
        auto it = config_json_.find(config_key_idle_timeout_);
        return it != config_json_.end() && it.value().is_number_integer() ? static_cast<std::size_t>(it.value()) : 10 * 1000 ;
    }
}