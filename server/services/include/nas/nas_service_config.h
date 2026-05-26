#ifndef __SERVER_NAS_SERVICE_CONFIG_H__
#define __SERVER_NAS_SERVICE_CONFIG_H__

#include "http/http_service.h"
#include "nas/nas.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yuan::server
{
    struct NasServiceConfig
    {
        struct SmbConfig
        {
            bool enabled = false;
            int port = 445;
            bool require_signing = false;
            bool enable_encryption = false;
            std::string server_name = "YUAN-NAS";
            std::string domain_name = "WORKGROUP";
        };

        bool production_mode = false;
        int port = 8080;
        yuan::net::http::HttpServerConfig http;
        yuan::server::nas::NasConfig nas;
        SmbConfig smb;
        std::shared_ptr<yuan::server::nas::NasMetadataStore> metadata;
        std::vector<yuan::server::nas::NasUser> bootstrap_users;
    };

    std::optional<NasServiceConfig> load_nas_service_config(const std::filesystem::path &path);
}

#endif
