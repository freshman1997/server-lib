#include "nas/nas_redis_webdav_lock_manager.h"

#include <chrono>
#include <atomic>
#include <sstream>

namespace yuan::server::nas
{
    namespace
    {
        std::int64_t now_unix_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        bool lock_is_active(const NasWebDavLockRecord &record)
        {
            return record.expires_at_unix_ms <= 0 || record.expires_at_unix_ms > now_unix_ms();
        }

        std::chrono::steady_clock::time_point to_steady_expiry(std::int64_t unix_ms)
        {
            if (unix_ms <= 0) {
                return std::chrono::steady_clock::now();
            }
            const auto sys_now = std::chrono::system_clock::now();
            const auto steady_now = std::chrono::steady_clock::now();
            const auto target = std::chrono::system_clock::time_point(std::chrono::milliseconds(unix_ms));
            if (target <= sys_now) {
                return steady_now;
            }
            return steady_now + (target - sys_now);
        }
    }

    NasRedisWebDavLockManager::NasRedisWebDavLockManager(std::shared_ptr<NasShareManager> shares,
                                                         std::shared_ptr<NasMetadataStore> metadata,
                                                         std::string mount_path)
        : shares_(std::move(shares)),
          metadata_(std::move(metadata)),
          adapter_(std::move(mount_path))
    {
    }

    yuan::net::webdav::LockInfo NasRedisWebDavLockManager::create(std::string href,
                                                                  yuan::net::webdav::LockScope scope,
                                                                  yuan::net::webdav::Depth depth,
                                                                  std::string owner,
                                                                  std::chrono::seconds timeout)
    {
        prune_expired();

        auto make_failed_lock = [&](std::string failed_href) {
            yuan::net::webdav::LockInfo info;
            info.token.clear();
            info.href = failed_href.empty() ? "/" : std::move(failed_href);
            info.scope = scope;
            info.depth = depth;
            info.owner = owner;
            info.expires_at = std::chrono::steady_clock::now();
            return info;
        };

        NasWebDavLockRecord record;
        record.token = make_token();
        record.scope = scope == yuan::net::webdav::LockScope::exclusive ? "exclusive" : "shared";
        record.depth = depth == yuan::net::webdav::Depth::zero ? "0" :
                       depth == yuan::net::webdav::Depth::one ? "1" : "infinity";
        record.owner = std::move(owner);
        record.expires_at_unix_ms = unix_ms_after(timeout);

        if (auto route = route_of(href)) {
            if (auto share = shares_ ? shares_->find_by_name(route->share_name) : std::nullopt) {
                record.share_id = share->id;
                record.path = route->relative_path.empty() ? "/" : "/" + route->relative_path;
            }
        }

        if (metadata_ && !record.share_id.empty()) {
            if (!metadata_->try_create_webdav_lock(record)) {
                return make_failed_lock(record.path);
            }
        } else if (metadata_) {
            return make_failed_lock(href);
        }
        return to_lock_info(record);
    }

    bool NasRedisWebDavLockManager::refresh(std::string_view token, std::chrono::seconds timeout)
    {
        if (!metadata_) {
            return false;
        }
        prune_expired();
        auto record = metadata_->find_webdav_lock(normalize_token(token));
        if (!record || !lock_is_active(*record)) {
            return false;
        }
        record->expires_at_unix_ms = unix_ms_after(timeout);
        return metadata_->upsert_webdav_lock(*record);
    }

    bool NasRedisWebDavLockManager::unlock(std::string_view token)
    {
        return metadata_ && metadata_->remove_webdav_lock(normalize_token(token));
    }

    bool NasRedisWebDavLockManager::allows(std::string_view href, std::string_view if_header_or_token) const
    {
        prune_expired();
        const std::string token = normalize_token(if_header_or_token);
        for (const auto &lock : active_locks(href)) {
            if (!token.empty() && token == lock.token) {
                continue;
            }
            return false;
        }
        return true;
    }

    std::vector<yuan::net::webdav::LockInfo> NasRedisWebDavLockManager::active_locks(std::string_view href) const
    {
        std::vector<yuan::net::webdav::LockInfo> out;
        if (!metadata_) {
            return out;
        }
        prune_expired();
        auto route = route_of(href);
        if (!route) {
            return out;
        }
        auto share = shares_ ? shares_->find_by_name(route->share_name) : std::nullopt;
        if (!share) {
            return out;
        }
        const std::string path = route->relative_path.empty() ? "/" : "/" + route->relative_path;
        for (const auto &record : metadata_->list_webdav_locks(share->id, path)) {
            if (!lock_is_active(record)) {
                continue;
            }
            out.push_back(to_lock_info(record));
        }
        return out;
    }

    std::optional<yuan::net::webdav::LockInfo> NasRedisWebDavLockManager::find(std::string_view token) const
    {
        if (!metadata_) {
            return std::nullopt;
        }
        prune_expired();
        auto record = metadata_->find_webdav_lock(normalize_token(token));
        if (!record || !lock_is_active(*record)) {
            return std::nullopt;
        }
        return record ? std::optional<yuan::net::webdav::LockInfo>(to_lock_info(*record)) : std::nullopt;
    }

    void NasRedisWebDavLockManager::prune_expired() const
    {
        if (!metadata_) {
            return;
        }
        (void)metadata_->prune_expired_webdav_locks();
    }

    std::optional<NasWebDavRoute> NasRedisWebDavLockManager::route_of(std::string_view href) const
    {
        return adapter_.parse_route(std::string(adapter_.mount_path()) + std::string(href));
    }

    std::string NasRedisWebDavLockManager::normalize_token(std::string_view token)
    {
        std::string text(token);
        const std::string marker = "opaquelocktoken:";
        const auto pos = text.find(marker);
        if (pos != std::string::npos) {
            text = text.substr(pos);
        }
        if (text.size() >= 2 && text.front() == '<' && text.back() == '>') {
            text = text.substr(1, text.size() - 2);
        }
        const auto end = text.find_first_of(">) \t\r\n");
        if (end != std::string::npos) {
            text.resize(end);
        }
        return text;
    }

    std::string NasRedisWebDavLockManager::make_token()
    {
        static std::atomic<uint64_t> seq{ 1 };
        const auto id = seq.fetch_add(1, std::memory_order_relaxed);
        const auto now = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream oss;
        oss << "opaquelocktoken:" << std::hex << now << id;
        return oss.str();
    }

    std::int64_t NasRedisWebDavLockManager::unix_ms_after(std::chrono::seconds timeout)
    {
        const auto tp = std::chrono::system_clock::now() + timeout;
        return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    }

    yuan::net::webdav::LockInfo NasRedisWebDavLockManager::to_lock_info(const NasWebDavLockRecord &record)
    {
        yuan::net::webdav::LockInfo info;
        info.token = record.token;
        info.href = record.path.empty() ? "/" : record.path;
        info.scope = record.scope == "shared" ? yuan::net::webdav::LockScope::shared
                                               : yuan::net::webdav::LockScope::exclusive;
        info.depth = record.depth == "0" ? yuan::net::webdav::Depth::zero :
                     record.depth == "1" ? yuan::net::webdav::Depth::one
                                           : yuan::net::webdav::Depth::infinity;
        info.owner = record.owner;
        info.expires_at = to_steady_expiry(record.expires_at_unix_ms);
        return info;
    }
}
