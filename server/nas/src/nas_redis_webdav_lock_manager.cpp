#include "nas/nas_redis_webdav_lock_manager.h"

#include <chrono>
#include <random>
#include <sstream>

namespace yuan::server::nas
{
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
            (void)metadata_->upsert_webdav_lock(record);
        }
        return to_lock_info(record);
    }

    bool NasRedisWebDavLockManager::refresh(std::string_view token, std::chrono::seconds timeout)
    {
        if (!metadata_) {
            return false;
        }
        auto record = metadata_->find_webdav_lock(normalize_token(token));
        if (!record) {
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
            out.push_back(to_lock_info(record));
        }
        return out;
    }

    std::optional<yuan::net::webdav::LockInfo> NasRedisWebDavLockManager::find(std::string_view token) const
    {
        if (!metadata_) {
            return std::nullopt;
        }
        auto record = metadata_->find_webdav_lock(normalize_token(token));
        return record ? std::optional<yuan::net::webdav::LockInfo>(to_lock_info(*record)) : std::nullopt;
    }

    void NasRedisWebDavLockManager::prune_expired() const
    {
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
        static std::random_device rd;
        static std::mt19937_64 rng(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        std::ostringstream oss;
        oss << "opaquelocktoken:" << std::hex << dist(rng) << dist(rng);
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
        info.expires_at = std::chrono::steady_clock::now() + std::chrono::hours(1);
        return info;
    }
}
