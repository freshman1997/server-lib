#ifndef __SERVER_NAS_SERVICE_READINESS_H__
#define __SERVER_NAS_SERVICE_READINESS_H__

#include "nas/nas_service_config.h"

#include <nlohmann/json.hpp>

namespace yuan::server
{
    struct NasServiceReadinessInput
    {
        const NasServiceConfig &config;
        bool initialized = false;
        bool started = false;
        bool mounted = false;
        bool metadata_available = false;
        std::vector<yuan::server::nas::NasUser> users;
        std::vector<yuan::server::nas::NasShare> shares;
    };

    nlohmann::json build_nas_service_readiness_json(NasServiceReadinessInput input);
}

#endif
