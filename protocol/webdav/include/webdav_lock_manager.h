#ifndef __NET_WEBDAV_LOCK_MANAGER_H__
#define __NET_WEBDAV_LOCK_MANAGER_H__

#include "webdav_types.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace yuan::net::webdav
{
    struct LockInfo
    {
        std::string token;
        std::string href;
        LockScope scope = LockScope::exclusive;
        Depth depth = Depth::infinity;
        std::string owner;
        std::chrono::steady_clock::time_point expires_at{};
    };

    class WebDavLockManager
    {
    public:
        virtual ~WebDavLockManager() = default;

        virtual LockInfo create(std::string href, LockScope scope, Depth depth, std::string owner,
                                std::chrono::seconds timeout);
        virtual bool refresh(std::string_view token, std::chrono::seconds timeout);
        virtual bool unlock(std::string_view token);
        virtual bool allows(std::string_view href, std::string_view if_header_or_token) const;
        virtual std::vector<LockInfo> active_locks(std::string_view href) const;
        virtual std::optional<LockInfo> find(std::string_view token) const;
        virtual void prune_expired() const;

    private:
        static bool covers(const LockInfo &lock, std::string_view href);
        static std::string normalize_token(std::string_view token);
        void prune_expired_locked(std::chrono::steady_clock::time_point now) const;

        mutable std::unordered_map<std::string, LockInfo> locks_;
        mutable std::mutex mutex_;
    };
}

#endif
