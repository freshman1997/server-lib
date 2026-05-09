#ifndef __YUAN_SERVER_NAS_REDIS_METADATA_STORE_H__
#define __YUAN_SERVER_NAS_REDIS_METADATA_STORE_H__

#include "nas/nas_metadata_store.h"

#include <memory>
#include <string>

namespace yuan::redis
{
    class RedisClient;
}

namespace yuan::server::nas
{
    class NasRedisMetadataStore : public NasMetadataStore
    {
    public:
        explicit NasRedisMetadataStore(NasRedisConfig config);
        NasRedisMetadataStore(NasRedisConfig config, std::shared_ptr<yuan::redis::RedisClient> client);

        bool init();
        bool available() const override;

        std::optional<NasUser> find_user_by_name(std::string_view username) const override;
        std::vector<NasUser> list_users() const override;
        bool upsert_user(const NasUser &user) override;

        std::optional<NasShare> find_share_by_name(std::string_view share_name) const override;
        std::vector<NasShare> list_shares() const override;
        bool upsert_share(const NasShare &share) override;

        NasPermission permissions_for(std::string_view share_id, std::string_view subject) const override;
        bool set_permissions(std::string_view share_id, std::string_view subject, NasPermission permissions) override;

        std::unordered_map<std::string, std::string> dead_properties(std::string_view share_id,
                                                                     std::string_view path) const override;
        bool set_dead_property(std::string_view share_id,
                               std::string_view path,
                               std::string_view name,
                               std::string_view value) override;
        bool remove_dead_property(std::string_view share_id,
                                  std::string_view path,
                                  std::string_view name) override;

        bool upsert_webdav_lock(const NasWebDavLockRecord &lock) override;
        bool try_create_webdav_lock(const NasWebDavLockRecord &lock);
        std::optional<NasWebDavLockRecord> find_webdav_lock(std::string_view token) const override;
        std::vector<NasWebDavLockRecord> list_webdav_locks(std::string_view share_id,
                                                           std::string_view path) const override;
        bool remove_webdav_lock(std::string_view token) override;

        bool append_audit_event(const NasAuditEvent &event) override;
        std::vector<NasAuditEvent> list_audit_events(std::size_t limit) const override;
        bool upsert_admin_session(const NasAdminSession &session) override;
        std::vector<NasAdminSession> list_admin_sessions(std::size_t limit) const override;

        std::string key(std::string_view suffix) const;
        std::string user_key(std::string_view user_id) const;
        std::string user_by_name_key(std::string_view username) const;
        std::string share_key(std::string_view share_id) const;
        std::string share_by_name_key(std::string_view share_name) const;
        std::string acl_key(std::string_view share_id) const;
        std::string dead_property_key(std::string_view share_id, std::string_view path) const;
        std::string lock_key(std::string_view token) const;
        std::string lock_guard_key(std::string_view token) const;
        std::string lock_index_key(std::string_view share_id) const;
        std::string admin_session_key(std::string_view session_id) const;

    private:
        std::optional<NasUser> load_user_by_id(std::string_view user_id) const;
        std::optional<NasShare> load_share_by_id(std::string_view share_id) const;
        std::optional<NasWebDavLockRecord> load_lock_by_token(std::string_view token) const;
        std::optional<NasAdminSession> load_admin_session_by_id(std::string_view session_id) const;
        static std::string normalize_path_key(std::string_view path);
        static std::string permission_to_string(NasPermission permissions);
        static NasPermission permission_from_string(std::string_view text);

        NasRedisConfig config_;
        std::shared_ptr<yuan::redis::RedisClient> client_;
    };
}

#endif
