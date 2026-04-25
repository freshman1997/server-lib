#ifndef __YUAN_SERVER_NAS_REDIS_WEBDAV_LOCK_MANAGER_H__
#define __YUAN_SERVER_NAS_REDIS_WEBDAV_LOCK_MANAGER_H__

#include "nas/nas_metadata_store.h"
#include "nas/nas_share_manager.h"
#include "nas/nas_webdav_adapter.h"

#include "webdav_lock_manager.h"

#include <memory>

namespace yuan::server::nas
{
    class NasRedisWebDavLockManager final : public yuan::net::webdav::WebDavLockManager
    {
    public:
        NasRedisWebDavLockManager(std::shared_ptr<NasShareManager> shares,
                                  std::shared_ptr<NasMetadataStore> metadata,
                                  std::string mount_path = "/dav");

        yuan::net::webdav::LockInfo create(std::string href,
                                           yuan::net::webdav::LockScope scope,
                                           yuan::net::webdav::Depth depth,
                                           std::string owner,
                                           std::chrono::seconds timeout) override;
        bool refresh(std::string_view token, std::chrono::seconds timeout) override;
        bool unlock(std::string_view token) override;
        bool allows(std::string_view href, std::string_view if_header_or_token) const override;
        std::vector<yuan::net::webdav::LockInfo> active_locks(std::string_view href) const override;
        std::optional<yuan::net::webdav::LockInfo> find(std::string_view token) const override;
        void prune_expired() const override;

    private:
        std::optional<NasWebDavRoute> route_of(std::string_view href) const;
        static std::string normalize_token(std::string_view token);
        static std::string make_token();
        static std::int64_t unix_ms_after(std::chrono::seconds timeout);
        static bool covers(const NasWebDavLockRecord &lock, std::string_view path);
        static yuan::net::webdav::LockInfo to_lock_info(const NasWebDavLockRecord &record);

        std::shared_ptr<NasShareManager> shares_;
        std::shared_ptr<NasMetadataStore> metadata_;
        NasWebDavAdapter adapter_;
    };
}

#endif
