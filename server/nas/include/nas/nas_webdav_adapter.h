#ifndef __YUAN_SERVER_NAS_WEBDAV_ADAPTER_H__
#define __YUAN_SERVER_NAS_WEBDAV_ADAPTER_H__

#include "nas/nas_share_manager.h"
#include "nas/nas_types.h"

#include <optional>
#include <string>
#include <string_view>

namespace yuan::server::nas
{
    struct NasWebDavRoute
    {
        std::string share_name;
        std::string relative_path;
    };

    class NasWebDavAdapter
    {
    public:
        explicit NasWebDavAdapter(std::string mount_path = "/dav");

        std::optional<NasWebDavRoute> parse_route(std::string_view request_path) const;
        std::optional<NasResolvedPath> resolve(const NasShareManager &shares, std::string_view request_path) const;
        const std::string &mount_path() const;

    private:
        static std::string normalize_mount(std::string mount_path);

        std::string mount_path_;
    };
}

#endif
