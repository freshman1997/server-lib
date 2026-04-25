#ifndef __YUAN_SERVER_NAS_AUTH_SERVICE_H__
#define __YUAN_SERVER_NAS_AUTH_SERVICE_H__

#include "nas/nas_metadata_store.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace yuan::server::nas
{
    struct NasAuthResult
    {
        bool authenticated = false;
        NasUser user;
        std::string error;
    };

    class NasAuthService
    {
    public:
        explicit NasAuthService(std::shared_ptr<NasMetadataStore> metadata);

        NasAuthResult authenticate_password(std::string_view username, std::string_view password) const;
        NasAuthResult authenticate_basic_header(std::string_view authorization_header) const;

        static std::string hash_password_for_config(std::string_view password, std::string_view salt);
        static bool verify_password(std::string_view stored_hash, std::string_view password);
        static std::optional<std::pair<std::string, std::string>> parse_basic_header(std::string_view authorization_header);

    private:
        std::shared_ptr<NasMetadataStore> metadata_;
    };
}

#endif
