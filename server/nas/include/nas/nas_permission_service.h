#ifndef __YUAN_SERVER_NAS_PERMISSION_SERVICE_H__
#define __YUAN_SERVER_NAS_PERMISSION_SERVICE_H__

#include "nas/nas_types.h"

#include "request.h"

#include <string_view>

namespace yuan::server::nas
{
    class NasPermissionService
    {
    public:
        bool allowed(const NasShare &share, std::string_view subject, NasPermission required) const
        {
            if (!share.enabled) {
                return false;
            }
            if (share.readonly && ((required & (NasPermission::write | NasPermission::remove)) != NasPermission::none)) {
                return false;
            }
            auto it = share.subject_permissions.find(std::string(subject));
            const NasPermission mask = it == share.subject_permissions.end() ? share.default_permissions : it->second;
            return has_permission(mask, required);
        }

        bool allowed(const NasShare &share, const NasUser &user, NasPermission required) const
        {
            if (user.admin) {
                return share.enabled;
            }
            return allowed(share, user.id, required);
        }

        static NasPermission required_for_webdav_request(const yuan::net::http::HttpRequest &req)
        {
            if (req.is_get() || req.is_head() || req.is_propfind() || req.is_report() || req.is_search()) {
                return NasPermission::read;
            }
            if (req.is_delete()) {
                return NasPermission::remove;
            }
            if (req.is_put() || req.is_mkcol() || req.is_proppatch() || req.is_copy() ||
                req.is_move() || req.is_lock() || req.is_unlock() || req.is_patch() || req.is_post()) {
                return NasPermission::write;
            }
            if (req.is_acl()) {
                return NasPermission::admin;
            }
            return NasPermission::read;
        }
    };
}

#endif
