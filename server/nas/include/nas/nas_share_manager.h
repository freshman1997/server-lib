#ifndef __YUAN_SERVER_NAS_SHARE_MANAGER_H__
#define __YUAN_SERVER_NAS_SHARE_MANAGER_H__

#include "nas/nas_types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yuan::server::nas
{
    struct NasResolvedPath
    {
        NasShare share;
        std::string relative_path;
        std::string absolute_path;
    };

    class NasShareManager
    {
    public:
        NasShareManager() = default;
        explicit NasShareManager(std::vector<NasShare> shares);

        void replace(std::vector<NasShare> shares);
        std::optional<NasShare> find_by_name(std::string_view name) const;
        std::optional<NasResolvedPath> resolve(std::string_view share_name, std::string_view relative_path) const;
        const std::vector<NasShare> &shares() const;

        static std::string normalize_relative_path(std::string_view path);

    private:
        std::vector<NasShare> shares_;
    };
}

#endif
