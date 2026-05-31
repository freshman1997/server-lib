#ifndef __YUAN_SERVER_NAS_METADATA_STORE_H__
#define __YUAN_SERVER_NAS_METADATA_STORE_H__

#include "nas/nas_types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace yuan::server::nas
{
    class NasMetadataStore
    {
    public:
        virtual ~NasMetadataStore() = default;

        virtual bool available() const = 0;

        virtual std::optional<NasUser> find_user_by_name(std::string_view username) const = 0;
        virtual std::vector<NasUser> list_users() const = 0;
        virtual bool upsert_user(const NasUser &user) = 0;

        virtual std::optional<NasShare> find_share_by_name(std::string_view share_name) const = 0;
        virtual std::vector<NasShare> list_shares() const = 0;
        virtual bool upsert_share(const NasShare &share) = 0;

        virtual NasPermission permissions_for(std::string_view share_id, std::string_view subject) const = 0;
        virtual bool set_permissions(std::string_view share_id, std::string_view subject, NasPermission permissions) = 0;
        virtual bool remove_permissions(std::string_view share_id, std::string_view subject)
        {
            return set_permissions(share_id, subject, NasPermission::none);
        }

        virtual std::unordered_map<std::string, std::string> dead_properties(std::string_view share_id,
                                                                             std::string_view path) const = 0;
        virtual bool set_dead_property(std::string_view share_id,
                                       std::string_view path,
                                       std::string_view name,
                                       std::string_view value) = 0;
        virtual bool remove_dead_property(std::string_view share_id,
                                          std::string_view path,
                                          std::string_view name) = 0;

        virtual bool upsert_webdav_lock(const NasWebDavLockRecord &lock) = 0;
        virtual bool try_create_webdav_lock(const NasWebDavLockRecord &lock)
        {
            return upsert_webdav_lock(lock);
        }
        virtual std::optional<NasWebDavLockRecord> find_webdav_lock(std::string_view token) const = 0;
        virtual std::vector<NasWebDavLockRecord> list_webdav_locks(std::string_view share_id,
                                                                   std::string_view path) const = 0;
        virtual bool remove_webdav_lock(std::string_view token) = 0;
        virtual std::size_t prune_expired_webdav_locks()
        {
            return 0;
        }

        virtual bool append_audit_event(const NasAuditEvent &event) = 0;
        virtual std::vector<NasAuditEvent> list_audit_events(std::size_t limit) const = 0;

        virtual bool upsert_admin_session(const NasAdminSession &session) = 0;
        virtual std::vector<NasAdminSession> list_admin_sessions(std::size_t limit) const = 0;
    };
}

#endif
